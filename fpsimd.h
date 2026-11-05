// Scalar floating point and AdvSIMD (NEON) for the rebar interpreter.
// Included from rebar.c after the CPU type and the integer helpers.
//
// Floating point is evaluated on the host in double precision and rounded to
// the destination width. For + - * / sqrt this is exactly the correctly
// rounded single-precision result (double rounding is innocuous when the
// intermediate has >= 2p+2 bits); fused ops use fmaf/fma. FPCR rounding modes,
// flush-to-zero, exception flags and the estimate instructions' exact
// table-driven results are not modelled.

#include <math.h>

// ----------------------------------------------------------- element helpers

static inline uint64_t vget(const VReg *r, int es, int k) {
    switch (es) {
    case 1: return r->b[k];
    case 2: return r->h[k];
    case 4: return r->s[k];
    default: return r->d[k];
    }
}
static inline void vset(VReg *r, int es, int k, uint64_t v) {
    switch (es) {
    case 1: r->b[k] = (uint8_t)v; break;
    case 2: r->h[k] = (uint16_t)v; break;
    case 4: r->s[k] = (uint32_t)v; break;
    default: r->d[k] = v; break;
    }
}
static inline uint64_t zx(uint64_t v, int es) { return es == 8 ? v : v & ((1ull << (es * 8)) - 1); }
static inline int64_t sx(uint64_t v, int es) { return sext(v, es * 8); }
static inline int64_t ashr(int64_t x, int s) { return s >= 64 ? (x < 0 ? -1 : 0) : x >> s; }
static inline uint64_t lshr(uint64_t x, int s) { return s >= 64 ? 0 : x >> s; }
static inline uint64_t lshl(uint64_t x, int s) { return s >= 64 ? 0 : x << s; }

static inline double hraw2f(uint16_t h) { _Float16 f; memcpy(&f, &h, 2); return (double)f; }
static inline uint16_t f2hraw(double v) { if (isnan(v)) return 0x7e00; _Float16 f = (_Float16)v; uint16_t h; memcpy(&h, &f, 2); return h; }
static inline double raw2f(uint64_t r, int es) {
    if (es == 2) return hraw2f((uint16_t)r);
    if (es == 4) { uint32_t u = (uint32_t)r; float f; memcpy(&f, &u, 4); return f; }
    double d; memcpy(&d, &r, 8); return d;
}
static inline uint64_t f2raw(double v, int es) {
    // ARM yields the positive default NaN for invalid operations; x86 yields a negative one.
    if (es == 2) return f2hraw(v);
    if (isnan(v)) return es == 4 ? 0x7fc00000ull : 0x7ff8000000000000ull;
    if (es == 4) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); return u; }
    uint64_t r; memcpy(&r, &v, 8); return r;
}

static uint64_t sat_s(__int128 v, int es) {
    int bits = es * 8;
    __int128 mx = ((__int128)1 << (bits - 1)) - 1, mn = -mx - 1;
    if (v > mx) v = mx; else if (v < mn) v = mn;
    return (uint64_t)v;
}
static uint64_t sat_u(__int128 v, int es) {
    int bits = es * 8;
    __int128 mx = ((__int128)1 << bits) - 1;
    if (v < 0) v = 0; else if (v > mx) v = mx;
    return (uint64_t)v;
}

// ------------------------------------------------------------- FP primitives

static double fp_fma(int es, double a, double b, double acc) {
    return es == 4 ? (double)fmaf((float)a, (float)b, (float)acc) : fma(a, b, acc);
}

static double fp_minmax(double a, double b, bool max, bool num) {
    if (isnan(a) || isnan(b)) {
        if (num && !(isnan(a) && isnan(b))) return isnan(a) ? b : a;
        return NAN;
    }
    if (a == b) {
        if (a == 0) return max ? (signbit(a) ? b : a) : (signbit(a) ? a : b);
        return a;
    }
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

// mode: 0 nearest-even, 1 +inf, 2 -inf, 3 zero, 4 nearest-away
static double fp_round(double v, int mode) {
    switch (mode) {
    case 0: return rint(v);
    case 1: return ceil(v);
    case 2: return floor(v);
    case 3: return trunc(v);
    default: return round(v);
    }
}

// FRINT32Z / FRINT32X / FRINT64Z / FRINT64X (FEAT_FRINTTS): round to an integral value that fits a signed 32 / 64-bit integer; a value that does not (or a NaN, an infinity) gives the most negative one.
// Z rounds toward zero, X with the rounding mode in force (to nearest even here).
static double frint_ts(double v, int bits, bool toward_zero) {
    double lim = ldexp(1.0, bits - 1);
    if (isnan(v) || isinf(v)) return -lim;
    double r = toward_zero ? trunc(v) : rint(v);
    return (r >= lim || r < -lim) ? -lim : r;
}

static uint64_t fcvt_int(double v, int mode, bool uns, int bits) {
    if (isnan(v)) return 0;
    double r = fp_round(v, mode);
    if (!uns) {
        double lim = ldexp(1.0, bits - 1);
        if (r >= lim) return bits == 64 ? 0x7fffffffffffffffull : 0x7fffffffull;
        if (r < -lim) return bits == 64 ? 0x8000000000000000ull : 0x80000000ull;
        return bits == 64 ? (uint64_t)(int64_t)r : (uint32_t)(int32_t)r;
    }
    if (r < 0) return 0;
    if (r >= ldexp(1.0, bits)) return bits == 64 ? ~0ull : 0xffffffffull;
    return (uint64_t)r;
}

static void fcmp_flags(CPU *c, double a, double b) {
    if (isnan(a) || isnan(b)) { c->n = 0; c->z = 0; c->c = 1; c->v = 1; }
    else if (a == b)          { c->n = 0; c->z = 1; c->c = 1; c->v = 0; }
    else if (a < b)           { c->n = 1; c->z = 0; c->c = 0; c->v = 0; }
    else                      { c->n = 0; c->z = 0; c->c = 1; c->v = 0; }
}

static uint64_t vfp_expand_imm(int imm8, int es) {
    uint64_t s = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, rest = imm8 & 0x3f;
    if (es == 4) return (s << 31) | ((b ^ 1ull) << 30) | ((b ? 0x1full : 0) << 25) | (rest << 19);
    return (s << 63) | ((b ^ 1ull) << 62) | ((b ? 0xffull : 0) << 54) | (rest << 48);
}

// ------------------------------------------------------- scalar FP registers

static double fpr(CPU *c, int r, int type) {
    switch (type) {
    case 0: return raw2f(c->vr[r].s[0], 4);
    case 1: return raw2f(c->vr[r].d[0], 8);
    default: return hraw2f(c->vr[r].h[0]);
    }
}
static void fpw(CPU *c, int r, int type, double v) {
    VReg t; memset(&t, 0, 16);
    switch (type) {
    case 0: t.s[0] = (uint32_t)f2raw(v, 4); break;
    case 1: t.d[0] = f2raw(v, 8); break;
    default: t.h[0] = f2hraw(v); break;
    }
    c->vr[r] = t;
}

// ------------------------------------------------------------ memory access

static void vmem(CPU *c, int rt, int size, int opc, uint64_t addr) {
    int bytes = (opc & 2) ? 16 : 1 << size;
    if (opc & 1) { VReg t; memset(&t, 0, 16); memcpy(&t, (void *)addr, bytes); c->vr[rt] = t; }
    else memcpy((void *)addr, &c->vr[rt], bytes);
}

// LD1..LD4 / ST1..ST4, multiple structures.
static void simd_ldst_multi(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), L = B(i, 22, 22), post = B(i, 23, 23), rm = B(i, 20, 16);
    int opcode = B(i, 15, 12), size = B(i, 11, 10), rn = B(i, 9, 5), rt = B(i, 4, 0);
    int rpt, selem;
    switch (opcode) {
    case 0:  rpt = 1; selem = 4; break;
    case 2:  rpt = 4; selem = 1; break;
    case 4:  rpt = 1; selem = 3; break;
    case 6:  rpt = 3; selem = 1; break;
    case 7:  rpt = 1; selem = 1; break;
    case 8:  rpt = 1; selem = 2; break;
    case 10: rpt = 2; selem = 1; break;
    default: die(c, "bad ldst multi", i);
    }
    int es = 1 << size, ds = Q ? 16 : 8, lanes = ds / es;
    uint64_t base = rd_sp(c, rn), addr = base;
    if (L) for (int r = 0; r < rpt * selem; r++) memset(&c->vr[(rt + r) & 31], 0, 16);
    for (int r = 0; r < rpt; r++)
        for (int e = 0; e < lanes; e++)
            for (int s = 0; s < selem; s++) {
                VReg *v = &c->vr[(rt + r + s) & 31];
                if (L) memcpy((uint8_t *)v + e * es, (void *)addr, es);
                else memcpy((void *)addr, (uint8_t *)v + e * es, es);
                addr += es;
            }
    if (post) wr_sp(c, rn, base + (rm == 31 ? (uint64_t)(rpt * selem * ds) : rd(c, rm)));
}

// LD1..LD4 single structure (lane) and LD1R..LD4R.
static void simd_ldst_single(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), L = B(i, 22, 22), R = B(i, 21, 21), post = B(i, 23, 23), rm = B(i, 20, 16);
    int opcode = B(i, 15, 13), S = B(i, 12, 12), size = B(i, 11, 10), rn = B(i, 9, 5), rt = B(i, 4, 0);
    int selem = (((opcode & 1) << 1) | R) + 1;
    int scale = opcode >> 1, es, idx;
    switch (scale) {
    case 0: es = 1; idx = (Q << 3) | (S << 2) | size; break;
    case 1: es = 2; idx = (Q << 2) | (S << 1) | (size >> 1); break;
    case 2:
        if (!(size & 1)) { es = 4; idx = (Q << 1) | S; } else { es = 8; idx = Q; }
        break;
    default: es = 1 << size; idx = 0; break;
    }
    uint64_t base = rd_sp(c, rn), addr = base;
    for (int s = 0; s < selem; s++) {
        VReg *v = &c->vr[(rt + s) & 31];
        if (scale == 3) {  // replicate
            if (!L) die(c, "bad ldst single", i);
            int lanes = (Q ? 16 : 8) / es;
            VReg t; memset(&t, 0, 16);
            uint64_t val = 0; memcpy(&val, (void *)addr, es);
            for (int e = 0; e < lanes; e++) vset(&t, es, e, val);
            *v = t;
        } else if (L) memcpy((uint8_t *)v + idx * es, (void *)addr, es);
        else memcpy((void *)addr, (uint8_t *)v + idx * es, es);
        addr += es;
    }
    if (post) wr_sp(c, rn, base + (rm == 31 ? (uint64_t)(selem * es) : rd(c, rm)));
}

// ------------------------------------------------------------- scalar FP ops

static void fp_scalar(CPU *c, uint32_t i) {
    int sf = B(i, 31, 31), type = B(i, 23, 22);
    int rdn = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);
    int es = type == 0 ? 4 : 8;

    if (B(i, 28, 24) == 0x1f) {  // fused multiply-add family
        int o1 = B(i, 21, 21), o0 = B(i, 15, 15), ra = B(i, 14, 10);
        double n = fpr(c, rn, type), m = fpr(c, rm, type), a = fpr(c, ra, type), r;
        if (!o1) r = o0 ? fp_fma(es, -n, m, a) : fp_fma(es, n, m, a);
        else     r = o0 ? fp_fma(es, n, m, -a) : fp_fma(es, -n, m, -a);
        fpw(c, rdn, type, r);
        return;
    }
    if (B(i, 21, 21)) {
        if (B(i, 15, 10) == 0) {  // FP <-> integer
            int rmode = B(i, 20, 19), opcode = B(i, 18, 16);
            int bits = sf ? 64 : 32;
            if (!sf && type == 1 && rmode == 3 && opcode == 6) {  // FJCVTZS Wd, Dn: JavaScript ToInt32; Z = exact, no overflow
                double d; memcpy(&d, &c->vr[rn].d[0], 8);
                uint32_t res = 0;
                bool exact = false;
                if (isfinite(d)) {
                    double t = trunc(d);
                    exact = t == d && !(d == 0 && signbit(d));
                    double m = fmod(t, 4294967296.0);              // modulo 2^32, as ToInt32 does
                    if (m < 0) m += 4294967296.0;
                    res = (uint32_t)(uint64_t)m;
                    if (t < -2147483648.0 || t > 2147483647.0) exact = false;
                }
                wr(c, rdn, res);
                c->n = 0; c->z = exact; c->c = 0; c->v = 0;
                return;
            }
            if (opcode == 6 || opcode == 7) {  // FMOV general <-> vector
                if (type == 2) {  // top half of a 128-bit register
                    if (opcode == 6) wr(c, rdn, c->vr[rn].d[1]); else c->vr[rdn].d[1] = rd(c, rn);
                } else if (opcode == 6) {
                    uint64_t v = type == 1 ? c->vr[rn].d[0] : type == 0 ? c->vr[rn].s[0] : c->vr[rn].h[0];
                    wr(c, rdn, v);
                } else {
                    VReg t; memset(&t, 0, 16);
                    uint64_t v = rd(c, rn);
                    if (type == 1) t.d[0] = v; else if (type == 0) t.s[0] = (uint32_t)v; else t.h[0] = (uint16_t)v;
                    c->vr[rdn] = t;
                }
                return;
            }
            if (opcode == 2 || opcode == 3) {  // SCVTF / UCVTF
                uint64_t x = rd(c, rn);
                bool u = opcode == 3;
                if (type == 0) {
                    float f = sf ? (u ? (float)(uint64_t)x : (float)(int64_t)x)
                                 : (u ? (float)(uint32_t)x : (float)(int32_t)x);
                    fpw(c, rdn, 0, (double)f);
                } else {
                    double d = sf ? (u ? (double)(uint64_t)x : (double)(int64_t)x)
                                  : (u ? (double)(uint32_t)x : (double)(int32_t)x);
                    fpw(c, rdn, type, d);
                }
                return;
            }
            // FCVT{N,P,M,Z,A}{S,U}
            int mode, uns = opcode & 1;
            if (opcode == 4 || opcode == 5) mode = 4;
            else if (opcode <= 1) mode = rmode == 0 ? 0 : rmode == 1 ? 1 : rmode == 2 ? 2 : 3;
            else die(c, "unsupported fp<->int", i);
            wr(c, rdn, fcvt_int(fpr(c, rn, type), mode, uns, bits));
            return;
        }
        if (B(i, 14, 10) == 0x10) {  // one source
            int op = B(i, 20, 15);
            double a = fpr(c, rn, type);
            VReg src = c->vr[rn], t; memset(&t, 0, 16);
            switch (op) {
            case 0:
                memcpy(&t, &src, type == 0 ? 4 : type == 1 ? 8 : 2);
                c->vr[rdn] = t; return;
            case 1: case 2:
                memcpy(&t, &src, type == 0 ? 4 : type == 1 ? 8 : 2);
                if (type == 0) t.s[0] = op == 1 ? t.s[0] & 0x7fffffffu : t.s[0] ^ 0x80000000u;
                else if (type == 1) t.d[0] = op == 1 ? t.d[0] & ~(1ull << 63) : t.d[0] ^ (1ull << 63);
                else t.h[0] = op == 1 ? t.h[0] & 0x7fff : t.h[0] ^ 0x8000;
                c->vr[rdn] = t; return;
            case 3: fpw(c, rdn, type, sqrt(a)); return;
            case 4: fpw(c, rdn, 0, a); return;
            case 5: fpw(c, rdn, 1, a); return;
            case 7: fpw(c, rdn, 3, a); return;
            case 8: fpw(c, rdn, type, fp_round(a, 0)); return;
            case 9: fpw(c, rdn, type, fp_round(a, 1)); return;
            case 10: fpw(c, rdn, type, fp_round(a, 2)); return;
            case 11: fpw(c, rdn, type, fp_round(a, 3)); return;
            case 12: fpw(c, rdn, type, fp_round(a, 4)); return;
            case 14: case 15: fpw(c, rdn, type, fp_round(a, 0)); return;
            case 16: case 17: case 18: case 19:   // FRINT32Z FRINT32X FRINT64Z FRINT64X: single and double only
                if (type > 1) die(c, "unsupported fp 1-source", i);
                fpw(c, rdn, type, frint_ts(a, op >= 18 ? 64 : 32, !(op & 1))); return;
            default: die(c, "unsupported fp 1-source", i);
            }
        }
        if (B(i, 13, 10) == 8) {  // FCMP / FCMPE
            double a = fpr(c, rn, type), b = (B(i, 4, 0) & 8) ? 0.0 : fpr(c, rm, type);
            fcmp_flags(c, a, b);
            return;
        }
        if (B(i, 12, 10) == 4 && B(i, 9, 5) == 0) {  // FMOV immediate
            int imm8 = B(i, 20, 13);
            VReg t; memset(&t, 0, 16);
            if (type == 0) t.s[0] = (uint32_t)vfp_expand_imm(imm8, 4);
            else if (type == 1) t.d[0] = vfp_expand_imm(imm8, 8);
            else {
                uint32_t f = (uint32_t)vfp_expand_imm(imm8, 4);
                float fl; memcpy(&fl, &f, 4);
                t.h[0] = f2hraw(fl);
            }
            c->vr[rdn] = t;
            return;
        }
        switch (B(i, 11, 10)) {
        case 1: {  // FCCMP / FCCMPE
            if (cond_holds(c, B(i, 15, 12))) fcmp_flags(c, fpr(c, rn, type), fpr(c, rm, type));
            else { int f = B(i, 3, 0); c->n = f >> 3 & 1; c->z = f >> 2 & 1; c->c = f >> 1 & 1; c->v = f & 1; }
            return;
        }
        case 2: {  // two source
            double a = fpr(c, rn, type), b = fpr(c, rm, type), r;
            switch (B(i, 15, 12)) {
            case 0: r = a * b; break;
            case 1: r = a / b; break;
            case 2: r = a + b; break;
            case 3: r = a - b; break;
            case 4: r = fp_minmax(a, b, true, false); break;
            case 5: r = fp_minmax(a, b, false, false); break;
            case 6: r = fp_minmax(a, b, true, true); break;
            case 7: r = fp_minmax(a, b, false, true); break;
            case 8: r = -(a * b); break;
            default: die(c, "unsupported fp 2-source", i);
            }
            fpw(c, rdn, type, r);
            return;
        }
        case 3: {  // FCSEL
            VReg src = c->vr[cond_holds(c, B(i, 15, 12)) ? rn : rm], t; memset(&t, 0, 16);
            memcpy(&t, &src, type == 0 ? 4 : type == 1 ? 8 : 2);
            c->vr[rdn] = t;
            return;
        }
        }
        die(c, "unsupported fp", i);
    }
    // fixed-point conversions
    {
        int rmode = B(i, 20, 19), opcode = B(i, 18, 16), fbits = 64 - B(i, 15, 10);
        int bits = sf ? 64 : 32;
        if (rmode == 0 && (opcode == 2 || opcode == 3)) {
            uint64_t x = rd(c, rn);
            bool u = opcode == 3;
            double d = sf ? (u ? (double)(uint64_t)x : (double)(int64_t)x)
                          : (u ? (double)(uint32_t)x : (double)(int32_t)x);
            fpw(c, rdn, type, ldexp(d, -fbits));
            return;
        }
        if (rmode == 3 && (opcode == 0 || opcode == 1)) {
            wr(c, rdn, fcvt_int(ldexp(fpr(c, rn, type), fbits), 3, opcode == 1, bits));
            return;
        }
    }
    die(c, "unsupported fixed-point conversion", i);
}

// ------------------------------------------------------ AdvSIMD: three same

static uint64_t pmul8(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int k = 0; k < 8; k++) if ((b >> k) & 1) r ^= (a & 0xff) << k;
    return r & 0xff;
}

static uint64_t vshift_reg(uint64_t a, int64_t sa, int es, int sh, bool u, bool round) {
    int bits = es * 8;
    if (sh >= 0) return sh >= bits ? 0 : (u ? a : (uint64_t)sa) << sh;
    int rs = -sh;
    __int128 x = u ? (__int128)a : (__int128)sa;
    if (round) {
        if (rs > bits) return 0;
        return (uint64_t)((x + ((__int128)1 << (rs - 1))) >> rs);
    }
    if (rs >= bits) return u ? 0 : (sa < 0 ? ~0ull : 0);
    return (uint64_t)(x >> rs);
}

// h16: the half-precision encoding, whose 3-bit opcode (bits 13..11) maps onto the FP opcodes 24..31 and whose size is just 'a'
static void simd_3same(CPU *c, uint32_t i, bool scalar, bool h16) {
    int Q = scalar ? 0 : B(i, 30, 30), U = B(i, 29, 29), size = h16 ? B(i, 23, 23) * 2 : B(i, 23, 22), op = h16 ? 24 + B(i, 13, 11) : B(i, 15, 11);
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn], m = c->vr[rm], d0 = c->vr[rdn];
    VReg res; memset(&res, 0, 16);
    int ds = Q ? 16 : 8;

    if (op == 3) {  // bitwise
        if (scalar) die(c, "bad scalar logical", i);
        for (int k = 0; k < 2; k++) {
            uint64_t a = n.d[k], b = m.d[k], d = d0.d[k], r;
            switch ((U << 2) | size) {
            case 0: r = a & b; break;
            case 1: r = a & ~b; break;
            case 2: r = a | b; break;
            case 3: r = a | ~b; break;
            case 4: r = a ^ b; break;
            case 5: r = (d & a) | (~d & b); break;       // BSL
            case 6: r = (d & ~b) | (a & b); break;       // BIT
            default: r = (d & b) | (a & ~b); break;      // BIF
            }
            res.d[k] = (k * 8 < ds) ? r : 0;
        }
        c->vr[rdn] = res;
        return;
    }

    bool fp = op >= 24;
    int hi = size >> 1;
    int es = fp ? (h16 ? 2 : (size & 1) ? 8 : 4) : (1 << size);
    int L = scalar ? 1 : ds / es;
    bool pw = U && (fp ? (op == 24 || op == 30 || (op == 26 && !hi)) : false);
    pw = pw || (!fp && (op == 20 || op == 21 || op == 23));
    uint64_t ea[16], eb[16];
    if (pw) {
        uint64_t cat[32];
        for (int k = 0; k < L; k++) { cat[k] = vget(&n, es, k); cat[L + k] = vget(&m, es, k); }
        for (int k = 0; k < L; k++) { ea[k] = cat[2 * k]; eb[k] = cat[2 * k + 1]; }
    } else {
        for (int k = 0; k < L; k++) { ea[k] = vget(&n, es, k); eb[k] = vget(&m, es, k); }
    }

    for (int k = 0; k < L; k++) {
        uint64_t a = ea[k], b = eb[k], r = 0, acc = vget(&d0, es, k);
        int64_t sa = sx(a, es), sb = sx(b, es);
        int bits = es * 8;
        if (!fp) {
            switch (op) {
            case 0: r = U ? (uint64_t)(((unsigned __int128)a + b) >> 1) : (uint64_t)(((__int128)sa + sb) >> 1); break;
            case 1: r = U ? sat_u((__int128)a + b, es) : sat_s((__int128)sa + sb, es); break;
            case 2: r = U ? (uint64_t)(((unsigned __int128)a + b + 1) >> 1) : (uint64_t)(((__int128)sa + sb + 1) >> 1); break;
            case 4: r = U ? (uint64_t)(((__int128)a - b) >> 1) : (uint64_t)(((__int128)sa - sb) >> 1); break;
            case 5: r = U ? sat_u((__int128)a - b, es) : sat_s((__int128)sa - sb, es); break;
            case 6: r = (U ? a > b : sa > sb) ? ~0ull : 0; break;
            case 7: r = (U ? a >= b : sa >= sb) ? ~0ull : 0; break;
            case 8: r = vshift_reg(a, sa, es, (int8_t)b, U, false); break;
            case 10: r = vshift_reg(a, sa, es, (int8_t)b, U, true); break;
            case 12: case 20: r = U ? (a > b ? a : b) : (uint64_t)(sa > sb ? sa : sb); break;
            case 13: case 21: r = U ? (a < b ? a : b) : (uint64_t)(sa < sb ? sa : sb); break;
            case 14: r = U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa); break;
            case 15: r = acc + (U ? (a > b ? a - b : b - a) : (uint64_t)(sa > sb ? sa - sb : sb - sa)); break;
            case 16: r = U ? a - b : a + b; break;
            case 17: r = U ? (a == b ? ~0ull : 0) : ((a & b) ? ~0ull : 0); break;
            case 18: r = U ? acc - a * b : acc + a * b; break;
            case 19: r = U ? pmul8(a, b) : a * b; break;
            case 22: {
                __int128 p = (__int128)sa * sb * 2;
                if (U) p += (__int128)1 << (bits - 1);
                r = sat_s(p >> bits, es);
                break;
            }
            case 23: r = a + b; break;
            default: die(c, "unsupported simd 3same", i);
            }
        } else {
            double fa = raw2f(a, es), fb = raw2f(b, es), fr = 0;
            bool isbits = false;
            uint64_t br = 0;
            if (!U) {
                switch (op) {
                case 24: fr = fp_minmax(fa, fb, !hi, true); break;
                case 25: fr = fp_fma(es, hi ? -fa : fa, fb, raw2f(acc, es)); break;
                case 26: fr = hi ? fa - fb : fa + fb; break;
                case 27: fr = fa * fb; break;
                case 28: isbits = true; br = fa == fb ? ~0ull : 0; break;
                case 30: fr = fp_minmax(fa, fb, !hi, false); break;
                case 31: fr = hi ? fp_fma(es, -fa, fb, 3.0) / 2 : fp_fma(es, -fa, fb, 2.0); break;
                default: die(c, "unsupported simd fp 3same", i);
                }
            } else {
                switch (op) {
                case 24: fr = fp_minmax(fa, fb, !hi, true); break;
                case 26: fr = hi ? fabs(fa - fb) : fa + fb; break;
                case 27: fr = fa * fb; break;
                case 28: isbits = true; br = (hi ? fa > fb : fa >= fb) ? ~0ull : 0; break;
                case 29: isbits = true; br = (hi ? fabs(fa) > fabs(fb) : fabs(fa) >= fabs(fb)) ? ~0ull : 0; break;
                case 30: fr = fp_minmax(fa, fb, !hi, false); break;
                case 31: fr = fa / fb; break;
                default: die(c, "unsupported simd fp 3same", i);
                }
            }
            r = isbits ? br : f2raw(fr, es);
        }
        vset(&res, es, k, r);
    }
    c->vr[rdn] = res;
}

// ---------------------------------------------------- AdvSIMD: three different

static void simd_3diff(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 15, 12);
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn], m = c->vr[rm], d0 = c->vr[rdn];
    VReg res; memset(&res, 0, 16);
    if (op == 14) {  // PMULL
        int hb = Q ? 8 : 0;
        if (size == 3) {
            uint64_t a = n.d[Q], b = m.d[Q];
            unsigned __int128 r = 0;
            for (int k = 0; k < 64; k++) if ((b >> k) & 1) r ^= (unsigned __int128)a << k;
            res.d[0] = (uint64_t)r; res.d[1] = (uint64_t)(r >> 64);
        } else if (size == 0) {
            for (int k = 0; k < 8; k++) {
                uint16_t r = 0;
                for (int j = 0; j < 8; j++) if ((m.b[hb + k] >> j) & 1) r ^= (uint16_t)n.b[hb + k] << j;
                res.h[k] = r;
            }
        } else die(c, "bad pmull", i);
        c->vr[rdn] = res;
        return;
    }
    if (op == 4 || op == 6) {  // ADDHN / RADDHN / SUBHN / RSUBHN (+2): the high half of a wide sum or difference
        int es = 1 << size, wes = es * 2, N = 8 / es;
        if (size == 3) die(c, "bad simd 3diff size", i);
        uint64_t mask = wes == 8 ? ~0ull : (1ull << (wes * 8)) - 1;
        if (Q) res = d0;
        for (int k = 0; k < N; k++) {
            uint64_t a = vget(&n, wes, k), b = vget(&m, wes, k);
            uint64_t r = (op == 4 ? a + b : a - b) + (U ? 1ull << (es * 8 - 1) : 0);
            vset(&res, es, k + (Q ? N : 0), (r & mask) >> (es * 8));
        }
        c->vr[rdn] = res;
        return;
    }
    int es = 1 << size, des = es * 2, L = 8 / es, off = Q ? L : 0;
    if (size == 3) die(c, "bad simd 3diff size", i);
    for (int k = 0; k < L; k++) {
        uint64_t a, b = vget(&m, es, off + k);
        int64_t sa, sb = sx(b, es);
        bool wide = op == 1 || op == 3;
        if (wide) { a = vget(&n, des, k); sa = sx(a, des); }
        else { a = vget(&n, es, off + k); sa = sx(a, es); }
        uint64_t acc = vget(&d0, des, k), r;
        __int128 x = U ? (__int128)a : (__int128)sa, y = U ? (__int128)b : (__int128)sb;
        switch (op) {
        case 0: case 1: r = (uint64_t)(x + y); break;
        case 2: case 3: r = (uint64_t)(x - y); break;
        case 5: r = acc + (uint64_t)(x > y ? x - y : y - x); break;
        case 7: r = (uint64_t)(x > y ? x - y : y - x); break;
        case 8: r = acc + (uint64_t)(x * y); break;
        case 10: r = acc - (uint64_t)(x * y); break;
        case 12: r = (uint64_t)(x * y); break;
        default: die(c, "unsupported simd 3diff", i);
        }
        vset(&res, des, k, r);
    }
    c->vr[rdn] = res;
}

// -------------------------------------------------- AdvSIMD: two-register misc

static void simd_misc2(CPU *c, uint32_t i, bool scalar, bool h16) {
    int Q = scalar ? 0 : B(i, 30, 30), U = B(i, 29, 29), size = h16 ? B(i, 23, 23) * 2 : B(i, 23, 22), op = B(i, 16, 12);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn], d0 = c->vr[rdn];
    VReg res; memset(&res, 0, 16);
    int ds = Q ? 16 : 8, hi = size >> 1, sz = size & 1;

    // narrowing
    if (op == 18 || op == 20 || (op == 22 && !scalar)) {
        if (op == 22) {  // FCVTN
            int srcs = sz ? 8 : 4, N = sz ? 2 : 4;
            if (Q) res = d0;
            for (int k = 0; k < N; k++) {
                double v = raw2f(vget(&n, srcs, k), srcs);
                if (sz) vset(&res, 4, k + (Q ? N : 0), f2raw(v, 4));
                else res.h[k + (Q ? N : 0)] = f2hraw(v);
            }
            c->vr[rdn] = res;
            return;
        }
        int es = 1 << size, es2 = es * 2, N = scalar ? 1 : 8 / es;
        if (Q && !scalar) res = d0;
        for (int k = 0; k < N; k++) {
            uint64_t a = vget(&n, es2, k), r;
            if (op == 18 && !U) r = a;
            else if (op == 18) r = sat_u((__int128)sx(a, es2), es);
            else r = U ? sat_u((__int128)a, es) : sat_s((__int128)sx(a, es2), es);
            vset(&res, es, k + (Q && !scalar ? N : 0), r);
        }
        c->vr[rdn] = res;
        return;
    }
    if (op == 19 && U) {  // SHLL
        int es = 1 << size, N = 8 / es, off = Q ? N : 0;
        for (int k = 0; k < N; k++) vset(&res, es * 2, k, zx(vget(&n, es, off + k), es) << (es * 8));
        c->vr[rdn] = res;
        return;
    }
    if (op == 23 && !scalar) {  // FCVTL
        int N = sz ? 2 : 4, off = Q ? N : 0;
        for (int k = 0; k < N; k++) {
            if (sz) vset(&res, 8, k, f2raw(raw2f(vget(&n, 4, off + k), 4), 8));
            else vset(&res, 4, k, f2raw(hraw2f(n.h[off + k]), 4));
        }
        c->vr[rdn] = res;
        return;
    }
    if (op == 2 || op == 6) {  // SADDLP/UADDLP, SADALP/UADALP
        int es = 1 << size, des = es * 2, N = ds / des;
        for (int k = 0; k < N; k++) {
            uint64_t a = vget(&n, es, 2 * k), b = vget(&n, es, 2 * k + 1);
            uint64_t r = U ? zx(a, es) + zx(b, es) : (uint64_t)(sx(a, es) + sx(b, es));
            if (op == 6) r += vget(&d0, des, k);
            vset(&res, des, k, r);
        }
        c->vr[rdn] = res;
        return;
    }
    if (op <= 1 && !scalar) {  // REV64 / REV32 / REV16
        int es = 1 << size, cb = op == 1 ? 2 : (U ? 4 : 8);
        for (int base = 0; base < ds; base += cb)
            for (int k = 0; k < cb / es; k++)
                memcpy(res.b + base + (cb / es - 1 - k) * es, n.b + base + k * es, es);
        c->vr[rdn] = res;
        return;
    }

    bool fp = op >= 12;  // integer forms of 12..15 do not exist; 18..23 returned above
    if (!fp) {
        int es = op == 5 ? 1 : 1 << size;  // CNT/NOT/RBIT: size selects the variant, lanes are bytes
        int L = scalar ? 1 : ds / es;
        for (int k = 0; k < L; k++) {
            uint64_t a = vget(&n, es, k), r;
            int64_t sa = sx(a, es);
            int bits = es * 8;
            switch (op) {
            case 4: {  // CLS / CLZ
                if (U) { uint64_t z = zx(a, es); r = z ? __builtin_clzll(z) - (64 - bits) : bits; }
                else { uint64_t t = (uint64_t)(sa ^ (sa >> 1)); t = zx(t, es); r = (t ? __builtin_clzll(t) - (64 - bits) : bits) - 1; }
                break;
            }
            case 5:
                if (!U) r = __builtin_popcountll(zx(a, es));
                else if (size == 0) r = ~a;
                else r = __builtin_bitreverse8((uint8_t)a);  // RBIT
                break;
            case 8: r = (U ? sa >= 0 : sa > 0) ? ~0ull : 0; break;
            case 9: r = (U ? sa <= 0 : sa == 0) ? ~0ull : 0; break;
            case 10: r = sa < 0 ? ~0ull : 0; break;
            case 11: r = U ? (uint64_t)(-sa) : (uint64_t)(sa < 0 ? -sa : sa); break;
            case 7: r = U ? sat_s(-(__int128)sa, es) : sat_s(sa < 0 ? -(__int128)sa : (__int128)sa, es); break;
            default: die(c, "unsupported simd misc2", i);
            }
            vset(&res, es, k, r);
        }
        c->vr[rdn] = res;
        return;
    }

    int es = h16 ? 2 : sz ? 8 : 4, L = scalar ? 1 : ds / es;
    for (int k = 0; k < L; k++) {
        uint64_t a = vget(&n, es, k), r = 0;
        double fa = raw2f(a, es), fr = 0;
        bool isint = false;
        switch (op) {
        case 12: isint = true; r = (U ? fa >= 0 : fa > 0) ? ~0ull : 0; break;
        case 13: isint = true; r = (U ? fa <= 0 : fa == 0) ? ~0ull : 0; break;
        case 14: isint = true; r = fa < 0 ? ~0ull : 0; break;
        case 15: isint = true; r = U ? a ^ (1ull << (es * 8 - 1)) : a & ~(1ull << (es * 8 - 1)); break;
        case 24: fr = hi ? fp_round(fa, 1) : fp_round(fa, U ? 4 : 0); break;
        case 25: fr = hi ? (U ? fp_round(fa, 0) : fp_round(fa, 3)) : (U ? fp_round(fa, 0) : fp_round(fa, 2)); break;
        case 26: isint = true; r = fcvt_int(fa, hi ? 1 : 0, U, es * 8); break;
        case 27: isint = true; r = fcvt_int(fa, hi ? 3 : 2, U, es * 8); break;
        case 28:
            if (hi) die(c, "unsupported simd misc2", i);
            isint = true; r = fcvt_int(fa, 4, U, es * 8); break;
        case 29:
            if (!hi) {
                isint = true;
                if (es == 4) { float f = U ? (float)(uint32_t)a : (float)(int32_t)a; r = f2raw(f, 4); }
                else if (es == 2) r = f2raw(U ? (double)(uint16_t)a : (double)(int16_t)a, 2);
                else r = f2raw(U ? (double)(uint64_t)a : (double)(int64_t)a, 8);
            } else fr = U ? 1.0 / sqrt(fa) : 1.0 / fa;  // FRSQRTE / FRECPE (exact, not the table estimate)
            break;
        case 30: if (hi || es < 4) die(c, "unsupported simd misc2", i); fr = frint_ts(fa, 32, !U); break;   // FRINT32Z / FRINT32X
        case 31:
            if (!hi && es >= 4) { fr = frint_ts(fa, 64, !U); break; }   // FRINT64Z / FRINT64X
            if (!(hi && U)) die(c, "unsupported simd misc2", i);
            fr = sqrt(fa); break;
        default: die(c, "unsupported simd fp misc2", i);
        }
        vset(&res, es, k, isint ? r : f2raw(fr, es));
    }
    c->vr[rdn] = res;
}

// ------------------------------------------------------ AdvSIMD: across lanes

static void simd_across(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 16, 12);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn];
    VReg res; memset(&res, 0, 16);
    int ds = Q ? 16 : 8;
    if ((op == 12 || op == 15) && !U) {  // the half-precision FMAXNMV / FMAXV / FMINNMV / FMINV
        int L = Q ? 8 : 4;
        double acc = hraw2f(n.h[0]);
        for (int k = 1; k < L; k++) acc = fp_minmax(acc, hraw2f(n.h[k]), !(size >> 1), op == 12);
        res.h[0] = f2hraw(acc);
        c->vr[rdn] = res;
        return;
    }
    if (op == 12 || op == 15) {  // FMAXNMV / FMAXV / FMINNMV / FMINV
        double acc = raw2f(n.s[0], 4);
        for (int k = 1; k < 4; k++) acc = fp_minmax(acc, raw2f(n.s[k], 4), !(size >> 1), op == 12);
        res.s[0] = (uint32_t)f2raw(acc, 4);
        c->vr[rdn] = res;
        return;
    }
    int es = 1 << size, L = ds / es;
    __int128 acc = 0;
    for (int k = 0; k < L; k++) {
        uint64_t a = vget(&n, es, k);
        __int128 v = U ? (__int128)zx(a, es) : (__int128)sx(a, es);
        if (k == 0) acc = v;
        else switch (op) {
        case 3: case 27: acc += v; break;
        case 10: if (v > acc) acc = v; break;
        case 26: if (v < acc) acc = v; break;
        default: die(c, "unsupported simd across", i);
        }
    }
    vset(&res, op == 3 ? es * 2 : es, 0, (uint64_t)acc);
    c->vr[rdn] = res;
}

// scalar pairwise: ADDP Dd,Vn.2D and FADDP/FMAXP/... Sd/Dd,Vn.2S/2D
static void simd_scalar_pairwise(CPU *c, uint32_t i) {
    int U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 16, 12);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    VReg res; memset(&res, 0, 16);
    const VReg n = c->vr[rn];
    if (!U && op == 27) { res.d[0] = n.d[0] + n.d[1]; c->vr[rdn] = res; return; }
    bool h16 = !U && (op == 12 || op == 13 || op == 15);  // FADDP / FMAXP / ... on two halves
    int es = h16 ? 2 : (size & 1) ? 8 : 4, hi = size >> 1;
    double a = raw2f(vget(&n, es, 0), es), b = raw2f(vget(&n, es, 1), es), r;
    switch (op) {
    case 12: r = fp_minmax(a, b, !hi, true); break;
    case 13: r = a + b; break;
    case 15: r = fp_minmax(a, b, !hi, false); break;
    default: die(c, "unsupported scalar pairwise", i);
    }
    vset(&res, es, 0, f2raw(r, es));
    c->vr[rdn] = res;
}

// ------------------------------------------------------------ AdvSIMD: copy

static void simd_copy(CPU *c, uint32_t i, bool scalar) {
    int Q = scalar ? 1 : B(i, 30, 30), op = B(i, 29, 29), imm5 = B(i, 20, 16), imm4 = B(i, 14, 11);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    if (!(imm5 & 15) && !(imm5 & 16)) die(c, "bad simd copy", i);
    int hb = __builtin_ctz(imm5);
    if (hb > 3) die(c, "bad simd copy", i);
    int es = 1 << hb, idx = imm5 >> (hb + 1), ds = Q ? 16 : 8;
    VReg res; memset(&res, 0, 16);
    if (op) {  // INS (element)
        res = c->vr[rdn];
        vset(&res, es, idx, vget(&c->vr[rn], es, imm4 >> hb));
        c->vr[rdn] = res;
        return;
    }
    switch (imm4) {
    case 0: case 1: {
        uint64_t v = imm4 == 0 ? vget(&c->vr[rn], es, idx) : rd(c, rn);
        int L = scalar ? 1 : ds / es;
        for (int k = 0; k < L; k++) vset(&res, es, k, v);
        c->vr[rdn] = res;
        return;
    }
    case 3:  // INS (general)
        res = c->vr[rdn];
        vset(&res, es, idx, rd(c, rn));
        c->vr[rdn] = res;
        return;
    case 5: {  // SMOV
        uint64_t v = (uint64_t)sx(vget(&c->vr[rn], es, idx), es);
        wr(c, rdn, Q ? v : (v & 0xffffffffull));
        return;
    }
    case 7:  // UMOV
        wr(c, rdn, zx(vget(&c->vr[rn], es, idx), es));
        return;
    }
    die(c, "unsupported simd copy", i);
}

// ------------------------------------------------- AdvSIMD: permute, EXT, TBL

static void simd_permute(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), size = B(i, 23, 22), op = B(i, 14, 12);
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn], m = c->vr[rm];
    VReg res; memset(&res, 0, 16);
    int es = 1 << size, ds = Q ? 16 : 8, L = ds / es, h = L / 2;
    for (int k = 0; k < L; k++) {
        uint64_t r;
        switch (op) {
        case 1: r = k < h ? vget(&n, es, 2 * k) : vget(&m, es, 2 * (k - h)); break;
        case 5: r = k < h ? vget(&n, es, 2 * k + 1) : vget(&m, es, 2 * (k - h) + 1); break;
        case 2: r = (k & 1) ? vget(&m, es, k - 1) : vget(&n, es, k); break;
        case 6: r = (k & 1) ? vget(&m, es, k) : vget(&n, es, k + 1); break;
        case 3: r = (k & 1) ? vget(&m, es, k / 2) : vget(&n, es, k / 2); break;
        case 7: r = (k & 1) ? vget(&m, es, h + k / 2) : vget(&n, es, h + k / 2); break;
        default: die(c, "bad permute", i);
        }
        vset(&res, es, k, r);
    }
    c->vr[rdn] = res;
}

static void simd_ext(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), imm4 = B(i, 14, 11);
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    uint8_t cat[32];
    memcpy(cat, &c->vr[rn], 16); memcpy(cat + 16, &c->vr[rm], 16);
    int ds = Q ? 16 : 8;
    if (!Q) { memcpy(cat + 8, &c->vr[rm], 8); }
    VReg res; memset(&res, 0, 16);
    memcpy(&res, cat + imm4, ds);
    c->vr[rdn] = res;
}

static void simd_tbl(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), len = B(i, 14, 13), tbx = B(i, 12, 12);
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    int ds = Q ? 16 : 8;
    uint8_t tab[64];
    for (int k = 0; k <= len; k++) memcpy(tab + 16 * k, &c->vr[(rn + k) & 31], 16);
    VReg res; memset(&res, 0, 16);
    if (tbx) res = c->vr[rdn];
    if (!Q) memset(res.b + 8, 0, 8);
    for (int k = 0; k < ds; k++) {
        uint8_t idx = c->vr[rm].b[k];
        if (idx < 16 * (len + 1)) res.b[k] = tab[idx];
        else if (!tbx) res.b[k] = 0;
    }
    c->vr[rdn] = res;
}

// --------------------------------------------------- AdvSIMD: modified immediate

static void simd_modimm(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), op = B(i, 29, 29), cmode = B(i, 15, 12), rdn = B(i, 4, 0);
    uint64_t imm8 = (B(i, 18, 16) << 5) | B(i, 9, 5), imm = 0;
    bool orr = false, inv = false;
    if ((cmode & 9) == 0) {
        uint64_t v = imm8 << (8 * ((cmode >> 1) & 3));
        imm = v | (v << 32); inv = op;
    } else if ((cmode & 9) == 1) {
        uint64_t v = imm8 << (8 * ((cmode >> 1) & 3));
        imm = v | (v << 32); orr = true; inv = op;
    } else if ((cmode & 13) == 8) {
        uint64_t v = imm8 << (8 * ((cmode >> 1) & 1));
        imm = v | (v << 16) | (v << 32) | (v << 48); inv = op;
    } else if ((cmode & 13) == 9) {
        uint64_t v = imm8 << (8 * ((cmode >> 1) & 1));
        imm = v | (v << 16) | (v << 32) | (v << 48); orr = true; inv = op;
    } else if ((cmode >> 1) == 6) {
        uint64_t v = cmode & 1 ? (imm8 << 16) | 0xffff : (imm8 << 8) | 0xff;
        imm = v | (v << 32); inv = op;
    } else if (cmode == 14) {
        if (!op) imm = imm8 * 0x0101010101010101ull;
        else for (int k = 0; k < 8; k++) if ((imm8 >> k) & 1) imm |= 0xffull << (8 * k);
    } else {  // cmode == 15: FMOV
        if (!op) { uint64_t f = vfp_expand_imm((int)imm8, 4); imm = f | (f << 32); }
        else imm = vfp_expand_imm((int)imm8, 8);
    }
    if (orr) {
        VReg d = c->vr[rdn];
        for (int k = 0; k < (Q ? 2 : 1); k++) d.d[k] = inv ? d.d[k] & ~imm : d.d[k] | imm;
        if (!Q) d.d[1] = 0;   // a 64-bit result clears the upper half
        c->vr[rdn] = d;
        return;
    }
    if (inv) imm = ~imm;
    VReg res; memset(&res, 0, 16);
    res.d[0] = imm;
    if (Q) res.d[1] = imm;
    c->vr[rdn] = res;
}

// ------------------------------------------------- AdvSIMD: shift by immediate

static void simd_shift_imm(CPU *c, uint32_t i, bool scalar) {
    int Q = scalar ? 1 : B(i, 30, 30), U = B(i, 29, 29), immh = B(i, 22, 19), immhb = B(i, 22, 16);
    int op = B(i, 15, 11), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    if (!immh) die(c, "bad shift imm", i);
    int hb = 31 - __builtin_clz(immh), es = 1 << hb, ebits = 8 << hb;
    int rs = 2 * ebits - immhb, ls = immhb - ebits;
    const VReg n = c->vr[rn], d0 = c->vr[rdn];
    VReg res; memset(&res, 0, 16);
    int ds = Q ? 16 : 8, L = scalar ? 1 : ds / es;

    if (op == 0x10 || op == 0x11 || op == 0x12 || op == 0x13) {  // narrowing right shifts
        int es2 = es * 2, N = scalar ? 1 : 8 / es;
        if (Q && !scalar) res = d0;
        bool rnd = op & 1;
        for (int k = 0; k < N; k++) {
            uint64_t a = vget(&n, es2, k), r;
            bool usrc = U && op >= 0x12;  // UQSHRN/UQRSHRN read an unsigned source
            __int128 x = usrc ? (__int128)a : (__int128)sx(a, es2);
            if (rnd) x += (__int128)1 << (rs - 1);
            x >>= rs;
            if (op <= 0x11) r = U ? sat_u(x, es) : (uint64_t)x;     // SHRN truncates, SQSHRUN saturates unsigned
            else r = U ? sat_u(x, es) : sat_s(x, es);
            vset(&res, es, k + (Q && !scalar ? N : 0), r);
        }
        c->vr[rdn] = res;
        return;
    }
    if (op == 0x14) {  // SSHLL / USHLL
        int N = 8 / es, off = Q ? N : 0;
        for (int k = 0; k < N; k++) {
            uint64_t a = vget(&n, es, off + k);
            vset(&res, es * 2, k, (U ? zx(a, es) : (uint64_t)sx(a, es)) << ls);
        }
        c->vr[rdn] = res;
        return;
    }
    for (int k = 0; k < L; k++) {
        uint64_t a = vget(&n, es, k), r, acc = vget(&d0, es, k);
        int64_t sa = sx(a, es);
        switch (op) {
        case 0x00: r = U ? lshr(a, rs) : (uint64_t)ashr(sa, rs); break;
        case 0x02: r = acc + (U ? lshr(a, rs) : (uint64_t)ashr(sa, rs)); break;
        case 0x04: case 0x06: {
            __int128 x = U ? (__int128)a : (__int128)sa;
            r = (uint64_t)((x + ((__int128)1 << (rs - 1))) >> rs);
            if (op == 0x06) r += acc;
            break;
        }
        case 0x08: {  // SRI
            uint64_t ones = zx(~0ull, es), keep = ~lshr(ones, rs) & ones;
            r = (acc & keep) | lshr(a, rs);
            break;
        }
        case 0x0a:
            r = U ? (acc & (ls ? (1ull << ls) - 1 : 0)) | lshl(a, ls) : lshl(a, ls);
            break;
        case 0x0c: r = sat_u((__int128)sa << ls, es); break;                       // SQSHLU
        case 0x0e: r = U ? sat_u((__int128)a << ls, es) : sat_s((__int128)sa << ls, es); break;
        case 0x1c:
            if (es == 4) r = f2raw(ldexp(U ? (double)(uint32_t)a : (double)(int32_t)a, -rs), 4);
            else r = f2raw(ldexp(U ? (double)(uint64_t)a : (double)(int64_t)a, -rs), 8);
            break;
        case 0x1f: r = fcvt_int(ldexp(raw2f(a, es), rs), 3, U, es * 8); break;
        default: die(c, "unsupported simd shift imm", i);
        }
        vset(&res, es, k, r);
    }
    c->vr[rdn] = res;
}

// ----------------------------------------------- AdvSIMD: by indexed element

static void simd_indexed(CPU *c, uint32_t i, bool scalar) {
    int Q = scalar ? 0 : B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22);
    int Lb = B(i, 21, 21), M = B(i, 20, 20), op = B(i, 15, 12), H = B(i, 11, 11);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0), rmlo = B(i, 19, 16);
    const VReg n = c->vr[rn], d0 = c->vr[rdn];
    VReg res; memset(&res, 0, 16);
    int ds = Q ? 16 : 8;
    bool fpop = op == 1 || op == 5 || op == 9;
    int es, idx, rm;
    if (fpop && size == 0) { es = 2; rm = rmlo; idx = (H << 2) | (Lb << 1) | M; }  // half precision
    else if (fpop) {
        es = (size & 1) ? 8 : 4;
        rm = (M << 4) | rmlo;
        idx = es == 4 ? (H << 1) | Lb : H;
    } else if (size == 1) { es = 2; rm = rmlo; idx = (H << 2) | (Lb << 1) | M; }
    else if (size == 2) { es = 4; rm = (M << 4) | rmlo; idx = (H << 1) | Lb; }
    else die(c, "bad indexed size", i);
    uint64_t b = vget(&c->vr[rm], es, idx);

    if (op == 0xa || op == 2 || op == 6) {  // SMULL / SMLAL / SMLSL (+ unsigned)
        int des = es * 2, N = 8 / es, off = Q ? N : 0;
        for (int k = 0; k < N; k++) {
            uint64_t a = vget(&n, es, off + k);
            __int128 p = U ? (__int128)zx(a, es) * zx(b, es) : (__int128)sx(a, es) * sx(b, es);
            uint64_t acc = vget(&d0, des, k);
            vset(&res, des, k, op == 0xa ? (uint64_t)p : op == 2 ? acc + (uint64_t)p : acc - (uint64_t)p);
        }
        c->vr[rdn] = res;
        return;
    }
    if (!U && (op == 3 || op == 7 || op == 0xb)) {  // SQDMLAL / SQDMLSL / SQDMULL (vector, "2" form, scalar): widening, doubling, saturating
        int des = es * 2, N = scalar ? 1 : 8 / es, off = Q ? N : 0;
        for (int k = 0; k < N; k++) {
            uint64_t a = vget(&n, es, off + k);
            __int128 p = (__int128)sx(a, es) * sx(b, es) * 2, acc = (__int128)sx(vget(&d0, des, k), des);
            vset(&res, des, k, sat_s(op == 0xb ? p : op == 3 ? acc + p : acc - p, des));
        }
        c->vr[rdn] = res;
        return;
    }
    int L = scalar ? 1 : ds / es;
    for (int k = 0; k < L; k++) {
        uint64_t a = vget(&n, es, k), acc = vget(&d0, es, k), r = 0;
        int bits = es * 8;
        if (fpop) {
            double fa = raw2f(a, es), fb = raw2f(b, es), fr;
            if (op == 1) fr = fp_fma(es, fa, fb, raw2f(acc, es));
            else if (op == 5) fr = fp_fma(es, -fa, fb, raw2f(acc, es));
            else fr = fa * fb;
            r = f2raw(fr, es);
        } else switch (op) {
            case 0: r = acc + a * b; break;
            case 4: r = acc - a * b; break;
            case 8: r = a * b; break;
            case 0xc: r = sat_s(((__int128)sx(a, es) * sx(b, es) * 2) >> bits, es); break;
            case 0xd:
                if (U) r = sat_s((((__int128)sx(acc, es) << bits) + (__int128)sx(a, es) * sx(b, es) * 2 + ((__int128)1 << (bits - 1))) >> bits, es);   // SQRDMLAH
                else r = sat_s((((__int128)sx(a, es) * sx(b, es) * 2) + ((__int128)1 << (bits - 1))) >> bits, es);
                break;
            case 0xf: if (!U) die(c, "unsupported simd indexed", i);   // SQRDMLSH
                r = sat_s((((__int128)sx(acc, es) << bits) - (__int128)sx(a, es) * sx(b, es) * 2 + ((__int128)1 << (bits - 1))) >> bits, es); break;
            default: die(c, "unsupported simd indexed", i);
        }
        vset(&res, es, k, r);
    }
    c->vr[rdn] = res;
}

// ------------------------------------------------------- SDOT / UDOT (dot product)

// Each 32-bit lane accumulates the dot product of four bytes of Vn with four bytes of Vm (vector form: the same
// lane; by-element form: the four bytes selected by the index).
static void simd_dot(CPU *c, uint32_t i, bool indexed) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn], m = c->vr[rm], d0 = c->vr[rdn];
    int idx = indexed ? (B(i, 11, 11) << 1) | B(i, 21, 21) : 0;
    VReg res; memset(&res, 0, 16);
    for (int k = 0; k < (Q ? 4 : 2); k++) {
        int32_t acc = (int32_t)d0.s[k];
        for (int j = 0; j < 4; j++) {
            uint8_t a = n.b[4 * k + j], b = m.b[4 * (indexed ? idx : k) + j];
            acc += U ? (int32_t)((uint32_t)a * b) : (int32_t)(int8_t)a * (int32_t)(int8_t)b;
        }
        res.s[k] = (uint32_t)acc;
    }
    c->vr[rdn] = res;
}

// ------------------------------------------------------------------ dispatcher

#include "crypto.h"

static bool exec_fpsimd(CPU *c, uint32_t i) {
    if (exec_crypto(c, i)) return true;
    if ((i & 0xbf200000) == 0x0c000000) { simd_ldst_multi(c, i); return true; }
    if ((i & 0xbf000000) == 0x0d000000) { simd_ldst_single(c, i); return true; }
    bool b31 = B(i, 31, 31), b30 = B(i, 30, 30), U = B(i, 29, 29);
    switch (B(i, 28, 24)) {
    case 0x0e:
        if (b31) return false;
        if (B(i, 21, 21)) {
            if (B(i, 10, 10)) { simd_3same(c, i, false, false); return true; }
            if (B(i, 11, 10) == 0) { simd_3diff(c, i); return true; }
            if (B(i, 21, 17) == 0x10) { simd_misc2(c, i, false, false); return true; }
            if (B(i, 21, 17) == 0x18) { simd_across(c, i); return true; }
            if (B(i, 22, 17) == 0x3c && B(i, 11, 10) == 2) { simd_misc2(c, i, false, true); return true; }   // half precision
            return false;
        }
        if (B(i, 22, 21) == 2 && !B(i, 15, 14) && B(i, 10, 10)) { simd_3same(c, i, false, true); return true; }   // half precision
        if (B(i, 23, 22) == 2 && !B(i, 21, 21) && B(i, 15, 11) == 0x12 && B(i, 10, 10)) { simd_dot(c, i, false); return true; }   // SDOT / UDOT
        if (!B(i, 15, 15) && B(i, 11, 10) == 2 && !U) { simd_permute(c, i); return true; }
        if (U && !B(i, 23, 22) && !B(i, 15, 15) && !B(i, 10, 10)) { simd_ext(c, i); return true; }
        if (!U && !B(i, 23, 22) && !B(i, 15, 15) && B(i, 11, 10) == 0) { simd_tbl(c, i); return true; }
        if (!B(i, 15, 15) && B(i, 10, 10) && !B(i, 23, 21)) { simd_copy(c, i, false); return true; }
        return false;
    case 0x0f:
        if (b31) return false;
        if (!B(i, 10, 10)) {
            if (B(i, 23, 22) == 2 && B(i, 15, 12) == 0xe) { simd_dot(c, i, true); return true; }   // SDOT / UDOT by element
            simd_indexed(c, i, false);
            return true;
        }
        if (!B(i, 22, 19)) { simd_modimm(c, i); return true; }
        simd_shift_imm(c, i, false);
        return true;
    case 0x1e:
        if (!b31 && b30) {  // scalar AdvSIMD
            if (B(i, 21, 21)) {
                if (B(i, 10, 10)) { simd_3same(c, i, true, false); return true; }
                if (B(i, 11, 10) == 2 && B(i, 21, 17) == 0x10) { simd_misc2(c, i, true, false); return true; }
                if (B(i, 11, 10) == 2 && B(i, 21, 17) == 0x18) { simd_scalar_pairwise(c, i); return true; }
                if (B(i, 22, 17) == 0x3c && B(i, 11, 10) == 2) { simd_misc2(c, i, true, true); return true; }   // half precision
                return false;
            }
            if (B(i, 22, 21) == 2 && !B(i, 15, 14) && B(i, 10, 10)) { simd_3same(c, i, true, true); return true; }   // half precision
            if (!U && !B(i, 15, 15) && B(i, 10, 10) && !B(i, 23, 21)) { simd_copy(c, i, true); return true; }
            return false;
        }
        if (!b30 && !U) { fp_scalar(c, i); return true; }
        return false;
    case 0x1f:
        if (!b31 && b30) {
            if (B(i, 10, 10)) simd_shift_imm(c, i, true); else simd_indexed(c, i, true);
            return true;
        }
        if (!b30 && !U) { fp_scalar(c, i); return true; }
        return false;
    }
    return false;
}
