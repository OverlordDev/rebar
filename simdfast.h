// Fast paths for the commonest AdvSIMD instructions. fpsimd.h decodes everything element by element, which is general
// but slow for the pixel loops of a graphics library; the integer forms here are written with vector types, so the host
// compiler turns each into a few SSE instructions. simd_fast() returns false for any encoding it does not handle (and
// then changes nothing), and the caller falls back to exec_fpsimd(). `rebar --simd-selftest` runs random encodings
// through both paths and compares the registers.

typedef uint8_t U8x16 __attribute__((vector_size(16)));
typedef int8_t S8x16 __attribute__((vector_size(16)));
typedef uint16_t U16x8 __attribute__((vector_size(16)));
typedef int16_t S16x8 __attribute__((vector_size(16)));
typedef uint32_t U32x4 __attribute__((vector_size(16)));
typedef int32_t S32x4 __attribute__((vector_size(16)));
typedef uint64_t U64x2 __attribute__((vector_size(16)));
typedef int64_t S64x2 __attribute__((vector_size(16)));
typedef uint8_t U8x8 __attribute__((vector_size(8)));
typedef int8_t S8x8 __attribute__((vector_size(8)));
typedef uint16_t U16x4 __attribute__((vector_size(8)));
typedef int16_t S16x4 __attribute__((vector_size(8)));
typedef uint32_t U32x2 __attribute__((vector_size(8)));
typedef int32_t S32x2 __attribute__((vector_size(8)));
typedef float F32x4 __attribute__((vector_size(16)));
typedef double F64x2 __attribute__((vector_size(16)));

#define VLD(T, r) ({ T v_; memcpy(&v_, (r), 16); v_; })
#define VST(r, v) do { __typeof__(v) t_ = (v); memcpy((r), &t_, 16); } while (0)

// A 64-bit (Q = 0) operation leaves the top half of the destination zero.
static inline void vwrite(VReg *d, U8x16 v, int Q) {
    if (!Q) { uint64_t lo; memcpy(&lo, &v, 8); d->d[0] = lo; d->d[1] = 0; }
    else memcpy(d, &v, 16);
}

// Three registers of the same size: integer forms.
#define IOP(S, N, TU, TS, ...)                                                                                     \
    case N: { TU a = VLD(TU, &n), b = VLD(TU, &m), d = VLD(TU, &d0); TS sa = (TS)a, sb = (TS)b; (void)d; (void)sa; (void)sb; \
              __VA_ARGS__ break; }

#define SIZED(MACRO)                                                                                                \
    switch (size) {                                                                                                 \
    case 0: MACRO(U8x16, S8x16, 8) break;                                                                           \
    case 1: MACRO(U16x8, S16x8, 16) break;                                                                          \
    case 2: MACRO(U32x4, S32x4, 32) break;                                                                          \
    default: MACRO(U64x2, S64x2, 64) break;                                                                         \
    }

static bool simd_3same_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 15, 11);
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn], m = c->vr[rm], d0 = c->vr[rdn];
    U8x16 res;
    if (op == 3) {  // AND, BIC, ORR, ORN, EOR, BSL, BIT, BIF
        U8x16 a = VLD(U8x16, &n), b = VLD(U8x16, &m), d = VLD(U8x16, &d0);
        switch ((U << 2) | size) {
        case 0: res = a & b; break;
        case 1: res = a & ~b; break;
        case 2: res = a | b; break;
        case 3: res = a | ~b; break;
        case 4: res = a ^ b; break;
        case 5: res = (d & a) | (~d & b); break;
        case 6: res = (d & ~b) | (a & b); break;
        default: res = (d & b) | (a & ~b); break;
        }
        vwrite(&c->vr[rdn], res, Q);
        return true;
    }
    if (op >= 24) {  // floating point
        bool dbl = size & 1;
        int hi = size >> 1;
        if (dbl && !Q) return false;
        // FADD FSUB FMUL FDIV FABD FMAX FMIN FMAXNM FMINNM FMLA FMLS FCMEQ FCMGE FCMGT FACGE FACGT, vector forms.
        // Any NaN input, or NaN result, goes to the general path (which yields the ARM default NaN).
        int kind;  // 0 add/sub, 1 mul, 2 div, 3 abd, 4 max/min, 5 fmla/fmls, 6 cmeq, 7 cmge/cmgt, 8 acge/acgt
        if (!U) {
            if (op == 26) kind = 0; else if (op == 24 || op == 30) kind = 4; else if (op == 25) kind = 5; else if (op == 28) kind = 6; else return false;
        } else {
            if (op == 26 && hi) kind = 3; else if (op == 27) kind = 1; else if (op == 31) kind = 2; else if (op == 28) kind = 7; else if (op == 29) kind = 8; else return false;
        }
        if (kind == 5 && dbl) { /* fused multiply-add: lane by lane below */ }
#define FP3(FT, IT, UT, LANES, FMAF, SIGN)                                                                           \
        {                                                                                                            \
            FT a = VLD(FT, &n), b = VLD(FT, &m), acc = VLD(FT, &d0), r;                                              \
            UT ua = VLD(UT, &n), ub = VLD(UT, &m);                                                                   \
            if (!Q) { UT z = {0}; for (int k_ = LANES / 2; k_ < LANES; k_++) { ua[k_] = 0; ub[k_] = 0; } a = (FT)ua; b = (FT)ub; (void)z; } \
            if (__builtin_reduce_or(a != a) || __builtin_reduce_or(b != b)) return false;                            \
            UT ures;                                                                                                 \
            bool isbits = true;                                                                                      \
            switch (kind) {                                                                                          \
            case 0: r = hi ? a - b : a + b; isbits = false; break;                                                   \
            case 1: r = a * b; isbits = false; break;                                                                \
            case 2: r = a / b; isbits = false; break;                                                                \
            case 3: r = a - b; r = (FT)((UT)r & ~SIGN); isbits = false; break;                                       \
            case 4: {                                                                                                \
                IT gt = hi ? (IT)(a < b) : (IT)(a > b), eq = (IT)(a == b);                                           \
                UT both = hi ? (ua | ub) : (ua & ub);                                                                \
                ures = ((UT)gt & ua) | (~(UT)gt & ~(UT)eq & ub) | ((UT)eq & both);                                   \
                break;                                                                                               \
            }                                                                                                        \
            case 5: {                                                                                                \
                for (int k_ = 0; k_ < LANES; k_++) r[k_] = FMAF(hi ? -a[k_] : a[k_], b[k_], acc[k_]);                \
                isbits = false; break;                                                                               \
            }                                                                                                        \
            case 6: ures = (UT)(a == b); break;                                                                      \
            case 7: ures = hi ? (UT)(a > b) : (UT)(a >= b); break;                                                   \
            default: { FT aa = (FT)(ua & ~SIGN), bb = (FT)(ub & ~SIGN); ures = hi ? (UT)(aa > bb) : (UT)(aa >= bb); break; } \
            }                                                                                                        \
            if (!isbits) { if (__builtin_reduce_or(r != r)) return false; memcpy(&ures, &r, 16); }                    \
            memcpy(&res, &ures, 16);                                                                                 \
        }
        if (!dbl) FP3(F32x4, S32x4, U32x4, 4, fmaf, 0x80000000u)
        else FP3(F64x2, S64x2, U64x2, 2, fma, 0x8000000000000000ull)
#undef FP3
        vwrite(&c->vr[rdn], res, Q);
        return true;
    }
    switch (op) {
    case 16:  // ADD / SUB
#define OP16(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m); TU r = U ? a - b : a + b; memcpy(&res, &r, 16); }
        if (size == 3 && !Q) return false;
        SIZED(OP16)
#undef OP16
        break;
    case 19:  // MUL (PMUL is left to the general path)
        if (U || size == 3) return false;
#define OP19(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m); TU r = a * b; memcpy(&res, &r, 16); }
        SIZED(OP19)
#undef OP19
        break;
    case 18:  // MLA / MLS
        if (size == 3) return false;
#define OP18(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m), d = VLD(TU, &d0); TU r = U ? d - a * b : d + a * b; memcpy(&res, &r, 16); }
        SIZED(OP18)
#undef OP18
        break;
    case 17:  // CMTST / CMEQ
        if (size == 3 && !Q) return false;
#define OP17(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m); TU r = U ? (TU)(a == b) : (TU)((a & b) != 0); memcpy(&res, &r, 16); }
        SIZED(OP17)
#undef OP17
        break;
    case 6: case 7:  // CMGT / CMHI, CMGE / CMHS
        if (size == 3 && !Q) return false;
#define OP67(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m); TS sa = (TS)a, sb = (TS)b; TU r; \
            if (op == 6) r = U ? (TU)(a > b) : (TU)(sa > sb); else r = U ? (TU)(a >= b) : (TU)(sa >= sb); memcpy(&res, &r, 16); }
        SIZED(OP67)
#undef OP67
        break;
    case 12: case 13:  // SMAX / UMAX, SMIN / UMIN
        if (size == 3) return false;
#define OP1213(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m); TS sa = (TS)a, sb = (TS)b; TU r; \
            if (op == 12) r = U ? __builtin_elementwise_max(a, b) : (TU)__builtin_elementwise_max(sa, sb); \
            else r = U ? __builtin_elementwise_min(a, b) : (TU)__builtin_elementwise_min(sa, sb); memcpy(&res, &r, 16); }
        SIZED(OP1213)
#undef OP1213
        break;
    case 14: case 15:  // SABD / UABD, SABA / UABA
        if (size == 3) return false;
#define OP1415(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m), d = VLD(TU, &d0); TS sa = (TS)a, sb = (TS)b; TU r; \
            if (U) r = __builtin_elementwise_max(a, b) - __builtin_elementwise_min(a, b); \
            else r = (TU)__builtin_elementwise_max(sa, sb) - (TU)__builtin_elementwise_min(sa, sb); \
            if (op == 15) r += d; memcpy(&res, &r, 16); }
        SIZED(OP1415)
#undef OP1415
        break;
    case 1: case 5:  // SQADD / UQADD, SQSUB / UQSUB
        if (size == 3 && !Q) return false;
#define OP15(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m); TS sa = (TS)a, sb = (TS)b; TU r; \
            if (op == 1) r = U ? __builtin_elementwise_add_sat(a, b) : (TU)__builtin_elementwise_add_sat(sa, sb); \
            else r = U ? __builtin_elementwise_sub_sat(a, b) : (TU)__builtin_elementwise_sub_sat(sa, sb); memcpy(&res, &r, 16); }
        SIZED(OP15)
#undef OP15
        break;
    case 0: case 2: case 4:  // SHADD / UHADD, SRHADD / URHADD, SHSUB / UHSUB
        if (size == 3) return false;
#define OP024(TU, TS, BITS) { TU a = VLD(TU, &n), b = VLD(TU, &m); TS sa = (TS)a, sb = (TS)b; TU r; \
            if (op == 0) r = U ? (a & b) + ((a ^ b) >> 1) : (TU)((sa & sb) + ((sa ^ sb) >> 1)); \
            else if (op == 2) r = U ? (a | b) - ((a ^ b) >> 1) : (TU)((sa | sb) - ((sa ^ sb) >> 1)); \
            else r = U ? (a >> 1) - (b >> 1) - ((~a & b) & 1) : (TU)((sa >> 1) - (sb >> 1) - ((~sa & sb) & 1)); \
            memcpy(&res, &r, 16); }
        SIZED(OP024)
#undef OP024
        break;
    case 23:  // ADDP (Q = 1 only)
        if (U || !Q) return false;
        if (size == 0) {
            U8x16 a = VLD(U8x16, &n), b = VLD(U8x16, &m);
            res = __builtin_shufflevector(a, b, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30)
                + __builtin_shufflevector(a, b, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31);
        } else if (size == 1) {
            U16x8 a = VLD(U16x8, &n), b = VLD(U16x8, &m);
            U16x8 r = __builtin_shufflevector(a, b, 0, 2, 4, 6, 8, 10, 12, 14) + __builtin_shufflevector(a, b, 1, 3, 5, 7, 9, 11, 13, 15);
            memcpy(&res, &r, 16);
        } else if (size == 2) {
            U32x4 a = VLD(U32x4, &n), b = VLD(U32x4, &m);
            U32x4 r = __builtin_shufflevector(a, b, 0, 2, 4, 6) + __builtin_shufflevector(a, b, 1, 3, 5, 7);
            memcpy(&res, &r, 16);
        } else {
            U64x2 a = VLD(U64x2, &n), b = VLD(U64x2, &m);
            U64x2 r = __builtin_shufflevector(a, b, 0, 2) + __builtin_shufflevector(a, b, 1, 3);
            memcpy(&res, &r, 16);
        }
        break;
    default:
        return false;
    }
    vwrite(&c->vr[rdn], res, Q);
    return true;
}

// Shift by immediate (and the widening / narrowing forms).
static bool simd_shift_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), immh = B(i, 22, 19), immhb = B(i, 22, 16);
    int op = B(i, 15, 11), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    int hb = 31 - __builtin_clz(immh), ebits = 8 << hb;
    int rs = 2 * ebits - immhb, ls = immhb - ebits;
    const VReg n = c->vr[rn], d0 = c->vr[rdn];
    U8x16 res;

    if (op == 0x14) {  // SSHLL / USHLL (SXTL / UXTL when the shift is 0)
        if (hb > 2) return false;
        int hi = Q ? 1 : 0;
        if (hb == 0) {
            U8x16 v = VLD(U8x16, &n);
            U8x8 h = hi ? __builtin_shufflevector(v, v, 8, 9, 10, 11, 12, 13, 14, 15) : __builtin_shufflevector(v, v, 0, 1, 2, 3, 4, 5, 6, 7);
            U16x8 r = U ? __builtin_convertvector(h, U16x8) : (U16x8)__builtin_convertvector((S8x8)h, S16x8);
            r <<= ls; memcpy(&res, &r, 16);
        } else if (hb == 1) {
            U16x8 v = VLD(U16x8, &n);
            U16x4 h = hi ? __builtin_shufflevector(v, v, 4, 5, 6, 7) : __builtin_shufflevector(v, v, 0, 1, 2, 3);
            U32x4 r = U ? __builtin_convertvector(h, U32x4) : (U32x4)__builtin_convertvector((S16x4)h, S32x4);
            r <<= ls; memcpy(&res, &r, 16);
        } else {
            U32x4 v = VLD(U32x4, &n);
            U32x2 h = hi ? __builtin_shufflevector(v, v, 2, 3) : __builtin_shufflevector(v, v, 0, 1);
            U64x2 r = U ? __builtin_convertvector(h, U64x2) : (U64x2)__builtin_convertvector((S32x2)h, S64x2);
            r <<= ls; memcpy(&res, &r, 16);
        }
        c->vr[rdn] = *(VReg *)&res;
        return true;
    }
    if (op == 0x10 || op == 0x11) {  // SHRN / RSHRN (the saturating and unsigned forms are left to the general path)
        if (U || hb > 2) return false;
        int rnd = op & 1;
        U64x2 narrow;  // the low 8 bytes hold the result
        if (hb == 0) {
            U16x8 v = VLD(U16x8, &n);
            if (rnd) v += (uint16_t)(1 << (rs - 1));
            v >>= rs;
            U8x8 r = __builtin_convertvector(v, U8x8); memcpy(&narrow, &r, 8);
        } else if (hb == 1) {
            U32x4 v = VLD(U32x4, &n);
            if (rnd) v += (uint32_t)(1u << (rs - 1));
            v >>= rs;
            U16x4 r = __builtin_convertvector(v, U16x4); memcpy(&narrow, &r, 8);
        } else {
            U64x2 v = VLD(U64x2, &n);
            if (rnd) v += (uint64_t)(1ull << (rs - 1));
            v >>= rs;
            U32x2 r = __builtin_convertvector(v, U32x2); memcpy(&narrow, &r, 8);
        }
        uint64_t lo = narrow[0];
        if (Q) { c->vr[rdn].d[1] = lo; }
        else { c->vr[rdn].d[0] = lo; c->vr[rdn].d[1] = 0; }
        return true;
    }
    if (op != 0x00 && op != 0x02 && op != 0x04 && op != 0x06 && op != 0x08 && op != 0x0a) return false;
    if (op == 0x08 && !U) return false;
    if (hb == 3 && !Q) return false;
    // Right shifts by the whole lane width (rs == ebits) are legal: the shift is done as (ebits - 1) plus a further bit.
#define SHIFT_CASE(TU, TS)                                                                                           \
    {                                                                                                                \
        TU a = VLD(TU, &n), d = VLD(TU, &d0), r, zero_ = {0};                                                        \
        TU one_ = zero_ + 1;                                                                                         \
        TS sa = (TS)a;                                                                                               \
        int w = ebits;                                                                                               \
        switch (op) {                                                                                                \
        case 0x00: r = U ? (rs == w ? a - a : a >> rs) : (TU)(sa >> (rs == w ? w - 1 : rs)); break;               \
        case 0x02: r = d + (U ? (rs == w ? a - a : a >> rs) : (TU)(sa >> (rs == w ? w - 1 : rs))); break;         \
        case 0x04: case 0x06: {                                                                                      \
            TU t = U ? (rs == w ? a - a : a >> rs) : (TU)(sa >> (rs == w ? w - 1 : rs));                             \
            TU bit = (a >> (rs - 1)) & 1;                                                                            \
            if (!U) bit = (TU)(sa >> (rs - 1)) & 1;                                                                  \
            r = t + bit;                                                                                             \
            if (op == 0x06) r += d;                                                                                  \
            break;                                                                                                   \
        }                                                                                                            \
        case 0x08: {  /* SRI */                                                                                      \
            TU ones = ~zero_, keep = rs == w ? ones : ~(ones >> rs);                                                 \
            TU sh = rs == w ? a - a : a >> rs;                                                                       \
            r = (d & keep) | sh;                                                                                     \
            break;                                                                                                   \
        }                                                                                                            \
        default:  /* SHL, SLI */                                                                                     \
            r = a << ls;                                                                                             \
            if (U) r |= d & (ls ? ((one_ << ls) - one_) : zero_);                                                      \
            break;                                                                                                   \
        }                                                                                                            \
        memcpy(&res, &r, 16);                                                                                        \
    }
    switch (hb) {
    case 0: SHIFT_CASE(U8x16, S8x16) break;
    case 1: SHIFT_CASE(U16x8, S16x8) break;
    case 2: SHIFT_CASE(U32x4, S32x4) break;
    default: SHIFT_CASE(U64x2, S64x2) break;
    }
#undef SHIFT_CASE
    vwrite(&c->vr[rdn], res, Q);
    return true;
}

// Three registers of different sizes: SADDL, UADDW, UMULL, UABAL, ...
static bool simd_3diff_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 15, 12);
    int rm = B(i, 20, 16), rn = B(i, 9, 5), rdn = B(i, 4, 0);
    if (size == 3 || (op != 0 && op != 1 && op != 2 && op != 3 && op != 5 && op != 7 && op != 8 && op != 10 && op != 12)) return false;
    const VReg n = c->vr[rn], m = c->vr[rm], d0 = c->vr[rdn];
    U8x16 res;
    bool wide = op == 1 || op == 3;
    if (size == 0) {
        U8x16 nv = VLD(U8x16, &n), mv = VLD(U8x16, &m);
        U8x8 mh = Q ? __builtin_shufflevector(mv, mv, 8, 9, 10, 11, 12, 13, 14, 15) : __builtin_shufflevector(mv, mv, 0, 1, 2, 3, 4, 5, 6, 7);
        U16x8 x, y = U ? __builtin_convertvector(mh, U16x8) : (U16x8)__builtin_convertvector((S8x8)mh, S16x8);
        if (wide) x = VLD(U16x8, &n);
        else {
            U8x8 nh = Q ? __builtin_shufflevector(nv, nv, 8, 9, 10, 11, 12, 13, 14, 15) : __builtin_shufflevector(nv, nv, 0, 1, 2, 3, 4, 5, 6, 7);
            x = U ? __builtin_convertvector(nh, U16x8) : (U16x8)__builtin_convertvector((S8x8)nh, S16x8);
        }
        U16x8 acc = VLD(U16x8, &d0), r;
        S16x8 sx_ = (S16x8)x, sy_ = (S16x8)y;
        U16x8 ad = U ? __builtin_elementwise_max(x, y) - __builtin_elementwise_min(x, y)
                     : (U16x8)__builtin_elementwise_max(sx_, sy_) - (U16x8)__builtin_elementwise_min(sx_, sy_);
        switch (op) {
        case 0: case 1: r = x + y; break;
        case 2: case 3: r = x - y; break;
        case 5: r = acc + ad; break;
        case 7: r = ad; break;
        case 8: r = acc + x * y; break;
        case 10: r = acc - x * y; break;
        default: r = x * y; break;
        }
        memcpy(&res, &r, 16);
    } else if (size == 1) {
        U16x8 nv = VLD(U16x8, &n), mv = VLD(U16x8, &m);
        U16x4 mh = Q ? __builtin_shufflevector(mv, mv, 4, 5, 6, 7) : __builtin_shufflevector(mv, mv, 0, 1, 2, 3);
        U32x4 x, y = U ? __builtin_convertvector(mh, U32x4) : (U32x4)__builtin_convertvector((S16x4)mh, S32x4);
        if (wide) x = VLD(U32x4, &n);
        else {
            U16x4 nh = Q ? __builtin_shufflevector(nv, nv, 4, 5, 6, 7) : __builtin_shufflevector(nv, nv, 0, 1, 2, 3);
            x = U ? __builtin_convertvector(nh, U32x4) : (U32x4)__builtin_convertvector((S16x4)nh, S32x4);
        }
        U32x4 acc = VLD(U32x4, &d0), r;
        S32x4 sx_ = (S32x4)x, sy_ = (S32x4)y;
        U32x4 ad = U ? __builtin_elementwise_max(x, y) - __builtin_elementwise_min(x, y)
                     : (U32x4)__builtin_elementwise_max(sx_, sy_) - (U32x4)__builtin_elementwise_min(sx_, sy_);
        switch (op) {
        case 0: case 1: r = x + y; break;
        case 2: case 3: r = x - y; break;
        case 5: r = acc + ad; break;
        case 7: r = ad; break;
        case 8: r = acc + x * y; break;
        case 10: r = acc - x * y; break;
        default: r = x * y; break;
        }
        memcpy(&res, &r, 16);
    } else {
        U32x4 nv = VLD(U32x4, &n), mv = VLD(U32x4, &m);
        U32x2 mh = Q ? __builtin_shufflevector(mv, mv, 2, 3) : __builtin_shufflevector(mv, mv, 0, 1);
        U64x2 x, y = U ? __builtin_convertvector(mh, U64x2) : (U64x2)__builtin_convertvector((S32x2)mh, S64x2);
        if (wide) x = VLD(U64x2, &n);
        else {
            U32x2 nh = Q ? __builtin_shufflevector(nv, nv, 2, 3) : __builtin_shufflevector(nv, nv, 0, 1);
            x = U ? __builtin_convertvector(nh, U64x2) : (U64x2)__builtin_convertvector((S32x2)nh, S64x2);
        }
        U64x2 acc = VLD(U64x2, &d0), r;
        S64x2 sx_ = (S64x2)x, sy_ = (S64x2)y;
        U64x2 ad = U ? __builtin_elementwise_max(x, y) - __builtin_elementwise_min(x, y)
                     : (U64x2)__builtin_elementwise_max(sx_, sy_) - (U64x2)__builtin_elementwise_min(sx_, sy_);
        switch (op) {
        case 0: case 1: r = x + y; break;
        case 2: case 3: r = x - y; break;
        case 5: r = acc + ad; break;
        case 7: r = ad; break;
        case 8: r = acc + x * y; break;
        case 10: r = acc - x * y; break;
        default: r = x * y; break;
        }
        memcpy(&res, &r, 16);
    }
    memcpy(&c->vr[rdn], &res, 16);
    return true;
}

// DUP, INS, UMOV, SMOV.
static bool simd_copy_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), op = B(i, 29, 29), imm5 = B(i, 20, 16), imm4 = B(i, 14, 11);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    if (!(imm5 & 15)) return false;
    int hb = __builtin_ctz(imm5), es = 1 << hb, idx = imm5 >> (hb + 1);
    if (op) {  // INS (element)
        int src = imm4 >> hb;
        uint8_t tmp[8];
        memcpy(tmp, (uint8_t *)&c->vr[rn] + src * es, es);
        memcpy((uint8_t *)&c->vr[rdn] + idx * es, tmp, es);
        return true;
    }
    switch (imm4) {
    case 0: case 1: {  // DUP (element / general)
        uint64_t v;
        if (imm4 == 0) { v = 0; memcpy(&v, (uint8_t *)&c->vr[rn] + idx * es, es); }
        else { v = rd(c, rn); }
        U8x16 r;
        switch (hb) {
        case 0: { U8x16 z = {0}; r = z + (uint8_t)v; break; }
        case 1: { U16x8 z = {0}; z = z + (uint16_t)v; memcpy(&r, &z, 16); break; }
        case 2: { U32x4 z = {0}; z = z + (uint32_t)v; memcpy(&r, &z, 16); break; }
        default: { if (!Q) { c->vr[rdn].d[0] = v; c->vr[rdn].d[1] = 0; return true; } U64x2 z = {0}; z = z + v; memcpy(&r, &z, 16); break; }
        }
        vwrite(&c->vr[rdn], r, Q);
        return true;
    }
    case 3: {  // INS (general)
        if (!Q) return false;
        uint64_t v = rd(c, rn);
        memcpy((uint8_t *)&c->vr[rdn] + idx * es, &v, es);
        return true;
    }
    case 5: {  // SMOV
        int64_t v = 0;
        switch (es) {
        case 1: v = *((int8_t *)&c->vr[rn] + idx); break;
        case 2: v = *((int16_t *)&c->vr[rn] + idx); break;
        case 4: v = *((int32_t *)&c->vr[rn] + idx); break;
        default: return false;
        }
        wr(c, rdn, Q ? (uint64_t)v : (uint64_t)v & 0xffffffffull);
        return true;
    }
    case 7: {  // UMOV
        uint64_t v = 0;
        memcpy(&v, (uint8_t *)&c->vr[rn] + idx * es, es);
        wr(c, rdn, v);
        return true;
    }
    }
    return false;
}

// Two-register miscellaneous: NOT, NEG, ABS, XTN, comparisons with zero, REV, CNT.
static bool simd_misc2_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 16, 12);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    const VReg n = c->vr[rn];
    U8x16 res;
    switch (op) {
    case 0x05:
        if (U && size == 0) { U8x16 a = VLD(U8x16, &n); res = ~a; break; }  // NOT
        if (!U && size == 0) {  // CNT
            U8x16 a = VLD(U8x16, &n);
            U8x16 t1 = a - ((a >> 1) & 0x55);
            U8x16 t2 = (t1 & 0x33) + ((t1 >> 2) & 0x33);
            res = (t2 + (t2 >> 4)) & 0x0f;
            break;
        }
        return false;
    case 0x0b:  // ABS / NEG
        if (size == 3 && !Q) return false;
#define OP0B(TU, TS, BITS) { TU a = VLD(TU, &n); TS sa = (TS)a; TU zero = {0}, m = (TU)(sa >> (BITS - 1)); TU r = U ? zero - a : (a ^ m) - m; memcpy(&res, &r, 16); }
        SIZED(OP0B)
#undef OP0B
        break;
    case 0x08: case 0x09: case 0x0a:  // CMGT / CMGE, CMEQ / CMLE, CMLT against zero
        if ((op == 0x0a && U) || (size == 3 && !Q)) return false;
#define OPCMP(TU, TS, BITS) { TU a = VLD(TU, &n); TS sa = (TS)a, z = {0}; TU r;                                      \
            if (op == 0x08) r = U ? (TU)(sa >= z) : (TU)(sa > z);                                                    \
            else if (op == 0x09) r = U ? (TU)(sa <= z) : (TU)(sa == z);                                              \
            else r = (TU)(sa < z);                                                                                   \
            memcpy(&res, &r, 16); }
        SIZED(OPCMP)
#undef OPCMP
        break;
    case 0x12: {  // XTN / XTN2 (the saturating narrowing forms are left alone)
        if (U || size == 3) return false;
        uint64_t narrow = 0;
        if (size == 0) { U16x8 a = VLD(U16x8, &n); U8x8 r = __builtin_convertvector(a, U8x8); memcpy(&narrow, &r, 8); }
        else if (size == 1) { U32x4 a = VLD(U32x4, &n); U16x4 r = __builtin_convertvector(a, U16x4); memcpy(&narrow, &r, 8); }
        else { U64x2 a = VLD(U64x2, &n); U32x2 r = __builtin_convertvector(a, U32x2); memcpy(&narrow, &r, 8); }
        if (Q) c->vr[rdn].d[1] = narrow; else { c->vr[rdn].d[0] = narrow; c->vr[rdn].d[1] = 0; }
        return true;
    }
    case 0x00: case 0x01: {  // REV64 / REV32 / REV16
        int grp = op == 0x00 ? (U ? 4 : 8) : 2;     // bytes per reversed group
        if (op == 0x01 && (U || size != 0)) return false;
        int es = 1 << size;
        if (es >= grp) return false;
        U8x16 a = VLD(U8x16, &n);
        if (grp == 8 && es == 1) res = __builtin_shufflevector(a, a, 7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
        else if (grp == 8 && es == 2) res = __builtin_shufflevector(a, a, 6, 7, 4, 5, 2, 3, 0, 1, 14, 15, 12, 13, 10, 11, 8, 9);
        else if (grp == 8 && es == 4) res = __builtin_shufflevector(a, a, 4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11);
        else if (grp == 4 && es == 1) res = __builtin_shufflevector(a, a, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        else if (grp == 4 && es == 2) res = __builtin_shufflevector(a, a, 2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13);
        else if (grp == 2 && es == 1) res = __builtin_shufflevector(a, a, 1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        else return false;
        break;
    }
    default:
        return false;
    }
    vwrite(&c->vr[rdn], res, Q);
    return true;
}

// Two-register miscellaneous, floating point: FABS FNEG FSQRT FRECPE FRSQRTE, compares with zero, SCVTF UCVTF FCVTZS FCVTZU.
static bool simd_fpmisc2_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), size = B(i, 23, 22), op = B(i, 16, 12);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    bool dbl = size & 1;
    int hi = size >> 1;
    if (dbl && !Q) return false;
    const VReg n = c->vr[rn];
    U8x16 res;
    if (!dbl) {
        F32x4 a = VLD(F32x4, &n);
        U32x4 ua = VLD(U32x4, &n);
        if (!Q) { ua[2] = 0; ua[3] = 0; a = (F32x4)ua; }
        F32x4 zero = {0, 0, 0, 0};
        U32x4 r;
        switch (op) {
        case 12: r = hi ? (U ? (U32x4)(a >= zero) : (U32x4)(a > zero)) : (U32x4){0}; if (!hi) return false; break;  // FCMGT / FCMGE (zero)
        case 13: if (!hi) return false; r = U ? (U32x4)(a <= zero) : (U32x4)(a == zero); break;                    // FCMLE / FCMEQ (zero)
        case 14: if (!hi || U) return false; r = (U32x4)(a < zero); break;                                         // FCMLT (zero)
        case 15:                                                                                                    // FABS / FNEG
            if (!hi) return false;
            r = U ? ua ^ 0x80000000u : ua & 0x7fffffffu;
            break;
        case 29:
            if (!hi) {  // SCVTF / UCVTF
                F32x4 f = U ? __builtin_convertvector(ua, F32x4) : __builtin_convertvector((S32x4)ua, F32x4);
                memcpy(&r, &f, 16);
            } else if (!U) {  // FRECPE: exact reciprocal
                F32x4 f = (F32x4){1, 1, 1, 1} / a;
                if (__builtin_reduce_or(f != f) || __builtin_reduce_or(a != a)) return false;
                memcpy(&r, &f, 16);
            } else return false;
            break;
        case 27:  // FCVTZS / FCVTZU (round toward zero, saturating, NaN -> 0)
            if (!hi) return false;
            if (!U) {
                S32x4 t = __builtin_convertvector(a, S32x4);   // 0x80000000 for out of range and NaN
                S32x4 nan = (S32x4)(a != a), big = (S32x4)(a >= 2147483648.0f);
                t = (t & ~nan);
                t = (t & ~big) | (big & 0x7fffffff);
                memcpy(&r, &t, 16);
            } else {
                F32x4 lo = a, hi2 = a - 2147483648.0f;
                S32x4 tl = __builtin_convertvector(lo, S32x4), th = __builtin_convertvector(hi2, S32x4);
                S32x4 ge31 = (S32x4)(a >= 2147483648.0f), ge32 = (S32x4)(a >= 4294967296.0f), nan = (S32x4)(a != a), neg = (S32x4)(a <= zero);
                S32x4 t = (tl & ~ge31) | ((th ^ (S32x4){(int)0x80000000, (int)0x80000000, (int)0x80000000, (int)0x80000000}) & ge31);
                t = t | ge32;                         // too big: all ones
                t = t & ~(nan | neg);                 // NaN and negative values: zero
                memcpy(&r, &t, 16);
            }
            break;
        case 31:
            if (!hi || !U) return false;  // FSQRT
            { F32x4 f = __builtin_elementwise_sqrt(a); if (__builtin_reduce_or(f != f)) return false; memcpy(&r, &f, 16); }
            break;
        default: return false;
        }
        memcpy(&res, &r, 16);
    } else {
        F64x2 a = VLD(F64x2, &n);
        U64x2 ua = VLD(U64x2, &n);
        F64x2 zero = {0, 0};
        U64x2 r;
        switch (op) {
        case 12: if (!hi) return false; r = U ? (U64x2)(a >= zero) : (U64x2)(a > zero); break;
        case 13: if (!hi) return false; r = U ? (U64x2)(a <= zero) : (U64x2)(a == zero); break;
        case 14: if (!hi || U) return false; r = (U64x2)(a < zero); break;
        case 15: if (!hi) return false; r = U ? ua ^ 0x8000000000000000ull : ua & 0x7fffffffffffffffull; break;
        case 31: { if (!hi || !U) return false; F64x2 f = __builtin_elementwise_sqrt(a); if (__builtin_reduce_or(f != f)) return false; memcpy(&r, &f, 16); break; }
        default: return false;
        }
        memcpy(&res, &r, 16);
    }
    vwrite(&c->vr[rdn], res, Q);
    return true;
}

// Fixed-point conversions: SCVTF / UCVTF / FCVTZS / FCVTZU by immediate (single precision only).
static bool simd_cvtfix_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), U = B(i, 29, 29), immh = B(i, 22, 19), immhb = B(i, 22, 16), op = B(i, 15, 11);
    int rn = B(i, 9, 5), rdn = B(i, 4, 0);
    if ((immh & 0xc) == 0 || (immh & 0x8)) return false;  // only the 32-bit lanes (immh = 01xx)
    int rs = 64 - immhb;
    const VReg n = c->vr[rn];
    U32x4 ua = VLD(U32x4, &n);
    if (!Q) { ua[2] = 0; ua[3] = 0; }
    F32x4 a = (F32x4)ua;
    U32x4 r;
    float scale = (float)(1.0 / (double)(1ull << rs));
    if (op == 0x1c) {  // SCVTF / UCVTF: int -> double -> * 2^-rs -> float, as the general code does
        for (int k = 0; k < 4; k++) {
            double v = U ? (double)(uint32_t)ua[k] : (double)(int32_t)ua[k];
            float f = (float)(v * (1.0 / (double)(1ull << rs)));
            uint32_t bits_; memcpy(&bits_, &f, 4); r[k] = bits_;
        }
        (void)scale;
    } else {  // FCVTZS / FCVTZU: * 2^rs, then truncate and saturate
        if (__builtin_reduce_or(a != a)) return false;
        for (int k = 0; k < 4; k++) {
            double v = (double)a[k] * (double)(1ull << rs);
            if (U) r[k] = v <= 0 ? 0 : v >= 4294967296.0 ? 0xffffffffu : (uint32_t)v;
            else r[k] = (uint32_t)(v >= 2147483648.0 ? 2147483647 : v <= -2147483648.0 ? INT32_MIN : (int32_t)v);
        }
    }
    U8x16 res; memcpy(&res, &r, 16);
    vwrite(&c->vr[rdn], res, Q);
    return true;
}

// LD1 / ST1 of one lane, and LD1R.
static bool vsingle_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), L = B(i, 22, 22), R = B(i, 21, 21), post = B(i, 23, 23), rm = B(i, 20, 16);
    int opcode = B(i, 15, 13), S = B(i, 12, 12), size = B(i, 11, 10), rn = B(i, 9, 5), rt = B(i, 4, 0);
    if (R || (opcode & 1)) return false;  // one register only
    int scale = opcode >> 1, es, idx;
    switch (scale) {
    case 0: es = 1; idx = (Q << 3) | (S << 2) | size; break;
    case 1: es = 2; idx = (Q << 2) | (S << 1) | (size >> 1); break;
    case 2:
        if (!(size & 1)) { es = 4; idx = (Q << 1) | S; } else { es = 8; idx = Q; }
        break;
    default: return false;  // LD1R is encoded with opcode 110 (R = 0): handled below
    }
    uint64_t base = rd_sp(c, rn);
    uint8_t *p = (uint8_t *)&c->vr[rt] + idx * es;
    if (L) {
        switch (es) {
        case 1: *p = *(volatile uint8_t *)base; break;
        case 2: memcpy(p, (void *)base, 2); break;
        case 4: memcpy(p, (void *)base, 4); break;
        default: memcpy(p, (void *)base, 8); break;
        }
    } else {
        switch (es) {
        case 1: *(volatile uint8_t *)base = *p; break;
        case 2: memcpy((void *)base, p, 2); break;
        case 4: memcpy((void *)base, p, 4); break;
        default: memcpy((void *)base, p, 8); break;
        }
    }
    if (post) wr_sp(c, rn, base + (rm == 31 ? (uint64_t)es : rd(c, rm)));
    return true;
}
static bool vld1r_fast(CPU *c, uint32_t i) {
    int Q = B(i, 30, 30), L = B(i, 22, 22), R = B(i, 21, 21), post = B(i, 23, 23), rm = B(i, 20, 16);
    int opcode = B(i, 15, 13), S = B(i, 12, 12), size = B(i, 11, 10), rn = B(i, 9, 5), rt = B(i, 4, 0);
    if (!L || R || opcode != 6 || S) return false;
    uint64_t base = rd_sp(c, rn);
    U8x16 r;
    switch (size) {
    case 0: { uint8_t v = *(volatile uint8_t *)base; U8x16 z = {0}; r = z + v; break; }
    case 1: { uint16_t v; memcpy(&v, (void *)base, 2); U16x8 z = {0}; z = z + v; memcpy(&r, &z, 16); break; }
    case 2: { uint32_t v; memcpy(&v, (void *)base, 4); U32x4 z = {0}; z = z + v; memcpy(&r, &z, 16); break; }
    default: { uint64_t v; memcpy(&v, (void *)base, 8); U64x2 z = {0}; z = z + v; memcpy(&r, &z, 16); break; }
    }
    vwrite(&c->vr[rt], r, Q);
    if (post) wr_sp(c, rn, base + (rm == 31 ? (uint64_t)(1 << size) : rd(c, rm)));
    return true;
}

// Top-level: handles what it can of the AdvSIMD data-processing groups.
static bool simd_fast(CPU *c, uint32_t i) {
    switch (B(i, 28, 24)) {
    case 0x0e:
        if (B(i, 31, 31)) return false;
        if (B(i, 21, 21)) {
            if (B(i, 10, 10)) return simd_3same_fast(c, i);
            if (B(i, 11, 10) == 0) return simd_3diff_fast(c, i);
            if (B(i, 11, 10) == 2 && B(i, 21, 17) == 0x10) return B(i, 16, 12) >= 12 ? simd_fpmisc2_fast(c, i) : simd_misc2_fast(c, i);
            return false;
        }
        if (!B(i, 15, 15) && B(i, 10, 10) && !B(i, 23, 21)) return simd_copy_fast(c, i);
        return false;
    case 0x0f:
        if (B(i, 31, 31)) return false;
        if (B(i, 10, 10) && B(i, 22, 19)) {
            int op = B(i, 15, 11);
            if (op == 0x1c || op == 0x1f) return simd_cvtfix_fast(c, i);
            return simd_shift_fast(c, i);
        }
        return false;
    }
    return false;
}

// ------------------------------------------------------------------ vector loads and stores

// Moves `bytes` (1, 2, 4, 8 or 16) between a vector register and memory; a load zeroes the rest of the register.
static inline void vreg_load(VReg *r, uint64_t addr, int bytes) {
    VReg t = {0};
    switch (bytes) {
    case 1: t.b[0] = *(volatile uint8_t *)addr; break;
    case 2: memcpy(&t, (void *)addr, 2); break;
    case 4: memcpy(&t, (void *)addr, 4); break;
    case 8: memcpy(&t, (void *)addr, 8); break;
    default: memcpy(&t, (void *)addr, 16); break;
    }
    *r = t;
}
static inline void vreg_store(const VReg *r, uint64_t addr, int bytes) {
    switch (bytes) {
    case 1: *(volatile uint8_t *)addr = r->b[0]; break;
    case 2: memcpy((void *)addr, r, 2); break;
    case 4: memcpy((void *)addr, r, 4); break;
    case 8: memcpy((void *)addr, r, 8); break;
    default: memcpy((void *)addr, r, 16); break;
    }
}

// LDR / STR (SIMD&FP register): unsigned offset, register offset, unscaled / pre- / post-indexed. false: not one of those.
static bool vmem_fast(CPU *c, uint32_t i) {
    int size = B(i, 31, 30), opc = B(i, 23, 22), rt = B(i, 4, 0), rn = B(i, 9, 5);
    int bytes = (opc & 2) ? 16 : 1 << size;
    if ((opc & 2) && size != 0) return false;
    int sc = (opc & 2) ? 4 : size;
    uint64_t base = rd_sp(c, rn), addr;
    bool wb = false;
    if (B(i, 25, 24) == 1) addr = base + ((uint64_t)B(i, 21, 10) << sc);
    else if (B(i, 21, 21)) {
        if (B(i, 11, 10) != 2 || (B(i, 15, 13) & 2) == 0) return false;
        addr = base + extend_val(rd(c, B(i, 20, 16)), B(i, 15, 13), B(i, 12, 12) ? sc : 0);
    } else {
        int idx = B(i, 11, 10);
        if (idx == 2) return false;
        int64_t imm = sext(B(i, 20, 12), 9);
        addr = idx == 1 ? base : base + imm;
        if (idx == 1 || idx == 3) wb = true, base += imm;
    }
    if (opc & 1) vreg_load(&c->vr[rt], addr, bytes); else vreg_store(&c->vr[rt], addr, bytes);
    if (wb) wr_sp(c, rn, base);
    return true;
}

// LDP / STP (SIMD&FP registers), LDNP / STNP.
static bool vpair_fast(CPU *c, uint32_t i) {
    int opc = B(i, 31, 30), L = B(i, 22, 22), idx = B(i, 24, 23), rt = B(i, 4, 0), rt2 = B(i, 14, 10), rn = B(i, 9, 5);
    if (opc == 3) return false;
    int scale = 2 + opc, bytes = 1 << scale;
    int64_t imm = sext(B(i, 21, 15), 7) * (1 << scale);
    uint64_t base = rd_sp(c, rn);
    uint64_t addr = idx == 1 ? base : base + imm;
    if (L) {
        VReg a, b;
        vreg_load(&a, addr, bytes); vreg_load(&b, addr + bytes, bytes);
        c->vr[rt] = a; c->vr[rt2] = b;
    } else {
        vreg_store(&c->vr[rt], addr, bytes); vreg_store(&c->vr[rt2], addr + bytes, bytes);
    }
    if (idx == 1 || idx == 3) wr_sp(c, rn, base + imm);
    return true;
}

// LD1 / ST1 with one to four registers (contiguous memory), and LD2..LD4 / ST2..ST4 (interleaved).
static bool vmulti_fast(CPU *c, uint32_t i) {
    if ((i & 0xbf200000) != 0x0c000000) return false;
    int Q = B(i, 30, 30), L = B(i, 22, 22), post = B(i, 23, 23), rm = B(i, 20, 16);
    int opcode = B(i, 15, 12), size = B(i, 11, 10), rn = B(i, 9, 5), rt = B(i, 4, 0);
    int ds = Q ? 16 : 8;
    uint64_t base = rd_sp(c, rn);
    int nreg;
    switch (opcode) {
    case 7: nreg = 1; break;
    case 10: nreg = 2; break;
    case 6: nreg = 3; break;
    case 2: nreg = 4; break;
    default: {  // LD2 / LD3 / LD4
        int selem = opcode == 8 ? 2 : opcode == 4 ? 3 : opcode == 0 ? 4 : 0;
        if (!selem) return false;
        int lanes = ds >> size;
        if (size == 3 && !Q) return false;
        VReg *o[4];
        VReg t[4];
        for (int s = 0; s < selem; s++) o[s] = L ? &t[s] : &c->vr[(rt + s) & 31];
        if (L) for (int s = 0; s < selem; s++) t[s] = (VReg){0};
#define INTERLEAVE(T)                                                                                           \
        {                                                                                                       \
            typedef T __attribute__((may_alias, aligned(1))) ta;                                                \
            ta *p = (ta *)base;                                                                                 \
            T *r0 = (T *)o[0], *r1 = (T *)o[1], *r2 = (T *)o[2], *r3 = (T *)o[3];                               \
            if (L) {                                                                                            \
                if (selem == 2) for (int e = 0; e < lanes; e++, p += 2) { r0[e] = p[0]; r1[e] = p[1]; }         \
                else if (selem == 3) for (int e = 0; e < lanes; e++, p += 3) { r0[e] = p[0]; r1[e] = p[1]; r2[e] = p[2]; } \
                else for (int e = 0; e < lanes; e++, p += 4) { r0[e] = p[0]; r1[e] = p[1]; r2[e] = p[2]; r3[e] = p[3]; } \
            } else {                                                                                            \
                if (selem == 2) for (int e = 0; e < lanes; e++, p += 2) { p[0] = r0[e]; p[1] = r1[e]; }         \
                else if (selem == 3) for (int e = 0; e < lanes; e++, p += 3) { p[0] = r0[e]; p[1] = r1[e]; p[2] = r2[e]; } \
                else for (int e = 0; e < lanes; e++, p += 4) { p[0] = r0[e]; p[1] = r1[e]; p[2] = r2[e]; p[3] = r3[e]; } \
            }                                                                                                   \
        }
        switch (size) {
        case 0: INTERLEAVE(uint8_t) break;
        case 1: INTERLEAVE(uint16_t) break;
        case 2: INTERLEAVE(uint32_t) break;
        default: INTERLEAVE(uint64_t) break;
        }
#undef INTERLEAVE
        if (L) for (int s = 0; s < selem; s++) c->vr[(rt + s) & 31] = t[s];
        if (post) wr_sp(c, rn, base + (rm == 31 ? (uint64_t)(selem * ds) : rd(c, rm)));
        return true;
    }
    }
    uint64_t addr = base;
    for (int r = 0; r < nreg; r++, addr += ds) {
        VReg *v = &c->vr[(rt + r) & 31];
        if (L) vreg_load(v, addr, ds); else vreg_store(v, addr, ds);
    }
    if (post) wr_sp(c, rn, base + (rm == 31 ? (uint64_t)(nreg * ds) : rd(c, rm)));
    return true;
}
