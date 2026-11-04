// SQDMULL / SQDMLAL / SQDMLSL (by element, vector, "2" and scalar forms) and SQRDMLAH / SQRDMLSH by element, against plain C formulas.
// Software renderers (Skia, SwiftShader) use them when there is no GPU.
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
static int bad, n;
static int64_t sat64(__int128 v) { return v > INT64_MAX ? INT64_MAX : v < INT64_MIN ? INT64_MIN : (int64_t)v; }
static int32_t sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }
static int16_t sat16(int64_t v) { return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v; }
#define CHECK(c) do { n++; if (!(c)) { bad++; if (bad < 6) printf("mismatch at %s:%d (seed %u)\n", __func__, __LINE__, seed); } } while (0)
static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
int main() {
    for (unsigned seed = 1; seed <= 400; seed++) {
        uint32_t r = seed * 7919u;
        int16_t a[8], bb[8]; int32_t acc2[4], a32[4], b32[4]; int64_t acc64[2];
        for (int k = 0; k < 8; k++) { a[k] = (int16_t)(rnd(&r) % 4 == 0 ? -32768 : rnd(&r)); bb[k] = (int16_t)(rnd(&r) % 4 == 0 ? -32768 : rnd(&r)); }
        for (int k = 0; k < 4; k++) { acc2[k] = (int32_t)(rnd(&r) % 5 == 0 ? INT32_MAX : rnd(&r) << 8); a32[k] = (int32_t)(rnd(&r) % 4 == 0 ? INT32_MIN : rnd(&r) << 8); b32[k] = (int32_t)(rnd(&r) % 4 == 0 ? INT32_MIN : rnd(&r) << 8); }
        for (int k = 0; k < 2; k++) acc64[k] = (int64_t)(((uint64_t)rnd(&r) << 32) | rnd(&r));
        int16x8_t va = vld1q_s16(a), vb = vld1q_s16(bb);
        int32x4_t vacc = vld1q_s32(acc2);
        int32x4_t m = vqdmull_laneq_s16(vget_low_s16(va), vb, 5), m2 = vqdmull_high_laneq_s16(va, vb, 2);
        int32x4_t la = vqdmlal_laneq_s16(vacc, vget_low_s16(va), vb, 7), ls = vqdmlsl_laneq_s16(vacc, vget_low_s16(va), vb, 3);
        int32_t g[4], g2[4], ga[4], gs[4]; vst1q_s32(g, m); vst1q_s32(g2, m2); vst1q_s32(ga, la); vst1q_s32(gs, ls);
        for (int k = 0; k < 4; k++) {
            int32_t p = sat32((int64_t)a[k] * bb[5] * 2), p2 = sat32((int64_t)a[k + 4] * bb[2] * 2);
            CHECK(g[k] == p); CHECK(g2[k] == p2);
            int64_t pa = (int64_t)a[k] * bb[7] * 2, ps = (int64_t)a[k] * bb[3] * 2;
            CHECK(ga[k] == sat32((int64_t)acc2[k] + pa)); CHECK(gs[k] == sat32((int64_t)acc2[k] - ps));
        }
        int32x4_t v32 = vld1q_s32(a32), w32 = vld1q_s32(b32);
        int64x2_t vacc64 = vld1q_s64(acc64);
        int64x2_t m64 = vqdmull_laneq_s32(vget_low_s32(v32), w32, 3), l64 = vqdmlal_lane_s32(vacc64, vget_high_s32(v32), vget_low_s32(w32), 1), s64 = vqdmlsl_laneq_s32(vacc64, vget_low_s32(v32), w32, 2);
        int64_t h[2], hl[2], hs[2]; vst1q_s64(h, m64); vst1q_s64(hl, l64); vst1q_s64(hs, s64);
        for (int k = 0; k < 2; k++) {
            CHECK(h[k] == sat64((__int128)a32[k] * b32[3] * 2));
            CHECK(hl[k] == sat64((__int128)acc64[k] + (__int128)a32[k + 2] * b32[1] * 2));
            CHECK(hs[k] == sat64((__int128)acc64[k] - (__int128)a32[k] * b32[2] * 2));
        }
        CHECK(vqdmullh_laneq_s16(a[1], vb, 6) == sat32((int64_t)a[1] * bb[6] * 2));
        CHECK(vqdmulls_laneq_s32(a32[2], w32, 1) == sat64((__int128)a32[2] * b32[1] * 2));
        CHECK(vqdmlals_lane_s32(acc64[0], a32[1], vget_low_s32(w32), 0) == sat64((__int128)acc64[0] + (__int128)a32[1] * b32[0] * 2));
        int16x8_t racc = vld1q_s16(bb);
        int16x8_t ra = vqrdmlahq_laneq_s16(racc, va, vb, 4), rs = vqrdmlshq_laneq_s16(racc, va, vb, 1);
        int16_t gr[8], gsr[8]; vst1q_s16(gr, ra); vst1q_s16(gsr, rs);
        for (int k = 0; k < 8; k++) {
            CHECK(gr[k] == sat16((((int64_t)bb[k] << 16) + (int64_t)a[k] * bb[4] * 2 + 0x8000) >> 16));
            CHECK(gsr[k] == sat16((((int64_t)bb[k] << 16) - (int64_t)a[k] * bb[1] * 2 + 0x8000) >> 16));
        }
    }
    printf("sqdmull indexed: %d checks, mismatches %d\n", n, bad);
    return 0;
}
