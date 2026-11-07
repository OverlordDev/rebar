// SSE templates for the translator (included by jit.h): scalar floating point and SIMD&FP register-pair loads and stores.
//
// A scalar result is written as the 16-byte register with the upper bits cleared (as the architecture does), the operands are read from the CPU
// structure like everything else. An instruction whose rare cases (out-of-range conversions, NaN handling the SSE instruction does differently)
// need the interpreter's exact answer is translated as the fast path plus a call of the helper for the rest: `rare` branches around the fast
// store to a helper call, so the common case never leaves the translated code.

static bool g_have_sse41, g_have_fma, g_have_avx2;

#define GVR(n) ((int32_t)(offsetof(CPU, vr) + 16 * (n)))

static void sse_m(JC *j, int prefix, int opc, int xr, int base, int32_t disp) { if (prefix) e1(j, prefix); rex(j, 0, xr, base, 0); e1(j, 0x0F); e1(j, opc); modrm_mem(j, xr, base, disp); }
static void sse_r(JC *j, int prefix, int opc, int xr, int xm) { if (prefix) e1(j, prefix); rex(j, 0, xr, xm, 0); e1(j, 0x0F); e1(j, opc); modrm_rr(j, xr, xm); }
static void sse_rex_w_m(JC *j, int prefix, int opc, int reg, int base, int32_t disp) { if (prefix) e1(j, prefix); rex(j, 1, reg, base, 0); e1(j, 0x0F); e1(j, opc); modrm_mem(j, reg, base, disp); }
// ---- the vector register cache.  A guest vector register that was stored from an xmm register (st_q) is also kept in one of xmm8..xmm13 for the rest of the block, so that the next
// instruction that reads it takes it from there instead of from memory (a store-forward round trip of ~5 cycles on every link of a chain of floating-point operations).  The memory copy is
// always written too, so nothing ever has to be written back: a fault, a helper, a side exit all see the registers in memory.  Everything that writes a vector register's memory in any other
// way (any modrm_mem on the CPU structure's vector area) drops that entry at the end of the instruction (vc_end_insn); a helper call drops all of them (emit_helper).  An st_q inside a forward
// branch region (open_fwd) does not enter the cache: the store is not on every path.
static int vc_find(JC *j, int n) { for (int s = 0; s < VC_N; s++) if (j->vc_reg_of[s] == n + 1) return s; return -1; }
static void vc_clear(JC *j) { memset(j->vc_reg_of, 0, sizeof j->vc_reg_of); j->vc_touch = j->vc_keep = 0; }
static void vc_end_insn(JC *j) {
    uint32_t drop = j->vc_touch & ~j->vc_keep;
    for (int s = 0; s < VC_N; s++) if (j->vc_reg_of[s] && ((drop >> (j->vc_reg_of[s] - 1)) & 1)) j->vc_reg_of[s] = 0;
    j->vc_touch = j->vc_keep = 0; j->open_fwd = 0;
}
static void sse_r(JC *j, int prefix, int opc, int xr, int xm);
static void vc_set(JC *j, int n, int xr) {   // V[n] has just been stored from xmm xr (0..7): keep a copy
    int s = vc_find(j, n);
    if (s < 0) { for (int q = 0; q < VC_N; q++) if (!j->vc_reg_of[q]) { s = q; break; }
        if (s < 0) { s = 0; for (int q = 1; q < VC_N; q++) if (j->vc_age[q] < j->vc_age[s]) s = q; } }
    j->vc_reg_of[s] = (uint8_t)(n + 1); j->vc_age[s] = ++j->vc_tick;
    sse_r(j, 0, 0x28, 8 + s, xr);   // movaps xmm(8+s), xr
    j->vc_keep |= 1u << n;
}
static void first_ld_note(JC *j, int xr, int n, int dbl) {
    if (j->first_ld_n >= 4) return;
    uint32_t insn = *(const uint32_t *)j->pc;
    j->first_lds[j->first_ld_n++] = (typeof(j->first_lds[0])){n, xr, dbl, (uint32_t)(j->p - j->start),
        (insn & 0xff000000) == 0x1f000000, j->open_fwd != 0};   // FMADD family: the rest of the instruction uses xmm0, not the address scratch
}
static void ld_s(JC *j, int xr, int n, int dbl) {   // movss / movsd xmm, [V[n]]: the upper bits of xmm are cleared
    bool note = g_loopwarm && j->pc == j->blockpc && j->first_ld_n < 4 && j->start;
    int s = vc_find(j, n);
    if (s >= 0) {
        j->vc_age[s] = ++j->vc_tick;
        if (dbl) sse_r(j, 0xF3, 0x7E, xr, 8 + s);                                   // movq xr, xmm: the upper 64 bits cleared
        else { sse_r(j, 0, 0x57, xr, xr); sse_r(j, 0xF3, 0x10, xr, 8 + s); }       // xorps xr, xr ; movss xr, xmm: only the low lane
        if (note) first_ld_note(j, xr, n, dbl);
        return;
    }
    sse_m(j, dbl ? 0xF2 : 0xF3, 0x10, xr, R15, GVR(n));
    if (note) first_ld_note(j, xr, n, dbl);
}
// the operand of a scalar operation: op xr, V[n] (the low lane) from the cache when it is there
static void sse_vm(JC *j, int prefix, int opc, int xr, int n) {
    int s = vc_find(j, n);
    if (s >= 0) { j->vc_age[s] = ++j->vc_tick; sse_r(j, prefix, opc, xr, 8 + s); } else sse_m(j, prefix, opc, xr, R15, GVR(n));
}
// A vector register is read as two 8-byte halves (movq + movhps), not with one 16-byte movups: many templates write a register as two 8-byte stores (or a 4-byte lane
// store after a 16-byte one), and a 16-byte load that spans two earlier stores cannot be forwarded from the store buffer (a stall of a dozen cycles on every dot product)
static void ld_q(JC *j, int xr, int n) {   // movq xmm, [V[n]] ; movhps xmm, [V[n]+8]
    int s = vc_find(j, n);
    if (s >= 0) { j->vc_age[s] = ++j->vc_tick; sse_r(j, 0, 0x28, xr, 8 + s); return; }
    sse_m(j, 0xF3, 0x7E, xr, R15, GVR(n)); sse_m(j, 0, 0x16, xr, R15, GVR(n) + 8);
}
static void st_q(JC *j, int n, int xr) { sse_m(j, 0, 0x11, xr, R15, GVR(n)); if (j->open_fwd == 0 && xr < 8) vc_set(j, n, xr); }   // movups [V[n]], xmm (and a copy in the cache)
static void mov_m64_zero(JC *j, int base, int32_t disp) { rex(j, 1, 0, base, 0); e1(j, 0xC7); modrm_mem(j, 0, base, disp); e4(j, 0); }
static void movq_xg(JC *j, int xr, int gr);
static void st_v64z(JC *j, int rd, int src) { movq_xg(j, 15, src); sse_m(j, 0, 0x11, 15, R15, GVR(rd)); }   // V[rd] = src (64 bits), upper half cleared: one 16-byte store (xmm15 is the scratch)   // V[rd] = src (64 bits), upper half cleared
static void movq_xg(JC *j, int xr, int gr) { e1(j, 0x66); rex(j, 1, xr, gr, 0); e1(j, 0x0F); e1(j, 0x6E); modrm_rr(j, xr, gr); }   // movq xmm, r64
static void v_const(JC *j, int xr, uint64_t v) { mov_ri_nf(j, RCX, v); movq_xg(j, xr, RCX); }                                   // the low 64 bits of xmm xr = v, the rest cleared (rcx is scratch)
static void btc_m(JC *j, int base, int32_t disp, int bit, int w) { rex(j, w, 0, base, 0); e1(j, 0x0F); e1(j, 0xBA); modrm_mem(j, 7, base, disp); e1(j, (uint8_t)bit); }

// ---- SIMD&FP LDP / STP (S, D and Q registers)
static bool g_nofp;   // REBAR_JIT_NOFP=1: no scalar-FP / vector-pair templates (to bisect)
static bool g_no_fpflags;   // REBAR_JIT_NOFPFLAGS=1: store the FCMP flags immediately (the old path, to bisect)
static JI emit_ldst_pair_vec(JC *j, uint32_t i) {
    if (g_nofp) return JI_NO;
    int opc = B(i, 31, 30), L = B(i, 22, 22), idx = B(i, 24, 23), rt = B(i, 4, 0), rt2 = B(i, 14, 10), rn = B(i, 9, 5);
    if (opc == 3 || idx == 0) return JI_NO;    // (idx 0 is the non-temporal LDNP / STNP: handled by the interpreter)
    int sz = 4 << opc;
    int32_t imm = (int32_t)sext(B(i, 21, 15), 7) * sz;
    ld_xsp(j, RCX, rn, 1);
    int32_t disp = idx == 1 ? 0 : imm;          // post-index: the access is at the old base
    int pre = opc == 0 ? 0xF3 : opc == 1 ? 0xF2 : 0;
    if (L) {
        sse_m(j, pre, 0x10, 0, RCX, disp); sse_m(j, pre, 0x10, 1, RCX, disp + sz);    // movss / movsd / movups
        st_q(j, rt, 0); st_q(j, rt2, 1);
    } else {
        if (opc == 2) { ld_q(j, 0, rt); ld_q(j, 1, rt2); sse_m(j, 0, 0x11, 0, RCX, disp); sse_m(j, 0, 0x11, 1, RCX, disp + sz); }
        else { mov_rm(j, RAX, R15, GVR(rt), 1); mov_rm(j, RDX, R15, GVR(rt2), 1); st_sized(j, RCX, disp, RAX, sz); st_sized(j, RCX, disp + sz, RDX, sz); }
    }
    if (idx == 1 || idx == 3) { lea_rm(j, RDX, RCX, imm, 1); st_xsp(j, rn, RDX); }
    return JI_OK;
}

// LD1 / ST1 (multiple structures) of one to four registers, with no offset or post-indexed (an immediate that is the number of bytes moved, or a register): the memory is contiguous
// whatever the element size, so it is a run of 8 or 16 byte moves (what the interpreter's fast function for it does, minus the helper call)
static JI emit_ld1_multi(JC *j, uint32_t i) {
    if (g_nofp) return JI_NO;
    int q = B(i, 30, 30), L = B(i, 22, 22), post = B(i, 23, 23), rm = B(i, 20, 16), op = B(i, 15, 12), rn = B(i, 9, 5), rt = B(i, 4, 0);
    int n = op == 0x7 ? 1 : op == 0xa ? 2 : op == 0x6 ? 3 : op == 0x2 ? 4 : 0;
    if (!n) return JI_NO;
    if (!post && rm) return JI_NO;
    int w = q ? 16 : 8;
    ld_xsp(j, RCX, rn, 1);
    for (int k = 0; k < n; k++) {
        int r = (rt + k) & 31;
        if (L) { if (q) sse_m(j, 0, 0x10, 0, RCX, k * w); else sse_m(j, 0xF3, 0x7E, 0, RCX, k * w); st_q(j, r, 0); }   // movups / movq xmm0, [rcx + k*w]
        else { ld_q(j, 0, r); if (q) sse_m(j, 0, 0x11, 0, RCX, k * w); else sse_m(j, 0x66, 0xD6, 0, RCX, k * w); }     // movups / movq [rcx + k*w], xmm0
    }
    if (post) {
        if (rm == 31) lea_rm(j, RDX, RCX, n * w, 1);
        else { ld_x(j, RDX, rm, 1); alu_rr(j, 0x01, RDX, RCX, 1); }
        st_xsp(j, rn, RDX);
    }
    return JI_OK;
}

// ---- scalar floating point: the instruction groups of the "data processing - scalar floating point" class
// An invalid operation (and any NaN operand) gives the positive default NaN, as the interpreter's f2raw does; x86 gives a negative one or passes the operand's payload on.
static void canon_nan(JC *j, int dbl) {   // xmm0 holds a scalar result
    sse_r(j, dbl ? 0x66 : 0, 0x2E, 0, 0);                                    // ucomis[sd] xmm0, xmm0: unordered (PF) only for a NaN
    // A plain jump over the two instructions of the NaN way: they touch nothing but rax and xmm0, so both ways reach the join with the same register cache and the cache is neither
    // flushed nor cleared (jcc_fwd / fix_here did both: every scalar FP operation threw the integer registers out of their host registers and stopped the vector cache from filling)
    e1(j, 0x0F); e1(j, 0x80 + CC_NP); uint8_t *at = j->p; e4(j, 0);
    mov_ri_nf(j, RAX, dbl ? 0x7ff8000000000000ull : 0x7fc00000ull);
    e1(j, 0x66); rex(j, 1, 0, RAX, 0); e1(j, 0x0F); e1(j, 0x6E); modrm_rr(j, 0, RAX);   // movq xmm0, rax
    int32_t dj = (int32_t)(j->p - (at + 4)); memcpy(at, &dj, 4);
}

// FMAX / FMIN / FMAXNM / FMINNM (scalar): without a NaN the four agree with x86's max / min, made symmetric for the zeros (+0 beats -0 for max, -0 beats +0 for min) by
// taking the operation both ways round and combining the results (andps for max, orps for min). Any NaN operand goes to the helper.
static JI emit_fpminmax(JC *j, uint32_t i) {
    int type = B(i, 23, 22), rm = B(i, 20, 16), op = B(i, 15, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    bool mn = op & 1;
    ld_s(j, 0, rn, type); ld_s(j, 1, rm, type);
    sse_r(j, type ? 0x66 : 0, 0x2E, 0, 1);                                          // ucomis[sd] xmm0, xmm1
    Fix rare = jcc_fwd(j, CC_P);
    sse_r(j, 0, 0x28, 2, 0);                                                        // movaps xmm2, xmm0
    int opc = mn ? 0x5D : 0x5F, pre = type ? 0xF2 : 0xF3;
    sse_r(j, pre, opc, 0, 1);                                                       // max/min xmm0, xmm1
    sse_r(j, pre, opc, 1, 2);                                                       // max/min xmm1, xmm2 (the other way round)
    sse_r(j, 0, mn ? 0x56 : 0x54, 0, 1);                                            // orps (min) / andps (max): the sign of a zero
    st_q(j, rd, 0);
    Fix done = jmp_fwd(j);
    fix_here(j, rare);
    emit_helper(j, jit_h_simd, i);
    fix_here(j, done);
    return JI_OK;
}

static JI emit_fp2(JC *j, uint32_t i) {   // two sources: FMUL FDIV FADD FSUB FNMUL
    int type = B(i, 23, 22), rm = B(i, 20, 16), op = B(i, 15, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (type <= 1 && op >= 4 && op <= 7) return emit_fpminmax(j, i);
    if (type > 1 || (op > 3 && op != 8)) return JI_NO;
    static const int opc[9] = {0x59, 0x5E, 0x58, 0x5C, 0, 0, 0, 0, 0x59};
    int pre = type ? 0xF2 : 0xF3;
    ld_s(j, 0, rn, type);
    sse_vm(j, pre, opc[op], 0, rm);
    if (op == 8) {   // FNMUL: the product, negated (a NaN stays the default NaN)
        sse_r(j, type ? 0x66 : 0, 0x2E, 0, 0);
        Fix nan = jcc_fwd(j, CC_P);
        st_q(j, rd, 0);
        btc_m(j, R15, GVR(rd), type ? 63 : 31, type);
        Fix done = jmp_fwd(j);
        fix_here(j, nan);
        mov_ri_nf(j, RAX, type ? 0x7ff8000000000000ull : 0x7fc00000ull);
        st_v64z(j, rd, RAX);
        fix_here(j, done);
        return JI_OK;
    }
    canon_nan(j, type);
    st_q(j, rd, 0);
    return JI_OK;
}

static void bit_ri(JC *j, int ext, int reg, int bit, int w) { rex(j, w, 0, reg, 0); e1(j, 0x0F); e1(j, 0xBA); modrm_rr(j, ext, reg); e1(j, (uint8_t)bit); }   // 4 bt, 5 bts, 6 btr, 7 btc

static JI emit_fp1(JC *j, uint32_t i) {   // one source: FMOV FABS FNEG FSQRT FCVT FRINT*
    int type = B(i, 23, 22), op = B(i, 20, 15), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (type > 1) return JI_NO;
    int pre = type ? 0xF2 : 0xF3;
    if (op <= 2) {   // FMOV, FABS, FNEG: integer work on the register
        mov_rm(j, RAX, R15, GVR(rn), type);
        if (op) bit_ri(j, op == 1 ? 6 : 7, RAX, type ? 63 : 31, type);
        st_v64z(j, rd, RAX);
        return JI_OK;
    }
    if (op == 3) { ld_s(j, 0, rn, type); sse_r(j, pre, 0x51, 0, 0); canon_nan(j, type); st_q(j, rd, 0); return JI_OK; }                        // FSQRT
    if (op == 4 && type == 1) { sse_r(j, 0, 0x57, 0, 0); sse_m(j, 0xF2, 0x5A, 0, R15, GVR(rn)); canon_nan(j, 0); st_q(j, rd, 0); return JI_OK; }              // FCVT Sd, Dn (cvtsd2ss)
    if (op == 5 && type == 0) { sse_r(j, 0, 0x57, 0, 0); sse_m(j, 0xF3, 0x5A, 0, R15, GVR(rn)); canon_nan(j, 1); st_q(j, rd, 0); return JI_OK; }              // FCVT Dd, Sn (cvtss2sd)
    if (op == 12 && g_have_sse41) {   // FRINTA: round to nearest, ties away from zero = trunc(x) + (|x - trunc(x)| >= 0.5 ? copysign(1, x) : 0)
        ld_s(j, 0, rn, type);
        sse_r(j, 0, 0x57, 1, 1);                                                                             // xorps xmm1, xmm1: the upper bits of the result stay clear
        e1(j, 0x66); e1(j, 0x0F); e1(j, 0x3A); e1(j, type ? 0x0B : 0x0A); modrm_rr(j, 1, 0); e1(j, 0x0B);   // roundss / roundsd xmm1, xmm0, trunc
        sse_r(j, 0, 0x28, 2, 0); sse_r(j, pre, 0x5C, 2, 1);                                                  // xmm2 = x - trunc(x), exact
        v_const(j, 3, type ? 0x7fffffffffffffffull : 0x7fffffffull); sse_r(j, 0, 0x54, 2, 3);                // andps: |frac|
        v_const(j, 4, type ? 0x3fe0000000000000ull : 0x3f000000ull);                                         // 0.5
        e1(j, pre); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 4, 2); e1(j, 2);                                  // cmpss / cmpsd xmm4, xmm2, LE: all ones when 0.5 <= |frac|
        sse_r(j, 0, 0x28, 5, 3); sse_r(j, 0, 0x55, 5, 0);                                                    // xmm5 = the sign bit of x (andnps)
        v_const(j, 6, type ? 0x3ff0000000000000ull : 0x3f800000ull); sse_r(j, 0, 0x56, 5, 6);                // orps: +-1.0
        v_const(j, 7, type ? 0x8000000000000000ull : 0x80000000ull);                                         // -0.0: the addend where no adjustment is wanted (x + -0.0 is x, also for a zero)
        sse_r(j, 0, 0x57, 5, 7); sse_r(j, 0, 0x54, 5, 4); sse_r(j, 0, 0x57, 5, 7);                          // addend = ((+-1.0 ^ -0.0) & mask) ^ -0.0
        sse_r(j, pre, 0x58, 1, 5);                                                                           // trunc + addend
        sse_r(j, 0, 0x28, 0, 1);
        canon_nan(j, type);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (op >= 8 && op <= 15 && op != 12 && op != 13 && g_have_sse41) {   // FRINTN P M Z, X, I (A has no SSE form): the rounding mode in force is to nearest even
        static const int imm[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0x08, 0x0A, 0x09, 0x0B, 0, 0, 0x0C, 0x0C};
        ld_s(j, 0, rn, type);
        e1(j, 0x66); e1(j, 0x0F); e1(j, 0x3A); e1(j, type ? 0x0B : 0x0A); modrm_rr(j, 0, 0); e1(j, (uint8_t)imm[op]);   // roundsd / roundss xmm0, xmm0, imm
        canon_nan(j, type);
        st_q(j, rd, 0);
        return JI_OK;
    }
    return JI_NO;
}

static void jfcmp_emit(JC *j, int type, int rn, int rm, bool zero) {   // ucomis[sd]: the x86 flags now mean the ARM N Z C V of a FP compare
    ld_s(j, 0, rn, type);
    if (zero) { sse_r(j, 0, 0x57, 1, 1); sse_r(j, type ? 0x66 : 0, 0x2E, 0, 1); }          // xorps xmm1, xmm1; ucomis[sd] xmm0, xmm1
    else sse_vm(j, type ? 0x66 : 0, 0x2E, 0, rm);                                              // ucomis[sd] xmm0, [m]
    j->flagkind = 4; j->kind_set = true;
}
static void jfcmp_flags(JC *j, int type, int rn, int rm, bool zero) {   // N Z C V (in memory): FCCMP's two paths set different flags, so store them at once
    jfcmp_emit(j, type, rn, rm, zero);
    flags_from_x86(j, 4);
    j->flagkind = 0;   // (the x86 flags are not the ARM ones on FCCMP's other path)
}
static JI emit_fcmp(JC *j, uint32_t i) {   // FCMP / FCMPE with a register or with #0.0
    int type = B(i, 23, 22), rm = B(i, 20, 16), rn = B(i, 9, 5), opc = B(i, 4, 3);
    if (type > 1) return JI_NO;
    if (g_no_fpflags) { jfcmp_flags(j, type, rn, rm, opc & 1); return JI_OK; }
    jfcmp_emit(j, type, rn, rm, opc & 1);
    flags_out(j, 4);   // store, defer or leave in the x86 flags (FCSEL / B.cond can read them directly)
    return JI_OK;
}
static JI emit_fccmp(JC *j, uint32_t i) {   // FCCMP / FCCMPE: the comparison when the condition holds, else the immediate flags
    int type = B(i, 23, 22), rm = B(i, 20, 16), cond = B(i, 15, 12), rn = B(i, 9, 5), nzcv = B(i, 3, 0);
    if (type > 1) return JI_NO;
    int cc = cond_cc(j, cond);
    if (cc == CC_NEVER) { mov_m8_imm(j, R15, GN, nzcv >> 3 & 1); mov_m8_imm(j, R15, GZ, nzcv >> 2 & 1); mov_m8_imm(j, R15, GC, nzcv >> 1 & 1); mov_m8_imm(j, R15, GV, nzcv & 1); j->flagkind = 0; j->kind_set = true; return JI_OK; }
    Fix skip = {0}, done = {0};
    if (cc != CC_ALWAYS) skip = jcc_fwd(j, cc ^ 1);
    jfcmp_flags(j, type, rn, rm, false);
    if (cc != CC_ALWAYS) {
        done = jmp_fwd(j);
        fix_here(j, skip);
        mov_m8_imm(j, R15, GN, nzcv >> 3 & 1); mov_m8_imm(j, R15, GZ, nzcv >> 2 & 1); mov_m8_imm(j, R15, GC, nzcv >> 1 & 1); mov_m8_imm(j, R15, GV, nzcv & 1);
        fix_here(j, done);
    }
    j->flagkind = 0; j->kind_set = true;
    return JI_OK;
}

static JI emit_fcsel(JC *j, uint32_t i) {
    int type = B(i, 23, 22), rm = B(i, 20, 16), cond = B(i, 15, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (type > 1) return JI_NO;
    mov_rm(j, RAX, R15, GVR(rm), type);    // the value when the condition fails
    mov_rm(j, RCX, R15, GVR(rn), type);
    int cc = cond_cc(j, cond);
    if (cc == CC_ALWAYS) mov_rr(j, RAX, RCX, type); else if (cc != CC_NEVER) cmov(j, cc, RAX, RCX, type);
    st_v64z(j, rd, RAX);
    return JI_OK;
}

static JI emit_fmov_imm(JC *j, uint32_t i) {
    int type = B(i, 23, 22), imm8 = B(i, 20, 13), rd = B(i, 4, 0);
    if (type > 1) return JI_NO;
    mov_ri_nf(j, RAX, vfp_expand_imm(imm8, type ? 8 : 4));
    st_v64z(j, rd, RAX);
    return JI_OK;
}

static void cvt_u64(JC *j, int dbl) {   // xmm0 = (float / double)(uint64_t)rax; xmm0 was cleared before
    int pre = dbl ? 0xF2 : 0xF3;
    alu_rr(j, 0x85, RAX, RAX, 1);                                  // test rax, rax
    Fix big = jcc_fwd(j, CC_S);
    e1(j, pre); rex(j, 1, 0, RAX, 0); e1(j, 0x0F); e1(j, 0x2A); modrm_rr(j, 0, RAX);   // cvtsi2s[sd] xmm0, rax
    Fix done = jmp_fwd(j);
    fix_here(j, big);
    mov_rr(j, RCX, RAX, 1); shift_ri(j, 5, RCX, 1, 1); alu_ri(j, 4, RAX, 1, 0); alu_rr(j, 0x09, RCX, RAX, 1);   // rcx = (rax >> 1) | (rax & 1): the halved value keeps the sticky bit
    e1(j, pre); rex(j, 1, 0, RCX, 0); e1(j, 0x0F); e1(j, 0x2A); modrm_rr(j, 0, RCX);
    sse_r(j, pre, 0x58, 0, 0);                                     // adds[sd] xmm0, xmm0
    fix_here(j, done);
}
static void fp_scale(JC *j, int dbl, int e) {   // xmm0 *= 2^e (a power of two: exact unless the result overflows)
    v_const(j, 1, dbl ? (uint64_t)(1023 + e) << 52 : (uint64_t)(127 + e) << 23);
    sse_r(j, dbl ? 0xF2 : 0xF3, 0x59, 0, 1);
}

// FMOV between the general and the vector registers, SCVTF / UCVTF (integer, or fixed-point with fbits fraction bits), FCVTZS / FCVTZU, FJCVTZS
static JI emit_fp_int(JC *j, uint32_t i, int fbits) {
    int sf = B(i, 31, 31), type = B(i, 23, 22), rmode = B(i, 20, 19), opcode = B(i, 18, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (fbits && (opcode > 3 || (!sf && !B(i, 15, 15)))) return JI_NO;
    if (!fbits && !sf && type == 1 && rmode == 3 && opcode == 6) {   // FJCVTZS: an integer that fits (and is not -0.0) here, everything else by the helper
        ld_s(j, 0, rn, 1);
        e1(j, 0xF2); e1(j, 0x0F); e1(j, 0x2C); modrm_rr(j, RAX, 0);       // cvttsd2si eax, xmm0
        alu_ri(j, 7, RAX, (int32_t)0x80000000u, 0);
        Fix rare1 = jcc_fwd(j, CC_E);                                     // out of range or NaN
        alu_rr(j, 0x85, RAX, RAX, 0);
        Fix ok = jcc_fwd(j, CC_NE);
        e1(j, 0x66); e1(j, 0x0F); e1(j, 0x50); modrm_rr(j, RCX, 0);       // movmskpd ecx, xmm0
        e1(j, 0xF6); modrm_rr(j, 0, RCX); e1(j, 1);                       // test cl, 1: the sign of a zero
        Fix rare2 = jcc_fwd(j, CC_NE);
        fix_here(j, ok);
        e1(j, 0xF2); e1(j, 0x0F); e1(j, 0x2A); modrm_rr(j, 1, RAX);       // cvtsi2sd xmm1, eax
        sse_r(j, 0x66, 0x2E, 0, 1);                                       // ucomisd xmm0, xmm1
        setcc_m(j, CC_E, R15, GZ);                                        // Z = the conversion was exact
        mov_m8_imm(j, R15, GN, 0); mov_m8_imm(j, R15, GC, 0); mov_m8_imm(j, R15, GV, 0);
        st_x(j, rd, RAX);
        Fix done = jmp_fwd(j);
        fix_here(j, rare1); fix_here(j, rare2);
        emit_helper(j, jit_h_simd, i);
        fix_here(j, done);
        j->flagkind = 0; j->kind_set = true;
        return JI_OK;
    }
    if (opcode == 6 || opcode == 7) {
        if (type == 2) {   // the top half: FMOV Xd, Vn.D[1] / FMOV Vd.D[1], Xn
            if (!sf || rmode != 1) return JI_NO;
            if (opcode == 6) { mov_rm(j, RAX, R15, GVR(rn) + 8, 1); st_x(j, rd, RAX); }
            else { ld_x(j, RAX, rn, 1); mov_mr(j, R15, GVR(rd) + 8, RAX, 1); }
            return JI_OK;
        }
        if (type > 1 || rmode != 0 || sf != type) return JI_NO;   // FMOV Wd, Sn / Sd, Wn (type 0, sf 0) and Xd, Dn / Dd, Xn (type 1, sf 1)
        if (opcode == 6) { mov_rm(j, RAX, R15, GVR(rn), sf); st_x(j, rd, RAX); }
        else { ld_x(j, RAX, rn, sf); st_v64z(j, rd, RAX); }
        return JI_OK;
    }
    if ((opcode == 2 || opcode == 3) && rmode == 0 && type <= 1) {   // SCVTF / UCVTF
        int dbl = type, uns = opcode == 3, pre = dbl ? 0xF2 : 0xF3;
        sse_r(j, 0, 0x57, 0, 0);                                       // xorps xmm0, xmm0: the upper bits of the result stay clear
        ld_x(j, RAX, rn, uns ? (sf ? 1 : 0) : sf);
        int w64 = sf || uns;                                           // a 32-bit unsigned value is zero-extended and converted as a signed 64-bit one
        if (uns && sf) {
            cvt_u64(j, dbl);
        } else {
            e1(j, pre); rex(j, w64, 0, RAX, 0); e1(j, 0x0F); e1(j, 0x2A); modrm_rr(j, 0, RAX);
        }
        if (fbits) fp_scale(j, dbl, -fbits);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if ((opcode == 0 || opcode == 1) && rmode == 3 && type <= 1) {   // FCVTZS / FCVTZU: the cases the instruction answers differently (NaN, out of range) go to the helper
        int uns = opcode, pre = type ? 0xF2 : 0xF3;
        ld_s(j, 0, rn, type);
        if (fbits) fp_scale(j, type, fbits);
        int w64 = sf || uns;
        e1(j, pre); rex(j, w64, 0, RAX, 0); e1(j, 0x0F); e1(j, 0x2C); modrm_rr(j, RAX, 0);   // cvtts[sd]2si rax / eax, xmm0
        Fix rare;
        if (!uns) { if (sf) { mov_ri_nf(j, RCX, 0x8000000000000000ull); alu_rr(j, 0x39, RAX, RCX, 1); } else alu_ri(j, 7, RAX, (int32_t)0x80000000u, 0); rare = jcc_fwd(j, CC_E); }
        else if (sf) { alu_rr(j, 0x85, RAX, RAX, 1); rare = jcc_fwd(j, CC_S); }
        else { mov_rr(j, RCX, RAX, 1); shift_ri(j, 5, RCX, 32, 1); rare = jcc_fwd(j, CC_NE); }
        st_x(j, rd, RAX);
        Fix done = jmp_fwd(j);
        fix_here(j, rare);
        if (!uns) {   // 0x80..0 came back: a NaN is 0, a positive overflow is the largest value, a negative one (or exactly the smallest value) is what cvtt gave
            sse_r(j, type ? 0x66 : 0, 0x2E, 0, 0);                  // ucomis[sd] xmm0, xmm0
            Fix notnan = jcc_fwd(j, CC_NP);
            alu_rr(j, 0x31, RAX, RAX, 0);                          // xor eax, eax
            Fix nandone = jmp_fwd(j);
            fix_here(j, notnan);
            sse_r(j, 0, 0x57, 1, 1);                               // xorps xmm1, xmm1
            sse_r(j, type ? 0x66 : 0, 0x2E, 0, 1);                 // ucomis[sd] xmm0, xmm1
            Fix keep = jcc_fwd(j, CC_BE);
            mov_ri_nf(j, RAX, sf ? 0x7fffffffffffffffull : 0x7fffffffull);
            fix_here(j, keep);
            fix_here(j, nandone);
            st_x(j, rd, RAX);
        } else emit_helper(j, jit_h_simd, i);
        fix_here(j, done);
        return JI_OK;
    }
    return JI_NO;
}

// FMADD / FMSUB / FNMADD / FNMSUB: one fused multiply-add (VEX-encoded FMA3) when the processor has it, else the helper.  xmm0 = xmm1 * [Vm] +- xmm0, with the sign pattern chosen by the opcode.
static JI emit_fmadd(JC *j, uint32_t i) {
    int type = B(i, 23, 22), o1 = B(i, 21, 21), o0 = B(i, 15, 15), ra = B(i, 14, 10), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (type > 1 || !g_have_fma) return JI_NO;
    static const uint8_t opc[4] = {0xB9, 0xBD, 0xBF, 0xBB};   // vfmadd231, vfnmadd231, vfnmsub231, vfmsub231: n*m + a, -(n*m) + a, -(n*m) - a, n*m - a
    ld_s(j, 0, ra, type);
    ld_s(j, 1, rn, type);
    e1(j, 0xC4); e1(j, 0xC2);   // VEX.0F38, register field xmm0 (no extension), memory base r15 (B extension)
    e1(j, (uint8_t)((type << 7) | (0xE << 3) | 1));   // W = double, vvvv = xmm1 (inverted), L = 0, pp = 66
    e1(j, opc[o1 * 2 + o0]); modrm_mem(j, 0, R15, GVR(rm));
    canon_nan(j, type);
    st_q(j, rd, 0);
    return JI_OK;
}

// The instruction decoder for the scalar floating-point groups. JI_NO: no template, the helper does it.
static JI emit_fp_scalar(JC *j, uint32_t i) {
    if (g_nofp) return JI_NO;
    if ((i & 0xff000000) == 0x1f000000) return emit_fmadd(j, i);                   // the fused multiply-add family (3 sources)
    if ((i & 0xff20fc07) == 0x1e202000) return emit_fcmp(j, i);                   // FCMP / FCMPE
    if ((i & 0xff200c00) == 0x1e200800) return emit_fp2(j, i);
    if ((i & 0xff207c00) == 0x1e204000) return emit_fp1(j, i);
    if ((i & 0xff200c00) == 0x1e200c00) return emit_fcsel(j, i);
    if ((i & 0xff200c00) == 0x1e200400) return emit_fccmp(j, i);
    if ((i & 0xff201fe0) == 0x1e201000) return emit_fmov_imm(j, i);
    if ((i & 0x7f20fc00) == 0x1e200000) return emit_fp_int(j, i, 0);
    if ((i & 0x7f200000) == 0x1e000000) return emit_fp_int(j, i, 64 - B(i, 15, 10));   // the fixed-point conversions
    return JI_NO;
}
