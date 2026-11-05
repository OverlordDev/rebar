// Objective-C support: registers the guest's classes with the host's libobjc and
// bridges objc_msgSend between the arm64 guest and the x86_64 runtime.
//
// The compiled class structures of the guest are handed to the host runtime in
// place (objc_readClassPair), so every pointer the guest already holds (classrefs,
// ivar offset variables, isa) stays valid. Method IMPs are guest addresses; when
// the host runtime or Foundation calls one, the exec-fault handler enters the
// interpreter. When the guest sends a message to a guest method we jump straight
// to the IMP, keeping the arm64 ABI (stack varargs, x8 struct returns) intact.

#include <objc/objc.h>
#include <spawn.h>
#include <objc/runtime.h>

typedef struct { uint64_t isa, superclass, cache, vtable, data; } GClass;
typedef struct {
    uint32_t flags, instanceStart, instanceSize, reserved;
    uint64_t ivarLayout, name, baseMethods, baseProtocols, ivars, weakIvarLayout, baseProperties;
} GRO;
typedef struct { uint64_t name, cls, instanceMethods, classMethods, protocols, instanceProperties, classProperties; } GCat;

static inline GRO *class_ro(const GClass *c) { return (GRO *)(c->data & ~7ull); }

static void *p_msgSend, *p_msgSend_stret, *p_msgForward;
static Class (*p_readClassPair)(Class, const void *);
static SEL s_alloc, s_autorelease, s_release, s_load;
static SEL s_initWithFormat_arguments, s_initWithFormat_locale_arguments, s_appendString, s_stringByAppendingString;
static SEL s_stringWithFormat, s_initWithFormat, s_appendFormat, s_stringByAppendingFormat, s_predicateWithFormat;
static SEL s_predicateWithFormat_arguments;
static SEL s_arrayWithObjects, s_initWithObjects, s_setWithObjects, s_orderedSetWithObjects;
static SEL s_dictionaryWithObjectsAndKeys, s_initWithObjectsAndKeys;
static SEL s_initWithColorsAndLocations, s_initWithColors_atLocations_colorSpace, s_handleFailureInFunction, s_handleFailureInMethod;
static SEL s_withObjects_count, s_initWithObjects_count, s_dictWith_forKeys_count, s_initWith_forKeys_count;
static SEL s_setWithObjects_count, s_orderedSetWithObjects_count;
static SEL s_raise_format, s_raise_format_arguments;

static void objc_bridge_init(void) {
    if (p_msgSend) return;
    p_msgSend = dlsym(RTLD_DEFAULT, "objc_msgSend");
    p_msgSend_stret = dlsym(RTLD_DEFAULT, "objc_msgSend_stret");
    p_msgForward = dlsym(RTLD_DEFAULT, "_objc_msgForward");
    p_readClassPair = dlsym(RTLD_DEFAULT, "objc_readClassPair");
#define SELN(var, str) var = sel_registerName(str)
    SELN(s_alloc, "alloc"); SELN(s_autorelease, "autorelease"); SELN(s_release, "release"); SELN(s_load, "load");
    SELN(s_initWithFormat_arguments, "initWithFormat:arguments:");
    SELN(s_initWithFormat_locale_arguments, "initWithFormat:locale:arguments:");
    SELN(s_appendString, "appendString:"); SELN(s_stringByAppendingString, "stringByAppendingString:");
    SELN(s_stringWithFormat, "stringWithFormat:"); SELN(s_initWithFormat, "initWithFormat:");
    SELN(s_appendFormat, "appendFormat:"); SELN(s_stringByAppendingFormat, "stringByAppendingFormat:");
    SELN(s_predicateWithFormat, "predicateWithFormat:"); SELN(s_predicateWithFormat_arguments, "predicateWithFormat:arguments:");
    SELN(s_arrayWithObjects, "arrayWithObjects:"); SELN(s_initWithObjects, "initWithObjects:");
    SELN(s_setWithObjects, "setWithObjects:"); SELN(s_orderedSetWithObjects, "orderedSetWithObjects:");
    SELN(s_dictionaryWithObjectsAndKeys, "dictionaryWithObjectsAndKeys:");
    SELN(s_initWithObjectsAndKeys, "initWithObjectsAndKeys:");
    SELN(s_handleFailureInFunction, "handleFailureInFunction:file:lineNumber:description:"); SELN(s_handleFailureInMethod, "handleFailureInMethod:object:file:lineNumber:description:");
    SELN(s_initWithColorsAndLocations, "initWithColorsAndLocations:"); SELN(s_initWithColors_atLocations_colorSpace, "initWithColors:atLocations:colorSpace:");
    SELN(s_withObjects_count, "arrayWithObjects:count:"); SELN(s_initWithObjects_count, "initWithObjects:count:");
    SELN(s_setWithObjects_count, "setWithObjects:count:"); SELN(s_orderedSetWithObjects_count, "orderedSetWithObjects:count:");
    SELN(s_dictWith_forKeys_count, "dictionaryWithObjects:forKeys:count:");
    SELN(s_initWith_forKeys_count, "initWithObjects:forKeys:count:");
    SELN(s_raise_format, "raise:format:"); SELN(s_raise_format_arguments, "raise:format:arguments:");
#undef SELN
}

// ------------------------------------------------------------ type encodings

typedef struct { char kind; int size, align, hfa; char hfa_t; } Enc;  // kind: v i f d p s

static const char *enc_skip_quals(const char *e) { while (*e && strchr("rnNoORV", *e)) e++; return e; }

static const char *enc_parse(const char *e, Enc *o) {
    e = enc_skip_quals(e);
    memset(o, 0, sizeof *o);
    switch (*e) {
    case 'v': o->kind = 'v'; return e + 1;
    case 'c': case 'C': case 'B': o->kind = 'i'; o->size = o->align = 1; return e + 1;
    case 's': case 'S': o->kind = 'i'; o->size = o->align = 2; return e + 1;
    case 'i': case 'I': case 'l': case 'L': o->kind = 'i'; o->size = o->align = 4; return e + 1;
    case 'q': case 'Q': o->kind = 'i'; o->size = o->align = 8; return e + 1;
    case 'f': o->kind = 'f'; o->size = o->align = 4; o->hfa = 1; o->hfa_t = 'f'; return e + 1;
    case 'd': o->kind = 'd'; o->size = o->align = 8; o->hfa = 1; o->hfa_t = 'd'; return e + 1;
    case '@': case '#': case ':': case '*': case '?': o->kind = 'p'; o->size = o->align = 8; return e + 1;
    case '^': {
        o->kind = 'p'; o->size = o->align = 8;
        Enc tmp; return enc_parse(e + 1, &tmp);
    }
    case '[': {
        int n = 0; e++;
        while (*e >= '0' && *e <= '9') n = n * 10 + (*e++ - '0');
        Enc el; e = enc_parse(e, &el);
        *o = el; o->kind = 's'; o->size = el.size * n; o->hfa = el.hfa * n;
        return *e == ']' ? e + 1 : e;
    }
    case '{': case '(': {
        char close = *e == '{' ? '}' : ')';
        bool is_struct = *e == '{';
        e++;
        while (*e && *e != '=' && *e != close) e++;
        if (*e == '=') e++;
        o->kind = 's'; o->align = 1;
        int off = 0; bool first = true, all_float = true;
        while (*e && *e != close) {
            Enc m; e = enc_parse(e, &m);
            while (*e >= '0' && *e <= '9') e++;
            if (m.align > o->align) o->align = m.align;
            if (is_struct) { off = (off + m.align - 1) / m.align * m.align; off += m.size; }
            else if (m.size > off) off = m.size;
            if (m.hfa && (first || m.hfa_t == o->hfa_t)) { o->hfa_t = m.hfa_t; o->hfa += m.hfa; }
            else all_float = false;
            first = false;
        }
        o->size = (off + o->align - 1) / o->align * o->align;
        if (!all_float || o->hfa > 4) { o->hfa = 0; o->hfa_t = 0; }
        return *e == close ? e + 1 : e;
    }
    }
    o->kind = 'p'; o->size = o->align = 8;
    return e + 1;
}

// ------------------------------------------------------------------ msgSend

static void normalize_return(CPU *c, const Enc *r) {
    if (r->kind != 'i') return;
    uint64_t x = c->x[0];
    switch (r->size) {
    case 1: c->x[0] = (uint64_t)(int64_t)(int8_t)x; break;  // BOOL / char: x86 leaves bits 8-31 undefined
    case 2: c->x[0] = (uint64_t)(int64_t)(int16_t)x; break;
    case 4: c->x[0] = (uint64_t)(int64_t)(int32_t)x; break;
    }
}

// Send to a host (x86) implementation through the real objc_msgSend, fixing up the
// return conventions that differ from arm64.
// Re-marshals a call whose signature has by-value structs. arm64 passes small integer
// structs in x registers, homogeneous float aggregates in v registers and big structs by
// reference; SysV passes integer structs in int registers (or on the stack when they do
// not fit) and every struct over 16 bytes on the stack. Float aggregates of single
// precision (packed into xmm registers on x86) are not handled and stop with a message.
typedef struct { CPU *c; uint64_t gs; int nx, nv; } ArmArgs;
static uint64_t arm_stack(ArmArgs *a, int size, int align) {
    a->gs = (a->gs + align - 1) & ~(uint64_t)(align - 1);
    uint64_t v = 0;
    memcpy(&v, (void *)a->gs, size > 8 ? 8 : size);
    a->gs += size;
    return v;
}
static uint64_t arm_int(ArmArgs *a, int size) { return a->nx < 8 ? a->c->x[a->nx++] : arm_stack(a, 8, 8); }
static uint64_t arm_vec(ArmArgs *a, int size) { return a->nv < 8 ? a->c->vr[a->nv++].d[0] : arm_stack(a, size, size); }

static void marshal_call(CPU *c, void *fn, const char *enc, const char *name, bool stret, uint64_t stret_ptr) {
    uint64_t ints[8] = {0}, dbls[8] = {0}, stack[80];
    int ix = 0, iv = 0, ns = 0;
    ArmArgs a = {c, c->sp, 0, 0};
    if (stret) ints[ix++] = stret_ptr;
    Enc t;
    const char *e = enc_parse(enc, &t);  // return type
    while (*e >= '0' && *e <= '9') e++;
#define PUT_INT(v) do { uint64_t _v = (v); if (ix < 6) ints[ix++] = _v; else stack[ns++] = _v; } while (0)
#define PUT_DBL(v) do { uint64_t _v = (v); if (iv < 8) dbls[iv++] = _v; else stack[ns++] = _v; } while (0)
    while (*e && ns < 70) {
        e = enc_parse(e, &t);
        while (*e >= '0' && *e <= '9') e++;
        if (t.kind == 'f' || t.kind == 'd') PUT_DBL(arm_vec(&a, t.size));
        else if (t.kind != 's') PUT_INT(arm_int(&a, 8));
        else if (t.hfa && t.hfa_t == 'd') {
            if (t.size <= 16) for (int k = 0; k < t.hfa; k++) PUT_DBL(arm_vec(&a, 8));
            else for (int k = 0; k < t.hfa; k++) stack[ns++] = arm_vec(&a, 8);  // memory class on x86
        } else if (t.hfa && t.hfa_t == 'f') {
            fprintf(stderr, "rebar: %s passes a float aggregate by value (not forwardable yet)\n", name);
            exit(127);
        } else if (t.size <= 16) {
            int words = (t.size + 7) / 8;
            uint64_t w[2] = {0, 0};
            if (a.nx + words > 8) a.nx = 8;   // (AAPCS64: a two-register struct that does not fit in the registers left goes wholly to the stack, as NSRange does after seven integer arguments)
            for (int k = 0; k < words; k++) w[k] = arm_int(&a, 8);
            if (ix + words > 6) for (int k = 0; k < words; k++) stack[ns++] = w[k];
            else for (int k = 0; k < words; k++) ints[ix++] = w[k];
        } else {  // arm64 passes a pointer to a copy; x86 copies the bytes onto the stack
            const uint64_t *src = (const uint64_t *)arm_int(&a, 8);
            for (int k = 0; k < (t.size + 7) / 8; k++) stack[ns++] = src[k];
        }
    }
#undef PUT_INT
#undef PUT_DBL
    if (ns & 1) stack[ns++] = 0;
    host_call_full(c, fn, ints, dbls, stack, (uint64_t)ns);
}

// Forward a call whose signature (as a type encoding) involves by-value structs, then put
// the result where arm64 expects it. `stret_fn` is the entry point to use for memory-class
// results (objc_msgSend_stret for messages, the function itself for plain C).
static void forward_enc(CPU *c, void *fn, void *stret_fn, const char *e, const char *name) {
    Enc r = {0};
    enc_parse(e, &r);
    if (r.kind == 's' && r.size > 16) {
        // Memory-class result. arm64 hands us a buffer in x8 (or expects 3-4 doubles in v0-v3).
        bool hfa = r.hfa >= 2 && r.hfa_t == 'd';
        uint64_t tmp[8];
        uint64_t dst = hfa ? (uint64_t)tmp : c->x[8];
        marshal_call(c, stret_fn, e, name, true, dst);
        if (hfa) for (int k = 0; k < r.hfa; k++) { memset(&c->vr[k], 0, 16); c->vr[k].d[0] = tmp[k]; }
        return;
    }
    marshal_call(c, fn, e, name, false, 0);
    normalize_return(c, &r);
    if (r.kind == 's' && r.hfa_t == 'f' && r.size <= 16) {
        // x86 packs {float,float,...} into xmm0/xmm1; arm64 wants one float per register
        VReg a = c->vr[0], b = c->vr[1];
        float f[4] = {a.f[0], a.f[1], b.f[0], b.f[1]};
        memset(&c->vr[0], 0, 64);
        for (int k = 0; k < r.hfa; k++) c->vr[k].f[0] = f[k];
    }
}

static void send_host(CPU *c, Class cls, SEL sel) {
    Method m = class_getInstanceMethod(cls, sel);
    const char *e = m ? method_getTypeEncoding(m) : NULL;
    Enc r = {0};
    if (e) enc_parse(e, &r);
    if (e && strchr(e, '{')) forward_enc(c, p_msgSend, p_msgSend_stret, e, sel_getName(sel));
    else { host_call(c, p_msgSend, false); normalize_return(c, &r); }
}

typedef id (*send_fn)(id, SEL, ...);

// objc_msgSend with up to four extra word arguments, made through rebar_host_call so that
// exceptions raised by Foundation (nil elements, bad arguments, ...) can reach guest handlers.
static id send_h(id self, SEL sel, ...) {
    va_list ap;
    va_start(ap, sel);
    uint64_t ints[8] = {(uint64_t)self, (uint64_t)sel, va_arg(ap, uint64_t), va_arg(ap, uint64_t),
                        va_arg(ap, uint64_t), va_arg(ap, uint64_t), 0, 0};
    va_end(ap);
    uint64_t dbls[8] = {0}, out[8] = {0, 8}, stack[2] = {0, 0};   // (out[0], out[1] are inputs: see rebar_host_call)
    host_invoke(cur_cpu(), p_msgSend, ints, dbls, stack, 2, out);
    return (id)out[0];
}

// Selectors whose variadic arguments arm64 puts on the stack: re-route them to the
// va_list / count-based forms of the same method.
#define send send_h
static bool variadic_send(CPU *c, id self, SEL sel) {
    uint64_t sp = c->sp;
    VA_INIT(ap, sp);
    if (sel == s_stringWithFormat || sel == s_predicateWithFormat) {
        SEL init = sel == s_stringWithFormat ? s_initWithFormat_arguments : s_predicateWithFormat_arguments;
        if (sel == s_stringWithFormat) {
            id obj = send(send(self, s_alloc), init, (id)c->x[2], ap);
            c->x[0] = (uint64_t)send(obj, s_autorelease);
        } else c->x[0] = (uint64_t)send(self, init, (id)c->x[2], ap);
        return true;
    }
    if (sel == s_raise_format) { send(self, s_raise_format_arguments, (id)c->x[2], (id)c->x[3], ap); return true; }
    if (sel == s_initWithFormat) { c->x[0] = (uint64_t)send(self, s_initWithFormat_arguments, (id)c->x[2], ap); return true; }
    if (sel == s_appendFormat || sel == s_stringByAppendingFormat) {
        id str = send(send((id)objc_getClass("NSString"), s_alloc), s_initWithFormat_arguments, (id)c->x[2], ap);
        if (sel == s_appendFormat) send(self, s_appendString, str);
        else c->x[0] = (uint64_t)send(self, s_stringByAppendingString, str);
        send(str, s_release);
        return true;
    }
    if (sel == s_arrayWithObjects || sel == s_initWithObjects || sel == s_setWithObjects || sel == s_orderedSetWithObjects) {
        id objs[256]; int n = 0;
        if (c->x[2]) { objs[n++] = (id)c->x[2]; for (const uint64_t *p = (const uint64_t *)sp; *p && n < 256; p++) objs[n++] = (id)*p; }
        SEL target = sel == s_arrayWithObjects ? s_withObjects_count : sel == s_initWithObjects ? s_initWithObjects_count
                   : sel == s_setWithObjects ? s_setWithObjects_count : s_orderedSetWithObjects_count;
        c->x[0] = (uint64_t)send(self, target, objs, (uint64_t)n);
        return true;
    }
    if (sel == s_handleFailureInFunction || sel == s_handleFailureInMethod) {   // NSAssertionHandler: the description is a format string with variadic arguments (on the arm64 stack); say what failed, then raise the exception the method would
        bool fn = sel == s_handleFailureInFunction;
        id fmt = (id)c->x[fn ? 5 : 6];
        id str = send(send((id)objc_getClass("NSString"), s_alloc), s_initWithFormat_arguments, fmt, ap);
        SEL utf8 = sel_registerName("UTF8String");
        const char *where_ = (const char *)send((id)c->x[fn ? 2 : 4], utf8), *what = (const char *)send(str, utf8);
        fprintf(stderr, "*** Assertion failure in %s%s, line %lld: %s\n", fn ? "" : "method, file ", where_ ? where_ : "?", (long long)c->x[fn ? 4 : 5], what ? what : "?");
        if (getenv("REBAR_ASSERT_BT")) dump_backtrace(c);
        id nm = send((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "NSInternalInconsistencyException");
        id f2 = send((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "%@");
        send((id)objc_getClass("NSException"), s_raise_format, nm, f2, str);
        return true;
    }
    if (sel == s_initWithColorsAndLocations) {   // NSGradient: (color, CGFloat location) pairs ending with a nil color: the first color is in x2, then on the stack: location, color, location, ...
        id cols[64]; double locs[64]; int n = 0;
        if (c->x[2]) {
            cols[0] = (id)c->x[2];
            const uint64_t *p = (const uint64_t *)sp;
            locs[0] = *(const double *)p++; n = 1;
            while (*p && n < 64) { cols[n] = (id)*p++; locs[n] = *(const double *)p++; n++; }
        }
        id arr = send((id)objc_getClass("NSArray"), s_withObjects_count, cols, (uint64_t)n);
        id cs = send((id)objc_getClass("NSColorSpace"), sel_registerName("genericRGBColorSpace"));
        c->x[0] = (uint64_t)send(self, s_initWithColors_atLocations_colorSpace, arr, locs, cs);
        return true;
    }
    if (sel == s_dictionaryWithObjectsAndKeys || sel == s_initWithObjectsAndKeys) {
        id objs[128], keys[128]; int n = 0;
        if (c->x[2]) {
            objs[0] = (id)c->x[2];
            const uint64_t *p = (const uint64_t *)sp;
            if (*p) { keys[0] = (id)*p++; n = 1; }
            while (*p && n < 128) { objs[n] = (id)*p++; if (!*p) break; keys[n] = (id)*p++; n++; }
        }
        SEL target = sel == s_dictionaryWithObjectsAndKeys ? s_dictWith_forKeys_count : s_initWith_forKeys_count;
        c->x[0] = (uint64_t)send(self, target, objs, keys, (uint64_t)n);
        return true;
    }
    return false;
}

// A guest va_list is a pointer to the argument area; the host methods that take one
// want an x86_64 va_list structure, so rebuild it.
static bool valist_send(CPU *c, id self, SEL sel) {
    int idx = sel == s_initWithFormat_arguments ? 3 : sel == s_predicateWithFormat_arguments ? 3
            : sel == s_initWithFormat_locale_arguments ? 4 : 0;
    if (!idx) return false;
    VA_INIT(ap, c->x[idx]);   // (x2 = the format, x3 = the va_list; with a locale in x3 the va_list is x4)
    c->x[0] = idx == 3 ? (uint64_t)send(self, sel, (id)c->x[2], ap)
                       : (uint64_t)send(self, sel, (id)c->x[2], (id)c->x[3], ap);
    return true;
}

#undef send

// A readable NSString at `v`, for the trace: copies at most n-1 bytes of its UTF-8 into `out` (empty when `v` is not obviously a string).
static void trace_string_arg(uint64_t v, char *out, size_t n) {
    out[0] = 0;
    if (v < 0x100000) return;
    if (!(v & 1)) {   // (an odd value is a tagged pointer: short strings live in the pointer itself)
        if (v >= 0x00007fffffffffffull || (v & 7)) return;
        char probe[8]; mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), v, 8, (mach_vm_address_t)probe, &got) != 0 || got != 8) return;
        uint64_t isa; memcpy(&isa, probe, 8);
        if (isa < 0x100000 || isa >= 0x00007fffffffffffull || (isa & 7)) return;
        if (mach_vm_read_overwrite(mach_task_self(), isa, 8, (mach_vm_address_t)probe, &got) != 0 || got != 8) return;
    }
    Class k = object_getClass((id)v);
    if (!k) return;
    const char *cn = class_getName(k);
    if (!cn || !strstr(cn, "String")) return;
    SEL u = sel_registerName("UTF8String");
    if (!class_respondsToSelector(k, u)) return;
    const char *t = ((const char *(*)(id, SEL))p_msgSend)((id)v, u);
    if (t) snprintf(out, n, "%s", t);
}
static const char *g_trace_msg;  // REBAR_TRACE_MSG=sub1,sub2: log the Objective-C messages whose selector contains one of them
static void trace_msg(CPU *c, const char *kind, id self, SEL sel) {
    const char *name = sel_getName(sel);
    bool hit = false;
    for (const char *p = g_trace_msg; *p && !hit;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char tmp[128]; snprintf(tmp, sizeof tmp, "%.*s", (int)l, p);
        if (l && strstr(name, tmp)) hit = true;
        p += l + (e ? 1 : 0);
    }
    if (!hit) return;
    if (getenv("REBAR_TRACE_MSG_BT")) dump_backtrace(c);   // the guest backtrace of every traced send
    char s2[96] = "", s3[96] = "";
    trace_string_arg(c->x[2], s2, sizeof s2); trace_string_arg(c->x[3], s3, sizeof s3);
    fprintf(stderr, "[objc] %s [%s %s] self=%p x2=%llx x3=%llx d0..3=%g %g %g %g%s%s%s%s%s%s\n", kind, self ? class_getName(object_getClass(self)) : "nil", name, (void *)self,
            (unsigned long long)c->x[2], (unsigned long long)c->x[3], c->vr[0].df[0], c->vr[1].df[0], c->vr[2].df[0], c->vr[3].df[0],
            s2[0] ? " x2=\"" : "", s2, s2[0] ? "\"" : "", s3[0] ? " x3=\"" : "", s3, s3[0] ? "\"" : "");
}
// REBAR_GPU_SYNC=1: an IOSurface handed to CoreAnimation (-[CALayer setContents:]) is only used by the window server once the
// GPU has written it. Chromium relies on the graphics driver for that ordering; here every Metal command buffer committed
// so far is waited for first. REBAR_GPU_SYNC=2 also prints what the surface holds (how many bytes are not zero).
static int g_gpu_sync;
static pthread_mutex_t g_gpu_mu = PTHREAD_MUTEX_INITIALIZER;
static id g_gpu_pending[512];
static int g_gpu_npending;
// The user's NVMTL driver marks an IOSurface it is writing with a "NVMTLBusyAt:p0" property (magic, done, stamp, seq, pid);
// WindowServer waits for done for at most 10 ms. Returns 1 / 0 for done / not done, -1 when the surface has no such mark.
static int gpu_busy_state(void *surf, unsigned long long *seq, int *pid) {
    static void *(*copyval)(void *, const void *); static const void *(*mk)(const void *, const char *, unsigned);
    static const unsigned char *(*bytes)(const void *); static long (*len)(const void *); static void (*rel)(const void *);
    if (!copyval) { copyval = dlsym(RTLD_DEFAULT, "IOSurfaceCopyValue"); mk = dlsym(RTLD_DEFAULT, "CFStringCreateWithCString"); bytes = dlsym(RTLD_DEFAULT, "CFDataGetBytePtr"); len = dlsym(RTLD_DEFAULT, "CFDataGetLength"); rel = dlsym(RTLD_DEFAULT, "CFRelease"); }
    const void *key = mk(NULL, "NVMTLBusyAt:p0", 0x08000100);
    void *v = copyval(surf, key); rel(key);
    int r = -1;
    if (v) {
        if (len(v) == 32) { const unsigned char *b = bytes(v); unsigned magic, done; memcpy(&magic, b, 4); memcpy(&done, b + 4, 4); if (magic == 0x4255535au) { r = done ? 1 : 0; if (seq) memcpy(seq, b + 16, 8); if (pid) memcpy(pid, b + 24, 4); } }
        rel(v);
    }
    return r;
}
// REBAR_GPU_SYNC=4: the IOSurface given to a layer is replaced by a copy in ordinary memory. The copy is read from the
// original under IOSurfaceLock, which makes the driver page the GPU's result out of VRAM; the window server then composites
// a surface whose pixels are in system memory instead of one that lives in the driver's shared VRAM.
static void *g_gpu_ring[256]; static unsigned g_gpu_ring_n;
static void gpu_copy_surface(CPU *c) {
    typedef void *(*Create)(const void *); typedef void *(*Copyv)(void *, const void *); typedef void (*Setv)(void *, const void *, const void *);
    static Create create; static Copyv copyv; static Setv setv;
    static int (*lock)(void *, unsigned, void *), (*unlock)(void *, unsigned, void *);
    static void *(*base)(void *); static size_t (*bpr)(void *), (*w)(void *), (*h)(void *), (*bpe)(void *), (*planes)(void *); static unsigned (*pixfmt)(void *);
    static const void *(*mkstr)(const void *, const char *, unsigned); static const void *(*mknum)(const void *, int, const void *);
    static const void *(*mkdict)(const void *, const void **, const void **, long, const void *, const void *);
    static void (*rel)(const void *); static const void *kcb_key, *kcb_val;
    static const void **k_w, **k_h, **k_bpe, **k_fmt;
    if (!create) {
        create = dlsym(RTLD_DEFAULT, "IOSurfaceCreate"); copyv = dlsym(RTLD_DEFAULT, "IOSurfaceCopyValue"); setv = dlsym(RTLD_DEFAULT, "IOSurfaceSetValue");
        lock = dlsym(RTLD_DEFAULT, "IOSurfaceLock"); unlock = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock"); base = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
        bpr = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow"); w = dlsym(RTLD_DEFAULT, "IOSurfaceGetWidth"); h = dlsym(RTLD_DEFAULT, "IOSurfaceGetHeight");
        bpe = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerElement"); planes = dlsym(RTLD_DEFAULT, "IOSurfaceGetPlaneCount"); pixfmt = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
        mkstr = dlsym(RTLD_DEFAULT, "CFStringCreateWithCString"); mknum = dlsym(RTLD_DEFAULT, "CFNumberCreate"); mkdict = dlsym(RTLD_DEFAULT, "CFDictionaryCreate"); rel = dlsym(RTLD_DEFAULT, "CFRelease");
        kcb_key = dlsym(RTLD_DEFAULT, "kCFTypeDictionaryKeyCallBacks"); kcb_val = dlsym(RTLD_DEFAULT, "kCFTypeDictionaryValueCallBacks");
        k_w = dlsym(RTLD_DEFAULT, "kIOSurfaceWidth"); k_h = dlsym(RTLD_DEFAULT, "kIOSurfaceHeight"); k_bpe = dlsym(RTLD_DEFAULT, "kIOSurfaceBytesPerElement"); k_fmt = dlsym(RTLD_DEFAULT, "kIOSurfacePixelFormat");
    }
    void *src = (void *)c->x[2];
    Class sc = object_getClass((id)src);
    if (!sc || strcmp(class_getName(sc), "IOSurface") || !k_w || !kcb_key || planes(src) > 1) return;
    long wv = (long)w(src), hv = (long)h(src), bv = (long)bpe(src), fv = (long)pixfmt(src);
    const void *nums[4] = { mknum(NULL, 4 /* kCFNumberSInt64Type */, &wv), mknum(NULL, 4, &hv), mknum(NULL, 4, &bv), mknum(NULL, 4, &fv) };
    const void *keys[4] = { *k_w, *k_h, *k_bpe, *k_fmt };
    const void *dict = mkdict(NULL, keys, nums, 4, kcb_key, kcb_val);
    void *dst = create(dict);
    for (int k = 0; k < 4; k++) rel(nums[k]);
    rel(dict);
    if (!dst) return;
    if (lock(src, 1, NULL) == 0) {
        lock(dst, 0, NULL);
        const char *sp = base(src); char *dp = base(dst);
        size_t sb = bpr(src), db = bpr(dst), row = sb < db ? sb : db;
        for (long y = 0; y < hv; y++) memcpy(dp + y * db, sp + y * sb, row);
        unlock(dst, 0, NULL);
        unlock(src, 1, NULL);
    }
    const void *key = mkstr(NULL, "IOSurfaceColorSpace", 0x08000100);
    void *cs = copyv(src, key);
    if (cs) { setv(dst, key, cs); rel(cs); }
    rel(key);
    unsigned slot = __atomic_fetch_add(&g_gpu_ring_n, 1, __ATOMIC_RELAXED) & 255;
    void *old = __atomic_exchange_n(&g_gpu_ring[slot], dst, __ATOMIC_ACQ_REL);  // (the layer keeps its own reference)
    if (old) rel(old);
    c->x[2] = (uint64_t)dst;
    static int shown; if (shown++ < 5) fprintf(stderr, "[gpu] copy of IOSurface %p (%ldx%ld, fmt %lx) given to the layer instead: %p\n", src, wv, hv, fv, dst);
}

static void gpu_hook(CPU *c, id self, SEL sel) {
    static SEL s_commit, s_set, s_wait;
    if (!s_commit) { s_commit = sel_registerName("commit"); s_set = sel_registerName("setContents:"); s_wait = sel_registerName("waitUntilCompleted"); }
    if (sel == s_commit) {
        if (self && class_respondsToSelector(object_getClass(self), s_wait)) {
            pthread_mutex_lock(&g_gpu_mu);
            if (g_gpu_npending < 512) { ((id (*)(id, SEL))p_msgSend)(self, sel_registerName("retain")); g_gpu_pending[g_gpu_npending++] = self; }
            pthread_mutex_unlock(&g_gpu_mu);
        }
    } else if (sel == s_set) {
        int st_before = -2; unsigned long long seq_before = 0; int pid_before = 0;
        if (g_gpu_sync >= 2 && c->x[2] > 0x10000) { Class sc0 = object_getClass((id)c->x[2]); if (sc0 && !strcmp(class_getName(sc0), "IOSurface")) st_before = gpu_busy_state((void *)c->x[2], &seq_before, &pid_before); }
        id list[512], later[512]; int n, nlater = 0;
        pthread_mutex_lock(&g_gpu_mu);
        n = g_gpu_npending; memcpy(list, g_gpu_pending, n * sizeof(id)); g_gpu_npending = 0;
        pthread_mutex_unlock(&g_gpu_mu);
        bool had = gil_release();  // (a completion handler on another thread may need the lock)
        static SEL s_status, s_release;
        if (!s_status) { s_status = sel_registerName("status"); s_release = sel_registerName("release"); }
        for (int k = 0; k < n; k++) {
            unsigned long st = ((unsigned long (*)(id, SEL))p_msgSend)(list[k], s_status);  // 2 committed, 3 scheduled, 4 completed, 5 error
            if (st < 2) { later[nlater++] = list[k]; continue; }  // another thread has not got to its commit yet
            if (st < 4) ((void (*)(id, SEL))p_msgSend)(list[k], s_wait);
            ((void (*)(id, SEL))p_msgSend)(list[k], s_release);
        }
        if (nlater) {
            pthread_mutex_lock(&g_gpu_mu);
            for (int k = 0; k < nlater && g_gpu_npending < 512; k++) g_gpu_pending[g_gpu_npending++] = later[k];
            pthread_mutex_unlock(&g_gpu_mu);
        }
        if (had) gil_acquire();
        if (g_gpu_sync >= 4 && c->x[2] > 0x10000) gpu_copy_surface(c);
        if (g_gpu_sync >= 2 && c->x[2] > 0x10000) {
            static unsigned long (*gettype)(void);
            static int (*lock)(void *, unsigned, void *), (*unlock)(void *, unsigned, void *);
            static void *(*base)(void *); static size_t (*alloc)(void *), (*width)(void *), (*height)(void *);
            if (!gettype) { gettype = dlsym(RTLD_DEFAULT, "IOSurfaceGetTypeID"); lock = dlsym(RTLD_DEFAULT, "IOSurfaceLock"); unlock = dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
                base = dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress"); alloc = dlsym(RTLD_DEFAULT, "IOSurfaceGetAllocSize"); width = dlsym(RTLD_DEFAULT, "IOSurfaceGetWidth"); height = dlsym(RTLD_DEFAULT, "IOSurfaceGetHeight"); }
            void *surf = (void *)c->x[2];
            Class sc = object_getClass((id)surf);  // (anything else can be the contents too: a CGImage, nil, ...)
            if (sc && !strcmp(class_getName(sc), "IOSurface") && gettype && lock(surf, 1, NULL) == 0) {
                const uint32_t *w = base(surf); size_t bytes = alloc(surf), nz = 0, opaque = 0, first = 0; uint32_t sample = 0;
                for (size_t k = 0; k < bytes / 4; k++) { if (w[k] & 0x00ffffff) { if (!nz) first = k, sample = w[k]; nz++; } opaque += (w[k] >> 24) == 0xff; }
                static unsigned (*pixfmt)(void *); static size_t (*bpe)(void *); static void *(*copyval)(void *, const void *); static unsigned long (*cflen)(const void *); static void (*cfrel)(const void *); static const void *(*csstr)(const void *, const char *, unsigned);
                if (!pixfmt) { pixfmt = dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat"); bpe = dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerElement"); copyval = dlsym(RTLD_DEFAULT, "IOSurfaceCopyValue"); cflen = dlsym(RTLD_DEFAULT, "CFDataGetLength"); cfrel = dlsym(RTLD_DEFAULT, "CFRelease"); csstr = dlsym(RTLD_DEFAULT, "CFStringCreateWithCString"); }
                unsigned pf = pixfmt(surf); char cc[5] = {(char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf, 0};
                const void *key = csstr(NULL, "IOSurfaceColorSpace", 0x08000100); void *cs = copyval(surf, key); cfrel(key);
                unsigned long cslen = cs ? ((unsigned long (*)(const void *))cflen)(cs) : 0; (void)cslen;
                { static int saved; if (cs && !saved && g_gpu_sync >= 5) { saved = 1; FILE *cf = fopen("/tmp/bench/cs.icc", "wb"); const unsigned char *(*bp)(const void *) = dlsym(RTLD_DEFAULT, "CFDataGetBytePtr"); if (cf) { fwrite(bp(cs), 1, cslen, cf); fclose(cf); } } }
                void *lm = NULL; (void)lm;
                { unsigned long long sq = 0; int pd = 0; int stn = gpu_busy_state(surf, &sq, &pd); fprintf(stderr, "[gpu] busy tag: before-wait %s (seq %llu pid %d), after-wait %s (seq %llu), our pid %d\n", st_before == -2 ? "?" : st_before < 0 ? "none" : st_before ? "done" : "NOT DONE", seq_before, pid_before, stn < 0 ? "none" : stn ? "done" : "NOT DONE", sq, (int)getpid()); }
                fprintf(stderr, "[gpu] setContents IOSurface %p %zux%zu fmt '%s' bpe %zu colourspace %s: %zu of %zu pixels have colour, %zu opaque, first coloured pixel #%zu = %08x\n", surf, width(surf), height(surf), cc, bpe(surf), cs ? "set" : "none", nz, bytes / 4, opaque, first, sample);
                if (cs) cfrel(cs);
                static int ndump;  // REBAR_GPU_SYNC=5: raw copies of the first few big surfaces, to look at what the GPU wrote into them
                if (g_gpu_sync >= 5 && width(surf) * height(surf) >= 150000 && ndump < 6 && (nz * 10 > bytes / 4)) {
                    char fn[128]; snprintf(fn, sizeof fn, "/tmp/bench/surf_%d_%dx%d.bgra", ndump++, (int)width(surf), (int)height(surf));
                    FILE *df = fopen(fn, "wb"); if (df) { size_t rb = ((size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow"))(surf); fwrite(&rb, 8, 1, df); fwrite(w, 1, bytes, df); fclose(df); }
                }
                static int dumped;
                if (g_gpu_sync >= 3 && dumped++ < 3) {  // every property of the surface, as CoreFoundation prints them
                    static void *(*allv)(void *); static void *(*desc)(const void *); static bool (*getc)(const void *, char *, long, unsigned);
                    if (!allv) { allv = dlsym(RTLD_DEFAULT, "IOSurfaceCopyAllValues"); desc = dlsym(RTLD_DEFAULT, "CFCopyDescription"); getc = dlsym(RTLD_DEFAULT, "CFStringGetCString"); }
                    void *all = allv ? allv(surf) : NULL;
                    void *d = all ? desc(all) : NULL;
                    static char buf[6000];
                    if (d && getc(d, buf, sizeof buf, 0x08000100)) fprintf(stderr, "[gpu] surface properties: %s\n", buf);
                    if (d) cfrel(d);
                    if (all) cfrel(all);
                }
                unlock(surf, 1, NULL);
            }
        }
    }
}
// REBAR_DUMP_LAYERS=<n>: print the CoreAnimation layer tree of the CAContext (CALayer's recursiveDescription: what CoreAnimation itself holds, not what
// the guest asked for) before every n-th -[CALayer setContents:]. The GPU process of Chromium builds its tree on a CAContext.
static int g_dump_layers;
static id g_ca_root;
static void layer_dump_hook(CPU *c, id self, SEL sel) {
    static SEL s_setLayer, s_set, s_rd, s_retain, s_utf8;
    if (!s_set) { s_setLayer = sel_registerName("setLayer:"); s_set = sel_registerName("setContents:"); s_rd = sel_registerName("recursiveDescription"); s_retain = sel_registerName("retain"); s_utf8 = sel_registerName("UTF8String"); }
    if (sel == s_setLayer && self && c->x[2] > 0x10000 && !strcmp(class_getName(object_getClass(self)), "CAContext")) {
        g_ca_root = (id)c->x[2];
        ((id (*)(id, SEL))p_msgSend)(g_ca_root, s_retain);
    } else if (sel == s_set && g_ca_root) {
        static int n;
        if (++n % g_dump_layers == 0) {
            id d = ((id (*)(id, SEL))p_msgSend)(g_ca_root, s_rd);
            if (d) fprintf(stderr, "[layers] before setContents #%d:\n%s\n", n, ((const char *(*)(id, SEL))p_msgSend)(d, s_utf8));
        }
    }
}
// +[NSBundle bundleForClass:] answers from dyld's list of images, and the images rebar maps are not on it: the host would answer with the main bundle (or nil), and an app
// that reads its framework's resources through the class's bundle (Bundle(for: Self.self).path(forResource:...)) found nothing. For a class defined in a guest image the bundle is
// the nearest enclosing .framework / .app / .bundle / .plugin / .xpc / .appex directory of the image's file.
static bool guest_bundle_root(const char *img, char *out, size_t n) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", img);
    for (char *p = strrchr(tmp, '/'); p && p > tmp; p = strrchr(tmp, '/')) {
        *p = 0;
        const char *base = strrchr(tmp, '/'); base = base ? base + 1 : tmp;
        const char *dot = strrchr(base, '.');
        if (dot && (!strcmp(dot, ".framework") || !strcmp(dot, ".app") || !strcmp(dot, ".bundle") || !strcmp(dot, ".plugin") || !strcmp(dot, ".xpc") || !strcmp(dot, ".appex"))) { snprintf(out, n, "%s", tmp); return true; }
    }
    return false;
}
static bool guest_bundle_for_class(CPU *c, uint64_t cls) {
    GImage *im = image_by_addr(cls);
    char root[PATH_MAX];
    if (getenv("REBAR_OBJC_TRACE")) fprintf(stderr, "  [objc] bundleForClass %llx: image %s\n", (unsigned long long)cls, im && im->path ? im->path : "(none)");
    if (!im || !im->path || !guest_bundle_root(im->path, root, sizeof root)) return false;
    objc_bridge_init();
    send_fn send = (send_fn)p_msgSend;
    id str = send((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), root);
    id b = str ? send((id)objc_getClass("NSBundle"), sel_registerName("bundleWithPath:"), str) : NULL;
    if (!b) return false;
    c->x[0] = (uint64_t)b; c->x[1] = 0;
    return true;
}
// ---- guest method implementations as host-callable thunks.  The host runtime calls a method's IMP like any function; a raw guest address there
// is an instruction-fetch fault per call (a signal, several microseconds: +[NSObject allocWithZone:] alone is called 10000 times a second in the GPU
// process).  Every IMP the guest installs gets a small x86 thunk instead (mov r11, imp; jmp [rebar_guest_entry]) that does what the fault handler does,
// and every IMP the guest reads back from the runtime is turned back into the guest address.
static struct { uint64_t guest; uint8_t *thunk; } g_imp_tab[8192];
static uint8_t *g_thunk_page, *g_thunk_top, *g_thunk_end;
static pthread_mutex_t g_thunk_mu = PTHREAD_MUTEX_INITIALIZER;
static bool is_thunk(uint64_t a) { return g_thunk_page && a >= (uint64_t)g_thunk_page && a < (uint64_t)g_thunk_end && ((a - (uint64_t)g_thunk_page) & 31) == 0; }
static uint64_t imp_unwrap(uint64_t imp) { return is_thunk(imp) ? *(uint64_t *)(imp + 2) : imp; }
static uint64_t imp_wrap_any(uint64_t imp, bool any);
static uint64_t imp_wrap(uint64_t imp) { return imp_wrap_any(imp, false); }
// any: also an address outside the guest's image range (a stub that a JIT made in memory of its own: CoreCLR's native-callable thunks)
static uint64_t imp_wrap_any(uint64_t imp, bool any) {
    if (!imp || (!in_guest(imp) && !(any && imp >= 0x10000 && imp < 0x100000000ull))) return imp;
    pthread_mutex_lock(&g_thunk_mu);
    unsigned h = (unsigned)((imp >> 2) * 0x9e3779b97f4a7c15ull >> 51);
    for (int k = 0; k < 8192; k++) {
        unsigned j = (h + k) & 8191;
        if (g_imp_tab[j].guest == imp) { uint64_t t = (uint64_t)g_imp_tab[j].thunk; pthread_mutex_unlock(&g_thunk_mu); return t; }
        if (!g_imp_tab[j].guest) {
            if (g_thunk_top == g_thunk_end) {
                g_thunk_page = g_thunk_top = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
                if (g_thunk_page == MAP_FAILED) { g_thunk_page = g_thunk_top = g_thunk_end = NULL; pthread_mutex_unlock(&g_thunk_mu); return imp; }
                g_thunk_end = g_thunk_page + (1 << 20);
            }
            uint8_t *t = g_thunk_top; g_thunk_top += 32;
            t[0] = 0x49; t[1] = 0xBB; memcpy(t + 2, &imp, 8);                         // mov r11, imp
            t[10] = 0xFF; t[11] = 0x25; memset(t + 12, 0, 4);                         // jmp [rip+0]: the qword that follows
            uint64_t entry = (uint64_t)rebar_guest_entry; memcpy(t + 16, &entry, 8);
            g_imp_tab[j].guest = imp; g_imp_tab[j].thunk = t;
            pthread_mutex_unlock(&g_thunk_mu);
            return (uint64_t)t;
        }
    }
    pthread_mutex_unlock(&g_thunk_mu);
    return imp;   // (the table is full: the raw address works, through the fault)
}
// Callbacks the guest hands to CoreFoundation / CoreVideo: function pointers in the argument, or in a context structure that is copied on the way in
// (retain, release, copyDescription, ... of an observer, timer or source context: every qword from offset 16 on is a function pointer).
static void wrap_ctx(CPU *c, int arg, size_t bytes, uint8_t *tmp) {
    if (c->x[arg] < 0x10000) return;
    memcpy(tmp, (void *)c->x[arg], bytes);
    for (size_t o = 16; o + 8 <= bytes; o += 8) { uint64_t v; memcpy(&v, tmp + o, 8); v = imp_wrap(v); memcpy(tmp + o, &v, 8); }
    c->x[arg] = (uint64_t)tmp;
}
H(h_CFRunLoopObserverCreate) { uint8_t t[40]; c->x[4] = imp_wrap(c->x[4]); wrap_ctx(c, 5, 40, t); host_call(c, HOSTFN("CFRunLoopObserverCreate"), false); }
H(h_CFRunLoopTimerCreate) { uint8_t t[40]; c->x[5] = imp_wrap(c->x[5]); wrap_ctx(c, 6, 40, t); host_call(c, HOSTFN("CFRunLoopTimerCreate"), false); }
// vfork: the child would run the interpreter on the parent's CPU structure and stack (shared memory) until its exec, and the parent's host frame would be returned through twice.
// A fork is what the program asked for at the level it can see (the child execs or exits at once): the child gets its own copy.
H(h_vfork) { host_call(c, HOSTFN("fork"), false); }
// ARC's return-value handshake. The host's versions decide by looking at the x86_64 bytes after the call (a `mov %rax,%rdi` marks a caller that takes the +1 object without the
// autorelease/retain pair) and keep the answer in a thread-local flag. What follows a call in rebar's own C code often looks like that marker, so a host method called from rebar can skip
// its autorelease and leave the flag set, and the guest's next objc_retainAutoreleasedReturnValue then sees "already +1", skips its retain and over-releases (Gemini: an object freed
// before its autorelease pool pops). The guest side never uses the shortcut: the callee autoreleases, the caller retains.
H(h_objc_autoreleaseReturnValue)    { id (*f)(id) = HOSTFN("objc_autorelease"); if (c->x[0]) f((id)c->x[0]); }
H(h_objc_retainAutoreleasedReturnValue) { id (*f)(id) = HOSTFN("objc_retain"); if (c->x[0]) f((id)c->x[0]); }
H(h_objc_claimAutoreleasedReturnValue) { id (*f)(id) = HOSTFN("objc_retain"); if (c->x[0]) f((id)c->x[0]); }
H(h_objc_unsafeClaimAutoreleasedReturnValue) { }   // (the object stays in its pool: nothing to take, nothing to give back)
// A query naming a file keychain of the app's own (Gemini: "Gemini Keys", never created here) makes Security ask the user in a dialog ("keychain not found"). Such a query is run
// against the default keychains instead: the keys naming a keychain (kSecUseKeychain, kSecMatchSearchList) are taken out of a copy of the dictionary. REBAR_KEEP_KEYCHAIN=1 keeps them.
// An iOS app (REBAR_IOS) keeps its secrets with SecItem* in the data-protection keychain, which needs the keychain-access-groups entitlement: a restricted one, that an ad-hoc signed host
// bundle cannot carry (AMFI kills it), so the real calls fail with -34018 (and a file keychain is not available to a Mac Catalyst process). Such an app gets librebar_sec.dylib (ios_sec.m):
// the four calls on a plist of its own.
static void *ios_sec_fn(const char *name) {
    static void *h; static int tried;
    if (!tried) { tried = 1;
        if (!getenv("REBAR_IOS") || getenv("REBAR_NO_IOS_KEYCHAIN")) return NULL;
        char p[PATH_MAX]; rebar_lib_path("librebar_sec.dylib", p, sizeof p); h = dlopen(p, RTLD_NOW);
        if (!h && getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "rebar: %s: %s\n", p, dlerror()); }
    return h ? dlsym(h, name) : NULL;
}
static void sec_query_without_keychain(CPU *c) {
    if (getenv("REBAR_KEEP_KEYCHAIN") || !c->x[0]) return;
    void *(*copy)(void *, long, const void *) = HOSTFN("CFDictionaryCreateMutableCopy"); void (*rm)(void *, const void *) = HOSTFN("CFDictionaryRemoveValue");
    const void **k1 = (const void **)dlsym(RTLD_DEFAULT, "kSecUseKeychain"), **k2 = (const void **)dlsym(RTLD_DEFAULT, "kSecMatchSearchList");
    if (!copy || !rm) return;
    void *m = copy(NULL, 0, (const void *)c->x[0]); if (!m) return;
    if (k1) rm(m, *k1); if (k2) rm(m, *k2);
    c->x[0] = (uint64_t)m;   // (the copy is leaked: a few per call)
}
#define SEC_HANDLER(Name) H(h_##Name) { void *f = ios_sec_fn("rebar_" #Name); if (f) { host_call(c, f, false); return; } sec_query_without_keychain(c); host_call(c, HOSTFN(#Name), false); }
SEC_HANDLER(SecItemAdd) SEC_HANDLER(SecItemCopyMatching) SEC_HANDLER(SecItemUpdate) SEC_HANDLER(SecItemDelete)
H(h_SSLSetIOFuncs) { c->x[1] = imp_wrap_any(c->x[1], true); c->x[2] = imp_wrap_any(c->x[2], true); host_call(c, HOSTFN("SSLSetIOFuncs"), false); }   // SecureTransport calls the guest's read / write functions (.NET's System.Net.Security)
// CGGetActiveDisplayList is empty on this Mac (the display is online but not "active" for Quartz), while the main display has valid bounds and a mode: GTK and others that count the
// active displays found no monitor. Answer with the main display when the host has none.
H(h_CGGetActiveDisplayList) {
    uint32_t max = (uint32_t)c->x[0]; uint32_t *ids = (uint32_t *)c->x[1]; uint32_t *cnt = (uint32_t *)c->x[2];
    int (*real)(uint32_t, uint32_t *, uint32_t *) = HOSTFN("CGGetActiveDisplayList");
    uint32_t n = 0; int r = real(max, ids, &n);
    if (r == 0 && n == 0 && max >= 1 && ids) { uint32_t (*mainid)(void) = HOSTFN("CGMainDisplayID"); ids[0] = mainid(); n = 1; }
    if (cnt) *cnt = n;
    c->x[0] = (uint64_t)(int64_t)r;
}
H(h_CVDisplayLinkCreateWithActiveCGDisplays) {   // (see above) the link of the main display when there is no active one
    int (*real)(void *) = HOSTFN("CVDisplayLinkCreateWithActiveCGDisplays"); void *out = (void *)c->x[0];
    int r = real(out);
    if (r != 0 && !getenv("REBAR_NO_DISPLAYLINK_FALLBACK")) { uint32_t (*mainid)(void) = HOSTFN("CGMainDisplayID"); int (*one)(uint32_t, void *) = HOSTFN("CVDisplayLinkCreateWithCGDisplay"); r = one(mainid(), out); }
    c->x[0] = (uint64_t)(int64_t)r;
}
H(h_CGDisplayIsActive) { uint32_t (*mainid)(void) = HOSTFN("CGMainDisplayID"); int (*real)(uint32_t) = HOSTFN("CGDisplayIsActive"); c->x[0] = real((uint32_t)c->x[0]) || (uint32_t)c->x[0] == mainid(); }
H(h_CFRunLoopSourceCreate) { uint8_t t[80]; wrap_ctx(c, 2, 80, t); host_call(c, HOSTFN("CFRunLoopSourceCreate"), false); }
// REBAR_DISPLAY_HZ=<n>: the display link of the guest ticks at n Hz instead of the display's own rate.  A translated browser needs ~15-17 ms for the frame of
// a scroll; on a 144 Hz display (ticks 6.9 ms apart) a frame that takes 14 ms waits for the third tick (20.8 ms, 48 fps), at a steady 60 Hz it fits in one tick.
// The host's display link keeps running (it is what tells us the link is started and the window is shown); its callback only notes the time and the latest
// time stamp, and a thread of ours calls the guest callback every 1/n s with that stamp moved onto our own grid while the link is running.
typedef struct { uint64_t cb, ctx; volatile uint64_t last_src; uint8_t ts_now[80], ts_out[80]; void *link; volatile int started; double hz; } RLink;
static uint64_t mach_ns_to_ticks(uint64_t ns) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return ns * tb.denom / tb.numer; }
static void *rl_ticker(void *arg) {
    RLink *r = arg;
    uint64_t period = mach_ns_to_ticks((uint64_t)(1e9 / r->hz)), next = mach_absolute_time() + period;
    for (;;) {
        mach_wait_until(next);
        uint64_t now = mach_absolute_time();
        if (now - r->last_src < mach_ns_to_ticks(60000000ull)) {   // the host link has ticked in the last 60 ms: it is running
            uint8_t tn[80], to[80]; memcpy(tn, r->ts_now, 80); memcpy(to, r->ts_out, 80);
            int32_t scale; memcpy(&scale, to + 4, 4);
            uint64_t hn = next, ho = next + period; memcpy(tn + 16, &hn, 8); memcpy(to + 16, &ho, 8);   // hostTime of both stamps
            int64_t per = (int64_t)((double)scale / r->hz); memcpy(tn + 32, &per, 8); memcpy(to + 32, &per, 8);   // videoRefreshPeriod
            uint64_t fout = 0;
            ((int (*)(void *, void *, void *, uint64_t, uint64_t *, void *))r->cb)(r->link, tn, to, 0, &fout, (void *)r->ctx);
        }
        next += period;
        if (next < now) next = now + period;   // fell behind: start over
    }
    return NULL;
}
static int rl_source_cb(void *link, const void *tsnow, const void *tsout, uint64_t fin, uint64_t *fout, void *ctx) {   // runs on the host's display-link thread
    RLink *r = ctx; (void)fin; (void)fout;
    if (!r->started && getenv("REBAR_DISPLAY_HZ_DEBUG")) fprintf(stderr, "rebar: first display-link tick, ticker starts\n");
    memcpy(r->ts_now, tsnow, 80); memcpy(r->ts_out, tsout, 80); r->link = link; r->last_src = mach_absolute_time();
    if (!r->started) { r->started = 1; pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED); pthread_create(&th, &at, rl_ticker, r); }
    return 0;
}
H(h_CVDisplayLinkSetOutputCallback) {
    const char *e = getenv("REBAR_DISPLAY_HZ"); double hz = e ? atof(e) : 0;
    if (hz >= 10 && hz <= 240 && c->x[1]) {
        RLink *r = calloc(1, sizeof *r); r->cb = imp_wrap(c->x[1]); r->ctx = c->x[2]; r->hz = hz;
        c->x[1] = (uint64_t)rl_source_cb; c->x[2] = (uint64_t)r;
        if (getenv("REBAR_DISPLAY_HZ_DEBUG")) fprintf(stderr, "rebar: display link %.0f Hz\n", hz);
    } else c->x[1] = imp_wrap(c->x[1]);
    host_call(c, HOSTFN("CVDisplayLinkSetOutputCallback"), false);
}
static void imp_register(uint64_t imp, const char *types);
static const char *imp_encoding(uint64_t imp);
// A method that the guest adds while it runs (class_addMethod, class_replaceMethod, method_setImplementation: Zed's window classes, the Rust objc crate) has its type encoding
// only in those calls; a method that takes or returns a struct by value (a CGRect result is in memory on x86_64, in d0-d3 on arm64) is called with the wrong registers without it.
// (The string may be a temporary of the caller: it is copied.)
static void imp_register_runtime(uint64_t imp, const char *types) { if (imp && types && strchr(types, '{') && !imp_encoding(imp)) imp_register(imp, strdup(types)); }
H(h_method_setImplementation) { imp_register_runtime(c->x[1], method_getTypeEncoding((Method)c->x[0])); c->x[1] = imp_wrap(c->x[1]); host_call(c, HOSTFN("method_setImplementation"), false); c->x[0] = imp_unwrap(c->x[0]); }
// swift_updateClassMetadata2 (completes a class's metadata: instance size, field offsets) and swift_getInitializedObjCClass (what a metadata accessor of a plain class ends with):
// the classes that were held back at load (swift_class_needs_runtime_init) are given to the Objective-C runtime when the Swift runtime is through with them.
static void swift_class_ready(uint64_t cls);
// A Swift class that ObjC-style code (an alloc / init through a class reference, a message to the class) reaches before any Swift code asked for its metadata: the Objective-C
// runtime would call the class's metadata initializer at realization; here the class was deferred, so do it before the use.
static int g_nswift_deferred;
static void swift_class_ensure_ready(CPU *c, uint64_t cls);
static inline void swift_ensure_receiver(CPU *c, uint64_t cls) { if (__builtin_expect(g_nswift_deferred != 0, 0) && in_guest(cls)) swift_class_ensure_ready(c, cls); }
H(h_objc_alloc_init) { swift_ensure_receiver(c, c->x[0]); host_call(c, HOSTFN("objc_alloc_init"), false); }
H(h_objc_alloc) { swift_ensure_receiver(c, c->x[0]); host_call(c, HOSTFN("objc_alloc"), false); }
H(h_objc_opt_new) { swift_ensure_receiver(c, c->x[0]); host_call(c, HOSTFN("objc_opt_new"), false); }
H(h_objc_allocWithZone) { swift_ensure_receiver(c, c->x[0]); host_call(c, HOSTFN("objc_allocWithZone"), false); }
H(h_swift_updateClassMetadata2) { uint64_t cls = c->x[0]; host_call(c, HOSTFN("swift_updateClassMetadata2"), false); swift_class_ready(cls); }
H(h_swift_getInitializedObjCClass) { swift_class_ready(c->x[0]); host_call(c, HOSTFN("swift_getInitializedObjCClass"), false); }
H(h_class_replaceMethod) { imp_register_runtime(c->x[2], (const char *)c->x[3]); c->x[2] = imp_wrap(c->x[2]); host_call(c, HOSTFN("class_replaceMethod"), false); c->x[0] = imp_unwrap(c->x[0]); }
H(h_class_addMethod) {
    static int trace = -1; if (trace < 0) trace = getenv("REBAR_TRACE_ENTRY") != NULL;
    if (trace) fprintf(stderr, "[addMethod] %s imp=%llx types=%s\n", c->x[1] ? sel_getName((SEL)c->x[1]) : "?", (unsigned long long)c->x[2], c->x[3] ? (const char *)c->x[3] : "?");
    imp_register_runtime(c->x[2], (const char *)c->x[3]);
    c->x[2] = imp_wrap(c->x[2]); host_call(c, HOSTFN("class_addMethod"), false);
}
H(h_method_getImplementation) { host_call(c, HOSTFN("method_getImplementation"), false); c->x[0] = imp_unwrap(c->x[0]); }
H(h_class_getMethodImplementation) { host_call(c, HOSTFN("class_getMethodImplementation"), false); c->x[0] = imp_unwrap(c->x[0]); }
// An app that opens another app of its own bundle (a menu-bar helper) with NSWorkspace asks LaunchServices, which cannot start an arm64-only program on this Mac and shows "cannot be
// opened because it is not supported by this computer". Such a launch is run through rebar instead (a child process); the completion handler is called with no app and no error.
static bool exe_is_arm64_only(const char *path) {
    int fd = open(path, O_RDONLY); if (fd < 0) return false; uint8_t h[4096]; ssize_t n = pread(fd, h, sizeof h, 0); close(fd); if (n < 32) return false;
    uint32_t magic = *(uint32_t *)h;
    if (magic == 0xbebafecau) { uint32_t na = __builtin_bswap32(*(uint32_t *)(h + 4)); bool arm = false, x86 = false;
        for (uint32_t k = 0; k < na && 8 + 20 * (k + 1) <= (size_t)n; k++) { uint32_t ct = __builtin_bswap32(*(uint32_t *)(h + 8 + 20 * k)); if (ct == 0x0100000c) arm = true; if (ct == 0x01000007) x86 = true; }
        return arm && !x86; }
    if (magic == 0xfeedfacf) return *(uint32_t *)(h + 4) == 0x0100000c;
    return false;
}
static bool try_open_arm64_app(CPU *c, id self, SEL sel) {
    static SEL s_open; if (!s_open) s_open = sel_registerName("openApplicationAtURL:configuration:completionHandler:");
    if (sel != s_open || getenv("REBAR_NO_APP_REDIRECT")) return false;
    id url = (id)c->x[2]; if (!url) return false;
    id pathstr = send_h(url, sel_registerName("path")); if (!pathstr) return false;
    const char *app = (const char *)send_h(pathstr, sel_registerName("UTF8String")); if (!app) return false;
    id bundle = send_h((id)objc_getClass("NSBundle"), sel_registerName("bundleWithPath:"), pathstr); if (!bundle) return false;
    id exestr = send_h(bundle, sel_registerName("executablePath")); if (!exestr) return false;
    const char *exe = (const char *)send_h(exestr, sel_registerName("UTF8String")); if (!exe || !exe_is_arm64_only(exe)) return false;
    pid_t pid = 0; char *argv[3] = {(char *)g_self_path, (char *)exe, NULL};
    int r = posix_spawn(&pid, g_self_path, NULL, NULL, argv, environ);
    if (getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[open application %s -> started through rebar as pid %d (%d)]\n", app, (int)pid, r);
    id running = NULL;   // the handler wants the NSRunningApplication of the new process (an async wrapper traps when it gets neither an app nor an error)
    for (int k = 0; k < 40 && !running && r == 0; k++) { running = send_h((id)objc_getClass("NSRunningApplication"), sel_registerName("runningApplicationWithProcessIdentifier:"), (long)pid); if (!running) usleep(100000); }
    id err = NULL; if (!running) err = send_h((id)objc_getClass("NSError"), sel_registerName("errorWithDomain:code:userInfo:"), send_h((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "NSCocoaErrorDomain"), (long)256, (id)NULL);
    uint64_t blk = c->x[4]; if (blk && in_guest(*(uint64_t *)(blk + 16))) call_guest(c, *(uint64_t *)(blk + 16), blk, (uint64_t)running, (uint64_t)err, 0);   // completionHandler(app, error)
    c->x[0] = 0; return true;
}
H(h_objc_msgSend) {
    id self = (id)c->x[0];
    SEL sel = (SEL)c->x[1];
    if (try_open_arm64_app(c, self, sel)) return;
    if (__builtin_expect((uint64_t)sel < 0x10000 && self, 0)) { fprintf(stderr, "rebar: objc_msgSend with selector %llx to %p (class %s) from guest pc %llx lr %llx\n", (unsigned long long)c->x[1], (void *)self, class_getName(object_getClass(self)), (unsigned long long)c->pc, (unsigned long long)c->x[30]); }
    swift_ensure_receiver(c, (uint64_t)self);
    if (__builtin_expect(g_gpu_sync != 0, 0)) gpu_hook(c, self, sel);
    if (__builtin_expect(g_dump_layers != 0, 0)) layer_dump_hook(c, self, sel);
    if (__builtin_expect(g_trace_msg != NULL, 0)) trace_msg(c, "send", self, sel);
    if (g_trace && self && (!strcmp(sel_getName(sel), "objectForKey:") || !strcmp(sel_getName(sel), "objectForInfoDictionaryKey:") || !strcmp(sel_getName(sel), "stringByAppendingString:") || !strcmp(sel_getName(sel), "stringByAppendingPathComponent:") || !strcmp(sel_getName(sel), "bundlePath"))) {
        static bool (*get_cstr)(uint64_t, char *, long, uint32_t);
        if (!get_cstr) get_cstr = dlsym(RTLD_DEFAULT, "CFStringGetCString");
        char buf[200] = "";
        if (c->x[2] > 0x10000 && get_cstr && get_cstr(c->x[2], buf, sizeof buf, 0x08000100)) fprintf(stderr, "  [arg] %s(\"%s\")\n", sel_getName(sel), buf);
    }
    if (g_trace) fprintf(stderr, "  [msg] %p %s%s\n", (void *)self, self ? (class_isMetaClass(object_getClass(self)) ? "+" : "-") : "", sel_getName(sel));
    if (!self) { c->x[0] = c->x[1] = 0; memset(&c->vr[0], 0, 64); return; }
    { static SEL s_bfc; if (!s_bfc) s_bfc = sel_registerName("bundleForClass:"); if (sel == s_bfc && c->x[2] > 0x10000 && in_guest(c->x[2]) && guest_bundle_for_class(c, c->x[2])) return; }
    {   // +[NSBundle mainBundle] is the bundle of the guest program (the .app around g_exe_path). CoreFoundation normally derives it from CFProcessPath, but it keeps the first answer it
        // gave, and for VLC that was the folder of rebar (Sparkle then raised "This host has no CFBundleVersion" and the macosx interface did not start).
        static SEL s_mb; static id mb_cache; static Class cNSBundle;
        if (!s_mb) { s_mb = sel_registerName("mainBundle"); cNSBundle = objc_getClass("NSBundle"); }
        if (sel == s_mb && self == (id)cNSBundle) {
            if (!mb_cache) {
                char root[PATH_MAX];
                if (g_exe_path[0] && guest_bundle_root(g_exe_path, root, sizeof root)) {
                    id str = send_h((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), root);
                    mb_cache = str ? send_h((id)cNSBundle, sel_registerName("bundleWithPath:"), str) : NULL;
                    if (mb_cache) send_h(mb_cache, sel_registerName("retain"));
                }
            }
            if (mb_cache) { c->x[0] = (uint64_t)mb_cache; c->x[1] = 0; return; }
        }
    }
    Class cls = object_getClass(self);
    // (a method lookup can run a class's +initialize and wait on libobjc's class initialization lock, which another thread may hold while it waits for the interpreter lock)
    bool gil_held_l = gil_release(); IMP imp_raw = class_getMethodImplementation(cls, sel); if (gil_held_l) gil_acquire();
    IMP imp = (IMP)imp_unwrap((uint64_t)imp_raw);
    if (in_guest((uint64_t)imp)) { c->pc = (uint64_t)imp; return; }  // guest -> guest: stay in the interpreter
    if ((void *)imp == p_msgForward) { host_call(c, p_msgSend, false); return; }
    if (variadic_send(c, self, sel) || valist_send(c, self, sel)) return;
    {   // NSTask (Process) starting an arm64-only executable: the host kernel refuses it ("Bad CPU type"), so the task runs rebar with the executable as its first argument,
        // the way posix_spawn does (see needs_rebar).  Parallels' installer starts its arm64 Parallels Desktop app like this.
        static SEL s_launch, s_launchRet, s_run, s_lp, s_slp, s_args, s_sargs, s_eu, s_seu, s_path, s_fileURL, s_utf8, s_swa, s_ins; static Class cNSString, cNSMutableArray, cNSURL;
        if (!s_launch) {
            s_launch = sel_registerName("launch"); s_launchRet = sel_registerName("launchAndReturnError:"); s_run = sel_registerName("run");
            s_lp = sel_registerName("launchPath"); s_slp = sel_registerName("setLaunchPath:"); s_args = sel_registerName("arguments"); s_sargs = sel_registerName("setArguments:");
            s_eu = sel_registerName("executableURL"); s_seu = sel_registerName("setExecutableURL:"); s_path = sel_registerName("path"); s_fileURL = sel_registerName("fileURLWithPath:");
            s_utf8 = sel_registerName("UTF8String"); s_swa = sel_registerName("arrayWithArray:"); s_ins = sel_registerName("insertObject:atIndex:");
            cNSString = objc_getClass("NSString"); cNSMutableArray = objc_getClass("NSMutableArray"); cNSURL = objc_getClass("NSURL");
        }
        if ((sel == s_launch || sel == s_launchRet || sel == s_run) && g_self_path[0] && strstr(class_getName(cls), "Task")) {
            id eu = (id)send_h(self, s_eu), lp = (id)send_h(self, s_lp);
            id pathStr = eu ? (id)send_h(eu, s_path) : lp;
            const char *cpath = pathStr ? (const char *)send_h(pathStr, s_utf8) : NULL;
            if (cpath && cpath[0] == '/' && strcmp(cpath, g_self_path) && needs_rebar(cpath)) {
                id args = (id)send_h(self, s_args);
                id arr = (id)send_h((id)cNSMutableArray, s_swa, args);   // (a nil array makes an empty one)
                send_h(arr, s_ins, pathStr, 0);
                send_h(self, s_sargs, arr);
                id me = (id)send_h((id)cNSString, sel_registerName("stringWithUTF8String:"), g_self_path);
                if (eu) send_h(self, s_seu, (id)send_h((id)cNSURL, s_fileURL, me)); else send_h(self, s_slp, me);
                if (g_trace) fprintf(stderr, "rebar: NSTask %s -> rebar\n", cpath);
            }
        }
    }
    {   // The guest is an arm64 program: asked what architecture it runs as, the running application says arm64 (the host process, rebar, is x86_64).  Installers and updaters
        // pick the build they download by this answer.  REBAR_NO_ARCH_SPOOF=1 leaves the host's answer.
        static SEL s_ea, s_pid; static int off = -1;
        if (!s_ea) { s_ea = sel_registerName("executableArchitecture"); s_pid = sel_registerName("processIdentifier"); }
        if (off < 0) off = getenv("REBAR_NO_ARCH_SPOOF") != NULL;
        if (sel == s_ea && !off) {
            send_host(c, cls, sel);
            uint64_t r0 = c->x[0];
            if (r0 == 0x01000007ull && (int)(intptr_t)send_h(self, s_pid) == getpid()) r0 = 0x0100000cull;   // NSBundleExecutableArchitectureX86_64 -> ARM64
            c->x[0] = r0;
            return;
        }
    }
    send_host(c, cls, sel);
}

H(h_objc_msgSendSuper2) {
    struct { id receiver; Class cur; } *sup = P(0);
    SEL sel = (SEL)c->x[1];
    id self = sup->receiver;
    if (__builtin_expect(g_trace_msg != NULL, 0)) trace_msg(c, "super", self, sel);
    if (!self) { c->x[0] = c->x[1] = 0; memset(&c->vr[0], 0, 64); return; }
    Class start = class_getSuperclass(sup->cur);
    bool gil_held_s = gil_release(); IMP imp_raw_s = class_getMethodImplementation(start, sel); if (gil_held_s) gil_acquire();
    IMP imp = (IMP)imp_unwrap((uint64_t)imp_raw_s);
    c->x[0] = (uint64_t)self;
    if (in_guest((uint64_t)imp)) { c->pc = (uint64_t)imp; return; }
    Method m = class_getInstanceMethod(start, sel);
    const char *e = m ? method_getTypeEncoding(m) : NULL;
    if (e && strchr(e, '{')) {  // structs by value (-setFrame:display:, -constrainFrameRect:toScreen:, ...): the same marshalling as a plain send
        forward_enc(c, (void *)imp, (void *)imp, e, sel_getName(sel));
        return;
    }
    Enc r = {0};
    if (e) enc_parse(e, &r);
    host_call(c, (void *)imp, r.kind == 's' && r.size > 16);
    normalize_return(c, &r);
}


// ------------------------------------------------- host -> guest with struct arguments
//
// When x86 host code calls a guest method whose signature has by-value structs
// (-drawRect:, -setFrame:, ...), the arguments arrive in the SysV layout. The guest's
// own method lists give us each IMP's type encoding, so we can rebuild the arm64 layout.

static uint64_t g_imp_key[8192];
static const char *g_imp_enc[8192];
static void imp_register(uint64_t imp, const char *types) {
    uint32_t h = (uint32_t)((imp >> 2) * 2654435761u) & 8191;
    while (g_imp_key[h] && g_imp_key[h] != imp) h = (h + 1) & 8191;
    g_imp_key[h] = imp; g_imp_enc[h] = types;
}
static const char *imp_encoding(uint64_t imp) {
    uint32_t h = (uint32_t)((imp >> 2) * 2654435761u) & 8191;
    while (g_imp_key[h]) { if (g_imp_key[h] == imp) return g_imp_enc[h]; h = (h + 1) & 8191; }
    return NULL;
}
static void imp_register_cb(const char *name, const char *types, uint64_t imp, void *ctx) {
    if (strchr(types, '{')) imp_register(imp, types);
}

static void entry_marshal(CPU *c, uint64_t *f, const char *enc, EntryState *st) {
    const uint64_t *xs = (const uint64_t *)f[15];
    int xi = 0, xv = 0, xsn = 0, nx = 0, nv = 0, tmpn = 0;
    st->mem = false; st->hfa = 0;
    for (int k = 0; k < 8; k++) { c->x[k] = 0; memset(&c->vr[k], 0, 16); }
    Enc t;
    const char *e = enc_parse(enc, &t);
    while (*e >= '0' && *e <= '9') e++;
    if (t.kind == 's' && t.size > 16) {  // hidden result pointer comes first on x86, in x8 on arm64
        st->mem = true; st->sret = f[xi++]; c->x[8] = st->sret;
        if (t.hfa >= 2 && t.hfa_t == 'd') st->hfa = t.hfa;
    }
#define X86_INT() (xi < 6 ? f[xi++] : xs[xsn++])
#define X86_DBL() (xv < 8 ? f[6 + xv++] : xs[xsn++])
    while (*e) {
        e = enc_parse(e, &t);
        while (*e >= '0' && *e <= '9') e++;
        if (nx >= 8 || nv >= 8) { fprintf(stderr, "rebar: too many arguments in callback into guest\n"); exit(127); }
        if (t.kind == 'f' || t.kind == 'd') c->vr[nv++].d[0] = X86_DBL();
        else if (t.kind != 's') c->x[nx++] = X86_INT();
        else if (t.hfa && t.hfa_t == 'd') {
            if (t.size <= 16) for (int k = 0; k < t.hfa && nv < 8; k++) c->vr[nv++].d[0] = X86_DBL();
            else for (int k = 0; k < t.hfa && nv < 8; k++) c->vr[nv++].d[0] = xs[xsn++];
        } else if (t.hfa && t.hfa_t == 'f') {
            fprintf(stderr, "rebar: callback into guest takes a float aggregate by value (not forwardable yet)\n"); exit(127);
        } else if (t.size <= 16) {
            int words = (t.size + 7) / 8;
            bool regs = xi + words <= 6;
            for (int k = 0; k < words && nx < 8; k++) c->x[nx++] = regs ? f[xi++] : xs[xsn++];
        } else {  // arm64 wants a pointer to a copy of the bytes the x86 caller put on the stack
            int words = (t.size + 7) / 8;
            uint64_t *buf = &st->tmp[tmpn];
            tmpn += words;
            if (tmpn > 60) { fprintf(stderr, "rebar: callback struct arguments too large\n"); exit(127); }
            for (int k = 0; k < words; k++) buf[k] = xs[xsn++];
            c->x[nx++] = (uint64_t)buf;
        }
    }
#undef X86_INT
#undef X86_DBL
}

static void entry_finish(CPU *c, uint64_t *f, EntryState *st) {
    if (st->mem) {
        if (st->hfa) for (int k = 0; k < st->hfa; k++) ((uint64_t *)st->sret)[k] = c->vr[k].d[0];
        f[16] = st->sret; f[17] = 0; f[18] = f[19] = 0;
        return;
    }
    f[16] = c->x[0]; f[17] = c->x[1]; f[18] = c->vr[0].d[0]; f[19] = c->vr[1].d[0];
}

// NSLog and friends: the format's variadic arguments are on the guest stack.
H(h_NSLog) {
    VA_INIT(ap, c->sp);
    void (*nslogv)(id, va_list) = dlsym(RTLD_DEFAULT, "NSLogv");
    nslogv((id)c->x[0], ap);
}
H(h_NSLogv) {
    VA_INIT(ap, c->x[1]);
    void (*nslogv)(id, va_list) = dlsym(RTLD_DEFAULT, "NSLogv");
    nslogv((id)c->x[0], ap);
}
H(h_CFStringCreateWithFormat) {
    VA_INIT(ap, c->sp);
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, va_list) = dlsym(RTLD_DEFAULT, "CFStringCreateWithFormatAndArguments");
    c->x[0] = f(c->x[0], c->x[1], c->x[2], ap);
}
H(h_CFStringCreateWithFormatAndArguments) {
    VA_INIT(ap, c->x[3]);
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, va_list) = dlsym(RTLD_DEFAULT, "CFStringCreateWithFormatAndArguments");
    c->x[0] = f(c->x[0], c->x[1], c->x[2], ap);
}

// NSLog and NSProcessInfo report the host process name unless told otherwise.
static void set_host_process_name(const char *name) {
    Class pi = objc_getClass("NSProcessInfo");
    if (!pi) return;
    objc_bridge_init();
    send_fn send = (send_fn)p_msgSend;
    id info = send((id)pi, sel_registerName("processInfo"));
    id str = send((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), name);
    send(info, sel_registerName("setProcessName:"), str);
}

// ---------------------------------------------------------- image registration

static const struct section_64 *find_section(const struct mach_header_64 *mh, const char *sect) {
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        const struct section_64 *s = (const void *)(sg + 1);
        for (uint32_t j = 0; j < sg->nsects; j++, s++)
            if (!strncmp(s->segname, "__DATA", 6) && !strncmp(s->sectname, sect, 16)) return s;
    }
    return NULL;
}

typedef void (*MethodFn)(const char *name, const char *types, uint64_t imp, void *ctx);
static void each_method(uint64_t list, MethodFn f, void *ctx) {
    if (!list) return;
    uint32_t ef = *(uint32_t *)list, cnt = *(uint32_t *)(list + 4);
    bool small = ef & 0x80000000u;
    uint32_t entsize = small ? 12 : (ef & 0xfffc);
    for (uint32_t k = 0; k < cnt; k++) {
        uint64_t m = list + 8 + (uint64_t)k * entsize;
        if (small) {
            const char *name = *(const char **)(m + *(int32_t *)m);  // offset points at a selref
            f(name, (const char *)(m + 4 + *(int32_t *)(m + 4)), m + 8 + *(int32_t *)(m + 8), ctx);
        } else f(*(const char **)m, *(const char **)(m + 8), *(uint64_t *)(m + 16), ctx);
    }
}

static void add_method_cb(const char *name, const char *types, uint64_t imp, void *ctx) {
    class_replaceMethod((Class)ctx, sel_registerName(name), (IMP)imp_wrap(imp), types);
}
struct find_load { uint64_t imp; };
static void find_load_cb(const char *name, const char *types, uint64_t imp, void *ctx) {
    if (!strcmp(name, "load")) ((struct find_load *)ctx)->imp = imp;
}

static uint64_t g_seen[16384];
static bool seen_test_and_set(uint64_t p) {
    uint32_t h = (uint32_t)((p >> 4) * 2654435761u) & 16383;
    while (g_seen[h]) { if (g_seen[h] == p) return true; h = (h + 1) & 16383; }
    g_seen[h] = p;
    return false;
}

static const void *g_imageinfo;
static uint64_t g_dropped[512];
static int g_ndropped;
static bool class_dropped(uint64_t c) { for (int k = 0; k < g_ndropped; k++) if (g_dropped[k] == c) return true; return false; }
// A Swift class whose descriptor asks for "singleton metadata initialization" (a resilient superclass, say: Rectangle's windows derive from NSWindow) is set up by the Swift
// runtime when the class is first used: swift_updateClassMetadata2 patches the class_ro_t that the class's data field points to. objc_readClassPair would have realized the
// class by then (data -> a class_rw_t on the heap), and the runtime writes the instance size into that. Such classes are not handed to the Objective-C runtime here; it learns
// of them from the Swift runtime and finds them unrealized, as it does on a real Mac.
static uint64_t g_swift_deferred[1024];
static bool swift_deferred(uint64_t c) { for (int k = 0; k < g_nswift_deferred; k++) if (g_swift_deferred[k] == c) return true; return false; }
static bool swift_class_needs_runtime_init(const GClass *c) {
    if (!(c->data & 3)) return false;   // not a Swift class
    uint64_t desc = ((const uint64_t *)c)[8];   // ClassMetadata::Description, after isa, superclass, cache (2), data, flags + address point, instance size + align mask, class size + address point
    if (!in_guest(desc)) return false;
    uint32_t flags = *(const uint32_t *)desc;
    return (flags & 0x1f) == 0x10 && ((flags >> 16) & 3) == 1;
}
static void register_class_now(GClass *c);   // (below)
// The Swift runtime has set the class up: now the Objective-C runtime may know it (and realize it, from the updated class_ro_t)
static uint64_t g_swift_done[1024]; static int g_nswift_done;
static bool swift_class_is_done(uint64_t cls) { for (int k = 0; k < g_nswift_done; k++) if (g_swift_done[k] == cls) return true; return false; }
static void swift_class_ready(uint64_t cls) {
    if (!cls || !swift_deferred(cls)) return;
    GClass *c = (GClass *)cls;
    if (swift_class_is_done(cls)) return;
    if (g_nswift_done < 1024) g_swift_done[g_nswift_done++] = cls;
    if (in_guest(c->superclass) && swift_deferred(c->superclass)) swift_class_ready(c->superclass);
    register_class_now(c);   // (the Swift runtime has realized the class in place by now; the runtime still has to learn its name and put it in its class table)
}
static void swift_class_ensure_ready(CPU *c, uint64_t cls) {
    if (!swift_deferred(cls) || swift_class_is_done(cls)) return;
    GClass *g = (GClass *)cls;
    const struct { uint32_t flags, istart, isize, res; uint64_t ivl, name, bm, bp, iv, wl, props, init; } *ro = (const void *)class_ro(g);
    if ((ro->flags & 0x40) && in_guest(ro->init)) call_guest(c, ro->init, cls, 0, 0, 0);   // RO_HAS_SWIFT_INITIALIZER: Class (*)(Class, void *)
    swift_class_ready(cls);
}
static void register_class(GClass *c) {
    if (seen_test_and_set((uint64_t)c)) return;
    if (in_guest(c->superclass)) register_class((GClass *)c->superclass);
    if (!getenv("REBAR_NO_SWIFT_DEFER") && ((in_guest(c->superclass) && swift_deferred(c->superclass)) || swift_class_needs_runtime_init(c))) {
        if (g_nswift_deferred < 1024) g_swift_deferred[g_nswift_deferred++] = (uint64_t)c;
        if (getenv("REBAR_OBJC_TRACE")) fprintf(stderr, "  [objc] deferred Swift class %p %s to the Swift runtime\n", (void *)c, (const char *)class_ro(c)->name);
        return;
    }
    if (getenv("REBAR_OBJC_TRACE")) fprintf(stderr, "  [objc] register class %p %s (super %p, isa %p)\n", (void *)c, (const char *)class_ro(c)->name, (void *)c->superclass, (void *)c->isa);
    register_class_now(c);
}
static void register_class_now(GClass *c) {
    Class r = p_readClassPair((Class)c, g_imageinfo);
    if (r != (Class)c) {
        // A class whose superclass is a weak import that this macOS lacks (a view class of a newer system, say) is dropped by the runtime: it does not exist
        // here, and neither does anything that derives from it. The class references to it become nil (see objc_map_image), as dyld does.
        if (!r && g_ndropped < 512) {
            g_dropped[g_ndropped++] = (uint64_t)c;
            if (getenv("REBAR_OBJC_TRACE")) fprintf(stderr, "  [objc] dropped class %s: its superclass is missing on this system\n", (const char *)class_ro(c)->name);
            return;
        }
        fprintf(stderr, "rebar: objc_readClassPair failed for class %s\n", (const char *)class_ro(c)->name);
        exit(1);
    }
}

static uint64_t class_load_imp(const GClass *c) {
    struct find_load fl = {0};
    each_method(class_ro((const GClass *)c->isa)->baseMethods, find_load_cb, &fl);
    return fl.imp;
}

static uint64_t g_load_cls[2048], g_load_imp[2048];
static int g_nload;
static void call_load(CPU *c, GClass *cls) {
    static uint64_t loaded[4096]; static int nloaded;
    for (int k = 0; k < nloaded; k++) if (loaded[k] == (uint64_t)cls) return;
    if (in_guest(cls->superclass)) call_load(c, (GClass *)cls->superclass);
    if (nloaded < 4096) loaded[nloaded++] = (uint64_t)cls;
    for (int k = 0; k < g_nload; k++)
        if (g_load_cls[k] == (uint64_t)cls && g_load_imp[k]) call_guest(c, g_load_imp[k], (uint64_t)cls, (uint64_t)s_load, 0, 0);
}

static uint64_t g_cur_slide;
#define SA(s) ((s)->addr + g_cur_slide)  // runtime address of a section

// The protocols compiled into the guest's images: the host runtime has not read them (it does not know the images), so NSProtocolFromString / objc_getProtocol look here first
// (Google's Gemini asserted "Either instanceClass or instanceProtocol must be provided" when the protocol of one of its own classes came back nil).
static uint64_t g_guest_protos[8192]; static int g_nguest_protos;
static uint64_t guest_protocol_named(const char *name) {
    for (int k = 0; k < g_nguest_protos; k++) { const char *n = *(const char **)(g_guest_protos[k] + 8); if (n && !strcmp(n, name)) return g_guest_protos[k]; }
    return 0;
}
H(h_objc_getProtocol) { uint64_t g = guest_protocol_named(S(0)); if (g) { c->x[0] = g; return; } host_call(c, dlsym(RTLD_DEFAULT, "objc_getProtocol"), false); }
H(h_NSProtocolFromString) {
    static SEL utf8; if (!utf8) utf8 = sel_registerName("UTF8String");
    const char *n = c->x[0] ? (const char *)send_h((id)c->x[0], utf8) : NULL;
    uint64_t g = n ? guest_protocol_named(n) : 0;
    if (g) { c->x[0] = g; return; }
    host_call(c, dlsym(RTLD_DEFAULT, "NSProtocolFromString"), false);
}
// REBAR_NO_CORNERMASK=1: AppKit of this macOS threw NSInternalInconsistencyException ("image returned from _cornerMask must have a resizing mode of NSImageResizingModeStretch")
// when VLC's first window was shown, although the image it checked has that mode: the window then has no corner mask, as a status bar window has none (VLC's macosx interface starts).
static id nil_corner_mask(id self, SEL sel) { (void)self; (void)sel; return nil; }
static void apply_no_cornermask(const char *path) {
    static bool done; if (done || !(getenv("REBAR_NO_CORNERMASK") || (path && strstr(path, "/VLC.app/")))) return;   // (on by itself for VLC)
    done = true;
    dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY);
    Class w = objc_getClass("NSWindow");
    Method m = w ? class_getInstanceMethod(w, sel_registerName("_cornerMask")) : NULL;
    if (m) method_setImplementation(m, (IMP)nil_corner_mask);
}
// Step 1, for every image before any class is registered: selectors, protocol isas, and the
// compiled method lists (read now: registering a class makes the runtime replace class_t.data).
static void objc_prepare_image(GImage *img) {
    const struct mach_header_64 *mh = img->mh;
    const struct section_64 *imageinfo = find_section(mh, "__objc_imageinfo");
    if (!imageinfo) return;
    img->objc_mapped = true;
    objc_bridge_init();
    apply_no_cornermask(img->path);
    if (!p_readClassPair) { fprintf(stderr, "rebar: libobjc lacks objc_readClassPair\n"); exit(1); }
    g_cur_slide = img->slide;

    const struct section_64 *s;
    if ((s = find_section(mh, "__objc_selrefs")))  // unique every selector
        for (uint64_t o = 0; o < s->size; o += 8) *(SEL *)(SA(s) + o) = sel_registerName(*(const char **)(SA(s) + o));
    if ((s = find_section(mh, "__objc_protolist"))) {  // compiled protocols need their isa
        Class protocol = objc_getClass("Protocol");
        for (uint64_t o = 0; o < s->size; o += 8) {
            uint64_t *proto = *(uint64_t **)(SA(s) + o);
            if (in_guest((uint64_t)proto) && !proto[0]) proto[0] = (uint64_t)protocol;
            if (in_guest((uint64_t)proto) && g_nguest_protos < 8192 && proto[1] > 0x10000) g_guest_protos[g_nguest_protos++] = (uint64_t)proto;
        }
    }
    if ((s = find_section(mh, "__objc_classlist")))
        for (uint64_t o = 0; o < s->size; o += 8) {
            GClass *gc = *(GClass **)(SA(s) + o);
            each_method(class_ro(gc)->baseMethods, imp_register_cb, NULL);
            each_method(class_ro((GClass *)gc->isa)->baseMethods, imp_register_cb, NULL);
        }
    if ((s = find_section(mh, "__objc_nlclslist")))
        for (uint64_t o = 0; o < s->size && g_nload < 2048; o += 8) {
            GClass *gc = *(GClass **)(SA(s) + o);
            g_load_cls[g_nload] = (uint64_t)gc; g_load_imp[g_nload++] = class_load_imp(gc);
        }
}

// Step 2: hand the compiled classes to the runtime.
static void objc_map_image(GImage *img) {
    if (!img->objc_mapped) return;
    const struct mach_header_64 *mh = img->mh;
    g_cur_slide = img->slide;
    g_imageinfo = (const void *)SA(find_section(mh, "__objc_imageinfo"));
    const struct section_64 *s;
    if ((s = find_section(mh, "__objc_classlist")))
        for (uint64_t o = 0; o < s->size; o += 8) register_class(*(GClass **)(SA(s) + o));
    if (g_ndropped)  // references to a dropped class are nil, so that guest code that tests for the class (availability checks) sees it is not there
        for (const char *name = "__objc_classrefs"; name; name = name[7] == 'c' ? "__objc_superrefs" : NULL)
            if ((s = find_section(mh, name)))
                for (uint64_t o = 0; o < s->size; o += 8) { uint64_t *ref = (uint64_t *)(SA(s) + o); if (class_dropped(*ref)) *ref = 0; }
}

// The host's Swift runtime finds protocol conformances and types by name through tables that dyld hands it image by image; the images rebar maps are not
// dyld's. Their tables are given to the runtime directly (swift_getTypeByMangledNameInContext of a guest type returned null, and the guest then read metadata - 8).
static const struct section_64 *find_text_section(const struct mach_header_64 *mh, const char *sect) {
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        if (strcmp(sg->segname, "__TEXT")) continue;
        const struct section_64 *s = (const void *)(sg + 1);
        for (uint32_t j = 0; j < sg->nsects; j++, s++) if (!strncmp(s->sectname, sect, 16)) return s;
    }
    return NULL;
}
static void swift_register_image(GImage *img) {
    void (*reg_conf)(const void *, const void *) = dlsym(RTLD_DEFAULT, "swift_registerProtocolConformances");
    void (*reg_types)(const void *, const void *) = dlsym(RTLD_DEFAULT, "swift_registerTypeMetadataRecords");
    if (!reg_conf && !reg_types) return;   // no Swift runtime in this process (yet)
    const struct section_64 *s;
    if (reg_conf && (s = find_text_section(img->mh, "__swift5_proto")) && s->size) reg_conf((void *)(s->addr + img->slide), (void *)(s->addr + img->slide + s->size));
    if (reg_types && (s = find_text_section(img->mh, "__swift5_types")) && s->size) reg_types((void *)(s->addr + img->slide), (void *)(s->addr + img->slide + s->size));
}

// Step 2b, after the classes of every image exist (a category may extend a class of another image).
static void objc_attach_categories(GImage *img) {
    if (!img->objc_mapped) return;
    const struct mach_header_64 *mh = img->mh;
    g_cur_slide = img->slide;
    const struct section_64 *s;
    if ((s = find_section(mh, "__objc_catlist")))
        for (uint64_t o = 0; o < s->size; o += 8) {
            GCat *cat = *(GCat **)(SA(s) + o);
            Class cls = (Class)cat->cls;
            if (class_dropped((uint64_t)cls)) continue;
            each_method(cat->instanceMethods, imp_register_cb, NULL);
            each_method(cat->classMethods, imp_register_cb, NULL);
            each_method(cat->instanceMethods, add_method_cb, cls);
            each_method(cat->classMethods, add_method_cb, object_getClass((id)cls));
            if (cat->protocols) {   // the protocols the category adopts (J2ObjC's checks `[NSString conformsToProtocol:...]` after loading its categories)
                const uint64_t *pl = (const uint64_t *)cat->protocols; uint64_t np = pl[0];
                for (uint64_t k = 0; k < np && k < 64; k++) if (in_guest(pl[1 + k])) class_addProtocol(cls, (Protocol *)pl[1 + k]);
            }
        }
}

// Step 3 (just before the image's static initializers): +load, superclasses first, then categories.
static void objc_load_image(CPU *c, GImage *img) {
    if (!img->objc_mapped) return;
    const struct mach_header_64 *mh = img->mh;
    g_cur_slide = img->slide;
    const struct section_64 *s;
    if ((s = find_section(mh, "__objc_nlclslist")))
        for (uint64_t o = 0; o < s->size; o += 8) call_load(c, *(GClass **)(SA(s) + o));
    if ((s = find_section(mh, "__objc_nlcatlist")))
        for (uint64_t o = 0; o < s->size; o += 8) {
            GCat *cat = *(GCat **)(SA(s) + o);
            struct find_load fl = {0};
            each_method(cat->classMethods, find_load_cb, &fl);
            if (fl.imp) call_guest(c, fl.imp, cat->cls, (uint64_t)s_load, 0, 0);
        }
}
