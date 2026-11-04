// Templates for the ordered and atomic memory instructions (included by jit.h): load-acquire / store-release, the LSE atomics and
// compare-and-swap, and the bit-counting data-processing instructions CLZ and RBIT.
//
// x86 gives every plain load acquire and every plain store release semantics, so LDAR, LDAPR and LDAPUR are plain loads.  A store-release
// followed by a load-acquire stays in order on the ARM side (RCsc) while x86 lets the load pass the store, so STLR and STLUR are exchanges
// (an implicitly locked xchg).  The read-modify-write instructions are locked ones; those without an x86 equivalent returning the old value
// (LDSET, LDCLR, LDEOR) are a compare-and-exchange loop.

static void xchg_mr(JC *j, int base, int32_t disp, int reg, int size) {   // xchg [base+disp], reg (the lock is implied)
    if (size == 2) e1(j, 0x66);
    rex(j, size == 8, reg, base, size == 1);
    e1(j, size == 1 ? 0x86 : 0x87); modrm_mem(j, reg, base, disp);
}
static void locked_0f(JC *j, int opc8, int base, int32_t disp, int reg, int size) {   // lock xadd / lock cmpxchg [base+disp], reg: opc8 is the byte form (C0 / B0), the other sizes are opc8 + 1
    e1(j, 0xF0);
    if (size == 2) e1(j, 0x66);
    rex(j, size == 8, reg, base, size == 1);
    e1(j, 0x0F); e1(j, size == 1 ? opc8 : opc8 + 1); modrm_mem(j, reg, base, disp);
}

// LDAR / LDLAR / STLR / STLLR, LDAPR, LDAPUR / STLUR (and their sign-extending forms)
static JI emit_ordered(JC *j, uint32_t i) {
    int size = B(i, 31, 30), rt = B(i, 4, 0), rn = B(i, 9, 5);
    if ((i & 0x3fbf7c00) == 0x089f7c00) {   // exclusive-pair space with o2 = 1, o1 = 0, Rs = Rt2 = 31: the ordered loads and stores
        ld_xsp(j, RCX, rn, 1);
        if (B(i, 22, 22)) { emit_load_sized(j, size, 1, RCX, 0); st_x(j, rt, RAX); }
        else { ld_x(j, RAX, rt, 1); xchg_mr(j, RCX, 0, RAX, 1 << size); }
        return JI_OK;
    }
    if ((i & 0x3ffffc00) == 0x38bfc000) {   // LDAPR
        ld_xsp(j, RCX, rn, 1);
        emit_load_sized(j, size, 1, RCX, 0); st_x(j, rt, RAX);
        return JI_OK;
    }
    if ((i & 0x3f200c00) == 0x19000000) {   // LDAPUR / STLUR family: an unscaled 9-bit offset
        int opc = B(i, 23, 22);
        if (opc >= 2 && size == 3) return JI_NO;
        if (opc == 3 && size == 2) return JI_NO;
        int32_t imm = (int32_t)sext(B(i, 20, 12), 9);
        ld_xsp(j, RCX, rn, 1);
        if (opc == 0) { ld_x(j, RAX, rt, 1); xchg_mr(j, RCX, imm, RAX, 1 << size); }
        else { emit_load_sized(j, size, opc, RCX, imm); st_x(j, rt, RAX); }
        return JI_OK;
    }
    return JI_NO;
}

// LDADD / LDCLR / LDEOR / LDSET / SWP (all orderings and sizes) and CAS
static JI emit_atomic(JC *j, uint32_t i) {
    int size = B(i, 31, 30), rt = B(i, 4, 0), rn = B(i, 9, 5), rs = B(i, 20, 16), bytes = 1 << size;
    if ((i & 0x3fa07c00) == 0x08a07c00) {   // CAS: Rs holds the value to compare and receives the old one, Rt the value to store
        ld_xsp(j, RCX, rn, 1);
        ld_x(j, RAX, rs, 1); ld_x(j, RDX, rt, 1);
        locked_0f(j, 0xB0, RCX, 0, RDX, bytes);                      // lock cmpxchg [rcx], rdx: rax = the old value either way
        if (size < 3) movzx_rr(j, RAX, RAX, bytes);
        st_x(j, rs, RAX);
        return JI_OK;
    }
    if ((i & 0x3f200c00) != 0x38200000) return JI_NO;
    int o3 = B(i, 15, 15), opc = B(i, 14, 12);
    if (o3 ? opc != 0 : opc > 3) return JI_NO;
    ld_xsp(j, RCX, rn, 1);
    if (o3 || opc == 0) {
        ld_x(j, RAX, rs, 1);
        if (o3) xchg_mr(j, RCX, 0, RAX, bytes); else locked_0f(j, 0xC0, RCX, 0, RAX, bytes);   // swp: xchg; ldadd: lock xadd; rax = the old value
        if (size < 3) movzx_rr(j, RAX, RAX, bytes);
    } else {
        ld_x(j, RSI, rs, 1);
        emit_load_sized(j, size, 1, RCX, 0);                         // rax = the old value, zero-extended
        uint8_t *loop = j->p;
        mov_rr(j, RDX, RAX, 1);
        if (opc == 3) alu_rr(j, 0x09, RDX, RSI, 1);                  // LDSET: or
        else if (opc == 2) alu_rr(j, 0x31, RDX, RSI, 1);             // LDEOR: xor
        else { mov_rr(j, R8, RSI, 1); unary(j, 2, R8, 1); alu_rr(j, 0x21, RDX, R8, 1); }   // LDCLR: and not
        locked_0f(j, 0xB0, RCX, 0, RDX, bytes);                      // lock cmpxchg [rcx], rdx; on failure rax = the current value
        jcc_to(j, CC_NE, loop);
    }
    st_x(j, rt, RAX);
    return JI_OK;
}

// CLZ and RBIT (the REV family is in emit_dp1)
static JI emit_bitcount(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op = B(i, 15, 10), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (op == 4) {   // CLZ without lzcnt (which an older processor would run as bsr): bsr, and 127 or 63 for a zero input, xor 63 / 31
        ld_x(j, RAX, rn, sf);
        mov_ri_nf(j, RCX, sf ? 127 : 63);
        op_rr(j, sf, 1, 0xBD, RAX, RAX);                             // bsr rax, rax
        cmov(j, CC_E, RAX, RCX, sf);
        alu_ri(j, 6, RAX, sf ? 63 : 31, sf);
        st_x(j, rd, RAX);
        return JI_OK;
    }
    if (op == 0) {   // RBIT: reverse the bytes, then the bits inside each byte by swapping nibbles, pairs and neighbours
        ld_x(j, RAX, rn, sf);
        bswap(j, RAX, sf);
        static const uint64_t mask[3] = {0x0f0f0f0f0f0f0f0full, 0x3333333333333333ull, 0x5555555555555555ull};
        for (int k = 0; k < 3; k++) {
            int sh = 4 >> k;
            mov_ri_nf(j, RCX, mask[k]);
            mov_rr(j, RDX, RAX, sf);
            shift_ri(j, 5, RDX, sh, sf);                             // shr
            alu_rr(j, 0x21, RDX, RCX, sf); alu_rr(j, 0x21, RAX, RCX, sf);
            shift_ri(j, 4, RAX, sh, sf);                             // shl
            alu_rr(j, 0x09, RAX, RDX, sf);
        }
        st_x(j, rd, RAX);
        return JI_OK;
    }
    return JI_NO;
}
