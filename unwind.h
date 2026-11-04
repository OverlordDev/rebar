// C++ / Objective-C exception handling across the guest/host boundary.
//
// Guest frames are unwound by us (arm64 compact unwind info + Itanium LSDA, with the
// personality routines re-implemented here); host frames by the host's libunwind. The two
// meet at rebar_host_call, whose CFI names rebar_personality:
//
//   guest code -> stub -> host_call -> [host code that throws]       (e.g. NSArray -objectAtIndex:)
//   guest code -> __cxa_throw stub -> host_call -> host __cxa_throw  (the throw itself is a host call)
//
// Either way the host unwinder reaches the rebar_host_call frame, and
//   phase 1: rebar_personality searches the guest frames of that activation for a catch;
//   phase 2: it lands in rebar_host_call's landing code, rebar_arrive runs the guest
//            cleanup pads (the interpreter executes them) and either installs the guest
//            catch handler (control returns normally into the interpreter loop, now at the
//            handler) or reports that unwinding must continue into host frames above.
// _Unwind_Resume called by guest cleanup pads is a stub that just ends the pad (h_Unwind_Resume).
//
// Limits: DWARF-mode unwind entries (rare), exception specifications, dependent exceptions
// (std::rethrow_exception) and exceptions through call_guest() callbacks are not handled.

#include <unwind.h>

typedef struct { uint64_t func, enc, lsda; int pers; uint64_t fde, pers_fn; } FuncInfo;  // pers: 0 none, 1 C++, 2 ObjC, 3 gcc/C (4 other); fde: the DWARF FDE of a function whose compact encoding is "see DWARF"
static void dwarf_fde_info(GImage *im, uint64_t off, FuncInfo *fi);
typedef struct { void *tinfo, *obj; bool objc, cxx, is_ptr; } ExcInfo;
typedef struct { struct _Unwind_Exception *exc; HostCallRec *rec; uint64_t sp, func, lp; int selector; void *adjusted; const uint8_t *action, *lsda; } Found;
// What a personality routine of the guest's own (LuaJIT's lj_err_unwind_dwarf) is handed as its _Unwind_Context: the _Unwind_GetCFA / SetGR / SetIP calls it makes come to our handlers.
typedef struct { uint64_t magic, cfa, ip, gr[2]; } GCtx;
#define GCTX_MAGIC 0x72656261725f6374ull

static __thread Found t_found;
static __thread HostCallRec *t_landing_rec;
static __thread int t_in_driver;


static void *g_objc_ehtype_vtable, *g_ptr_ti_vtable;

static inline uint64_t rd64(uint64_t a) { return *(const uint64_t *)a; }
static inline uint32_t rd32(uint64_t a) { return *(const uint32_t *)a; }

[[noreturn]] static void unwind_fatal(const char *msg, uint64_t pc) {
    fprintf(stderr, "rebar: exception unwinding: %s (pc 0x%llx)\n", msg, (unsigned long long)pc);
    exit(134);
}

// ------------------------------------------------------------- compact unwind info

static int classify_personality(uint64_t fn) {
    if (fn - g_stub_base >= g_stub_size) return 4;
    const char *n = g_stubs[(fn - g_stub_base) / 16].name;
    if (!strcmp(n, "___gxx_personality_v0")) return 1;
    if (!strcmp(n, "___objc_personality_v0")) return 2;
    if (!strcmp(n, "___gcc_personality_v0")) return 3;
    return 4;
}

static bool unwind_lookup(uint64_t pc, FuncInfo *fi) {
    GImage *im = image_by_addr(pc);
    const uint8_t *ui = im ? im->unwind_info : NULL;
    if (!ui) return false;
    const uint64_t ibase = im->base;
    uint32_t common_off = rd32((uint64_t)ui + 4), common_cnt = rd32((uint64_t)ui + 8);
    uint32_t pers_off = rd32((uint64_t)ui + 12), idx_off = rd32((uint64_t)ui + 20), idx_cnt = rd32((uint64_t)ui + 24);
    uint32_t off = (uint32_t)(pc - ibase);
    const uint32_t *idx = (const uint32_t *)(ui + idx_off);  // {funcOffset, secondLevelOffset, lsdaIndexOffset}
    int lo = 0, hi = (int)idx_cnt - 2, page = -1;            // last index entry is a sentinel
    while (lo <= hi) { int mid = (lo + hi) / 2; if (idx[mid * 3] <= off) { page = mid; lo = mid + 1; } else hi = mid - 1; }
    if (page < 0 || off >= idx[(idx_cnt - 1) * 3]) return false;
    const uint8_t *pg = ui + idx[page * 3 + 1];
    uint32_t kind = rd32((uint64_t)pg), enc = 0, func_off = 0;
    if (kind == 2) {  // regular
        uint16_t eoff = *(const uint16_t *)(pg + 4), ecnt = *(const uint16_t *)(pg + 6);
        const uint32_t *ent = (const uint32_t *)(pg + eoff);
        int l = 0, h = (int)ecnt - 1, best = -1;
        while (l <= h) { int m = (l + h) / 2; if (ent[m * 2] <= off) { best = m; l = m + 1; } else h = m - 1; }
        if (best < 0) return false;
        func_off = ent[best * 2]; enc = ent[best * 2 + 1];
    } else if (kind == 3) {  // compressed
        uint16_t eoff = *(const uint16_t *)(pg + 4), ecnt = *(const uint16_t *)(pg + 6);
        uint16_t coff = *(const uint16_t *)(pg + 8);
        const uint32_t *ent = (const uint32_t *)(pg + eoff);
        uint32_t base_off = idx[page * 3];
        int l = 0, h = (int)ecnt - 1, best = -1;
        while (l <= h) { int m = (l + h) / 2; if (base_off + (ent[m] & 0xffffff) <= off) { best = m; l = m + 1; } else h = m - 1; }
        if (best < 0) return false;
        func_off = base_off + (ent[best] & 0xffffff);
        uint32_t ei = ent[best] >> 24;
        enc = ei < common_cnt ? ((const uint32_t *)(ui + common_off))[ei] : ((const uint32_t *)(pg + coff))[ei - common_cnt];
    } else return false;
    fi->func = ibase + func_off; fi->enc = enc; fi->lsda = 0; fi->pers = 0;
    uint32_t p = (enc >> 28) & 3;
    if (p) fi->pers = classify_personality(rd64(ibase + rd32((uint64_t)ui + pers_off + (p - 1) * 4)));
    if (enc & 0x40000000) {  // LSDA: search this page's slice of the LSDA index
        const uint32_t *l0 = (const uint32_t *)(ui + idx[page * 3 + 2]), *l1 = (const uint32_t *)(ui + idx[(page + 1) * 3 + 2]);
        for (const uint32_t *e = l0; e < l1; e += 2)
            if (e[0] == func_off) { fi->lsda = ibase + e[1]; break; }
    }
    fi->fde = 0;
    if (((enc >> 24) & 0xf) == 3 && im->eh_frame) dwarf_fde_info(im, enc & 0xffffff, fi);   // the personality and the LSDA of such a function are in its FDE
    return true;
}

// ------------------------------------------------------------------- frame stepping

static void uframe_to_cpu(CPU *c, const UFrame *f) {
    for (int k = 0; k < 10; k++) c->x[19 + k] = f->x[k];
    c->x[29] = f->fp; c->sp = f->sp;
    for (int k = 0; k < 8; k++) c->vr[8 + k].d[0] = f->d[k];
}

// ----------------------------------------------- DWARF call frame information
// A function whose compact unwind encoding is mode 3 ("DWARF") keeps its frame description in an FDE of __eh_frame (the encoding's low 24 bits are the FDE's
// offset in the section): functions with a variable-size frame (alloca, big arrays), hand-written assembly and the like. Registers are numbered as in the arm64
// DWARF ABI: x0-x30 = 0-30, sp = 31, d8-d15 = 72-79.

typedef struct { const uint8_t *insn, *insn_end; uint64_t caf; int64_t daf; int ra; uint8_t fde_enc, lsda_enc; uint64_t pers; bool z; } DwCie;
typedef struct { uint8_t kind; int64_t off; } DwRule;      // kind: 0 same value, 1 saved at CFA+off, 2 undefined, 3 value is CFA+off
typedef struct { int cfa_reg; int64_t cfa_off; DwRule r[80]; } DwState;
static uint64_t read_enc(const uint8_t **pp, uint8_t enc, uint64_t func);

static bool dw_parse_fde(uint64_t fde, DwCie *cie, uint64_t *pc_begin, uint64_t *pc_range, uint64_t *lsda, const uint8_t **insn, const uint8_t **insn_end) {
    uint32_t len = rd32(fde);
    if (len == 0 || len == 0xffffffffu) return false;
    uint64_t cie_at = fde + 4 - rd32(fde + 4);
    const uint8_t *cp = (const uint8_t *)cie_at, *cend = cp + 4 + rd32(cie_at);
    cp += 8;                                   // length, CIE id
    int version = *cp++;
    const char *aug = (const char *)cp; cp += strlen(aug) + 1;
    if (version >= 4) cp += 2;                 // address size, segment size
    memset(cie, 0, sizeof *cie);
    cie->caf = uleb(&cp); cie->daf = sleb(&cp); cie->ra = version == 1 ? *cp++ : (int)uleb(&cp);
    cie->fde_enc = 0; cie->lsda_enc = 0xff;
    if (aug[0] == 'z') {
        cie->z = true;
        uint64_t al = uleb(&cp); const uint8_t *aend = cp + al;
        for (const char *a = aug + 1; *a; a++) {
            if (*a == 'L') cie->lsda_enc = *cp++;
            else if (*a == 'R') cie->fde_enc = *cp++;
            else if (*a == 'P') { uint8_t pe = *cp++; cie->pers = read_enc(&cp, pe, 0); }
            else if (*a == 'S' || *a == 'B' || *a == 'G') continue;
            else break;
        }
        cp = aend;
    }
    cie->insn = cp; cie->insn_end = cend;
    const uint8_t *fp = (const uint8_t *)fde + 8;
    *pc_begin = read_enc(&fp, cie->fde_enc, 0);
    *pc_range = read_enc(&fp, cie->fde_enc & 0x0f, 0);
    *lsda = 0;
    if (cie->z) {
        uint64_t al = uleb(&fp); const uint8_t *aend = fp + al;
        if (cie->lsda_enc != 0xff && al) *lsda = read_enc(&fp, cie->lsda_enc, 0);
        fp = aend;
    }
    *insn = fp; *insn_end = (const uint8_t *)fde + 4 + len;
    return true;
}

static void dwarf_fde_info(GImage *im, uint64_t off, FuncInfo *fi) {
    DwCie cie; uint64_t pb, pr, lsda; const uint8_t *i0, *i1;
    uint64_t fde = im->eh_frame + off;
    if (off >= im->eh_size || !dw_parse_fde(fde, &cie, &pb, &pr, &lsda, &i0, &i1)) return;
    fi->fde = fde;
    if (cie.pers) { fi->pers = classify_personality(cie.pers); if (fi->pers == 4) fi->pers_fn = cie.pers; }
    if (lsda) fi->lsda = lsda;
}

// Runs call frame instructions from `p` to `end` up to the address `target`; `loc` is the address the state describes.
static bool dw_run(const uint8_t *p, const uint8_t *end, const DwCie *cie, DwState *st, const DwState *init, uint64_t *loc, uint64_t target) {
    DwState stack[8]; int sp = 0;
    while (p < end) {
        uint8_t op = *p++, hi = op & 0xc0, lo = op & 0x3f;
        uint64_t reg; int64_t off;
        if (hi == 0x40) { *loc += (uint64_t)lo * cie->caf; if (*loc > target) return true; continue; }
        if (hi == 0x80) { off = (int64_t)uleb(&p) * cie->daf; if (lo < 80) st->r[lo] = (DwRule){1, off}; continue; }
        if (hi == 0xc0) { if (lo < 80) st->r[lo] = init ? init->r[lo] : (DwRule){0, 0}; continue; }
        switch (op) {
        case 0x00: break;
        case 0x01: *loc = read_enc(&p, cie->fde_enc, 0); if (*loc > target) return true; break;
        case 0x02: *loc += (uint64_t)*p++ * cie->caf; if (*loc > target) return true; break;
        case 0x03: *loc += (uint64_t)*(const uint16_t *)p * cie->caf; p += 2; if (*loc > target) return true; break;
        case 0x04: *loc += (uint64_t)*(const uint32_t *)p * cie->caf; p += 4; if (*loc > target) return true; break;
        case 0x05: reg = uleb(&p); off = (int64_t)uleb(&p) * cie->daf; if (reg < 80) st->r[reg] = (DwRule){1, off}; break;
        case 0x06: reg = uleb(&p); if (reg < 80) st->r[reg] = init ? init->r[reg] : (DwRule){0, 0}; break;
        case 0x07: reg = uleb(&p); if (reg < 80) st->r[reg] = (DwRule){2, 0}; break;
        case 0x08: reg = uleb(&p); if (reg < 80) st->r[reg] = (DwRule){0, 0}; break;
        case 0x09: reg = uleb(&p); uleb(&p); if (reg < 80) st->r[reg] = (DwRule){2, 0}; break;   // saved in another register: not tracked
        case 0x0a: if (sp < 8) stack[sp++] = *st; break;
        case 0x0b: if (sp > 0) *st = stack[--sp]; break;
        case 0x0c: st->cfa_reg = (int)uleb(&p); st->cfa_off = (int64_t)uleb(&p); break;
        case 0x0d: st->cfa_reg = (int)uleb(&p); break;
        case 0x0e: st->cfa_off = (int64_t)uleb(&p); break;
        case 0x11: reg = uleb(&p); off = sleb(&p) * cie->daf; if (reg < 80) st->r[reg] = (DwRule){1, off}; break;
        case 0x12: st->cfa_reg = (int)uleb(&p); st->cfa_off = sleb(&p) * cie->daf; break;
        case 0x13: st->cfa_off = sleb(&p) * cie->daf; break;
        case 0x14: reg = uleb(&p); off = (int64_t)uleb(&p) * cie->daf; if (reg < 80) st->r[reg] = (DwRule){3, off}; break;
        case 0x15: reg = uleb(&p); off = sleb(&p) * cie->daf; if (reg < 80) st->r[reg] = (DwRule){3, off}; break;
        case 0x2d: break;                                                  // DW_CFA_AARCH64_negate_ra_state (pointer authentication is the identity here)
        case 0x2e: uleb(&p); break;                                        // DW_CFA_GNU_args_size
        default: return false;                                             // expressions: not supported
        }
    }
    return true;
}

static void dwarf_step(UFrame *f, const FuncInfo *fi) {
    DwCie cie; uint64_t pb, pr, lsda; const uint8_t *i0, *i1;
    if (!fi->fde || !dw_parse_fde(fi->fde, &cie, &pb, &pr, &lsda, &i0, &i1)) unwind_fatal("no DWARF frame description for a frame", f->pc);
    DwState st, init; memset(&st, 0, sizeof st);
    uint64_t loc = pb, target = f->pc - 1;
    if (!dw_run(cie.insn, cie.insn_end, &cie, &st, NULL, &loc, ~0ull)) unwind_fatal("unsupported DWARF instruction in a CIE", f->pc);
    init = st; loc = pb;
    if (!dw_run(i0, i1, &cie, &st, &init, &loc, target)) unwind_fatal("unsupported DWARF instruction (an expression) in an FDE", f->pc);
    uint64_t cfa_base = st.cfa_reg == 31 ? f->sp : st.cfa_reg == 29 ? f->fp : st.cfa_reg == 30 ? f->lr : (st.cfa_reg >= 19 && st.cfa_reg <= 28) ? f->x[st.cfa_reg - 19] : 0;
    if (!cfa_base) unwind_fatal("DWARF frame whose CFA register is not tracked", f->pc);
    uint64_t cfa = cfa_base + (uint64_t)st.cfa_off;
    UFrame n = *f;
    for (int r = 19; r <= 30; r++) {
        DwRule ru = st.r[r];
        if (ru.kind != 1 && ru.kind != 3) continue;
        uint64_t v = ru.kind == 1 ? rd64(cfa + (uint64_t)ru.off) : cfa + (uint64_t)ru.off;
        if (r <= 28) n.x[r - 19] = v; else if (r == 29) n.fp = v; else n.lr = v;
    }
    for (int r = 72; r <= 79; r++) if (st.r[r].kind == 1) n.d[r - 72] = rd64(cfa + (uint64_t)st.r[r].off);
    DwRule rar = st.r[cie.ra < 80 ? cie.ra : 30];
    n.pc = rar.kind == 1 ? rd64(cfa + (uint64_t)rar.off) : f->lr;
    n.sp = cfa;
    n.lr = 0;
    *f = n;
}

static void uframe_step(UFrame *f, const FuncInfo *fi) {
    uint32_t enc = (uint32_t)fi->enc;
    int mode = (enc >> 24) & 0xf;
    uint64_t sp_new, pc_new, fp_new = f->fp, loc;
    if (mode == 4) {  // frame-based: x29/x30 saved at [fp], [fp+8]; callee-saved pairs below fp
        loc = f->fp;  // saved pairs start at fp-8 (x19), fp-16 (x20), ...
        sp_new = f->fp + 16; pc_new = rd64(f->fp + 8); fp_new = rd64(f->fp);
    } else if (mode == 2) {  // frameless leaf-style: lr still in x30
        uint64_t stack = (uint64_t)((enc >> 12) & 0xfff) * 16;
        loc = f->sp + stack;
        sp_new = f->sp + stack; pc_new = f->lr;
    } else if (mode == 3) {  // described by an FDE of __eh_frame
        dwarf_step(f, fi);
        return;
    } else unwind_fatal("no unwind info for frame", f->pc);
    for (int k = 0; k < 5; k++)
        if (enc & (1u << k)) { loc -= 8; f->x[2 * k] = rd64(loc); loc -= 8; f->x[2 * k + 1] = rd64(loc); }
    for (int k = 0; k < 4; k++)   // UNWIND_ARM64_FRAME_D8_D9_PAIR .. D14_D15_PAIR are 0x100, 0x200, 0x400, 0x800 (bits 8-11, after the five x pairs and a gap)
        if (enc & (1u << (8 + k))) { loc -= 8; f->d[2 * k] = rd64(loc); loc -= 8; f->d[2 * k + 1] = rd64(loc); }
    f->sp = sp_new; f->pc = pc_new; f->fp = fp_new; f->lr = 0;
}

static bool at_boundary(uint64_t pc) {
    return pc == stub_addr(STUB_RET) || pc == stub_addr(STUB_EXIT) || !in_guest(pc);
}

// ---------------------------------------------------------------------- LSDA parsing

static uint64_t read_enc(const uint8_t **pp, uint8_t enc, uint64_t func) {
    if (enc == 0xff) return 0;
    const uint8_t *start = *pp;
    uint64_t v;
    switch (enc & 0x0f) {
    case 0x00: v = *(const uint64_t *)*pp; *pp += 8; break;
    case 0x01: v = uleb(pp); break;
    case 0x02: v = *(const uint16_t *)*pp; *pp += 2; break;
    case 0x03: v = *(const uint32_t *)*pp; *pp += 4; break;
    case 0x04: v = *(const uint64_t *)*pp; *pp += 8; break;
    case 0x09: v = (uint64_t)sleb(pp); break;
    case 0x0a: v = (uint64_t)(int64_t)*(const int16_t *)*pp; *pp += 2; break;
    case 0x0b: v = (uint64_t)(int64_t)*(const int32_t *)*pp; *pp += 4; break;
    case 0x0c: v = *(const uint64_t *)*pp; *pp += 8; break;
    default: unwind_fatal("unsupported pointer encoding in LSDA", (uint64_t)start);
    }
    if (!v) return 0;
    switch (enc & 0x70) {
    case 0x10: v += (uint64_t)start; break;   // pcrel
    case 0x40: v += func; break;              // funcrel
    case 0x00: break;
    default: unwind_fatal("unsupported pointer base in LSDA", (uint64_t)start);
    }
    if (enc & 0x80) v = *(const uint64_t *)v;
    return v;
}
static int enc_size(uint8_t enc) {
    switch (enc & 0x0f) { case 0x02: case 0x0a: return 2; case 0x03: case 0x0b: return 4; default: return 8; }
}

typedef struct { bool found; uint64_t lp; const uint8_t *action, *ttype_base; uint8_t ttype_enc; } CallSite;

static CallSite lsda_callsite(const uint8_t *lsda, uint64_t func, uint64_t ip) {
    CallSite cs = {0};
    const uint8_t *p = lsda;
    uint8_t lpstart_enc = *p++;
    uint64_t lpstart = lpstart_enc != 0xff ? read_enc(&p, lpstart_enc, func) : func;
    cs.ttype_enc = *p++;
    if (cs.ttype_enc != 0xff) { uint64_t off = uleb(&p); cs.ttype_base = p + off; }
    uint8_t cs_enc = *p++;
    uint64_t cs_len = uleb(&p);
    const uint8_t *end = p + cs_len, *actions = end;
    while (p < end) {
        uint64_t start = read_enc(&p, cs_enc & 0x0f, func), len = read_enc(&p, cs_enc & 0x0f, func);
        uint64_t lp = read_enc(&p, cs_enc & 0x0f, func), act = uleb(&p);
        if (ip >= lpstart + start && ip < lpstart + start + len) {
            cs.found = true; cs.lp = lp ? lpstart + lp : 0; cs.action = act ? actions + act - 1 : NULL;
            return cs;
        }
    }
    return cs;
}

// ---------------------------------------------------------------- catch matching

static void exc_info(struct _Unwind_Exception *exc, ExcInfo *e) {
    e->cxx = (exc->exception_class & ~0xffull) == 0x434C4E47432B2B00ull;
    e->tinfo = e->cxx ? *(void **)((char *)exc - 80) : NULL;
    e->obj = (char *)exc + 32;
    void *vt = e->tinfo ? *(void **)e->tinfo : NULL;
    e->objc = vt && vt == (char *)g_objc_ehtype_vtable + 16;
    e->is_ptr = vt && vt == (char *)g_ptr_ti_vtable + 16;
}

static bool type_matches(const ExcInfo *e, void *catch_ti, void **adjusted) {
    *adjusted = e->obj;
    if (!e->cxx) return !catch_ti;                      // foreign exception: only catch(...)
    if (!catch_ti) return true;                         // catch(...)
    if (g_trace) fprintf(stderr, "  [eh] catch_ti=%p words: %llx %llx %llx (objc vtable %p)\n", catch_ti, (unsigned long long)((uint64_t *)catch_ti)[0], (unsigned long long)((uint64_t *)catch_ti)[1], (unsigned long long)((uint64_t *)catch_ti)[2], (char *)g_objc_ehtype_vtable + 16);
    if (*(void **)catch_ti == (char *)g_objc_ehtype_vtable + 16) {  // @catch(Foo *)
        if (!e->objc) return false;
        const char *name = ((const char **)catch_ti)[1];
        Class cls = ((Class *)catch_ti)[2];
        *adjusted = *(id *)e->obj;  // objc_begin_catch is just __cxa_begin_catch: the handler gets the id itself
        if (!name || !strcmp(name, "id")) return true;
        if (!cls) cls = objc_getClass(name);
        if (g_trace) fprintf(stderr, "  [eh] objc catch type name=%s cls=%p (%s) thrown class=%s\n", name ? name : "(null)", (void *)cls, cls ? class_getName(cls) : "-", class_getName(object_getClass(*(id *)e->obj)));
        if (!cls) return false;
        for (Class k = object_getClass(*(id *)e->obj); k; k = class_getSuperclass(k))  // objc_exception_match is not exported
            if (k == cls) return true;
        return false;
    }
    if (e->objc) return false;
    typedef bool (*can_catch_fn)(void *self, void *thrown, void **adj);
    can_catch_fn can_catch = (can_catch_fn)((void **)*(void **)catch_ti)[4];  // __shim_type_info::can_catch
    return can_catch(catch_ti, e->tinfo, adjusted);
}

// Returns the catch selector (>0) of the first matching clause, or 0 with *cleanup set.
static int lsda_actions(const CallSite *cs, uint64_t func, const ExcInfo *e, bool *cleanup, void **adjusted) {
    *cleanup = false;
    if (!cs->action) { *cleanup = true; return 0; }
    const uint8_t *p = cs->action;
    for (;;) {
        int64_t filter = sleb(&p);
        const uint8_t *next = p;
        int64_t disp = sleb(&p);
        if (filter > 0 && cs->ttype_base) {
            const uint8_t *q = cs->ttype_base - filter * enc_size(cs->ttype_enc);
            void *ti = (void *)read_enc(&q, cs->ttype_enc, func);
            if (type_matches(e, ti, adjusted)) return (int)filter;
        } else if (filter == 0) *cleanup = true;
        if (!disp) break;
        p = next + disp;
    }
    return 0;
}

[[noreturn]] static void call_terminate(void) {
    void (*terminate)(void) = dlsym(RTLD_DEFAULT, "_ZSt9terminatev");
    terminate();
    abort();
}

// ------------------------------------------------------------- phase 1 and phase 2

// A personality routine that is not one of the three we know is guest code: run it with a context of our own (call frame address, registers to set, address to resume at).
static uint64_t cfa_of_frame(const UFrame *f, const FuncInfo *fi) { UFrame g = *f; uframe_step(&g, fi); return g.sp; }
static int call_guest_personality(CPU *c, uint64_t fn, int actions, struct _Unwind_Exception *exc, GCtx *gc) {
    CPU saved = *c;
    c->x[0] = 1; c->x[1] = (uint64_t)actions; c->x[2] = exc->exception_class; c->x[3] = (uint64_t)exc; c->x[4] = (uint64_t)gc;
    c->x[30] = stub_addr(STUB_RET); c->pc = fn;
    bool held = gil_release();
    run_until(c, stub_addr(STUB_RET));
    if (held) gil_acquire(); else if (t_have_gil) gil_release();
    int r = (int)c->x[0];
    *c = saved;
    return r;
}
// (the stubs of the context accessors; the host's own _Unwind_Context is something else, those calls go on to the host)
H(h_Unwind_GetCFA)  { GCtx *g = P(0); if (g->magic == GCTX_MAGIC) c->x[0] = g->cfa; else host_call(c, HOSTFN("_Unwind_GetCFA"), false); }
H(h_Unwind_SetGR)   { GCtx *g = P(0); if (g->magic == GCTX_MAGIC) { if (c->x[1] < 2) g->gr[c->x[1]] = c->x[2]; } else host_call(c, HOSTFN("_Unwind_SetGR"), false); }
H(h_Unwind_SetIP)   { GCtx *g = P(0); if (g->magic == GCTX_MAGIC) g->ip = c->x[1]; else host_call(c, HOSTFN("_Unwind_SetIP"), false); }
H(h_Unwind_GetIP)   { GCtx *g = P(0); if (g->magic == GCTX_MAGIC) c->x[0] = g->ip; else host_call(c, HOSTFN("_Unwind_GetIP"), false); }
H(h_Unwind_GetGR)   { GCtx *g = P(0); if (g->magic == GCTX_MAGIC) c->x[0] = c->x[1] < 2 ? g->gr[c->x[1]] : 0; else host_call(c, HOSTFN("_Unwind_GetGR"), false); }

// Phase 1: is there a guest catch clause for this exception in this activation?
static bool guest_search(HostCallRec *rec, struct _Unwind_Exception *exc) {
    ExcInfo e;
    exc_info(exc, &e);
    UFrame f = rec->f;
    for (int depth = 0; depth < 8192; depth++) {
        FuncInfo fi;
        if (!unwind_lookup(f.pc - 1, &fi)) return false;
        if (g_trace) fprintf(stderr, "  [eh] search frame pc=%llx func=%llx enc=%llx lsda=%llx pers=%d\n", (unsigned long long)f.pc, (unsigned long long)fi.func, (unsigned long long)fi.enc, (unsigned long long)fi.lsda, fi.pers);
        if (fi.pers == 4 && fi.pers_fn) {   // a personality routine of the guest's own
            GCtx gc = {GCTX_MAGIC, f.sp, 0, {0, 0}};   // (_Unwind_GetCFA of the context of a frame is that frame's own stack pointer at the call)
            if (g_trace) fprintf(stderr, "  [eh] guest personality %llx for func %llx: frame sp=%llx fp=%llx\n", (unsigned long long)fi.pers_fn, (unsigned long long)fi.func, (unsigned long long)f.sp, (unsigned long long)f.fp);
            CPU *c = cur_cpu();
            if (call_guest_personality(c, fi.pers_fn, _UA_SEARCH_PHASE, exc, &gc) == _URC_HANDLER_FOUND) {
                t_found = (Found){exc, rec, f.sp, fi.func, 0, -2, NULL, NULL, NULL};
                return true;
            }
        }
        if (fi.lsda && fi.pers && fi.pers <= 3) {
            CallSite cs = lsda_callsite((const uint8_t *)fi.lsda, fi.func, f.pc - 1);
            if (!cs.found) call_terminate();
            if (cs.lp) {
                bool cleanup; void *adj = e.obj;
                int sel = lsda_actions(&cs, fi.func, &e, &cleanup, &adj);
                if (sel > 0) {
                    t_found = (Found){exc, rec, f.sp, fi.func, cs.lp, sel, adj, cs.action, (const uint8_t *)fi.lsda};
                    return true;
                }
            }
        }
        uframe_step(&f, &fi);
        if (at_boundary(f.pc)) return false;
    }
    unwind_fatal("unwind walk too deep", f.pc);
}

// Phase 2 for one activation. Returns 1 if the guest handler was installed (the CPU now
// stands at the catch landing pad), 0 if the guest frames are exhausted and unwinding
// must continue in host frames.
void rebar_gil_drop(void) { gil_release(); }

int rebar_arrive(struct _Unwind_Exception *exc, long is_handler_frame) {
    gil_acquire();  // the exception arrives from host code; guest cleanups and handlers run under the interpreter lock
    CPU *c = cur_cpu();
    if (g_trace) fprintf(stderr, "  [eh] arrive handler_frame=%ld\n", is_handler_frame);
    HostCallRec *rec = t_landing_rec;
    ExcInfo e;
    exc_info(exc, &e);
    UFrame f = rec->f;
    for (int depth = 0; depth < 8192; depth++) {
        FuncInfo fi;
        if (!unwind_lookup(f.pc - 1, &fi)) return 0;
        if (fi.pers == 4 && fi.pers_fn) {   // a personality routine of the guest's own (LuaJIT's catch frames): it says where to resume, and with what in x0
            bool mine = is_handler_frame && t_found.exc == exc && t_found.sp == f.sp && t_found.func == fi.func && t_found.selector == -2;
            GCtx gc = {GCTX_MAGIC, f.sp, 0, {0, 0}};   // (_Unwind_GetCFA of the context of a frame is that frame's own stack pointer at the call)
            if (call_guest_personality(c, fi.pers_fn, _UA_CLEANUP_PHASE | (mine ? _UA_HANDLER_FRAME : 0), exc, &gc) == _URC_INSTALL_CONTEXT && gc.ip) {
                uframe_to_cpu(c, &f);
                c->pc = gc.ip;
                t_pend_x0 = gc.gr[0]; t_pend_x1 = gc.gr[1];
                t_landed = true;
                return 1;
            }
        }
        if (fi.lsda && fi.pers && fi.pers <= 3) {
            CallSite cs = lsda_callsite((const uint8_t *)fi.lsda, fi.func, f.pc - 1);
            if (!cs.found) call_terminate();
            if (is_handler_frame && t_found.exc == exc && t_found.sp == f.sp && t_found.func == fi.func) {
                // install the catch landing pad
                char *hdr = (char *)exc;
                *(void **)(hdr - 8) = t_found.adjusted;             // adjustedPtr
                *(int *)(hdr - 36) = t_found.selector;              // handlerSwitchValue
                *(const uint8_t **)(hdr - 32) = t_found.action;     // actionRecord
                *(const uint8_t **)(hdr - 24) = t_found.lsda;       // languageSpecificData
                *(void **)(hdr - 16) = (void *)t_found.lp;          // catchTemp
                uframe_to_cpu(c, &f);
                c->pc = t_found.lp;
                t_pend_x0 = (uint64_t)exc; t_pend_x1 = (uint64_t)t_found.selector;
                t_landed = true;
                return 1;
            }
            if (cs.lp) {
                bool cleanup; void *adj = e.obj;
                lsda_actions(&cs, fi.func, &e, &cleanup, &adj);
                if (cleanup) {  // run the cleanup pad; it ends in _Unwind_Resume (h_Unwind_Resume)
                    uframe_to_cpu(c, &f);
                    c->pc = cs.lp; c->x[0] = (uint64_t)exc; c->x[1] = 0;
                    t_in_driver++;
                    run_until(c, stub_addr(STUB_RET));
                    t_in_driver--;
                    uframe_from_cpu(c, &f);  // continue from the resume call inside this same function
                    continue;
                }
            }
        }
        uframe_step(&f, &fi);
        if (at_boundary(f.pc)) return 0;
    }
    unwind_fatal("unwind walk too deep", f.pc);
}

H(h_Unwind_Resume) {
    if (!t_in_driver) unwind_fatal("_Unwind_Resume outside of a guest cleanup", c->x[30]);
    c->pc = stub_addr(STUB_RET);  // ends the cleanup pad; rebar_arrive continues the walk
}

// ------------------------------------------------------------ host-side personalities

extern void *rebar_host_landing_addr(void);

static HostCallRec *rec_for_cfa(uint64_t cfa) {
    HostCallRec *r = t_hc;
    while (r && (uint64_t)r < cfa) r = r->prev;  // records below this frame belong to abandoned inner calls
    return r;
}

_Unwind_Reason_Code rebar_personality(int version, _Unwind_Action actions, uint64_t cls,
                                        struct _Unwind_Exception *exc, struct _Unwind_Context *ctx) {
    if (_Unwind_GetIP(ctx) >= (uintptr_t)rebar_host_landing_addr()) return _URC_CONTINUE_UNWIND;  // resumed from our landing code
    if (actions & _UA_FORCE_UNWIND) return _URC_CONTINUE_UNWIND;
    HostCallRec *rec = rec_for_cfa(_Unwind_GetCFA(ctx));
    if (g_trace) fprintf(stderr, "  [eh] personality actions=0x%x cfa=%llx rec=%p\n", actions, (unsigned long long)_Unwind_GetCFA(ctx), (void *)rec);
    if (!rec) return _URC_CONTINUE_UNWIND;
    if (actions & _UA_SEARCH_PHASE) return guest_search(rec, exc) ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
    bool handler = (actions & _UA_HANDLER_FRAME) && t_found.exc == exc && t_found.rec == rec;
    t_landing_rec = rec;
    _Unwind_SetGR(ctx, 0, (uintptr_t)exc);
    _Unwind_SetGR(ctx, 1, handler ? 1 : 0);
    _Unwind_SetIP(ctx, (uintptr_t)rebar_host_landing_addr());
    return _URC_INSTALL_CONTEXT;
}

// The entry thunk is where guest code was entered from host code: when an exception leaves
// it, the CPU must go back to the state of the host call that the activation interrupted.
_Unwind_Reason_Code rebar_entry_personality(int version, _Unwind_Action actions, uint64_t cls,
                                              struct _Unwind_Exception *exc, struct _Unwind_Context *ctx) {
    if (actions & _UA_SEARCH_PHASE) return _URC_CONTINUE_UNWIND;
    uint64_t cfa = _Unwind_GetCFA(ctx);
    Act *outermost = NULL;
    for (Act *a = t_act; a && (uint64_t)a < cfa; a = a->prev) { outermost = a; if (a->own_gil) gil_release(); }  // activations abandoned by this unwind
    if (outermost) {
        *outermost->cpu = *outermost->saved;
        t_hc = outermost->hc_at_entry;
        t_act = outermost->prev;
    }
    return _URC_CONTINUE_UNWIND;
}

// ------------------------------------------------------------------- setup

// arm64 libc++ marks non-unique RTTI by setting bit 63 of the name pointer. Guest code compares type names against constants that include the tag (std::any_cast,
// typeid ==), so the tag stays in the guest's typeinfo; clearing it made every std::any_cast of a guest type throw bad_any_cast (Blender). The one place where the host
// reads such a name is the terminate handler (rebar_terminate in rebar.c clears the tag of the uncaught exception's type right before it runs).
// REBAR_CLEAR_TI_TAG=1 brings the old behaviour (clear it in every guest typeinfo at load) back.
static uint64_t g_ti_vts[16];
static int g_nti_vts;
// On macOS dyld coalesces the weak definitions of a type's typeinfo across images; rebar does not, so the same type has one typeinfo (and one name string) per image, and the host's
// libc++abi, which compares typeinfos by their name pointer, would take them for different types: an exception thrown in one library never matched a catch clause in another
// (LibreOffice's NoSuchElementException). Every typeinfo's name pointer is therefore replaced by the first one seen for that name.
typedef struct TiName { struct TiName *next; uint64_t canon; } TiName;
static TiName *g_ti_names[1 << 14];
static pthread_mutex_t g_ti_mu = PTHREAD_MUTEX_INITIALIZER;
static void ti_canonical_name(uint64_t *field) {
    static int off = -1; if (off < 0) off = getenv("REBAR_NO_TI_UNIFY") != NULL;   // REBAR_NO_TI_UNIFY=1: leave every image's typeinfo names alone (to compare)
    if (off) return;
    uint64_t v = *field, tag = v & (1ull << 63);
    const char *str = (const char *)(v & ~(1ull << 63));
    if (!str || (uint64_t)str < 0x10000) return;
    uint32_t h = 2166136261u;
    for (const char *q = str; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
    h &= (1 << 14) - 1;
    pthread_mutex_lock(&g_ti_mu);
    for (TiName *t = g_ti_names[h]; t; t = t->next)
        if (!strcmp((const char *)(t->canon & ~(1ull << 63)), str)) { *field = t->canon | tag; pthread_mutex_unlock(&g_ti_mu); return; }
    TiName *n = malloc(sizeof *n);
    n->canon = v; n->next = g_ti_names[h]; g_ti_names[h] = n;
    pthread_mutex_unlock(&g_ti_mu);
}
// A typeinfo made at run time (UNO's gcc3 bridge builds the ones of exceptions that no library defines, with a name it copied) is thrown with its name and those of its bases unified
// with the static ones, so that a catch clause of the same type in a library matches it.
static uint64_t g_ti_vt_si, g_ti_vt_vmi;
static void ti_canonicalize_tree(uint64_t ti, int depth) {
    if (ti < 0x100000000ull || ti >= 0x7ff800000000ull || (ti & 7) || depth > 16) return;   // only the guest's own typeinfos (and those made on its heap): the host's are read-only and stay as they are
    uint64_t vt = rd64(ti);
    ti_canonical_name((uint64_t *)(ti + 8));
    if (vt == g_ti_vt_si) ti_canonicalize_tree(rd64(ti + 16), depth + 1);
    else if (vt == g_ti_vt_vmi) { uint32_t nb = rd32(ti + 20); for (uint32_t k = 0; k < nb && k < 32; k++) ti_canonicalize_tree(rd64(ti + 24 + 16 * (uint64_t)k), depth + 1); }
}
static void unwind_setup_image(GImage *img) {
    const struct mach_header_64 *mh = img->mh;
    const struct section_64 *ui = NULL;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        const struct section_64 *s = (const void *)(sg + 1);
        for (uint32_t j = 0; j < sg->nsects; j++, s++)
            { if (!strcmp(s->segname, "__TEXT") && !strcmp(s->sectname, "__unwind_info")) ui = s;
              if (!strcmp(s->segname, "__TEXT") && !strcmp(s->sectname, "__eh_frame")) { img->eh_frame = s->addr + img->slide; img->eh_size = s->size; } }
    }
    if (ui) img->unwind_info = (const uint8_t *)(ui->addr + img->slide);
    static bool once;
    if (!once) {
        once = true;
        g_objc_ehtype_vtable = dlsym(RTLD_DEFAULT, "objc_ehtype_vtable");
        g_ptr_ti_vtable = dlsym(RTLD_DEFAULT, "_ZTVN10__cxxabiv119__pointer_type_infoE");
        static const char *vt_names[] = {
            "_ZTVN10__cxxabiv117__class_type_infoE", "_ZTVN10__cxxabiv120__si_class_type_infoE",
            "_ZTVN10__cxxabiv121__vmi_class_type_infoE", "_ZTVN10__cxxabiv119__pointer_type_infoE",
            "_ZTVN10__cxxabiv116__enum_type_infoE", "_ZTVN10__cxxabiv123__fundamental_type_infoE",
            "_ZTVN10__cxxabiv117__array_type_infoE", "_ZTVN10__cxxabiv120__function_type_infoE",
            "_ZTVN10__cxxabiv129__pointer_to_member_type_infoE", NULL};
        for (int k = 0; vt_names[k]; k++) { void *p = dlsym(RTLD_DEFAULT, vt_names[k]); if (p) g_ti_vts[g_nti_vts++] = (uint64_t)p + 16; }
        { void *a = dlsym(RTLD_DEFAULT, "_ZTVN10__cxxabiv120__si_class_type_infoE"), *b = dlsym(RTLD_DEFAULT, "_ZTVN10__cxxabiv121__vmi_class_type_infoE"); if (a) g_ti_vt_si = (uint64_t)a + 16; if (b) g_ti_vt_vmi = (uint64_t)b + 16; }
    }
    if (!g_nti_vts) return;
    bool clear_tag = getenv("REBAR_CLEAR_TI_TAG") != NULL;
    lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        if (strncmp(sg->segname, "__DATA", 6)) continue;
        uint64_t lo = sg->vmaddr + img->slide;
        for (uint64_t a = lo; a + 16 <= lo + sg->filesize; a += 8) {
            uint64_t w = rd64(a);
            for (int v = 0; v < g_nti_vts; v++)
                if (w == g_ti_vts[v]) {
                    if (clear_tag && (rd64(a + 8) >> 63)) *(uint64_t *)(a + 8) &= ~(1ull << 63);
                    ti_canonical_name((uint64_t *)(a + 8));
                    break;
                }
        }
    }
}

// ------------------------------------------------------------- libunwind's local API for the guest (CoreCLR's PAL_VirtualUnwind and its stack walks)
// The guest's unw_* imports would reach the host's libunwind, which knows nothing of arm64 registers or of the guest's frames. The context and the cursor hold a UFrame
// (the callee-saved view this unwinder already uses) behind a marker; registers the view lacks (x0-x18) read as 0. Register numbers are libunwind's for arm64:
// x0-x30 = 0-30, sp = 31, pc = 32, d0-d31 = 64-95, UNW_REG_IP = -1, UNW_REG_SP = -2.
#define UNWC_MAGIC 0x756e776374783030ull
typedef struct { uint64_t magic; UFrame f; } UnwBox;
static void unwind_cpu_frame(CPU *c, UFrame *f) {
    memset(f, 0, sizeof *f);
    f->pc = c->x[30]; f->sp = c->sp; f->fp = c->x[29]; f->lr = 0;
    for (int k = 0; k < 10; k++) f->x[k] = c->x[19 + k];
    for (int k = 0; k < 8; k++) f->d[k] = c->vr[8 + k].d[0];
}
H(h_unw_getcontext) { UnwBox *b = (UnwBox *)c->x[0]; b->magic = UNWC_MAGIC; unwind_cpu_frame(c, &b->f); c->x[0] = 0; }
H(h_unw_init_local) { UnwBox *cur = (UnwBox *)c->x[0], *ctx = (UnwBox *)c->x[1]; *cur = *ctx; cur->magic = UNWC_MAGIC; c->x[0] = 0; }
H(h_unw_step) {
    UnwBox *cur = (UnwBox *)c->x[0]; FuncInfo fi;
    if (!cur->f.pc) { c->x[0] = 0; return; }
    if (!unwind_lookup(cur->f.pc - 1, &fi)) { c->x[0] = (uint64_t)(int64_t)-6540; return; }   // UNW_ENOINFO
    uframe_step(&cur->f, &fi);
    c->x[0] = cur->f.pc ? 1 : 0;
}
H(h_unw_get_reg) {
    UnwBox *cur = (UnwBox *)c->x[0]; int r = (int)c->x[1]; uint64_t v = 0;
    if (r == -1 || r == 32) v = cur->f.pc; else if (r == -2 || r == 31) v = cur->f.sp; else if (r == 29) v = cur->f.fp; else if (r == 30) v = cur->f.lr ? cur->f.lr : 0;
    else if (r >= 19 && r <= 28) v = cur->f.x[r - 19];
    *(uint64_t *)c->x[2] = v; c->x[0] = 0;
}
H(h_unw_set_reg) {
    UnwBox *cur = (UnwBox *)c->x[0]; int r = (int)c->x[1]; uint64_t v = c->x[2];
    if (r == -1 || r == 32) cur->f.pc = v; else if (r == -2 || r == 31) cur->f.sp = v; else if (r == 29) cur->f.fp = v; else if (r == 30) cur->f.lr = v; else if (r >= 19 && r <= 28) cur->f.x[r - 19] = v;
    c->x[0] = 0;
}
H(h_unw_get_fpreg) { UnwBox *cur = (UnwBox *)c->x[0]; int r = (int)c->x[1]; *(uint64_t *)c->x[2] = (r >= 72 && r <= 79) ? cur->f.d[r - 72] : 0; c->x[0] = 0; }
H(h_unw_set_fpreg) { UnwBox *cur = (UnwBox *)c->x[0]; int r = (int)c->x[1]; if (r >= 72 && r <= 79) cur->f.d[r - 72] = c->vr[0].d[0]; c->x[0] = 0; }
H(h_unw_is_signal_frame) { c->x[0] = 0; }

