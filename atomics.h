// Exclusive and LSE atomics for the rebar interpreter. Guest threads run concurrently on
// host threads, so these are real host atomics: LSE operations map onto compare-and-swap
// loops, and LDXR/STXR pairs are emulated as "store succeeds iff the memory still holds
// the value the LDXR saw" (a per-thread monitor; ABA is tolerated, as in most emulators).

static __thread uint64_t g_excl_addr = ~0ull, g_excl_val;

static uint64_t a_load(uint64_t a, int size) {
    switch (size) {
    case 0: return __atomic_load_n((uint8_t *)a, __ATOMIC_SEQ_CST);
    case 1: return __atomic_load_n((uint16_t *)a, __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST);
    }
}
static void a_store(uint64_t a, int size, uint64_t v) {
    switch (size) {
    case 0: __atomic_store_n((uint8_t *)a, (uint8_t)v, __ATOMIC_SEQ_CST); break;
    case 1: __atomic_store_n((uint16_t *)a, (uint16_t)v, __ATOMIC_SEQ_CST); break;
    case 2: __atomic_store_n((uint32_t *)a, (uint32_t)v, __ATOMIC_SEQ_CST); break;
    default: __atomic_store_n((uint64_t *)a, v, __ATOMIC_SEQ_CST); break;
    }
}
// Compare-and-swap; on failure *expected receives the current value.
static bool a_cas(uint64_t a, int size, uint64_t *expected, uint64_t desired) {
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
static uint64_t a_rmw(uint64_t a, int size, int opc, uint64_t v) {
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

static bool exec_atomic(CPU *c, uint32_t i) {
    int rt = B(i, 4, 0), rn = B(i, 9, 5), rs = B(i, 20, 16), size = B(i, 31, 30);

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
