// Exclusive and LSE atomics for the rebar interpreter. Guest threads run concurrently on
// host threads, so these are real host atomics: LSE operations map onto compare-and-swap
// loops, and LDXR/STXR pairs are emulated as "store succeeds iff the memory still holds
// the value the LDXR saw" (a per-thread monitor; ABA is tolerated, as in most emulators).

static __thread uint64_t g_excl_addr = ~0ull, g_excl_val;

static uint64_t a_load(uint64_t a, int size) { a = TBI(a);
    switch (size) {
    case 0: return __atomic_load_n((uint8_t *)a, __ATOMIC_SEQ_CST);
    case 1: return __atomic_load_n((uint16_t *)a, __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST);
    }
}
static void a_store(uint64_t a, int size, uint64_t v) { a = TBI(a);
    switch (size) {
    case 0: __atomic_store_n((uint8_t *)a, (uint8_t)v, __ATOMIC_SEQ_CST); break;
    case 1: __atomic_store_n((uint16_t *)a, (uint16_t)v, __ATOMIC_SEQ_CST); break;
    case 2: __atomic_store_n((uint32_t *)a, (uint32_t)v, __ATOMIC_SEQ_CST); break;
    default: __atomic_store_n((uint64_t *)a, v, __ATOMIC_SEQ_CST); break;
    }
}
// Compare-and-swap; on failure *expected receives the current value.
static bool a_cas(uint64_t a, int size, uint64_t *expected, uint64_t desired) { a = TBI(a);
#define CAS(T) { T e = (T)*expected; bool ok = __atomic_compare_exchange_n((T *)a, &e, (T)desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); *expected = e; return ok; }
    switch (size) {
    case 0: CAS(uint8_t)
    case 1: CAS(uint16_t)
    case 2: CAS(uint32_t)
    default: CAS(uint64_t)
    }
#undef CAS
}
// opc as in LDADD..LDUMIN (0 add, 1 clr, 2 eor, 3 set, 4 smax, 5 smin, 6 umax, 7 umin), 8 = swap.
static uint64_t a_rmw(uint64_t a, int size, int opc, uint64_t v) { a = TBI(a);
    int es = 1 << size;
    v = zx(v, es);
    uint64_t old = a_load(a, size), nv;
    do {
        switch (opc) {
        case 0: nv = old + v; break;
        case 1: nv = old & ~v; break;
        case 2: nv = old ^ v; break;
        case 3: nv = old | v; break;
        case 4: nv = sx(old, es) > sx(v, es) ? old : v; break;
        case 5: nv = sx(old, es) < sx(v, es) ? old : v; break;
        case 6: nv = old > v ? old : v; break;
        case 7: nv = old < v ? old : v; break;
        default: nv = v; break;
        }
    } while (!a_cas(a, size, &old, nv));
    return old;
}

// 128-bit compare-and-swap on a 16-byte aligned address (lock cmpxchg16b); *lo/*hi receive the value that was there
static bool a_cas128(uint64_t a, uint64_t *lo, uint64_t *hi, uint64_t nlo, uint64_t nhi) { a = TBI(a);
    uint64_t rlo = *lo, rhi = *hi; unsigned char ok;
    __asm__ volatile("lock cmpxchg16b %1; setz %0" : "=q"(ok), "+m"(*(volatile __int128 *)a), "+a"(rlo), "+d"(rhi) : "b"(nlo), "c"(nhi) : "memory", "cc");
    *lo = rlo; *hi = rhi; return ok;
}
// the 128-bit read-modify-write of SWPP / LDCLRP / LDSETP and their RCW forms: op 0 swap, 1 clear bits, 3 set bits; returns the old value
static void a_rmw128(uint64_t a, int op, uint64_t vlo, uint64_t vhi, uint64_t *olo, uint64_t *ohi) { a = TBI(a);
    uint64_t lo = *(volatile uint64_t *)a, hi = *(volatile uint64_t *)(a + 8), nlo, nhi;
    do {
        nlo = op == 0 ? vlo : op == 1 ? lo & ~vlo : lo | vlo;
        nhi = op == 0 ? vhi : op == 1 ? hi & ~vhi : hi | vhi;
    } while (!a_cas128(a, &lo, &hi, nlo, nhi));
    *olo = lo; *ohi = hi;
}

static bool exec_atomic(CPU *c, uint32_t i) {
    int rt = B(i, 4, 0), rn = B(i, 9, 5), rs = B(i, 20, 16), size = B(i, 31, 30);

    if (B(i, 29, 24) == 0x08 && !B(i, 31, 31) && !B(i, 23, 23) && B(i, 21, 21) && B(i, 14, 10) == 31) {   // CASP: compare the pair Rs, Rs+1 with memory, store the pair Rt, Rt+1; Rs, Rs+1 receive the old value
        if ((rs & 1) || (rt & 1)) return false;
        uint64_t addr = rd_sp(c, rn);
        if (size == 0) {
            uint64_t exp = zx(rd(c, rs), 4) | (zx(rd(c, rs + 1), 4) << 32);
            a_cas(addr, 3, &exp, zx(rd(c, rt), 4) | (zx(rd(c, rt + 1), 4) << 32));
            wr(c, rs, exp & 0xffffffffull); wr(c, rs + 1, exp >> 32);
        } else {
            uint64_t lo = rd(c, rs), hi = rd(c, rs + 1);
            a_cas128(addr, &lo, &hi, rd(c, rt), rd(c, rt + 1));
            wr(c, rs, lo); wr(c, rs + 1, hi);
        }
        return true;
    }
    // FEAT_LSE128 SWPP / LDCLRP / LDSETP, FEAT_THE RCWCAS / RCWCASP / RCWCLR[P] / RCWSET[P] / RCWSWP[P] (and their RCWS software forms): the read-check-write permission check is not modelled,
    // so they are the plain atomic operations on 64 bits (RCW...) or on the 128-bit pair (...P); the flags report that the check passed
    if (B(i, 29, 24) == 0x19 && B(i, 21, 21) && !B(i, 31, 31)) {
        int S = B(i, 30, 30), rt2 = B(i, 20, 16), form = B(i, 15, 10);
        uint64_t addr = rd_sp(c, rn);
        if (form == 2 || form == 3) {   // RCWCAS (64-bit) / RCWCASP (128-bit): Rs, Rt as CAS / CASP
            if (form == 2) {
                uint64_t e = rd(c, rs); a_cas(addr, 3, &e, rd(c, rt)); wr(c, rs, e);
            } else {
                if ((rs & 1) || (rt & 1)) return false;
                uint64_t lo = rd(c, rs), hi = rd(c, rs + 1);
                a_cas128(addr, &lo, &hi, rd(c, rt), rd(c, rt + 1)); wr(c, rs, lo); wr(c, rs + 1, hi);
            }
            c->n = c->z = c->c = c->v = 0; return true;
        }
        if (B(i, 11, 10) == 0) {
            int f = B(i, 15, 12), op = -1; bool rcw = false;
            if (f == 8) op = 0; else if (f == 1) op = 1; else if (f == 3) op = 3;
            else if (f == 10) { op = 0; rcw = true; } else if (f == 9) { op = 1; rcw = true; } else if (f == 11) { op = 3; rcw = true; }
            if (op >= 0 && (rcw || !S)) {
                uint64_t olo, ohi;
                a_rmw128(addr, op, rd(c, rt), rd(c, rt2), &olo, &ohi);
                wr(c, rt, olo); wr(c, rt2, ohi);
                if (rcw) c->n = c->z = c->c = c->v = 0;
                return true;
            }
        }
        return false;
    }
    // RCWCLR / RCWSET / RCWSWP (64-bit, plain and software forms): the LSE encoding space with o3 = 1 and opc 1 (clear), 2 (swap), 3 (set)
    if (B(i, 29, 24) == 0x38 && B(i, 31, 31) == 0 && B(i, 21, 21) && B(i, 11, 10) == 0 && B(i, 15, 15) && B(i, 14, 12) >= 1 && B(i, 14, 12) <= 3) {
        int opc = B(i, 14, 12);
        uint64_t old = a_rmw(rd_sp(c, rn), 3, opc == 2 ? 8 : opc == 1 ? 1 : 3, rd(c, rs));
        wr(c, rt, old);
        c->n = c->z = c->c = c->v = 0;
        return true;
    }

    // FEAT_MOPS: CPYFP/M/E (forward copy), CPYP/M/E (copy that may go backwards), SETP/M/E and SETGP/M/E (set).  The prologue instruction does the whole job (a legal implementation: the main and epilogue
    // instructions then find nothing left); afterwards the pointers are past the block and the size is 0.  The unprivileged / non-temporal variants (the option letters) behave alike.
    if ((B(i, 29, 24) == 0x19 || B(i, 29, 24) == 0x1d) && B(i, 31, 30) == 0 && !B(i, 21, 21) && B(i, 11, 10) == 1) {
        int kind = B(i, 23, 22), rd_ = rt, rn_ = rn, rs_ = rs;
        if (kind == 3) {   // SET*: Xd = destination, Xn = size, Xs = the byte to store
            if (B(i, 15, 14) == 0) {   // prologue (op2 0..3): the whole memset
                uint64_t d = rd(c, rd_), n = rd(c, rn_);
                if (n) memset((void *)d, (int)(rd(c, rs_) & 0xff), n);
                wr(c, rd_, d + n); wr(c, rn_, 0);
                c->n = c->z = c->c = c->v = 0;
            }
            return true;
        }
        if (kind == 0) {   // CPY*P / CPYF*P: Xd = destination, Xs = source, Xn = size
            uint64_t d = rd(c, rd_), sr = rd(c, rs_), n = rd(c, rn_);
            if (n) { if (B(i, 29, 24) == 0x19 && d > sr && d < sr + n) { for (uint64_t k = 0; k < n; k++) ((volatile uint8_t *)d)[k] = ((volatile uint8_t *)sr)[k]; } else memmove((void *)d, (void *)sr, n); }   // (a forward copy over an overlapping higher destination repeats the pattern: byte by byte)
            wr(c, rd_, d + n); wr(c, rs_, sr + n); wr(c, rn_, 0);
            c->n = c->z = c->c = c->v = 0;
        }
        return true;   // main and epilogue: nothing is left to do
    }
    // MTE (allocation tags are not kept: every tag is 0, tag-check never fails): STG / STZG / ST2G / STZ2G, LDG, STGM / LDGM
    if ((i & 0xff200000) == 0xd9200000) {
        int opc = B(i, 23, 22), op2 = B(i, 11, 10);
        uint64_t base = rd_sp(c, rn);
        if (op2) {
            uint64_t off = (uint64_t)sext(B(i, 20, 12), 9) * 16, addr = (op2 == 1 ? base : base + off) & ~(0xfull << 56);
            if (opc == 1) memset((void *)addr, 0, 16); else if (opc == 3) memset((void *)addr, 0, 32);
            if (op2 == 1 || op2 == 3) wr_sp(c, rn, base + off);
            return true;
        }
        if (opc == 1) { wr(c, rt, rd(c, rt) & ~(0xfull << 56)); return true; }   // LDG
        if (opc == 3) { wr(c, rt, 0); return true; }                           // LDGM
        return true;                                                            // STGM / STZGM
    }

    if (B(i, 29, 24) == 0x08) {  // load/store exclusive, load-acquire/store-release, CAS
        int o2 = B(i, 23, 23), L = B(i, 22, 22), o1 = B(i, 21, 21), rt2 = B(i, 14, 10);
        uint64_t addr = rd_sp(c, rn);
        if (o2 && o1) {
            if (rt2 != 31) return false;  // CASP
            uint64_t expected = zx(rd(c, rs), 1 << size);
            a_cas(addr, size, &expected, rd(c, rt));
            wr(c, rs, expected);  // the old value, whether or not the swap happened
            return true;
        }
        if (!o2 && !o1) {  // LDXR/LDAXR, STXR/STLXR
            if (L) { g_excl_val = a_load(addr, size); g_excl_addr = addr; wr(c, rt, g_excl_val); }
            else {
                uint64_t expected = g_excl_val;
                bool ok = g_excl_addr == addr && a_cas(addr, size, &expected, rd(c, rt));
                wr(c, rs, ok ? 0 : 1);
                g_excl_addr = ~0ull;
            }
            return true;
        }
        if (o2 && !o1) {  // LDAR/STLR (and LDLAR/STLLR)
            if (L) wr(c, rt, a_load(addr, size)); else a_store(addr, size, rd(c, rt));
            return true;
        }
        int esz = size == 3 ? 3 : 2;  // LDXP/STXP: pairs are not atomic here
        if (L) {
            wr(c, rt, mem_ld(addr, esz)); wr(c, rt2, mem_ld(addr + (1 << esz), esz));
            g_excl_addr = addr;
        } else if (g_excl_addr == addr) {
            mem_st(addr, esz, rd(c, rt)); mem_st(addr + (1 << esz), esz, rd(c, rt2));
            wr(c, rs, 0); g_excl_addr = ~0ull;
        } else { wr(c, rs, 1); g_excl_addr = ~0ull; }
        return true;
    }

    // LDAPUR / STLUR of the SIMD&FP registers (FEAT_LRCPC2): size:opc = 00:00 B store, 00:01 B load, 00:10 Q store, 00:11 Q load, 01/10/11 H/S/D store (opc 0) or load (opc 1)
    if (B(i, 29, 24) == 0x1d && B(i, 11, 10) == 2 && !B(i, 21, 21)) {
        int opc = B(i, 23, 22), q = size == 0 && (opc & 2);
        if (!q && (opc & 2)) return false;
        int sz = q ? 4 : size;
        uint64_t addr = rd_sp(c, rn) + (uint64_t)sext(B(i, 20, 12), 9);
        if (opc & 1) {
            VReg r; memset(&r, 0, 16);
            if (q) { r.d[0] = a_load(addr, 3); r.d[1] = a_load(addr + 8, 3); } else r.d[0] = a_load(addr, sz);
            c->vr[rt] = r;
        } else if (q) { a_store(addr, 3, c->vr[rt].d[0]); a_store(addr + 8, 3, c->vr[rt].d[1]); }
        else a_store(addr, sz, c->vr[rt].d[0]);
        return true;
    }
    // FEAT_LRCPC3: STLR with pre-decrement and LDAPR with post-increment (the offset is the access size), LDIAPP / STILP (pairs, with or without writeback)
    if (B(i, 29, 24) == 0x19 && B(i, 11, 10) == 2 && !B(i, 21, 21) && size >= 2) {
        int opc = B(i, 23, 22), es = 1 << size;
        if (B(i, 20, 12) == 0 && opc >= 2) {   // STLR (opc 2) / LDAPR (opc 3)
            uint64_t base = rd_sp(c, rn);
            if (opc == 2) { base -= (uint64_t)es; a_store(base, size, rd(c, rt)); wr_sp(c, rn, base); }
            else { wr(c, rt, a_load(base, size)); wr_sp(c, rn, base + (uint64_t)es); }
            return true;
        }
        if (opc <= 1) {   // STILP (opc 0) / LDIAPP (opc 1): bits 15:12 = 0 writeback (STILP pre-index, LDIAPP post-index), 1 plain
            int rt2 = B(i, 20, 16), form = B(i, 15, 12);
            if (form > 1) return false;
            uint64_t base = rd_sp(c, rn), addr = base;
            if (form == 0 && opc == 0) addr = base - 2 * (uint64_t)es;
            if (opc == 0) { a_store(addr, size, rd(c, rt)); a_store(addr + (uint64_t)es, size, rd(c, rt2)); }
            else { uint64_t a = a_load(addr, size), b = a_load(addr + (uint64_t)es, size); wr(c, rt, a); wr(c, rt2, b); }
            if (form == 0) wr_sp(c, rn, opc == 0 ? addr : base + 2 * (uint64_t)es);
            return true;
        }
    }

    // LDAPUR / STLUR family (RCpc, unscaled immediate)
    if (B(i, 29, 24) == 0x19 && B(i, 11, 10) == 0 && !B(i, 21, 21)) {
        int opc = B(i, 23, 22);
        uint64_t addr = rd_sp(c, rn) + (uint64_t)sext(B(i, 20, 12), 9);
        if (opc == 0) a_store(addr, size, rd(c, rt));
        else if (opc == 1) wr(c, rt, a_load(addr, size));
        else {
            uint64_t v = (uint64_t)sext(a_load(addr, size), 8 << size);
            wr(c, rt, opc == 3 ? v & 0xffffffffull : v);
        }
        return true;
    }

    // LSE: LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN, LDUMAX, LDUMIN, SWP
    if (B(i, 29, 27) == 7 && !B(i, 26, 26) && B(i, 25, 24) == 0 && B(i, 21, 21) && B(i, 11, 10) == 0) {
        int opc = B(i, 14, 12), o3 = B(i, 15, 15);
        if (o3 && opc == 4 && rs == 31) {  // LDAPR (RCpc load-acquire)
            wr(c, rt, a_load(rd_sp(c, rn), size));
            return true;
        }
        if (o3 && opc != 0) return false;
        uint64_t old = a_rmw(rd_sp(c, rn), size, o3 ? 8 : opc, rd(c, rs));
        wr(c, rt, old);
        return true;
    }
    return false;
}
