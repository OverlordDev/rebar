// NEON templates for the translator (included by jit.h after jitfp.h): the AdvSIMD instructions that the profile shows most often in
// real code (vector copies and logic, immediates, lane moves, duplicates, EXT, integer add / subtract / compare / multiply / min / max,
// shifts by an immediate, and floating-point arithmetic and comparisons).  Operands are read with movups into xmm0 / xmm1 (the vector
// registers live in the CPU structure, which is not necessarily 16-byte aligned memory for the packed SSE forms), the result is written
// back whole; a 64-bit (Q = 0) result has its upper half cleared.  A floating-point result with a NaN lane goes to the helper instead, which
// redoes the instruction from the untouched inputs with the interpreter's NaN rules.

static bool g_have_ssse3;

static void sse38_r(JC *j, int opc, int xr, int xm) { e1(j, 0x66); rex(j, 0, xr, xm, 0); e1(j, 0x0F); e1(j, 0x38); e1(j, opc); modrm_rr(j, xr, xm); }          // 66 0F 38 xx
static void sse3a_r(JC *j, int opc, int xr, int xm, int imm) { e1(j, 0x66); rex(j, 0, xr, xm, 0); e1(j, 0x0F); e1(j, 0x3A); e1(j, opc); modrm_rr(j, xr, xm); e1(j, (uint8_t)imm); }   // 66 0F 3A xx ib
static void sse_shift_imm(JC *j, int opc, int ext, int xm, int imm) { e1(j, 0x66); rex(j, 0, 0, xm, 0); e1(j, 0x0F); e1(j, opc); modrm_rr(j, ext, xm); e1(j, (uint8_t)imm); }   // 66 0F 71/72/73 /ext ib
static void v_zero_upper(JC *j, int xr) { sse_r(j, 0xF3, 0x7E, xr, xr); }                      // movq xr, xr: the upper 64 bits become zero
static void v_ld_lo(JC *j, int xr, int n) { sse_m(j, 0xF3, 0x7E, xr, R15, GVR(n)); }            // movq xr, [V[n]]: the low 64 bits, the rest cleared

static void sse_ri(JC *j, int prefix, int opc, int xr, int xm, int imm) { if (prefix) e1(j, prefix); rex(j, 0, xr, xm, 0); e1(j, 0x0F); e1(j, opc); modrm_rr(j, xr, xm); e1(j, (uint8_t)imm); }   // an SSE instruction with an immediate byte (pshufd 66 0F 70, shufps 0F C6)
static void v_narrow(JC *j, int desz) {   // the elements of xmm0 narrowed to desz bytes each, into its low 64 bits (the upper half is junk)
    if (desz == 4) { sse_ri(j, 0x66, 0x70, 0, 0, 0x08); return; }                 // pshufd xmm0, xmm0, 8: dwords 0 and 2
    v_const(j, 1, desz == 1 ? 0x0e0c0a0806040200ull : 0x0d0c090805040100ull);
    sse38_r(j, 0x00, 0, 1);                                                        // pshufb xmm0, xmm1
}
static void v_store_narrow(JC *j, int rd, bool q) {   // the 64 bits in xmm0 are the result: the upper half of Vd for the "2" forms (the lower half stays), else the whole register
    if (q) sse_m(j, 0x66, 0xD6, 0, R15, GVR(rd) + 8);                              // movq [Vd+8], xmm0
    else { sse_m(j, 0x66, 0xD6, 0, R15, GVR(rd)); mov_m64_zero(j, R15, GVR(rd) + 8); }
}

// a lane of 1, 2, 4 or 8 bytes between a vector register (in memory) and rax
static void v_lane_load(JC *j, int n, int idx, int esz, bool sign, int w) {
    int32_t d = GVR(n) + idx * esz;
    if (!sign) { if (esz == 8) mov_rm(j, RAX, R15, d, 1); else if (esz == 4) mov_rm(j, RAX, R15, d, 0); else if (esz == 2) movzx16_rm(j, RAX, R15, d); else movzx8_rm(j, RAX, R15, d); }
    else movsx_rm(j, RAX, R15, d, esz == 4 ? 4 : esz == 2 ? 2 : 1, w);
}
static void v_replicate(JC *j, int esz) {   // rax = the low element of rax repeated over 64 bits
    if (esz == 8) return;
    if (esz == 4) { mov_rr(j, RAX, RAX, 0); mov_rr(j, RCX, RAX, 1); shift_ri(j, 4, RCX, 32, 1); alu_rr(j, 0x09, RAX, RCX, 1); return; }
    movzx_rr(j, RAX, RAX, esz);
    mov_ri_nf(j, RCX, esz == 2 ? 0x0001000100010001ull : 0x0101010101010101ull);
    imul_rr(j, RAX, RCX, 1);
}
static void v_store_pattern(JC *j, int rd, bool q) {   // V[rd] = rax in the low half, rax (or zero) in the high half
    mov_mr(j, R15, GVR(rd), RAX, 1);
    if (q) mov_mr(j, R15, GVR(rd) + 8, RAX, 1); else mov_m64_zero(j, R15, GVR(rd) + 8);
}

// ---- modified immediate: MOVI / MVNI / ORR / BIC / FMOV with a vector destination
static JI emit_modimm(JC *j, uint32_t i) {
    int q = B(i, 30, 30), op = B(i, 29, 29), cmode = B(i, 15, 12), rd = B(i, 4, 0);
    bool orr = (cmode & 9) == 1 || (cmode & 13) == 9;
    static __thread CPU t_modimm_cpu; CPU *tmp = &t_modimm_cpu; memset(tmp, 0, sizeof *tmp);   // (not calloc: the translator also runs on a thread of ours, which must not call a guest's allocator)
    if (orr) for (int k = 0; k < 2; k++) tmp->vr[rd].d[k] = op ? ~0ull : 0;   // BIC clears the immediate's bits of all ones, ORR sets them in zero: the result is the operand to use
    simd_modimm(tmp, i);
    uint64_t lo = tmp->vr[rd].d[0], hi = tmp->vr[rd].d[1];
    if (!orr) {
        mov_ri_nf(j, RAX, lo); mov_mr(j, R15, GVR(rd), RAX, 1);
        if (q && hi != lo) mov_ri_nf(j, RAX, hi);
        if (q) mov_mr(j, R15, GVR(rd) + 8, RAX, 1); else mov_m64_zero(j, R15, GVR(rd) + 8);
        return JI_OK;
    }
    mov_ri_nf(j, RAX, lo);   // ORR: or with it; BIC: and with the complement (lo is the all-ones case's result, i.e. ~imm)
    rex(j, 1, RAX, R15, 0); e1(j, op ? 0x21 : 0x09); modrm_mem(j, RAX, R15, GVR(rd));
    if (q) { rex(j, 1, RAX, R15, 0); e1(j, op ? 0x21 : 0x09); modrm_mem(j, RAX, R15, GVR(rd) + 8); }
    else mov_m64_zero(j, R15, GVR(rd) + 8);
    return JI_OK;
}

// ---- DUP / INS / UMOV / SMOV
static JI emit_vcopy(JC *j, uint32_t i) {
    int q = B(i, 30, 30), op = B(i, 29, 29), imm5 = B(i, 20, 16), imm4 = B(i, 14, 11), rn = B(i, 9, 5), rd = B(i, 4, 0);
    int sz = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
    if (sz < 0) return JI_NO;
    int esz = 1 << sz, idx = imm5 >> (sz + 1);
    if (!op && (imm4 == 0 || imm4 == 1)) {   // DUP (element) / DUP (general)
        if (!q && sz == 3) return JI_NO;
        if (imm4 == 0) v_lane_load(j, rn, idx, esz, false, 1);
        else ld_x(j, RAX, rn, 1);
        v_replicate(j, esz);
        v_store_pattern(j, rd, q);
        return JI_OK;
    }
    if (!op && imm4 == 3) {   // INS (general)
        if (!q) return JI_NO;
        ld_x(j, RAX, rn, 1);
        st_sized(j, R15, GVR(rd) + idx * esz, RAX, esz);
        return JI_OK;
    }
    if (op && q) {            // INS (element): the source index is imm4
        int si = imm4 >> sz;
        v_lane_load(j, rn, si, esz, false, 1);
        st_sized(j, R15, GVR(rd) + idx * esz, RAX, esz);
        return JI_OK;
    }
    if (!op && (imm4 == 5 || imm4 == 7)) {   // SMOV / UMOV
        bool sgn = imm4 == 5;
        if (sgn ? (sz == 3 || (sz == 2 && !q)) : (q ? sz != 3 : sz == 3)) return JI_NO;
        v_lane_load(j, rn, idx, esz, sgn, q);
        st_x(j, rd, RAX);
        return JI_OK;
    }
    return JI_NO;
}

// ---- EXT
static JI emit_vext(JC *j, uint32_t i) {
    int q = B(i, 30, 30), imm4 = B(i, 14, 11), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (!q) {
        if (imm4 >= 8) return JI_NO;
        v_ld_lo(j, 0, rn); v_ld_lo(j, 1, rm);
        sse_r(j, 0x66, 0x6C, 0, 1);                                  // punpcklqdq xmm0, xmm1: Vn low | Vm low << 64
        if (imm4) sse_shift_imm(j, 0x73, 3, 0, imm4);                // psrldq xmm0, imm
        v_zero_upper(j, 0); st_q(j, rd, 0);
        return JI_OK;
    }
    if (!g_have_ssse3) return JI_NO;
    ld_q(j, 0, rn); ld_q(j, 1, rm);
    sse3a_r(j, 0x0F, 1, 0, imm4);                                    // palignr xmm1, xmm0, imm: (Vm:Vn) >> imm bytes
    st_q(j, rd, 1);
    return JI_OK;
}

// ---- three registers of the same type
static void v_nan_check_or_helper(JC *j, uint32_t i, int dbl, bool q, bool blend) {   // xmm0 holds a floating-point result: store it, or have the helper redo the instruction if a lane is NaN
    // (blend: for the arithmetic operations the redone result is only "the NaN lanes become the default NaN", which is done here without the helper)
    int rd = B(i, 4, 0);
    if (!q) v_zero_upper(j, 0);
    sse_r(j, 0, 0x28, 2, 0);                                         // movaps xmm2, xmm0
    if (dbl) { e1(j, 0x66); rex(j, 0, 2, 2, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 2, 2); e1(j, 3); }   // cmpunordpd xmm2, xmm2
    else { rex(j, 0, 2, 2, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 2, 2); e1(j, 3); }                  // cmpunordps
    if (dbl) e1(j, 0x66);
    e1(j, 0x0F); e1(j, 0x50); modrm_rr(j, RAX, 2);                   // movmskps / movmskpd eax, xmm2
    alu_rr(j, 0x85, RAX, RAX, 0);
    Fix rare = jcc_fwd(j, CC_NE);
    st_q(j, rd, 0);
    Fix done = jmp_fwd(j);
    fix_here(j, rare);
    if (blend) {
        mov_ri_nf(j, RAX, dbl ? 0x7ff8000000000000ull : 0x7fc000007fc00000ull);
        e1(j, 0x66); rex(j, 1, 0, RAX, 0); e1(j, 0x0F); e1(j, 0x6E); modrm_rr(j, 3, RAX);   // movq xmm3, rax
        sse_r(j, 0x66, 0x6C, 3, 3);                                  // punpcklqdq xmm3, xmm3
        sse_r(j, 0x66, 0xDB, 3, 2);                                  // pand xmm3, xmm2: the default NaN in the NaN lanes
        sse_r(j, 0x66, 0xDF, 2, 0);                                  // pandn xmm2, xmm0: the result in the others
        sse_r(j, 0x66, 0xEB, 2, 3);                                  // por
        sse_r(j, 0, 0x28, 0, 2);                                     // movaps xmm0, xmm2
        st_q(j, rd, 0);
    } else emit_helper(j, jit_h_simd, i);
    fix_here(j, done);
}
// FADDP Sd, Vn.2S / Dd, Vn.2D: the sum of the two lanes
static JI emit_faddp_s(JC *j, uint32_t i) {
    int sz = B(i, 22, 22), rn = B(i, 9, 5), rd = B(i, 4, 0);
    ld_s(j, 0, rn, sz);
    sse_m(j, sz ? 0xF2 : 0xF3, 0x58, 0, R15, GVR(rn) + (sz ? 8 : 4));      // addsd / addss xmm0, [the second lane]
    canon_nan(j, sz);
    st_q(j, rd, 0);
    return JI_OK;
}
// USHL Vd.2D, Vn.2D, Vm.2D: each lane of Vn shifted by the signed low byte of the same lane of Vm (left when positive, right when negative, 0 for 64 or more); AVX2 has the per-lane shifts
static JI emit_ushl_2d(JC *j, uint32_t i) {
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (!g_have_avx2) return JI_NO;
    ld_q(j, 0, rn); ld_q(j, 1, rm);
    mov_ri_nf(j, RAX, 0x0800); e1(j, 0x66); rex(j, 1, 0, RAX, 0); e1(j, 0x0F); e1(j, 0x6E); modrm_rr(j, 3, RAX);   // movq xmm3, rax: the pshufb mask (byte 0 and byte 8)
    sse_r(j, 0, 0x28, 2, 1);                       // movaps xmm2, xmm1
    sse38_r(j, 0x00, 2, 3);                        // pshufb xmm2, xmm3
    sse38_r(j, 0x22, 2, 2);                        // pmovsxbq xmm2, xmm2: the two shift counts as signed 64-bit numbers
    sse_r(j, 0x66, 0xEF, 4, 4);                    // pxor xmm4, xmm4
    sse_r(j, 0, 0x28, 5, 4);                       // movaps xmm5, xmm4
    sse38_r(j, 0x37, 5, 2);                        // pcmpgtq xmm5, xmm2: all ones where the count is negative
    sse_r(j, 0, 0x28, 6, 5);                       // movaps xmm6, xmm5
    sse_r(j, 0x66, 0xDF, 6, 2);                    // pandn xmm6, xmm2: the left counts (negative ones are 0)
    sse_r(j, 0x66, 0xFB, 4, 2);                    // psubq xmm4, xmm2: minus the count
    sse_r(j, 0x66, 0xDB, 4, 5);                    // pand xmm4, xmm5: the right counts
    e1(j, 0xC4); e1(j, 0xE2); e1(j, 0xF9); e1(j, 0x47); e1(j, 0xC6);   // vpsllvq xmm0, xmm0, xmm6
    e1(j, 0xC4); e1(j, 0xE2); e1(j, 0xF9); e1(j, 0x45); e1(j, 0xC4);   // vpsrlvq xmm0, xmm0, xmm4
    st_q(j, rd, 0);
    return JI_OK;
}
static JI emit_vthree(JC *j, uint32_t i) {
    int q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), rm = B(i, 20, 16), op = B(i, 15, 11), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (op == 3) {   // AND BIC ORR ORN / EOR BSL BIT BIF
        ld_q(j, 0, rn); ld_q(j, 1, rm);
        if (!U) {
            if (size == 0) sse_r(j, 0x66, 0xDB, 0, 1);               // pand
            else if (size == 1) { sse_r(j, 0x66, 0xDF, 1, 0); sse_r(j, 0, 0x28, 0, 1); }   // BIC: ~Vm & Vn (pandn xmm1, xmm0), result to xmm0
            else if (size == 2) sse_r(j, 0x66, 0xEB, 0, 1);          // por
            else { sse_r(j, 0x66, 0x76, 2, 2); sse_r(j, 0x66, 0xEF, 1, 2); sse_r(j, 0x66, 0xEB, 0, 1); }   // ORN: Vn | ~Vm
        } else if (size == 0) sse_r(j, 0x66, 0xEF, 0, 1);            // pxor
        else {
            ld_q(j, 2, rd);                                          // the destination is an operand
            if (size == 1) { sse_r(j, 0x66, 0xEF, 0, 1); sse_r(j, 0x66, 0xDB, 0, 2); sse_r(j, 0x66, 0xEF, 0, 1); }                        // BSL: ((Vn ^ Vm) & Vd) ^ Vm
            else if (size == 2) { sse_r(j, 0x66, 0xEF, 0, 2); sse_r(j, 0x66, 0xDB, 0, 1); sse_r(j, 0x66, 0xEF, 0, 2); }                  // BIT: Vd ^ ((Vd ^ Vn) & Vm)
            else { sse_r(j, 0x66, 0xEF, 0, 2); sse_r(j, 0x66, 0xDF, 1, 0); sse_r(j, 0, 0x28, 0, 1); sse_r(j, 0x66, 0xEF, 0, 2); }       // BIF: Vd ^ ((Vd ^ Vn) & ~Vm)
        }
        if (!q) v_zero_upper(j, 0);
        st_q(j, rd, 0);
        return JI_OK;
    }
    // the integer operations by element size
    static const uint8_t padd[4] = {0xFC, 0xFD, 0xFE, 0xD4}, psub[4] = {0xF8, 0xF9, 0xFA, 0xFB}, peq[4] = {0x74, 0x75, 0x76, 0}, pgt[4] = {0x64, 0x65, 0x66, 0};
    if (op == 0x10) { ld_q(j, 0, rn); ld_q(j, 1, rm); sse_r(j, 0x66, U ? psub[size] : padd[size], 0, 1); if (!q) v_zero_upper(j, 0); st_q(j, rd, 0); return JI_OK; }
    if ((op == 0x01 || op == 0x05) && (size < 2 || (size == 2 && !U))) {   // SQADD UQADD / SQSUB UQSUB
        bool sub = op == 0x05;
        ld_q(j, 0, rn); ld_q(j, 1, rm);
        if (size < 2) {
            static const uint8_t add_s[2] = {0xEC, 0xED}, add_u[2] = {0xDC, 0xDD}, sub_s[2] = {0xE8, 0xE9}, sub_u[2] = {0xD8, 0xD9};
            sse_r(j, 0x66, (sub ? (U ? sub_u : sub_s) : (U ? add_u : add_s))[size], 0, 1);
        } else if (!U) {   // 32-bit signed: the wrapped result, and INT_MAX / INT_MIN (by the sign of Vn) in the lanes that overflowed
            sse_r(j, 0, 0x28, 2, 0);                                           // xmm2 = a
            sse_r(j, 0x66, sub ? 0xFA : 0xFE, 0, 1);                           // xmm0 = a -+ b (psubd / paddd)
            sse_r(j, 0, 0x28, 3, 2); sse_r(j, 0x66, 0xEF, 3, 0);              // xmm3 = a ^ r
            if (sub) { sse_r(j, 0, 0x28, 4, 2); sse_r(j, 0x66, 0xEF, 4, 1); }  // xmm4 = a ^ b
            else { sse_r(j, 0, 0x28, 4, 1); sse_r(j, 0x66, 0xEF, 4, 0); }      // xmm4 = b ^ r
            sse_r(j, 0x66, 0xDB, 3, 4);                                        // overflow: the sign bit of (a ^ r) & (a ^ b) or (b ^ r)
            sse_shift_imm(j, 0x72, 4, 3, 31);                                  // psrad xmm3, 31: the mask
            sse_shift_imm(j, 0x72, 4, 2, 31);                                  // psrad xmm2, 31: all ones where a is negative
            sse_r(j, 0x66, 0x76, 4, 4); sse_shift_imm(j, 0x72, 2, 4, 1);       // xmm4 = 0x7fffffff
            sse_r(j, 0x66, 0xEF, 2, 4);                                        // the saturated value: INT_MIN for a negative a (all ones ^ 0x7fffffff), else INT_MAX
            sse_r(j, 0x66, 0xDB, 2, 3);                                        // & mask
            sse_r(j, 0x66, 0xDF, 3, 0);                                        // xmm3 = ~mask & r
            sse_r(j, 0x66, 0xEB, 3, 2); sse_r(j, 0, 0x28, 0, 3);
        }
        if (!q) v_zero_upper(j, 0);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (op == 0x11 && U && size < 3) { ld_q(j, 0, rn); ld_q(j, 1, rm); sse_r(j, 0x66, peq[size], 0, 1); if (!q) v_zero_upper(j, 0); st_q(j, rd, 0); return JI_OK; }   // CMEQ
    if (op == 0x06 && !U && size < 3) { ld_q(j, 0, rn); ld_q(j, 1, rm); sse_r(j, 0x66, pgt[size], 0, 1); if (!q) v_zero_upper(j, 0); st_q(j, rd, 0); return JI_OK; }   // CMGT
    if (op == 0x13 && !U && (size == 1 || (size == 2 && g_have_sse41))) {   // MUL
        ld_q(j, 0, rn); ld_q(j, 1, rm);
        if (size == 1) sse_r(j, 0x66, 0xD5, 0, 1); else sse38_r(j, 0x40, 0, 1);
        if (!q) v_zero_upper(j, 0); st_q(j, rd, 0); return JI_OK;
    }
    if ((op == 0x0c || op == 0x0d) && size < 3) {   // SMAX UMAX SMIN UMIN
        bool mx = op == 0x0c;
        //                       signed: b, h, s              unsigned: b, h, s     (0x100 | opc: the SSE4.1 forms behind 0F 38)
        static const uint16_t mn[2][3] = {{0x138, 0xEA, 0x139}, {0xDA, 0x13A, 0x13B}}, mxs[2][3] = {{0x13C, 0xEE, 0x13D}, {0xDE, 0x13E, 0x13F}};
        int code = (mx ? mxs : mn)[U][size];
        if ((code & 0x100) && !g_have_sse41) return JI_NO;
        ld_q(j, 0, rn); ld_q(j, 1, rm);
        if (code & 0x100) sse38_r(j, code & 0xff, 0, 1); else sse_r(j, 0x66, code, 0, 1);
        if (!q) v_zero_upper(j, 0); st_q(j, rd, 0); return JI_OK;
    }
    // floating point: size bit 1 chooses the operation within the group, bit 0 (22) the width
    int sz = B(i, 22, 22), hi = B(i, 23, 23);
    if (op >= 0x18 && !(q == 0 && sz == 1)) {
        int pre = sz ? 0x66 : 0;
        bool arith = (!U && op == 0x1a) || (U && op == 0x1b && !hi) || (U && op == 0x1f && !hi);
        if (arith) {
            ld_q(j, 0, rn); ld_q(j, 1, rm);
            int opc = op == 0x1a ? (hi ? 0x5C : 0x58) : op == 0x1b ? 0x59 : 0x5E;   // subps / addps / mulps / divps
            sse_r(j, pre, opc, 0, 1);
            v_nan_check_or_helper(j, i, sz, q, true);
            return JI_OK;
        }
        if ((!U && op == 0x19) && g_have_fma) {   // FMLA / FMLS: Vd = Vd +- Vn * Vm, fused
            ld_q(j, 0, rd); ld_q(j, 1, rn); ld_q(j, 2, rm);
            e1(j, 0xC4); e1(j, 0xE2); e1(j, (uint8_t)((sz << 7) | ((~1 & 15) << 3) | 1)); e1(j, hi ? 0xBC : 0xB8); modrm_rr(j, 0, 2);   // vfmadd231ps/pd or vfnmadd231 xmm0, xmm1, xmm2
            v_nan_check_or_helper(j, i, sz, q, true);
            return JI_OK;
        }
        if (op == 0x1c && (!hi || U)) {   // FCMEQ (U = 0), FCMGE (U = 1, hi = 0), FCMGT (U = 1, hi = 1): ordered comparisons, false for NaN
            ld_q(j, 0, rn); ld_q(j, 1, rm);
            int pred;
            if (!U) { pred = 0; }                                    // eq: xmm0 = (Vn == Vm)
            else {
                pred = hi ? 1 : 2;                                   // gt: Vm < Vn, ge: Vm <= Vn: compare with the operands the other way round
                sse_r(j, 0, 0x28, 2, 1); sse_r(j, 0, 0x28, 1, 0); sse_r(j, 0, 0x28, 0, 2);   // swap xmm0 and xmm1 (movaps through xmm2)
            }
            if (pre) e1(j, pre);
            rex(j, 0, 0, 1, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 0, 1); e1(j, (uint8_t)pred);   // cmpps / cmppd xmm0, xmm1, pred
            if (!q) v_zero_upper(j, 0); st_q(j, rd, 0);
            return JI_OK;
        }
    }
    return JI_NO;
}

// ---- shifts by an immediate: SHL, USHR, SSHR for 16, 32 and 64-bit lanes
static JI emit_vshift(JC *j, uint32_t i) {
    int q = B(i, 30, 30), U = B(i, 29, 29), immh = B(i, 22, 19), immhb = B(i, 22, 16), op = B(i, 15, 11), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (op == 0x14 && immh && immh < 8) {   // SSHLL / USHLL (SXTL / UXTL when the shift is 0), and the "2" forms
        if (!g_have_sse41) return JI_NO;
        int es = immh >= 4 ? 32 : immh >= 2 ? 16 : 8, shl = immhb - es;
        ld_q(j, 0, rn);
        if (q) sse_shift_imm(j, 0x73, 3, 0, 8);                                  // psrldq xmm0, 8: the upper half is the source
        sse38_r(j, (U ? 0x30 : 0x20) + (es == 8 ? 0 : es == 16 ? 3 : 5), 0, 0);  // pmovsx / pmovzx bw, wd, dq
        if (shl) sse_shift_imm(j, es == 8 ? 0x71 : es == 16 ? 0x72 : 0x73, 6, 0, shl);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (op == 0x10 && !U && immh && immh < 8) {   // SHRN / SHRN2: shift the wide elements right, keep the low half of each
        int des = immh >= 4 ? 32 : immh >= 2 ? 16 : 8;
        if (des < 32 && !g_have_ssse3) return JI_NO;
        ld_q(j, 0, rn);
        sse_shift_imm(j, des == 8 ? 0x71 : des == 16 ? 0x72 : 0x73, 2, 0, 2 * des - immhb);   // psrlw / psrld / psrlq
        v_narrow(j, des / 8); v_store_narrow(j, rd, q);
        return JI_OK;
    }
    int es = immh >= 8 ? 64 : immh >= 4 ? 32 : immh >= 2 ? 16 : 8;
    if (es == 8 || (!q && es == 64)) return JI_NO;
    int sh;
    if (op == 0x0a && !U) {   // SHL
        sh = immhb - es;
        ld_q(j, 0, rn);
        sse_shift_imm(j, es == 16 ? 0x71 : es == 32 ? 0x72 : 0x73, 6, 0, sh);
    } else if (op == 0x00 && (U || es != 64)) {   // USHR / SSHR
        sh = 2 * es - immhb;
        ld_q(j, 0, rn);
        sse_shift_imm(j, es == 16 ? 0x71 : es == 32 ? 0x72 : 0x73, U ? 2 : 4, 0, sh);
    } else return JI_NO;
    if (!q) v_zero_upper(j, 0);
    st_q(j, rd, 0);
    return JI_OK;
}


// ---- DUP (element), scalar: Vd = one lane, the rest cleared
static JI emit_scopy(JC *j, uint32_t i) {
    int imm5 = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    int sz = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
    if (sz < 0) return JI_NO;
    v_lane_load(j, rn, imm5 >> (sz + 1), 1 << sz, false, 1);
    st_v64z(j, rd, RAX);
    return JI_OK;
}

static void v_ucvtf32(JC *j) {   // the unsigned 32-bit lanes of xmm0 as floats: hi * 65536 + lo, both halves exact in single precision, the sum rounded once
    sse_r(j, 0x66, 0x6F, 1, 0);                                      // movdqa xmm1, xmm0
    sse_shift_imm(j, 0x72, 2, 1, 16);                                // psrld xmm1, 16
    sse_shift_imm(j, 0x72, 6, 0, 16); sse_shift_imm(j, 0x72, 2, 0, 16);   // the low halves
    sse_r(j, 0, 0x5B, 0, 0); sse_r(j, 0, 0x5B, 1, 1);                // cvtdq2ps both
    v_const(j, 2, 0x4780000047800000ull); sse_r(j, 0x66, 0x6C, 2, 2);   // 65536.0 in every lane
    sse_r(j, 0, 0x59, 1, 2); sse_r(j, 0, 0x58, 0, 1);                // mulps xmm1, xmm2; addps xmm0, xmm1
}
// ---- scalar SCVTF / UCVTF whose source is a SIMD&FP register (S or D)
static JI emit_smisc2(JC *j, uint32_t i) {
    int U = B(i, 29, 29), sz = B(i, 22, 22), op = B(i, 16, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (B(i, 23, 23) || op != 0x1d) return JI_NO;
    if (!sz) {
        ld_s(j, 0, rn, 0);                                           // the low 32 bits, the rest cleared
        if (!U) sse_r(j, 0, 0x5B, 0, 0); else v_ucvtf32(j);
    } else {
        sse_r(j, 0, 0x57, 0, 0);                                     // xorps xmm0, xmm0
        mov_rm(j, RAX, R15, GVR(rn), 1);
        if (!U) { e1(j, 0xF2); rex(j, 1, 0, RAX, 0); e1(j, 0x0F); e1(j, 0x2A); modrm_rr(j, 0, RAX); }   // cvtsi2sd xmm0, rax
        else cvt_u64(j, 1);
    }
    st_q(j, rd, 0);
    return JI_OK;
}

// ---- two registers, miscellaneous: XTN, NOT, compares against zero, FABS / FNEG, SCVTF / UCVTF and FCVTZS / FCVTZU on single-precision lanes, SHLL
static JI emit_vmisc2(JC *j, uint32_t i) {
    int q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 16, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (!U && op == 0x12 && size < 3) {   // XTN / XTN2
        if (size < 2 && !g_have_ssse3) return JI_NO;
        ld_q(j, 0, rn); v_narrow(j, 1 << size); v_store_narrow(j, rd, q);
        return JI_OK;
    }
    if (U && op == 0x05 && size == 0) {   // NOT
        ld_q(j, 0, rn); sse_r(j, 0x66, 0x76, 1, 1); sse_r(j, 0x66, 0xEF, 0, 1);
        if (!q) v_zero_upper(j, 0);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (op == 0x0f && size >= 2) {        // FABS / FNEG: only the sign bits change (a NaN too)
        int sz = size & 1;
        if (!q && sz) return JI_NO;
        ld_q(j, 0, rn);
        sse_r(j, 0x66, 0x76, 1, 1);                                          // pcmpeqd xmm1, xmm1
        sse_shift_imm(j, sz ? 0x73 : 0x72, 6, 1, sz ? 63 : 31);              // psllq / pslld: the sign bits
        if (U) sse_r(j, 0x66, 0xEF, 0, 1);                                   // FNEG: pxor
        else { sse_r(j, 0x66, 0xDF, 1, 0); sse_r(j, 0, 0x28, 0, 1); }        // FABS: pandn xmm1, xmm0; movaps xmm0, xmm1
        if (!q) v_zero_upper(j, 0);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (size < 3 && ((!U && (op == 0x08 || op == 0x09 || op == 0x0a)) || (U && (op == 0x08 || op == 0x09)))) {   // CMGT CMEQ CMLT / CMGE CMLE against zero
        static const uint8_t peq[3] = {0x74, 0x75, 0x76}, pgt[3] = {0x64, 0x65, 0x66};
        ld_q(j, 0, rn); sse_r(j, 0x66, 0xEF, 1, 1);                          // xmm1 = 0
        int res = 0; bool inv = false;
        if (!U && op == 0x09) sse_r(j, 0x66, peq[size], 0, 1);               // CMEQ: x == 0
        else if (!U && op == 0x08) sse_r(j, 0x66, pgt[size], 0, 1);          // CMGT: x > 0
        else if (!U && op == 0x0a) { sse_r(j, 0x66, pgt[size], 1, 0); res = 1; }                 // CMLT: 0 > x
        else if (U && op == 0x08) { sse_r(j, 0x66, pgt[size], 1, 0); res = 1; inv = true; }      // CMGE: not (0 > x)
        else { sse_r(j, 0x66, pgt[size], 0, 1); inv = true; }                // CMLE: not (x > 0)
        if (inv) { sse_r(j, 0x66, 0x76, 2, 2); sse_r(j, 0x66, 0xEF, res, 2); }
        if (!q) v_zero_upper(j, res);
        st_q(j, rd, res);
        return JI_OK;
    }
    if (op == 0x1d && size == 0) {        // SCVTF / UCVTF, 32-bit lanes
        ld_q(j, 0, rn);
        if (!U) sse_r(j, 0, 0x5B, 0, 0);                                     // cvtdq2ps
        else v_ucvtf32(j);
        if (!q) v_zero_upper(j, 0);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (op == 0x1b && size == 2) {        // FCVTZS / FCVTZU, 32-bit lanes: a lane that x86 answers differently (NaN, out of range, negative for the unsigned form) goes to the helper
        ld_q(j, 0, rn);
        sse_r(j, 0xF3, 0x5B, 1, 0);                                          // cvttps2dq xmm1, xmm0
        if (!U) { v_const(j, 2, 0x8000000080000000ull); sse_r(j, 0x66, 0x6C, 2, 2); sse_r(j, 0x66, 0x76, 2, 1); sse_r(j, 0, 0x50, RAX, 2); }   // lanes that are the "integer indefinite"
        else sse_r(j, 0, 0x50, RAX, 1);                                      // the sign bits of the results
        if (!q) alu_ri(j, 4, RAX, 3, 0);
        alu_rr(j, 0x85, RAX, RAX, 0);
        Fix rare = jcc_fwd(j, CC_NE);
        if (!q) v_zero_upper(j, 1);
        st_q(j, rd, 1);
        Fix done = jmp_fwd(j);
        fix_here(j, rare);
        emit_helper(j, jit_h_simd, i);
        fix_here(j, done);
        return JI_OK;
    }
    bool rev64 = !U && op == 0 && size <= 2, rev32 = U && op == 0 && size <= 1, rev16 = !U && op == 1 && size == 0;
    if (rev64 || rev32 || rev16) {   // REV64 / REV32 / REV16: the elements of each 8 / 4 / 2 byte group in reverse order
        int group = rev64 ? 8 : rev32 ? 4 : 2, es = 1 << size;
        if (es >= group) return JI_NO;
        ld_q(j, 0, rn);
        if (group == 8 && es == 4) { e1(j, 0x66); e1(j, 0x0F); e1(j, 0x70); e1(j, 0xC0); e1(j, 0xB1); }   // pshufd xmm0, xmm0, 0xb1: the two floats / words of each qword swap
        else {
            if (!g_have_ssse3) return JI_NO;
            uint8_t mask[16];
            for (int p = 0; p < 16; p++) { int g0 = p / group * group, w = p % group, e = w / es, b = w % es; mask[p] = (uint8_t)(g0 + (group / es - 1 - e) * es + b); }
            uint64_t lo, hi; memcpy(&lo, mask, 8); memcpy(&hi, mask + 8, 8);
            v_const(j, 1, lo); v_const(j, 2, hi); sse_r(j, 0x66, 0x6C, 1, 2);                              // punpcklqdq xmm1, xmm2: the shuffle mask
            sse38_r(j, 0x00, 0, 1);                                                                       // pshufb xmm0, xmm1
        }
        if (!q) v_zero_upper(j, 0);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (g_have_sse41 && (op == 0x18 || op == 0x19) && !(U && op == 0x18)) {   // FRINTN FRINTP FRINTM FRINTZ (U = 0), FRINTX FRINTI (U = 1): roundps / roundpd; a NaN lane goes to the helper
        int sz = size & 1, hi = size >> 1;
        if (!q && sz) return JI_NO;
        int imm = U ? 0x0C : op == 0x18 ? (hi ? 0x0A : 0x08) : (hi ? 0x0B : 0x09);
        ld_q(j, 0, rn);
        sse3a_r(j, sz ? 0x09 : 0x08, 0, 0, imm);                     // roundps / roundpd xmm0, xmm0, imm
        v_nan_check_or_helper(j, i, sz, q, false);
        return JI_OK;
    }
    if (U && op == 0x13 && size < 3) {    // SHLL / SHLL2: the (upper) half widened and shifted left by the element size
        if (!g_have_sse41) return JI_NO;
        ld_q(j, 0, rn);
        if (q) sse_shift_imm(j, 0x73, 3, 0, 8);                              // psrldq xmm0, 8
        sse38_r(j, 0x30 + (size == 0 ? 0 : size == 1 ? 3 : 5), 0, 0);        // pmovzx
        sse_shift_imm(j, size == 0 ? 0x71 : size == 1 ? 0x72 : 0x73, 6, 0, 8 << size);
        st_q(j, rd, 0);
        return JI_OK;
    }
    return JI_NO;
}

// ---- ZIP1 / ZIP2 / UZP1 / UZP2
static JI emit_vperm(JC *j, uint32_t i) {
    int q = B(i, 30, 30), size = B(i, 23, 22), rm = B(i, 20, 16), op = B(i, 14, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (op == 2 || op == 6) {   // TRN1 / TRN2: the even / odd elements of Vn and Vm interleaved
        bool hi2 = op == 6;
        if ((!q && size == 3) || (size < 2 && !g_have_ssse3)) return JI_NO;
        if (q) { ld_q(j, 0, rn); ld_q(j, 1, rm); } else { v_ld_lo(j, 0, rn); v_ld_lo(j, 1, rm); }
        if (size == 3) sse_r(j, 0x66, hi2 ? 0x6D : 0x6C, 0, 1);                         // punpck[lh]qdq
        else if (size == 2) { sse_ri(j, 0, 0xC6, 0, 1, hi2 ? 0xDD : 0x88); sse_ri(j, 0x66, 0x70, 0, 0, 0xD8); }   // shufps (the even / odd lanes of each) then pshufd 0xD8: a0 b0 a2 b2
        else {
            v_const(j, 2, size == 1 ? (hi2 ? 0x0f0e0b0a07060302ull : 0x0d0c090805040100ull) : (hi2 ? 0x0f0d0b0907050301ull : 0x0e0c0a0806040200ull));
            sse38_r(j, 0x00, 0, 2); sse38_r(j, 0x00, 1, 2);                             // pshufb: the wanted elements into the low half of each
            sse_r(j, 0x66, size == 1 ? 0x61 : 0x60, 0, 1);                              // punpcklwd / punpcklbw xmm0, xmm1
        }
        if (!q) v_zero_upper(j, 0);
        st_q(j, rd, 0);
        return JI_OK;
    }
    if (op != 1 && op != 3 && op != 5 && op != 7) return JI_NO;
    bool zip = (op & 3) == 3, hi = op >= 5;
    if (!q && size == 3) return JI_NO;
    if (!zip && size < 2 && !g_have_ssse3) return JI_NO;
    static const uint8_t ulo[4] = {0x60, 0x61, 0x62, 0x6C}, uhi[4] = {0x68, 0x69, 0x6A, 0x6D};   // punpckl / punpckh bw wd dq qdq
    if (q) {
        ld_q(j, 0, rn); ld_q(j, 1, rm);
        if (zip) sse_r(j, 0x66, (hi ? uhi : ulo)[size], 0, 1);
        else if (size == 3) sse_r(j, 0x66, hi ? 0x6D : 0x6C, 0, 1);                 // punpck[lh]qdq
        else if (size == 2) sse_ri(j, 0, 0xC6, 0, 1, hi ? 0xDD : 0x88);             // shufps xmm0, xmm1: elements 0, 2 / 1, 3 of each
        else {
            v_const(j, 2, size == 1 ? (hi ? 0x0f0e0b0a07060302ull : 0x0d0c090805040100ull) : (hi ? 0x0f0d0b0907050301ull : 0x0e0c0a0806040200ull));
            sse38_r(j, 0x00, 0, 2); sse38_r(j, 0x00, 1, 2);                         // pshufb: the wanted elements into the low half of each
            sse_r(j, 0x66, 0x6C, 0, 1);                                              // punpcklqdq xmm0, xmm1
        }
        st_q(j, rd, 0);
        return JI_OK;
    }
    v_ld_lo(j, 0, rn); v_ld_lo(j, 1, rm);
    if (zip) { sse_r(j, 0x66, ulo[size], 0, 1); if (hi) sse_shift_imm(j, 0x73, 3, 0, 8); }   // the interleave of the lower halves; ZIP2 wants its upper half
    else {
        sse_r(j, 0x66, 0x6C, 0, 1);                                                  // xmm0 = Vn low : Vm low
        if (size == 2) sse_ri(j, 0x66, 0x70, 0, 0, hi ? 0x0d : 0x08);                // pshufd: dwords 0, 2 / 1, 3
        else { v_const(j, 2, size == 1 ? (hi ? 0x0f0e0b0a07060302ull : 0x0d0c090805040100ull) : (hi ? 0x0f0d0b0907050301ull : 0x0e0c0a0806040200ull)); sse38_r(j, 0x00, 0, 2); }
    }
    v_zero_upper(j, 0); st_q(j, rd, 0);
    return JI_OK;
}

// ---- FMUL / FMLA / FMLS by element (vector and scalar, single and double precision)
static JI emit_vindexed(JC *j, uint32_t i) {
    bool scalar = (i >> 28) == 5;
    int q = scalar ? 0 : B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), L = B(i, 21, 21), M = B(i, 20, 20), op = B(i, 15, 12), H = B(i, 11, 11), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (U || size < 2 || (op != 1 && op != 5 && op != 9)) return JI_NO;
    int sz = size & 1, rm = (M << 4) | B(i, 19, 16), idx;
    if (sz) { if (L || (!q && !scalar)) return JI_NO; idx = H; } else idx = (H << 1) | L;
    if (op != 9 && !g_have_fma) return JI_NO;
    sse_m(j, sz ? 0xF2 : 0xF3, 0x10, 2, R15, GVR(rm) + idx * (4 << sz));            // movss / movsd xmm2, the element
    if (!scalar) { if (sz) sse_r(j, 0x66, 0x14, 2, 2); else sse_ri(j, 0, 0xC6, 2, 2, 0); }   // unpcklpd / shufps: the element in every lane
    if (op == 9) {
        if (scalar) { ld_s(j, 0, rn, sz); sse_r(j, sz ? 0xF2 : 0xF3, 0x59, 0, 2); canon_nan(j, sz); st_q(j, rd, 0); }
        else { ld_q(j, 0, rn); sse_r(j, sz ? 0x66 : 0, 0x59, 0, 2); v_nan_check_or_helper(j, i, sz, q, false); }
        return JI_OK;
    }
    if (scalar) { ld_s(j, 0, rd, sz); ld_s(j, 1, rn, sz); } else { ld_q(j, 0, rd); ld_q(j, 1, rn); }
    e1(j, 0xC4); e1(j, 0xE2); e1(j, (uint8_t)((sz << 7) | (0xE << 3) | 1));       // VEX.66.0F38 W = sz, vvvv = xmm1: xmm0 = +-(xmm1 * xmm2) + xmm0
    e1(j, scalar ? (op == 1 ? 0xB9 : 0xBD) : (op == 1 ? 0xB8 : 0xBC)); modrm_rr(j, 0, 2);
    if (scalar) { canon_nan(j, sz); st_q(j, rd, 0); } else v_nan_check_or_helper(j, i, sz, q, false);
    return JI_OK;
}

// SDOT / UDOT Vd.4S, Vn.16B, Vm.16B (and the by-element form Vm.4B[idx]): every 32-bit lane of Vd gets the sum of the products of four bytes of Vn and four bytes of Vm (signed
// or unsigned). The bytes are widened to words (pmovsxbw / pmovzxbw), multiplied and summed in pairs (pmaddwd), the pairs are summed (phaddd) and added to Vd. It is what the
// quantised dot products of llama.cpp and the like spend their time in; the interpreter's loop for it was ~40x slower.
static JI emit_dot(JC *j, uint32_t i, bool indexed) {
    if (!g_have_avx2) return JI_NO;
    int q = B(i, 30, 30), U = B(i, 29, 29), rm = indexed ? B(i, 20, 16) : B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    int idx = indexed ? (B(i, 11, 11) << 1) | B(i, 21, 21) : 0;
    int widen = U ? 0x30 : 0x20;   // pmovzxbw / pmovsxbw
    ld_q(j, 0, rn);
    ld_q(j, 1, rm);
    if (indexed) sse_ri(j, 0x66, 0x70, 1, 1, idx * 0x55);                        // pshufd xmm1, xmm1, idx * 0x55: the chosen 4 bytes in every lane
    if (q) { sse_r(j, 0, 0x28, 3, 0); sse_r(j, 0, 0x28, 4, 1); }                // movaps xmm3, xmm0; movaps xmm4, xmm1
    sse38_r(j, widen, 0, 0); sse38_r(j, widen, 1, 1);                             // the low 8 bytes of each as words
    sse_r(j, 0x66, 0xF5, 0, 1);                                                   // pmaddwd xmm0, xmm1
    if (q) {
        sse_shift_imm(j, 0x73, 3, 3, 8); sse_shift_imm(j, 0x73, 3, 4, 8);        // psrldq xmm3, 8; psrldq xmm4, 8: the high 8 bytes
        sse38_r(j, widen, 3, 3); sse38_r(j, widen, 4, 4);
        sse_r(j, 0x66, 0xF5, 3, 4);                                               // pmaddwd xmm3, xmm4
        sse38_r(j, 0x02, 0, 3);                                                   // phaddd xmm0, xmm3
    } else {
        sse38_r(j, 0x02, 0, 0);                                                   // phaddd xmm0, xmm0: lanes 0 and 1 are the two sums
    }
    ld_q(j, 2, rd);
    sse_r(j, 0x66, 0xFE, 0, 2);                                                   // paddd xmm0, the accumulator
    if (!q) v_zero_upper(j, 0);
    st_q(j, rd, 0);
    return JI_OK;
}

// FCVTL / FCVTL2 (half -> single, single -> double: exact) and FCVTN / FCVTN2 (single -> half, double -> single, rounded to nearest as the default FPCR says; a NaN anywhere in the
// source goes to the helper, which has the interpreter's NaN rules). F16C does the half conversions.
static bool g_have_f16c;
static JI emit_fcvtln(JC *j, uint32_t i) {
    int q = B(i, 30, 30), sz = B(i, 22, 22), rn = B(i, 9, 5), rd = B(i, 4, 0);
    bool narrow = !B(i, 12, 12);   // 0x7800: FCVTL, 0x6800: FCVTN (bit 12 tells them apart)
    if (!sz && !g_have_f16c) return JI_NO;
    if (!narrow) {   // FCVTL: the low (or, with "2", the high) 64 bits widened
        sse_m(j, 0xF3, 0x7E, 0, R15, GVR(rn) + (q ? 8 : 0));              // movq xmm0, [V[rn] (+ 8)]
        if (sz) sse_r(j, 0, 0x5A, 0, 0);                                    // cvtps2pd xmm0, xmm0
        else { e1(j, 0xC4); e1(j, 0xE2); e1(j, 0x79); e1(j, 0x13); modrm_rr(j, 0, 0); }   // vcvtph2ps xmm0, xmm0
        v_nan_check_or_helper(j, i, sz, true, false);                       // (a NaN lane: the helper has the interpreter's NaN rules)
        return JI_OK;
    }
    ld_q(j, 0, rn);
    sse_r(j, 0, 0x28, 2, 0);                                                // movaps xmm2, xmm0
    if (sz) { e1(j, 0x66); rex(j, 0, 2, 2, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 2, 2); e1(j, 3); }   // cmpunordpd xmm2, xmm2
    else { rex(j, 0, 2, 2, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 2, 2); e1(j, 3); }                  // cmpunordps xmm2, xmm2
    if (sz) e1(j, 0x66);
    e1(j, 0x0F); e1(j, 0x50); modrm_rr(j, RAX, 2);                          // movmskps / movmskpd eax, xmm2
    alu_rr(j, 0x85, RAX, RAX, 0);
    Fix rare = jcc_fwd(j, CC_NE);
    if (sz) sse_r(j, 0x66, 0x5A, 0, 0);                                     // cvtpd2ps xmm0, xmm0
    else { e1(j, 0xC4); e1(j, 0xE3); e1(j, 0x79); e1(j, 0x1D); modrm_rr(j, 0, 0); e1(j, 0); }   // vcvtps2ph xmm0, xmm0, 0 (round to nearest even)
    v_store_narrow(j, rd, q);
    Fix done = jmp_fwd(j);
    fix_here(j, rare);
    emit_helper(j, jit_h_simd, i);
    fix_here(j, done);
    return JI_OK;
}

static void v_const128(JC *j, int xr, uint64_t v) { v_const(j, xr, v); sse_r(j, 0x66, 0x6C, xr, xr); }   // xmm xr = v in both halves (punpcklqdq)
// FMAX / FMIN Vd, Vn, Vm (single or double lanes): maxps both ways and AND (min: OR) the results so that the zero with the right sign wins as on ARM (+0 for max, -0 for min); a NaN
// in either source goes to the helper
static JI emit_fmaxmin(JC *j, uint32_t i) {
    int q = B(i, 30, 30), dbl = B(i, 22, 22), mn = B(i, 23, 23), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (dbl && !q) return JI_NO;
    if (q) { ld_q(j, 0, rn); ld_q(j, 1, rm); } else { v_ld_lo(j, 0, rn); v_ld_lo(j, 1, rm); }
    sse_r(j, 0, 0x28, 2, 0);                                                       // movaps xmm2, xmm0
    if (dbl) { e1(j, 0x66); rex(j, 0, 2, 1, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 2, 1); e1(j, 3); }   // cmpunordpd xmm2, xmm1
    else { rex(j, 0, 2, 1, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 2, 1); e1(j, 3); }                  // cmpunordps xmm2, xmm1
    if (dbl) e1(j, 0x66);
    e1(j, 0x0F); e1(j, 0x50); modrm_rr(j, RAX, 2);                                 // movmskps / movmskpd eax, xmm2
    alu_rr(j, 0x85, RAX, RAX, 0);
    Fix rare = jcc_fwd(j, CC_NE);
    sse_r(j, 0, 0x28, 3, 0);                                                       // movaps xmm3, xmm0
    int opc = mn ? 0x5D : 0x5F, pre = dbl ? 0x66 : 0;
    sse_r(j, pre, opc, 0, 1);                                                      // max/min xmm0, xmm1
    sse_r(j, pre, opc, 1, 3);                                                      // max/min xmm1, xmm3 (the other way round)
    sse_r(j, pre, mn ? 0x56 : 0x54, 0, 1);                                         // orps (min) / andps (max): the sign of a zero
    st_q(j, rd, 0);
    Fix done = jmp_fwd(j);
    fix_here(j, rare);
    emit_helper(j, jit_h_simd, i);
    fix_here(j, done);
    return JI_OK;
}
// FCVTNS Vd.4S, Vn.4S (to nearest, ties to even): cvtps2dq; the lanes that came out as 0x80000000 (x86's "indefinite": overflow or NaN, where ARM saturates) go to the helper
static JI emit_fcvtns(JC *j, uint32_t i) {
    int q = B(i, 30, 30), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (q) ld_q(j, 0, rn); else v_ld_lo(j, 0, rn);
    sse_r(j, 0x66, 0x5B, 0, 0);                                                    // cvtps2dq xmm0, xmm0
    v_const128(j, 2, 0x8000000080000000ull);
    sse_r(j, 0, 0x28, 1, 0);                                                       // movaps xmm1, xmm0
    sse_r(j, 0x66, 0x76, 1, 2);                                                    // pcmpeqd xmm1, xmm2
    e1(j, 0x0F); e1(j, 0x50); modrm_rr(j, RAX, 1);                                 // movmskps eax, xmm1
    alu_rr(j, 0x85, RAX, RAX, 0);
    Fix rare = jcc_fwd(j, CC_NE);
    st_q(j, rd, 0);
    Fix done = jmp_fwd(j);
    fix_here(j, rare);
    emit_helper(j, jit_h_simd, i);
    fix_here(j, done);
    return JI_OK;
}
// SCVTF Vd.4S, Vn.4S, #fbits (32-bit lanes): cvtdq2ps, then the exact scaling by 2^-fbits
static JI emit_scvtf_fixed(JC *j, uint32_t i) {
    int q = B(i, 30, 30), immh = B(i, 22, 19), immhb = B(i, 22, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if ((immh >> 2) != 1) return JI_NO;                                            // 32-bit lanes only (immh = 01xx)
    int fbits = 64 - immhb;
    if (fbits < 1 || fbits > 32) return JI_NO;
    if (q) ld_q(j, 0, rn); else v_ld_lo(j, 0, rn);
    sse_r(j, 0, 0x5B, 0, 0);                                                       // cvtdq2ps xmm0, xmm0
    uint64_t f = (uint64_t)(127 - fbits) << 23; f |= f << 32;
    v_const128(j, 1, f);
    sse_r(j, 0, 0x59, 0, 1);                                                       // mulps xmm0, xmm1
    st_q(j, rd, 0);
    return JI_OK;
}

// FADDP Vd.4S/2S/2D, Vn, Vm (vector, pairwise): the pairs of Vn then the pairs of Vm, which is what haddps / haddpd (SSE3) computes with Vn as the destination
static JI emit_faddp_v(JC *j, uint32_t i) {
    int q = B(i, 30, 30), sz = B(i, 22, 22), rm = B(i, 20, 16), rn = B(i, 9, 5);
    if (!g_have_ssse3 || (sz && !q)) return JI_NO;
    if (q) { ld_q(j, 0, rn); ld_q(j, 1, rm); } else { v_ld_lo(j, 0, rn); v_ld_lo(j, 1, rm); }
    if (sz) e1(j, 0x66); else e1(j, 0xF2);
    e1(j, 0x0F); e1(j, 0x7C); modrm_rr(j, 0, 1);                                    // haddpd / haddps xmm0, xmm1
    if (!q) sse_ri(j, 0x66, 0x70, 0, 0, 0x08);                                     // pshufd xmm0, xmm0, 8: [n0+n1, m0+m1] in the low half
    v_nan_check_or_helper(j, i, sz, q, true);
    return JI_OK;
}

// USHL Vd.4S / Vd.2S, Vn, Vm: each lane of Vn shifted by the signed low byte of the same lane of Vm (left when positive, right when negative, 0 for 32 or more). It is half of the
// movemask that sse2neon-style code (Cycles' BVH traversal) builds from "and with a bit pattern, ushl by 0..3, addv": 124 million helper calls in one small render.
static JI emit_ushl_s(JC *j, uint32_t i) {
    int q = B(i, 30, 30), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (!g_have_avx2 || !g_have_sse41) return JI_NO;
    if (q) { ld_q(j, 0, rn); ld_q(j, 1, rm); } else { v_ld_lo(j, 0, rn); v_ld_lo(j, 1, rm); }
    sse_r(j, 0, 0x28, 2, 1);                       // movaps xmm2, xmm1
    sse_shift_imm(j, 0x72, 6, 2, 24);              // pslld xmm2, 24
    sse_shift_imm(j, 0x72, 4, 2, 24);              // psrad xmm2, 24: the signed low byte of every count
    sse_r(j, 0x66, 0xEF, 3, 3);                    // pxor xmm3, xmm3
    sse_r(j, 0, 0x28, 4, 2);                       // movaps xmm4, xmm2
    sse38_r(j, 0x3D, 4, 3);                        // pmaxsd xmm4, xmm3: the left counts (negative ones are 0)
    sse_r(j, 0, 0x28, 5, 3);                       // movaps xmm5, xmm3
    sse_r(j, 0x66, 0xFA, 5, 2);                    // psubd xmm5, xmm2: minus the count
    sse38_r(j, 0x3D, 5, 3);                        // pmaxsd xmm5, xmm3: the right counts
    e1(j, 0xC4); e1(j, 0xE2); e1(j, 0x79); e1(j, 0x47); e1(j, 0xF4);   // vpsllvd xmm6, xmm0, xmm4
    e1(j, 0xC4); e1(j, 0xE2); e1(j, 0x79); e1(j, 0x45); e1(j, 0xFD);   // vpsrlvd xmm7, xmm0, xmm5
    sse_r(j, 0x66, 0x66, 3, 2);                    // pcmpgtd xmm3, xmm2: all ones where the count is negative
    sse_r(j, 0x66, 0xDB, 7, 3);                    // pand xmm7, xmm3: the right-shifted lanes
    sse_r(j, 0x66, 0xDF, 3, 6);                    // pandn xmm3, xmm6: the left-shifted ones in the others
    sse_r(j, 0x66, 0xEB, 3, 7);                    // por xmm3, xmm7
    if (!q) v_zero_upper(j, 3);
    st_q(j, rd, 3);
    return JI_OK;
}
// ADDV Sd, Vn.4S: the sum of the four lanes (two horizontal adds)
static JI emit_addv_4s(JC *j, uint32_t i) {
    int rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (!g_have_ssse3) return JI_NO;
    ld_q(j, 0, rn);
    sse38_r(j, 0x02, 0, 0); sse38_r(j, 0x02, 0, 0);   // phaddd xmm0, xmm0 (twice)
    sse_r(j, 0x66, 0x7E, 0, RAX);                  // movd eax, xmm0
    st_v64z(j, rd, RAX);
    return JI_OK;
}
// FSQRT Vd.4S/2S/2D: sqrtps / sqrtpd (correctly rounded in both); a NaN lane (the square root of a negative number, or a NaN operand) goes to the helper
static JI emit_fsqrt_v(JC *j, uint32_t i) {
    int q = B(i, 30, 30), sz = B(i, 22, 22), rn = B(i, 9, 5);
    if (sz && !q) return JI_NO;
    if (q) ld_q(j, 0, rn); else v_ld_lo(j, 0, rn);
    sse_r(j, sz ? 0x66 : 0, 0x51, 0, 0);           // sqrtps / sqrtpd xmm0, xmm0
    v_nan_check_or_helper(j, i, sz, q, false);
    return JI_OK;
}
// TBL Vd, {Vn..Vn+len}, Vm (one to four table registers): pshufb for each 16-byte table with the index moved into that table's range; an index outside the tables selects
// zero (the sum idx - 16k + 0x70 saturates to a byte with bit 7 set, which pshufb turns into 0, for every index that is not in [16k, 16k + 15])
static JI emit_tbl(JC *j, uint32_t i) {
    int q = B(i, 30, 30), rm = B(i, 20, 16), len = B(i, 14, 13), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (!g_have_ssse3) return JI_NO;
    if (q) ld_q(j, 1, rm); else v_ld_lo(j, 1, rm);
    sse_r(j, 0x66, 0xEF, 0, 0);                    // pxor xmm0, xmm0: the result
    v_const128(j, 5, 0x7070707070707070ull);
    for (int k = 0; k <= len; k++) {
        sse_r(j, 0, 0x28, 2, 1);                   // movaps xmm2, xmm1
        if (k) { v_const128(j, 4, 0x1010101010101010ull * (uint64_t)k); sse_r(j, 0x66, 0xF8, 2, 4); }   // psubb xmm2, 16k
        sse_r(j, 0x66, 0xDC, 2, 5);                // paddusb xmm2, 0x70
        ld_q(j, 3, (rn + k) & 31);
        sse38_r(j, 0x00, 3, 2);                    // pshufb xmm3, xmm2
        sse_r(j, 0x66, 0xEB, 0, 3);                // por xmm0, xmm3
    }
    if (!q) v_zero_upper(j, 0);
    st_q(j, rd, 0);
    return JI_OK;
}
// FCMGT / FCMGE / FCMEQ / FCMLE / FCMLT against zero (vector, single and double): the ordered compare predicates, false for a NaN as in the architecture
static JI emit_fcm0(JC *j, uint32_t i) {
    int q = B(i, 30, 30), U = B(i, 29, 29), sz = B(i, 22, 22), op = B(i, 16, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (sz && !q) return JI_NO;
    int pre = sz ? 0x66 : 0, res, pred;
    if (!U && op == 0xc) { res = 1; pred = 1; }          // GT: 0 < a
    else if (U && op == 0xc) { res = 1; pred = 2; }      // GE: 0 <= a
    else if (!U && op == 0xd) { res = 0; pred = 0; }     // EQ
    else if (U && op == 0xd) { res = 0; pred = 2; }      // LE: a <= 0
    else if (!U && op == 0xe) { res = 0; pred = 1; }     // LT: a < 0
    else return JI_NO;
    if (q) ld_q(j, 0, rn); else v_ld_lo(j, 0, rn);
    sse_r(j, 0, 0x57, 1, 1);                             // xorps xmm1, xmm1
    if (res) sse_ri(j, pre, 0xC2, 1, 0, pred); else sse_ri(j, pre, 0xC2, 0, 1, pred);   // cmpps / cmppd: xmm1 = (0 op a), or xmm0 = (a op 0)
    if (!q) v_zero_upper(j, res);
    st_q(j, rd, res);
    return JI_OK;
}
// LD1 / ST1 (single structure, one lane) and LD1R (load one element and replicate it), with no offset or post-indexed
static JI emit_ld1_lane(JC *j, uint32_t i) {
    if (g_nofp) return JI_NO;
    int q = B(i, 30, 30), post = B(i, 23, 23), L = B(i, 22, 22), R = B(i, 21, 21), rm = B(i, 20, 16), opc = B(i, 15, 13), S = B(i, 12, 12), size = B(i, 11, 10), rn = B(i, 9, 5), rt = B(i, 4, 0);
    if (R || (!post && rm)) return JI_NO;
    int esz, idx = 0; bool rep = false;
    if (opc == 0) { esz = 1; idx = (q << 3) | (S << 2) | size; }
    else if (opc == 2) { if (size & 1) return JI_NO; esz = 2; idx = (q << 2) | (S << 1) | (size >> 1); }
    else if (opc == 4) { if (size == 0) { esz = 4; idx = (q << 1) | S; } else if (size == 1 && !S) { esz = 8; idx = q; } else return JI_NO; }
    else if (opc == 6 && L && !S) { esz = 1 << size; rep = true; }
    else return JI_NO;
    int szl = esz == 1 ? 0 : esz == 2 ? 1 : esz == 4 ? 2 : 3;
    ld_xsp(j, RCX, rn, 1);
    if (L) {
        emit_load_sized(j, szl, 1, RCX, 0);                                    // rax = the element, zero-extended
        if (rep) { v_replicate(j, esz); v_store_pattern(j, rt, q); if (post) ld_xsp(j, RCX, rn, 1); }   // (v_replicate used rcx)
        else st_sized(j, R15, GVR(rt) + idx * esz, RAX, esz);                  // only that lane of Vt changes
    } else {
        v_lane_load(j, rt, idx, esz, false, 1);
        st_sized(j, RCX, 0, RAX, esz);
    }
    if (post) {
        if (rm == 31) lea_rm(j, RDX, RCX, esz, 1); else { ld_x(j, RDX, rm, 1); alu_rr(j, 0x01, RDX, RCX, 1); }
        st_xsp(j, rn, RDX);
    }
    return JI_OK;
}

// FMAXV / FMINV / FMAXNMV / FMINNMV Sd, Vn.4S: two steps of "combine with the lanes swapped", each the symmetric max / min (so that +0 beats -0 for max and -0 beats +0 for min);
// any NaN lane goes to the helper
static JI emit_fredv(JC *j, uint32_t i) {
    int rn = B(i, 9, 5), rd = B(i, 4, 0);
    bool mn = B(i, 23, 23);
    ld_q(j, 0, rn);
    sse_r(j, 0, 0x28, 1, 0);                                                        // movaps xmm1, xmm0
    rex(j, 0, 1, 1, 0); e1(j, 0x0F); e1(j, 0xC2); modrm_rr(j, 1, 1); e1(j, 3);      // cmpunordps xmm1, xmm1
    e1(j, 0x0F); e1(j, 0x50); modrm_rr(j, RAX, 1);                                  // movmskps eax, xmm1
    alu_rr(j, 0x85, RAX, RAX, 0);
    Fix rare = jcc_fwd(j, CC_NE);
    for (int step = 0; step < 2; step++) {
        sse_ri(j, 0x66, 0x70, 2, 0, step ? 0xB1 : 0x4E);                            // pshufd xmm2, xmm0: the lanes swapped in pairs / halves
        sse_r(j, 0, 0x28, 3, 0);                                                    // movaps xmm3, xmm0
        sse_r(j, 0, mn ? 0x5D : 0x5F, 0, 2);                                        // minps / maxps xmm0, xmm2
        sse_r(j, 0, mn ? 0x5D : 0x5F, 2, 3);                                        // the other way round
        sse_r(j, 0, mn ? 0x56 : 0x54, 0, 2);                                        // orps / andps: the sign of a zero
    }
    sse_r(j, 0x66, 0x7E, 0, RAX);                                                   // movd eax, xmm0
    st_v64z(j, rd, RAX);
    Fix done = jmp_fwd(j);
    fix_here(j, rare);
    emit_helper(j, jit_h_simd, i);
    fix_here(j, done);
    return JI_OK;
}

static JI emit_fabd_s(JC *j, uint32_t i) {   // FABD Sd, Sn, Sm / Dd, Dn, Dm: |n - m|
    int dbl = B(i, 22, 22), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    ld_s(j, 0, rn, dbl);
    sse_m(j, dbl ? 0xF2 : 0xF3, 0x5C, 0, R15, GVR(rm));                                                   // subss / subsd xmm0, [m]
    v_const(j, 3, dbl ? 0x7fffffffffffffffull : 0x7fffffffull); sse_r(j, 0, 0x54, 0, 3);                    // andps: the absolute value
    canon_nan(j, dbl);
    st_q(j, rd, 0);
    return JI_OK;
}
static JI emit_neon(JC *j, uint32_t i) {
    if ((i & 0xff7ffc00) == 0x6e30f800 || (i & 0xff7ffc00) == 0x6e30c800) return emit_fredv(j, i);   // FMAXV / FMINV / FMAXNMV / FMINNMV (4S)
    if ((i & 0xbfe0fc00) == 0x2ea04400) return emit_ushl_s(j, i);                 // USHL (32-bit lanes)
    if ((i & 0xfffffc00) == 0x4eb1b800) return emit_addv_4s(j, i);                // ADDV Sd, Vn.4S
    if ((i & 0xbfbffc00) == 0x2ea1f800) return emit_fsqrt_v(j, i);                // FSQRT (vector)
    if ((i & 0xbfe09c00) == 0x0e000000) return emit_tbl(j, i);                    // TBL (not TBX)
    if ((i & 0x9f3e0c00) == 0x0e200800 && B(i, 23, 23) && B(i, 16, 12) >= 0xc && B(i, 16, 12) <= 0xe) { JI r = emit_fcm0(j, i); if (r != JI_NO) return r; }   // FCM* against #0.0
    if ((i & 0xbf20fc00) == 0x0e20c400) return emit_fmaxmin(j, i);                // FMAXNM / FMINNM (the same as FMAX / FMIN when no operand is a NaN)
    if ((i & 0xbfa0fc00) == 0x2e20d400) return emit_faddp_v(j, i);                // FADDP (U = 1, opcode 11010, size<1> = 0)
    if ((i & 0xbf20fc00) == 0x0e20f400) return emit_fmaxmin(j, i);                // FMAX / FMIN (U = 0, opcode 11110)
    if ((i & 0xbfbffc00) == 0x0e21a800) { if (!B(i, 22, 22)) return emit_fcvtns(j, i); }   // FCVTNS (single lanes)
    if ((i & 0xbf80fc00) == 0x0f00e400) return emit_scvtf_fixed(j, i);            // SCVTF (vector, fixed-point)
    if ((i & 0xbfbffc00) == 0x0e217800 || (i & 0xbfbffc00) == 0x0e216800) return emit_fcvtln(j, i);
    if ((i & 0x9fe0fc00) == 0x0e809400) return emit_dot(j, i, false);
    if ((i & 0x9fc0f400) == 0x0f80e000) return emit_dot(j, i, true);
    if ((i & 0xffa0fc00) == 0x7ea0d400) return emit_fabd_s(j, i);                 // FABD (scalar)
    if ((i & 0xffbffc00) == 0x7e30d800) return emit_faddp_s(j, i);
    if ((i & 0xffe0fc00) == 0x6ee04400) return emit_ushl_2d(j, i);
    if ((i & 0x9ff80400) == 0x0f000400) return emit_modimm(j, i);
    if ((i & 0x9f200400) == 0x0e200400) return emit_vthree(j, i);
    if ((i & 0x9fe08400) == 0x0e000400) return emit_vcopy(j, i);
    if ((i & 0xffe0fc00) == 0x5e000400) return emit_scopy(j, i);
    if ((i & 0xbfe08400) == 0x2e000000) return emit_vext(j, i);
    if ((i & 0x9f800400) == 0x0f000400) return emit_vshift(j, i);
    if ((i & 0x9f3e0c00) == 0x0e200800) return emit_vmisc2(j, i);
    if ((i & 0xbf208c00) == 0x0e000800) return emit_vperm(j, i);
    if ((i & 0xdf3e0c00) == 0x5e200800) return emit_smisc2(j, i);
    if ((i & 0x9f000400) == 0x0f000000 || (i & 0xdf000400) == 0x5f000000) return emit_vindexed(j, i);
    return JI_NO;
}
