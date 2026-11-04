// NEON conformance: every vector op is compared against a scalar C reference
// computed in the same (emulated) process. Prints FAIL lines and a summary.
#include <arm_neon.h>
#include <stdint.h>
typedef uint64_t u64; typedef int64_t i64; typedef uint32_t u32;

static long sys3(long n, long a, long b, long c) {
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x16 __asm__("x16") = n;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    return x0;
}
static void puts_(const char *s) { int n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static void putnum(u64 v) { char b[24]; int i = 24; b[--i] = ' '; do { b[--i] = '0' + v % 10; v /= 10; } while (v); sys3(4, 1, (long)(b + i), 24 - i); }
static void puthex8(unsigned v) { char b[3]; b[0] = "0123456789abcdef"[v >> 4]; b[1] = "0123456789abcdef"[v & 15]; b[2] = ' '; sys3(4, 1, (long)b, 3); }

static u64 rng_s = 0x9e3779b97f4a7c15ull;
static u64 rnd(void) { rng_s ^= rng_s >> 12; rng_s ^= rng_s << 25; rng_s ^= rng_s >> 27; return rng_s * 0x2545F4914F6CDD1Dull; }

// ---- reference helpers
#define SATS(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
static u64 ushl_ref(u64 x, int sh, int bits) { sh = (int8_t)sh; if (sh >= 0) return sh >= bits ? 0 : x << sh; return -sh >= bits ? 0 : x >> -sh; }
static i64 sshl_ref(i64 x, i64 y, int bits) { int sh = (int8_t)y; if (sh >= 0) return sh >= bits ? 0 : (i64)((u64)x << sh); return -sh >= bits ? (x < 0 ? -1 : 0) : x >> -sh; }
static uint8_t rbit8(uint8_t x) { uint8_t r = 0; for (int i = 0; i < 8; i++) if (x & (1 << i)) r |= 0x80 >> i; return r; }
static float fabsf_(float x) { return x < 0 ? -x : x; }
static float fz_(float r, float x) { return r == 0 ? (x < 0 || (x == 0 && 1 / x < 0) ? -0.0f : 0.0f) : r; }
static float ftrunc_(float x) { return fz_((float)(int)x, x); }
static float ffloor_(float x) { float t = (float)(int)x; if (t > x) t -= 1; return fz_(t, x); }
static float fceil_(float x) { float t = (float)(int)x; if (t < x) t += 1; return fz_(t, x); }
static float frne_(float x) { float t = (float)(int)x, d = x - t; int odd = (int)t & 1; if (d > 0.5f || (d == 0.5f && odd)) t += 1; else if (d < -0.5f || (d == -0.5f && odd)) t -= 1; return fz_(t, x); }

// ---- harness
#define BUF 256
static uint8_t A[BUF] __attribute__((aligned(16))), Bv[BUF] __attribute__((aligned(16)));
static uint8_t D[BUF] __attribute__((aligned(16))), R[BUF] __attribute__((aligned(16)));
typedef void (*testfn)(const uint8_t *, const uint8_t *, uint8_t *, uint8_t *);
static int g_pass, g_fail;

static void fill(int kind) {
    for (int i = 0; i < BUF; i++) { A[i] = (uint8_t)rnd(); Bv[i] = (uint8_t)rnd(); }
    if (kind == 1) for (int i = 0; i < 4; i++) {
        float *fa = (float *)A, *fb = (float *)Bv;
        fa[i] = (float)((int)(rnd() % 2000001) - 1000000) / (float)(rnd() % 999 + 1);
        fb[i] = (float)((int)(rnd() % 2000001) - 1000000) / (float)(rnd() % 999 + 1);
    }
    if (kind == 2) for (int i = 0; i < 2; i++) {
        double *fa = (double *)A, *fb = (double *)Bv;
        fa[i] = (double)((long)(rnd() % 2000001) - 1000000) / (double)(rnd() % 999 + 1);
        fb[i] = (double)((long)(rnd() % 2000001) - 1000000) / (double)(rnd() % 999 + 1);
    }
}
static void run(const char *name, testfn f, int kind) {
    int bad = 0;
    for (int it = 0; it < 200 && !bad; it++) {
        fill(kind);
        for (int i = 0; i < BUF; i++) D[i] = R[i] = 0;
        f(A, Bv, D, R);
        for (int i = 0; i < BUF; i++) if (D[i] != R[i]) {
            puts_("FAIL "); puts_(name); puts_(" iter "); putnum(it); puts_("byte "); putnum(i);
            puts_("dut "); puthex8(D[i]); puts_("ref "); puthex8(R[i]); puts_("\n"); bad = 1; break;
        }
    }
    if (bad) g_fail++; else g_pass++;
}

#define TESTFN(NAME, ET, OT, N, LD, ST, VEXPR, REXPR) \
static void NAME(const uint8_t *pa, const uint8_t *pb, uint8_t *d, uint8_t *r) { \
    const ET *a = (const ET *)pa, *b = (const ET *)pb; OT *o = (OT *)r; \
    __typeof__(LD(a)) va = LD(a), vb = LD(b); (void)vb; (void)b; \
    ST((OT *)d, VEXPR); \
    for (int k = 0; k < (N); k++) { ET x = a[k], y = b[k]; (void)y; o[k] = (OT)(REXPR); } }
#define T0(...) TESTFN(__VA_ARGS__)
#define T1(...) TESTFN(__VA_ARGS__)
#define T2(...) TESTFN(__VA_ARGS__)
#include "neon_tests.def"
#undef T0
#undef T1
#undef T2

#define SPEC(NAME) static void NAME(const uint8_t *pa, const uint8_t *pb, uint8_t *d, uint8_t *r)
#define VST(off, v) __builtin_memcpy(d + (off), &(v), sizeof(v))
#define A8 ((const uint8_t *)pa)
#define A16 ((const uint16_t *)pa)
#define A32 ((const uint32_t *)pa)
#define A64 ((const uint64_t *)pa)
#define B8 ((const uint8_t *)pb)
#define B16 ((const uint16_t *)pb)
#define B32 ((const uint32_t *)pb)
#define B64 ((const uint64_t *)pb)
#define R8 ((uint8_t *)r)
#define R16(o) ((uint16_t *)(r + (o)))
#define R32(o) ((uint32_t *)(r + (o)))
#define R64(o) ((uint64_t *)(r + (o)))

SPEC(widen_ops) {
    uint8x16_t va8 = vld1q_u8(pa), vb8 = vld1q_u8(pb);
    int8x16_t sa8 = vreinterpretq_s8_u8(va8);
    int16x8_t sa16 = vreinterpretq_s16_u8(va8), sb16 = vreinterpretq_s16_u8(vb8);
    uint16x8_t ua16 = vreinterpretq_u16_u8(va8), ub16 = vreinterpretq_u16_u8(vb8);
    int32x4_t sa32 = vreinterpretq_s32_u8(va8), sb32 = vreinterpretq_s32_u8(vb8);
    uint32x4_t ua32 = vreinterpretq_u32_u8(va8), ub32 = vreinterpretq_u32_u8(vb8);
    int16x8_t t0 = vmovl_s8(vget_low_s8(sa8)); VST(0, t0);
    uint32x4_t t1 = vmovl_high_u16(ua16); VST(16, t1);
    int32x4_t t2 = vmull_s16(vget_low_s16(sa16), vget_low_s16(sb16)); VST(32, t2);
    uint64x2_t t3 = vmull_high_u32(ua32, ub32); VST(48, t3);
    uint16x8_t t4 = vaddl_u8(vget_low_u8(va8), vget_low_u8(vb8)); VST(64, t4);
    int32x4_t t5 = vsubl_s16(vget_low_s16(sa16), vget_low_s16(sb16)); VST(80, t5);
    uint16x8_t t6 = vabdl_u8(vget_low_u8(va8), vget_low_u8(vb8)); VST(96, t6);
    uint32x4_t t7 = vmlal_u16(ub32, vget_low_u16(ua16), vget_low_u16(ub16)); VST(112, t7);
    int32x4_t t8 = vaddw_s16(sa32, vget_low_s16(sb16)); VST(128, t8);
    int32x4_t t9 = vmlsl_s16(sa32, vget_low_s16(sb16), vget_low_s16(sa16)); VST(144, t9);
    int32x4_t t10 = vmull_high_s16(sa16, sb16); VST(160, t10);
    int16x8_t t11 = vshll_n_s8(vget_low_s8(sa8), 3); VST(176, t11);
    uint32x4_t t12 = vshll_n_u16(vget_low_u16(ua16), 5); VST(192, t12);
    uint64x2_t t13 = vmull_u32(vget_low_u32(ua32), vget_low_u32(ub32)); VST(208, t13);
    (void)sb32;
    for (int k = 0; k < 8; k++) R16(0)[k] = (int16_t)(int8_t)A8[k];
    for (int k = 0; k < 4; k++) R32(16)[k] = A16[4 + k];
    for (int k = 0; k < 4; k++) R32(32)[k] = (int32_t)((int16_t)A16[k] * (int16_t)B16[k]);
    for (int k = 0; k < 2; k++) R64(48)[k] = (u64)A32[2 + k] * B32[2 + k];
    for (int k = 0; k < 8; k++) R16(64)[k] = A8[k] + B8[k];
    for (int k = 0; k < 4; k++) R32(80)[k] = (int16_t)A16[k] - (int16_t)B16[k];
    for (int k = 0; k < 8; k++) R16(96)[k] = A8[k] > B8[k] ? A8[k] - B8[k] : B8[k] - A8[k];
    for (int k = 0; k < 4; k++) R32(112)[k] = B32[k] + (u32)A16[k] * B16[k];
    for (int k = 0; k < 4; k++) R32(128)[k] = (u32)((int32_t)A32[k] + (int16_t)B16[k]);
    for (int k = 0; k < 4; k++) R32(144)[k] = (u32)((int32_t)A32[k] - (int16_t)B16[k] * (int16_t)A16[k]);
    for (int k = 0; k < 4; k++) R32(160)[k] = (u32)((int16_t)A16[4 + k] * (int16_t)B16[4 + k]);
    for (int k = 0; k < 8; k++) R16(176)[k] = (uint16_t)((int8_t)A8[k] * 8);
    for (int k = 0; k < 4; k++) R32(192)[k] = (u32)A16[k] << 5;
    for (int k = 0; k < 2; k++) R64(208)[k] = (u64)A32[k] * B32[k];
}

SPEC(narrow_ops) {
    uint32x4_t ua = vld1q_u32((const uint32_t *)pa), ub = vld1q_u32((const uint32_t *)pb);
    int32x4_t sa = vreinterpretq_s32_u32(ua);
    int16x8_t sa16 = vreinterpretq_s16_u32(ua);
    uint16x8_t ua16 = vreinterpretq_u16_u32(ua);
    uint16x4_t t0 = vmovn_u32(ua); VST(0, t0);
    uint16x8_t t1 = vmovn_high_u32(vmovn_u32(ua), ub); VST(16, t1);
    int16x4_t t2 = vqmovn_s32(sa); VST(32, t2);
    uint16x4_t t3 = vqmovn_u32(ua); VST(48, t3);
    uint8x8_t t4 = vqmovun_s16(sa16); VST(64, t4);
    uint8x8_t t5 = vshrn_n_u16(ua16, 4); VST(80, t5);
    int16x4_t t6 = vqshrn_n_s32(sa, 5); VST(96, t6);
    uint8x8_t t7 = vrshrn_n_u16(ua16, 3); VST(112, t7);
    uint8x8_t t8 = vqmovn_u16(ua16); VST(128, t8);
    int8x8_t t9 = vqmovn_s16(sa16); VST(144, t9);
    const uint16_t *a16 = (const uint16_t *)pa; const int16_t *s16 = (const int16_t *)pa; const uint32_t *a32 = (const uint32_t *)pa;
    const int32_t *s32 = (const int32_t *)pa; const uint32_t *b32 = (const uint32_t *)pb;
    for (int k = 0; k < 4; k++) R16(0)[k] = (uint16_t)a32[k];
    for (int k = 0; k < 4; k++) { R16(16)[k] = (uint16_t)a32[k]; R16(16)[4 + k] = (uint16_t)b32[k]; }
    for (int k = 0; k < 4; k++) R16(32)[k] = (uint16_t)SATS(s32[k], -32768, 32767);
    for (int k = 0; k < 4; k++) R16(48)[k] = a32[k] > 65535 ? 65535 : (uint16_t)a32[k];
    for (int k = 0; k < 8; k++) R8[64 + k] = s16[k] < 0 ? 0 : s16[k] > 255 ? 255 : (uint8_t)s16[k];
    for (int k = 0; k < 8; k++) R8[80 + k] = (uint8_t)(a16[k] >> 4);
    for (int k = 0; k < 4; k++) R16(96)[k] = (uint16_t)SATS(s32[k] >> 5, -32768, 32767);
    for (int k = 0; k < 8; k++) R8[112 + k] = (uint8_t)((a16[k] + 4) >> 3);
    for (int k = 0; k < 8; k++) R8[128 + k] = a16[k] > 255 ? 255 : (uint8_t)a16[k];
    for (int k = 0; k < 8; k++) R8[144 + k] = (uint8_t)(int8_t)SATS((int)s16[k], -128, 127);
}

SPEC(reduce_ops) {
    uint8x16_t u8 = vld1q_u8(pa); uint16x8_t u16 = vld1q_u16((const uint16_t *)pa);
    uint32x4_t u32v = vld1q_u32((const uint32_t *)pa);
    int16x8_t s16 = vld1q_s16((const int16_t *)pa); int32x4_t s32 = vld1q_s32((const int32_t *)pa);
    u64 t[16];
    t[0] = vaddvq_u8(u8); t[1] = vaddvq_u16(u16); t[2] = (u32)vaddvq_s32(s32); t[3] = vmaxvq_u8(u8);
    t[4] = (u64)(int64_t)vminvq_s16(s16); t[5] = vmaxvq_u32(u32v); t[6] = vaddlvq_u8(u8);
    t[7] = (u64)(int64_t)vaddlvq_s16(s16); t[8] = vminvq_u8(u8); t[9] = (u32)vmaxvq_s32(s32);
    t[10] = vaddvq_u32(u32v);
    for (int k = 0; k < 11; k++) VST(8 * k, t[k]);
    unsigned s = 0, mx = 0, mn = 255; for (int k = 0; k < 16; k++) { s += A8[k]; if (A8[k] > mx) mx = A8[k]; if (A8[k] < mn) mn = A8[k]; }
    R64(0)[0] = (uint8_t)s; unsigned s2 = 0; for (int k = 0; k < 8; k++) s2 += A16[k]; R64(8)[0] = (uint16_t)s2;
    u32 s3 = 0; for (int k = 0; k < 4; k++) s3 += A32[k]; R64(16)[0] = s3; R64(24)[0] = mx;
    int16_t m16 = ((const int16_t *)pa)[0]; for (int k = 1; k < 8; k++) if (((const int16_t *)pa)[k] < m16) m16 = ((const int16_t *)pa)[k];
    R64(32)[0] = (u64)(i64)m16;
    u32 m32 = 0; for (int k = 0; k < 4; k++) if (A32[k] > m32) m32 = A32[k]; R64(40)[0] = m32;
    R64(48)[0] = s; i64 s16sum = 0; for (int k = 0; k < 8; k++) s16sum += (int16_t)A16[k]; R64(56)[0] = (u64)s16sum;
    R64(64)[0] = mn;
    int32_t x32 = (int32_t)A32[0]; for (int k = 1; k < 4; k++) if ((int32_t)A32[k] > x32) x32 = (int32_t)A32[k]; R64(72)[0] = (u32)x32;
    R64(80)[0] = s3;
    // pairwise
    uint32x4_t p0 = vpaddq_u32(u32v, vld1q_u32((const uint32_t *)pb)); VST(96, p0);
    uint8x16_t p1 = vpmaxq_u8(u8, vld1q_u8(pb)); VST(112, p1);
    uint16x8_t p2 = vpaddlq_u8(u8); VST(128, p2);
    int32x4_t p3 = vpaddlq_s16(s16); VST(144, p3);
    uint16x8_t p4 = vpadalq_u8(vld1q_u16((const uint16_t *)pb), u8); VST(160, p4);
    R32(96)[0] = A32[0] + A32[1]; R32(96)[1] = A32[2] + A32[3]; R32(96)[2] = B32[0] + B32[1]; R32(96)[3] = B32[2] + B32[3];
    for (int k = 0; k < 8; k++) { uint8_t x = A8[2 * k], y = A8[2 * k + 1]; R8[112 + k] = x > y ? x : y; x = B8[2 * k]; y = B8[2 * k + 1]; R8[120 + k] = x > y ? x : y; }
    for (int k = 0; k < 8; k++) R16(128)[k] = A8[2 * k] + A8[2 * k + 1];
    for (int k = 0; k < 4; k++) R32(144)[k] = (u32)((int16_t)A16[2 * k] + (int16_t)A16[2 * k + 1]);
    for (int k = 0; k < 8; k++) R16(160)[k] = B16[k] + A8[2 * k] + A8[2 * k + 1];
}

SPEC(reduce_f32) {  // float inputs
    float32x4_t v = vld1q_f32((const float *)pa), w = vld1q_f32((const float *)pb);
    const float *a = (const float *)pa, *b = (const float *)pb;
    float t0 = vaddvq_f32(v), t1 = vmaxvq_f32(v), t2 = vminvq_f32(v);
    VST(0, t0); VST(4, t1); VST(8, t2);
    float32x4_t t3 = vpaddq_f32(v, w); VST(16, t3);
    float32x2_t t4 = vadd_f32(vget_low_f32(v), vget_high_f32(w)); VST(32, t4);
    float *rf = (float *)r;
    rf[0] = (a[0] + a[1]) + (a[2] + a[3]);
    float mx = a[0], mn = a[0]; for (int k = 1; k < 4; k++) { if (a[k] > mx) mx = a[k]; if (a[k] < mn) mn = a[k]; }
    rf[1] = mx; rf[2] = mn;
    rf[4] = a[0] + a[1]; rf[5] = a[2] + a[3]; rf[6] = b[0] + b[1]; rf[7] = b[2] + b[3];
    rf[8] = a[0] + b[2]; rf[9] = a[1] + b[3];
}

SPEC(permute_ops) {
    uint8x16_t a = vld1q_u8(pa), b = vld1q_u8(pb);
    uint16x8_t a16 = vreinterpretq_u16_u8(a), b16 = vreinterpretq_u16_u8(b);
    uint32x4_t a32 = vreinterpretq_u32_u8(a), b32 = vreinterpretq_u32_u8(b);
    uint64x2_t a64 = vreinterpretq_u64_u8(a), b64 = vreinterpretq_u64_u8(b);
    uint8x16_t t0 = vzip1q_u8(a, b), t1 = vzip2q_u8(a, b), t2 = vuzp1q_u8(a, b), t3 = vuzp2q_u8(a, b), t4 = vtrn1q_u8(a, b), t5 = vtrn2q_u8(a, b);
    VST(0, t0); VST(16, t1); VST(32, t2); VST(48, t3); VST(64, t4); VST(80, t5);
    uint16x8_t u0 = vzip1q_u16(a16, b16), u1 = vuzp2q_u16(a16, b16), u2 = vtrn1q_u16(a16, b16), u3 = vzip2q_u16(a16, b16);
    VST(96, u0); VST(112, u1); VST(128, u2); VST(144, u3);
    uint32x4_t w0 = vzip2q_u32(a32, b32), w1 = vuzp1q_u32(a32, b32), w2 = vtrn2q_u32(a32, b32);
    VST(160, w0); VST(176, w1); VST(192, w2);
    uint64x2_t q0 = vzip1q_u64(a64, b64), q1 = vzip2q_u64(a64, b64);
    VST(208, q0); VST(224, q1);
    for (int k = 0; k < 8; k++) {
        R8[2 * k] = A8[k]; R8[2 * k + 1] = B8[k];
        R8[16 + 2 * k] = A8[8 + k]; R8[16 + 2 * k + 1] = B8[8 + k];
        R8[32 + k] = A8[2 * k]; R8[40 + k] = B8[2 * k];
        R8[48 + k] = A8[2 * k + 1]; R8[56 + k] = B8[2 * k + 1];
        R8[64 + 2 * k] = A8[2 * k]; R8[64 + 2 * k + 1] = B8[2 * k];
        R8[80 + 2 * k] = A8[2 * k + 1]; R8[80 + 2 * k + 1] = B8[2 * k + 1];
    }
    for (int k = 0; k < 4; k++) {
        R16(96)[2 * k] = A16[k]; R16(96)[2 * k + 1] = B16[k];
        R16(112)[k] = A16[2 * k + 1]; R16(112)[4 + k] = B16[2 * k + 1];
        R16(128)[2 * k] = A16[2 * k]; R16(128)[2 * k + 1] = B16[2 * k];
        R16(144)[2 * k] = A16[4 + k]; R16(144)[2 * k + 1] = B16[4 + k];
    }
    R32(160)[0] = A32[2]; R32(160)[1] = B32[2]; R32(160)[2] = A32[3]; R32(160)[3] = B32[3];
    R32(176)[0] = A32[0]; R32(176)[1] = A32[2]; R32(176)[2] = B32[0]; R32(176)[3] = B32[2];
    R32(192)[0] = A32[1]; R32(192)[1] = B32[1]; R32(192)[2] = A32[3]; R32(192)[3] = B32[3];
    R64(208)[0] = A64[0]; R64(208)[1] = B64[0]; R64(224)[0] = A64[1]; R64(224)[1] = B64[1];
}

SPEC(ext_rev_ops) {
    uint8x16_t a = vld1q_u8(pa), b = vld1q_u8(pb);
    uint8x16_t t0 = vextq_u8(a, b, 5); VST(0, t0);
    uint8x16_t t1 = vextq_u8(a, b, 15); VST(16, t1);
    uint32x4_t t2 = vextq_u32(vreinterpretq_u32_u8(a), vreinterpretq_u32_u8(b), 1); VST(32, t2);
    uint8x16_t t3 = vrev64q_u8(a); VST(48, t3);
    uint16x8_t t4 = vrev64q_u16(vreinterpretq_u16_u8(a)); VST(64, t4);
    uint32x4_t t5 = vrev64q_u32(vreinterpretq_u32_u8(a)); VST(80, t5);
    uint8x16_t t6 = vrev32q_u8(a); VST(96, t6);
    uint16x8_t t7 = vrev32q_u16(vreinterpretq_u16_u8(a)); VST(112, t7);
    uint8x16_t t8 = vrev16q_u8(a); VST(128, t8);
    uint8x8_t t9 = vext_u8(vget_low_u8(a), vget_low_u8(b), 3); VST(144, t9);
    uint8x8_t t10 = vzip1_u8(vget_low_u8(a), vget_low_u8(b)); VST(160, t10);
    uint8x8_t t11 = vrev64_u8(vget_low_u8(a)); VST(176, t11);
    uint8_t cat[32]; for (int k = 0; k < 16; k++) { cat[k] = A8[k]; cat[16 + k] = B8[k]; }
    for (int k = 0; k < 16; k++) { R8[k] = cat[5 + k]; R8[16 + k] = cat[15 + k]; }
    for (int k = 0; k < 16; k++) R8[32 + k] = cat[4 + k];
    for (int k = 0; k < 16; k++) { R8[48 + k] = A8[(k & ~7) + 7 - (k & 7)]; R8[96 + k] = A8[(k & ~3) + 3 - (k & 3)]; R8[128 + k] = A8[k ^ 1]; }
    for (int k = 0; k < 8; k++) R16(64)[k] = A16[(k & ~3) + 3 - (k & 3)];
    for (int k = 0; k < 4; k++) R32(80)[k] = A32[k ^ 1];
    for (int k = 0; k < 8; k++) R16(112)[k] = A16[k ^ 1];
    for (int k = 0; k < 8; k++) { R8[144 + k] = k + 3 < 8 ? A8[k + 3] : B8[k + 3 - 8]; R8[176 + k] = A8[7 - k]; }
    for (int k = 0; k < 4; k++) { R8[160 + 2 * k] = A8[k]; R8[160 + 2 * k + 1] = B8[k]; }
}

SPEC(tbl_ops) {
    uint8x16_t a = vld1q_u8(pa), b = vld1q_u8(pb);
    uint8x16_t t0 = vqtbl1q_u8(a, b); VST(0, t0);
    uint8x16_t t1 = vqtbx1q_u8(a, b, vshrq_n_u8(a, 3)); VST(16, t1);
    uint8x16x2_t tab = {{a, b}};
    uint8x16_t t2 = vqtbl2q_u8(tab, vshrq_n_u8(vmvnq_u8(a), 2)); VST(32, t2);
    uint8x8_t t3 = vtbl1_u8(vget_low_u8(a), vget_low_u8(b)); VST(48, t3);
    uint8x16_t t4 = vqtbx2q_u8(b, tab, vshrq_n_u8(vmvnq_u8(b), 2)); VST(64, t4);
    for (int k = 0; k < 16; k++) {
        uint8_t i0 = B8[k]; R8[k] = i0 < 16 ? A8[i0] : 0;
        uint8_t i1 = A8[k] >> 3; R8[16 + k] = i1 < 16 ? B8[i1] : A8[k];
        uint8_t i2 = (uint8_t)~A8[k] >> 2; R8[32 + k] = i2 < 16 ? A8[i2] : i2 < 32 ? B8[i2 - 16] : 0;
        uint8_t i4 = (uint8_t)~B8[k] >> 2; R8[64 + k] = i4 < 16 ? A8[i4] : i4 < 32 ? B8[i4 - 16] : B8[k];
    }
    for (int k = 0; k < 8; k++) { uint8_t i3 = B8[k]; R8[48 + k] = i3 < 8 ? A8[i3] : 0; }
}

SPEC(lane_ops) {
    uint8x16_t a = vld1q_u8(pa), b = vld1q_u8(pb);
    uint32x4_t a32 = vreinterpretq_u32_u8(a);
    uint8x16_t t0 = vdupq_laneq_u8(a, 9); VST(0, t0);
    uint16x8_t t1 = vdupq_laneq_u16(vreinterpretq_u16_u8(a), 5); VST(16, t1);
    uint32x4_t t2 = vdupq_laneq_u32(a32, 2); VST(32, t2);
    uint64x2_t t3 = vdupq_laneq_u64(vreinterpretq_u64_u8(a), 1); VST(48, t3);
    uint32x2_t t4 = vdup_laneq_u32(a32, 3); VST(64, t4);
    uint16x8_t t5 = vdupq_n_u16(((const uint16_t *)pb)[3]); VST(80, t5);
    uint64_t g[8];
    g[0] = vgetq_lane_u8(a, 5); g[1] = (u64)(int64_t)(int32_t)vgetq_lane_s8(vreinterpretq_s8_u8(a), 14);
    g[2] = vgetq_lane_u16(vreinterpretq_u16_u8(a), 3); g[3] = (u64)(int64_t)(int32_t)vgetq_lane_s16(vreinterpretq_s16_u8(a), 7);
    g[4] = vgetq_lane_u32(a32, 1); g[5] = (u64)(int64_t)vgetq_lane_s32(vreinterpretq_s32_u8(a), 3);
    g[6] = vgetq_lane_u64(vreinterpretq_u64_u8(a), 1);
    for (int k = 0; k < 7; k++) VST(96 + 8 * k, g[k]);
    uint8x16_t t6 = vsetq_lane_u8(B8[3], a, 11); VST(160, t6);
    uint32x4_t t7 = vsetq_lane_u32(B32[1], a32, 0); VST(176, t7);
    uint64x2_t t8 = vsetq_lane_u64(B64[0], vreinterpretq_u64_u8(a), 1); VST(192, t8);
    uint8x16_t t9 = vcopyq_laneq_u8(a, 4, b, 9); VST(208, t9);
    uint32x4_t t10 = vcopyq_laneq_u32(a32, 3, vreinterpretq_u32_u8(b), 0); VST(224, t10);
    uint64x2_t t11 = vcombine_u64(vget_high_u64(vreinterpretq_u64_u8(b)), vget_low_u64(vreinterpretq_u64_u8(a))); VST(240, t11);
    for (int k = 0; k < 16; k++) R8[k] = A8[9];
    for (int k = 0; k < 8; k++) R16(16)[k] = A16[5];
    for (int k = 0; k < 4; k++) R32(32)[k] = A32[2];
    R64(48)[0] = R64(48)[1] = A64[1];
    R32(64)[0] = R32(64)[1] = A32[3];
    for (int k = 0; k < 8; k++) R16(80)[k] = B16[3];
    R64(96)[0] = A8[5]; R64(104)[0] = (u64)(int64_t)(int8_t)A8[14]; R64(112)[0] = A16[3]; R64(120)[0] = (u64)(int64_t)(int16_t)A16[7];
    R64(128)[0] = A32[1]; R64(136)[0] = (u64)(int64_t)(int32_t)A32[3]; R64(144)[0] = A64[1];
    for (int k = 0; k < 16; k++) { R8[160 + k] = A8[k]; R8[208 + k] = A8[k]; }
    R8[160 + 11] = B8[3]; R8[208 + 4] = B8[9];
    for (int k = 0; k < 4; k++) { R32(176)[k] = A32[k]; R32(224)[k] = A32[k]; }
    R32(176)[0] = B32[1]; R32(224)[3] = B32[0];
    R64(192)[0] = A64[0]; R64(192)[1] = B64[0];
    R64(240)[0] = B64[1]; R64(240)[1] = A64[0];
}

SPEC(ldst_ops) {
    uint8x16x2_t l2 = vld2q_u8(pa); VST(0, l2);
    uint8x16x3_t l3 = vld3q_u8(pa); VST(32, l3);
    uint16x8x4_t l4 = vld4q_u16((const uint16_t *)pa); VST(80, l4);
    uint32x4x2_t l5 = vld2q_u32((const uint32_t *)pa); VST(144, l5);
    uint16x8_t d1 = vld1q_dup_u16((const uint16_t *)(pa + 6)); VST(176, d1);
    uint64x2_t d2 = vld1q_dup_u64((const uint64_t *)(pa + 8)); VST(192, d2);
    uint8x16_t ln = vld1q_lane_u8(pa + 3, vld1q_u8(pb), 13); VST(208, ln);
    uint8x8_t l8 = vld1_u8(pa + 8); VST(224, l8);
    uint8x16_t b = vld1q_u8(pb);
    vst1q_lane_u16((uint16_t *)(d + 232), vreinterpretq_u16_u8(b), 6);
    vst1q_lane_u32((uint32_t *)(d + 236), vreinterpretq_u32_u8(b), 2);
    for (int k = 0; k < 16; k++) { R8[k] = A8[2 * k]; R8[16 + k] = A8[2 * k + 1]; }
    for (int k = 0; k < 16; k++) { R8[32 + k] = A8[3 * k]; R8[48 + k] = A8[3 * k + 1]; R8[64 + k] = A8[3 * k + 2]; }
    for (int k = 0; k < 8; k++) { R16(80)[k] = A16[4 * k]; R16(80)[8 + k] = A16[4 * k + 1]; R16(80)[16 + k] = A16[4 * k + 2]; R16(80)[24 + k] = A16[4 * k + 3]; }
    for (int k = 0; k < 4; k++) { R32(144)[k] = A32[2 * k]; R32(144)[4 + k] = A32[2 * k + 1]; }
    for (int k = 0; k < 8; k++) R16(176)[k] = A16[3];
    R64(192)[0] = R64(192)[1] = A64[1];
    for (int k = 0; k < 16; k++) R8[208 + k] = B8[k];
    R8[208 + 13] = A8[3];
    for (int k = 0; k < 8; k++) R8[224 + k] = A8[8 + k];
    R16(232)[0] = B16[6]; R32(236)[0] = B32[2];
}

SPEC(ldst_store_ops) {
    uint8x16_t a = vld1q_u8(pa), b = vld1q_u8(pb), c = vld1q_u8(pa + 16), e = vld1q_u8(pb + 16);
    uint8x16x2_t s2 = {{a, b}}; vst2q_u8(d, s2);
    uint8x16x3_t s3 = {{a, b, c}}; vst3q_u8(d + 32, s3);
    uint8x16x4_t s4 = {{a, b, c, e}}; vst4q_u8(d + 80, s4);
    vst1q_u8_x2(d + 144, s2);
    uint16x8x2_t s5 = {{vreinterpretq_u16_u8(a), vreinterpretq_u16_u8(b)}}; vst2q_u16((uint16_t *)(d + 176), s5);
    for (int k = 0; k < 16; k++) { R8[2 * k] = A8[k]; R8[2 * k + 1] = B8[k]; }
    for (int k = 0; k < 16; k++) { R8[32 + 3 * k] = A8[k]; R8[32 + 3 * k + 1] = B8[k]; R8[32 + 3 * k + 2] = A8[16 + k]; }
    for (int k = 0; k < 16; k++) { R8[80 + 4 * k] = A8[k]; R8[80 + 4 * k + 1] = B8[k]; R8[80 + 4 * k + 2] = A8[16 + k]; R8[80 + 4 * k + 3] = B8[16 + k]; }
    for (int k = 0; k < 16; k++) { R8[144 + k] = A8[k]; R8[160 + k] = B8[k]; }
    for (int k = 0; k < 8; k++) { R16(176)[2 * k] = A16[k]; R16(176)[2 * k + 1] = B16[k]; }
}

// Post-indexed vector loads/stores in a loop.
SPEC(loop_ops) {
    uint32x4_t acc = vdupq_n_u32(0);
    const uint8_t *p = pa;
    for (int k = 0; k < 8; k++) { acc = vaddq_u32(acc, vld1q_u32((const uint32_t *)p)); p += 16; }
    VST(0, acc);
    u32 s[4] = {0, 0, 0, 0};
    for (int k = 0; k < 8; k++) for (int j = 0; j < 4; j++) s[j] += A32[4 * k + j];
    for (int j = 0; j < 4; j++) R32(0)[j] = s[j];
    uint8_t *q = d + 16;
    for (int k = 0; k < 4; k++) { vst1q_u8(q, vld1q_u8(pb + 16 * k)); q += 16; }
    for (int k = 0; k < 64; k++) R8[16 + k] = B8[k];
}

volatile uint32_t vk32 = 0x5a, vk16 = 0x12, vkf = 0x3fc00000;
volatile uint64_t vk64 = 0xff00ff00ff00ff00ull;
SPEC(imm_ops) {
    uint8x16_t va = vld1q_u8(pa);
    uint8x16_t c0 = vdupq_n_u8(0x5a); VST(0, c0);
    uint16x8_t c1 = vdupq_n_u16(0x1200); VST(16, c1);
    uint16x8_t c2 = vdupq_n_u16(0x0034); VST(32, c2);
    uint32x4_t c3 = vdupq_n_u32(0x0000ab00); VST(48, c3);
    uint32x4_t c4 = vdupq_n_u32(0x00cd0000); VST(64, c4);
    uint32x4_t c5 = vdupq_n_u32(0xef000000); VST(80, c5);
    uint32x4_t c6 = vdupq_n_u32(0xffffff00u); VST(96, c6);
    uint32x4_t c7 = vdupq_n_u32(0x0000ffffu); VST(112, c7);
    uint32x4_t c8 = vdupq_n_u32(0x00ffffffu); VST(128, c8);
    uint64x2_t c9 = vdupq_n_u64(0xff00ff00ff00ff00ull); VST(144, c9);
    float32x4_t c10 = vdupq_n_f32(1.5f); VST(160, c10);
    float64x2_t c11 = vdupq_n_f64(-3.0); VST(176, c11);
    uint32x4_t c12 = vorrq_u32(vreinterpretq_u32_u8(va), vdupq_n_u32(0x00ff0000)); VST(192, c12);
    uint16x8_t c13 = vbicq_u16(vreinterpretq_u16_u8(va), vdupq_n_u16(0x0f00)); VST(208, c13);
    uint8x8_t c14 = vdup_n_u8(0xa5); VST(224, c14);
    uint64x2_t c15 = vdupq_n_u64(0); VST(232, c15);
    uint32_t k5a = vk32, k12 = vk16;
    for (int k = 0; k < 16; k++) R8[k] = k5a;
    for (int k = 0; k < 8; k++) { R16(16)[k] = k12 << 8; R16(32)[k] = k12 + 0x22; }
    for (int k = 0; k < 4; k++) {
        R32(48)[k] = (k5a + 0x51) << 8; R32(64)[k] = (0xcd0000 ^ (k5a & 0)); R32(80)[k] = 0xef000000u ^ (k5a & 0);
        R32(96)[k] = ~(uint32_t)(k5a & 0 ? 1 : 0xff); R32(112)[k] = 0xffffu ^ (k5a & 0); R32(128)[k] = 0xffffffu ^ (k5a & 0);
    }
    for (int k = 0; k < 2; k++) R64(144)[k] = vk64;
    for (int k = 0; k < 4; k++) R32(160)[k] = vkf;
    for (int k = 0; k < 2; k++) { double dd = -3.0 * (vk32 ? 1 : 0); R64(176)[k] = *(uint64_t *)&dd; }
    for (int k = 0; k < 4; k++) R32(192)[k] = A32[k] | (0xff0000u ^ (vk32 & 0));
    for (int k = 0; k < 8; k++) R16(208)[k] = A16[k] & ~(0x0f00u ^ (vk32 & 0));
    for (int k = 0; k < 8; k++) R8[224 + k] = 0xa5 ^ (vk32 & 0);
}

int main(void);
void start(void) {
#define T0(name, ...) run(#name, name, 0);
#define T1(name, ...) run(#name, name, 1);
#define T2(name, ...) run(#name, name, 2);
#include "neon_tests.def"
#undef T0
#undef T1
#undef T2
    run("widen_ops", widen_ops, 0); run("narrow_ops", narrow_ops, 0); run("reduce_ops", reduce_ops, 0);
    run("reduce_f32", reduce_f32, 1); run("permute_ops", permute_ops, 0); run("ext_rev_ops", ext_rev_ops, 0);
    run("tbl_ops", tbl_ops, 0); run("lane_ops", lane_ops, 0); run("ldst_ops", ldst_ops, 0);
    run("ldst_store_ops", ldst_store_ops, 0); run("loop_ops", loop_ops, 0); run("imm_ops", imm_ops, 0);
    puts_("pass "); putnum(g_pass); puts_("fail "); putnum(g_fail); puts_("\n");
    sys3(1, g_fail ? 1 : 0, 0, 0);
}
