// Half-precision (FP16) vector and scalar arithmetic: arm64 NEON intrinsics against plain _Float16 code on x86_64.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

typedef _Float16 h;
static const float in_a[16] = {1.5f, -2.25f, 3.0f, 0.5f, 100.25f, -0.125f, 7.0f, 3000.0f, 0.1f, -3.5f, 12.0f, 0.75f, 2.0f, -8.0f, 4.5f, 9.0f};
static const float in_b[16] = {0.5f, 4.0f, -1.5f, 8.0f, 2.0f, 0.25f, -3.0f, 1.0f, 6.0f, 2.5f, -0.5f, 3.0f, 0.25f, 5.0f, -1.0f, 0.5f};

static void show(const char *name, const h *r, int n) {
    printf("%-8s", name);
    for (int k = 0; k < n; k++) {
        uint16_t u; memcpy(&u, &r[k], 2);
        if (isnan((float)r[k])) printf(" nan"); else printf(" %04x", u);
    }
    printf("\n");
}
static void showi(const char *name, const int16_t *r, int n) {
    printf("%-8s", name);
    for (int k = 0; k < n; k++) printf(" %d", r[k]);
    printf("\n");
}
static void showu(const char *name, const uint16_t *r, int n) {
    printf("%-8s", name);
    for (int k = 0; k < n; k++) printf(" %04x", r[k]);
    printf("\n");
}

int main(void) {
    h a[16], b[16], r[16];
    for (int k = 0; k < 16; k++) { a[k] = (h)in_a[k]; b[k] = (h)in_b[k]; }
    for (int blk = 0; blk < 2; blk++) {
        const h *pa = a + blk * 8, *pb = b + blk * 8;
        printf("-- block %d\n", blk);
#ifdef __aarch64__
        float16x8_t va = vld1q_f16((const __fp16 *)pa), vb = vld1q_f16((const __fp16 *)pb);
#define OUT(vec) vst1q_f16((__fp16 *)r, vec)
#define OP2(name, vexpr, sexpr) OUT(vexpr); show(name, r, 8)
        float16x4_t la = vget_low_f16(va), lb = vget_low_f16(vb);
        OP2("add", vaddq_f16(va, vb), 0);
        OP2("sub", vsubq_f16(va, vb), 0);
        OP2("mul", vmulq_f16(va, vb), 0);
        OP2("div", vdivq_f16(va, vb), 0);
        OP2("neg", vnegq_f16(va), 0);
        OP2("abs", vabsq_f16(va), 0);
        OP2("fma", vfmaq_f16(vb, va, vb), 0);
        OP2("fms", vfmsq_f16(vb, va, vb), 0);
        OP2("sqrt", vsqrtq_f16(vabsq_f16(va)), 0);
        OP2("max", vmaxq_f16(va, vb), 0);
        OP2("min", vminq_f16(va, vb), 0);
        OP2("abd", vabdq_f16(va, vb), 0);
        OP2("rndz", vrndq_f16(va), 0);
        OP2("rndn", vrndnq_f16(va), 0);
        OP2("rndm", vrndmq_f16(va), 0);
        OP2("rndp", vrndpq_f16(va), 0);
        OP2("lane", vmulq_laneq_f16(va, vb, 3), 0);
        OP2("fmal", vfmaq_laneq_f16(vb, va, vb, 5), 0);
        OP2("cvtf", vcvtq_f16_s16(vcvtq_s16_f16(va)), 0);
        vst1_f16((__fp16 *)r, vadd_f16(la, lb)); show("add4", r, 4);
        vst1_f16((__fp16 *)r, vneg_f16(la)); show("neg4", r, 4);
        vst1_f16((__fp16 *)r, vmul_f16(la, lb)); show("mul4", r, 4);
        { int16_t ir[8]; vst1q_s16(ir, vcvtq_s16_f16(va)); showi("tos16", ir, 8); }
        { uint16_t ur[8]; vst1q_u16(ur, vceqq_f16(va, vb)); showu("ceq", ur, 8);
          vst1q_u16(ur, vcgtq_f16(va, vb)); showu("cgt", ur, 8);
          vst1q_u16(ur, vcgeq_f16(va, vb)); showu("cge", ur, 8);
          vst1q_u16(ur, vcltzq_f16(va)); showu("cltz", ur, 8);
          vst1q_u16(ur, vcgtzq_f16(va)); showu("cgtz", ur, 8); }
        { h m = (h)vmaxvq_f16(va), n = (h)vminvq_f16(va); show("maxv", &m, 1); show("minv", &n, 1); }
        { h m = (h)vmaxv_f16(la), n = (h)vminv_f16(la); show("maxv4", &m, 1); show("minv4", &n, 1); }
#else
#define EACH(name, expr) do { for (int k = 0; k < 8; k++) { h x = pa[k], y = pb[k]; (void)y; r[k] = (expr); } show(name, r, 8); } while (0)
        EACH("add", (h)((float)x + (float)y));
        EACH("sub", (h)((float)x - (float)y));
        EACH("mul", (h)((float)x * (float)y));
        EACH("div", (h)((float)x / (float)y));
        EACH("neg", (h)(-(float)x));
        EACH("abs", (h)fabsf((float)x));
        EACH("fma", (h)((float)y + (float)x * (float)y));
        EACH("fms", (h)((float)y - (float)x * (float)y));
        EACH("sqrt", (h)sqrtf(fabsf((float)x)));
        EACH("max", (h)((float)x > (float)y ? (float)x : (float)y));
        EACH("min", (h)((float)x < (float)y ? (float)x : (float)y));
        EACH("abd", (h)fabsf((float)x - (float)y));
        EACH("rndz", (h)truncf((float)x));
        EACH("rndn", (h)nearbyintf((float)x));
        EACH("rndm", (h)floorf((float)x));
        EACH("rndp", (h)ceilf((float)x));
        EACH("lane", (h)((float)x * (float)pb[3]));
        EACH("fmal", (h)((float)y + (float)x * (float)pb[5]));
        EACH("cvtf", (h)(float)(int16_t)(float)x);
        for (int k = 0; k < 4; k++) r[k] = (h)((float)pa[k] + (float)pb[k]); show("add4", r, 4);
        for (int k = 0; k < 4; k++) r[k] = (h)(-(float)pa[k]); show("neg4", r, 4);
        for (int k = 0; k < 4; k++) r[k] = (h)((float)pa[k] * (float)pb[k]); show("mul4", r, 4);
        { int16_t ir[8]; for (int k = 0; k < 8; k++) ir[k] = (int16_t)(float)pa[k]; showi("tos16", ir, 8); }
        { uint16_t ur[8];
          for (int k = 0; k < 8; k++) ur[k] = (float)pa[k] == (float)pb[k] ? 0xffff : 0; showu("ceq", ur, 8);
          for (int k = 0; k < 8; k++) ur[k] = (float)pa[k] > (float)pb[k] ? 0xffff : 0; showu("cgt", ur, 8);
          for (int k = 0; k < 8; k++) ur[k] = (float)pa[k] >= (float)pb[k] ? 0xffff : 0; showu("cge", ur, 8);
          for (int k = 0; k < 8; k++) ur[k] = (float)pa[k] < 0 ? 0xffff : 0; showu("cltz", ur, 8);
          for (int k = 0; k < 8; k++) ur[k] = (float)pa[k] > 0 ? 0xffff : 0; showu("cgtz", ur, 8); }
        { float m = (float)pa[0], n = m; for (int k = 1; k < 8; k++) { if ((float)pa[k] > m) m = (float)pa[k]; if ((float)pa[k] < n) n = (float)pa[k]; }
          h hm = (h)m, hn = (h)n; show("maxv", &hm, 1); show("minv", &hn, 1); }
        { float m = (float)pa[0], n = m; for (int k = 1; k < 4; k++) { if ((float)pa[k] > m) m = (float)pa[k]; if ((float)pa[k] < n) n = (float)pa[k]; }
          h hm = (h)m, hn = (h)n; show("maxv4", &hm, 1); show("minv4", &hn, 1); }
#endif
        // scalar half arithmetic
        volatile h sa = pa[0], sb = pb[1];
        h s[8] = {sa + sb, sa - sb, sa * sb, sa / sb, (h)sqrtf(fabsf((float)sa)), -sa, (h)fabsf((float)sa), (h)(sa < sb)};
        show("scalar", s, 8);
        volatile float f = (float)sa * 1.25f;
        volatile double d = (double)sb * 3.5;
        h c1 = (h)f, c2 = (h)d; float f2 = (float)sa; double d2 = (double)sb;
        h conv[2] = {c1, c2}; show("cvt", conv, 2);
        printf("cvtback %g %g\n", f2, d2);
        // scalar half: conversions to/from integers, rounding, min/max, select
        {
            volatile h x = pa[2], y = pb[4], z = pa[5];
            volatile int ik = (int)(float)x * 7 - 3;
            volatile unsigned uk = (unsigned)(ik < 0 ? -ik : ik);
            volatile long long lk = (long long)ik * 40;
            printf("toint %d %u %lld %d\n", (int)x, (unsigned)(float)(x < 0 ? -x : x), (long long)z, (int)(h)-2.75f);
            h fi[3] = {(h)ik, (h)uk, (h)lk};
            show("fromint", fi, 3);
            h rr[8];
#ifdef __aarch64__
            rr[0] = __builtin_floorf16(z); rr[1] = __builtin_ceilf16(z); rr[2] = __builtin_truncf16(z); rr[3] = __builtin_roundevenf16(z);
            rr[4] = __builtin_rintf16(z); rr[5] = __builtin_fminf16(x, y); rr[6] = __builtin_fmaxf16(x, y);
#else
            rr[0] = (h)floorf((float)z); rr[1] = (h)ceilf((float)z); rr[2] = (h)truncf((float)z); rr[3] = (h)nearbyintf((float)z);
            rr[4] = (h)rintf((float)z); rr[5] = (float)x < (float)y ? x : y; rr[6] = (float)x > (float)y ? x : y;
#endif
            rr[7] = x > y ? x : y;
            show("round", rr, 8);
            printf("cmp %d %d %d %d\n", x == y, x < y, x >= z, x != y);
        }
    }
    return 0;
}
