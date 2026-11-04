// A Skia-style source-over blend with NEON (de-interleaving loads, widening multiplies, rounding shifts, saturating adds,
// narrowing shifts), checked against a plain C version of the same arithmetic.
#include <arm_neon.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    int n = 1 << 14;
    uint8_t *src = malloc(n * 4), *dst = malloc(n * 4), *ref = malloc(n * 4);
    for (int k = 0; k < n * 4; k++) { src[k] = (uint8_t)(k * 7 + (k >> 5)); dst[k] = (uint8_t)(k * 13 + (k >> 3)); }
    memcpy(ref, dst, n * 4);
    for (int rep = 0; rep < 4; rep++) {
        for (int k = 0; k < n * 4; k += 32) {
            uint8x8x4_t s = vld4_u8(src + k), d = vld4_u8(dst + k);
            uint16x8_t ia = vsubq_u16(vdupq_n_u16(255), vmovl_u8(s.val[3]));
            uint8x8x4_t o;
            for (int ch = 0; ch < 4; ch++) {
                uint16x8_t t = vmulq_u16(vmovl_u8(d.val[ch]), ia);
                t = vaddq_u16(t, vdupq_n_u16(128));
                t = vaddq_u16(t, vshrq_n_u16(t, 8));
                o.val[ch] = vqadd_u8(s.val[ch], vshrn_n_u16(t, 8));
            }
            vst4_u8(dst + k, o);
        }
        for (int k = 0; k < n * 4; k += 4) {
            unsigned ia = 255 - src[k + 3];
            for (int ch = 0; ch < 4; ch++) {
                unsigned t = ref[k + ch] * ia + 128;
                t += t >> 8;
                unsigned r = src[k + ch] + (t >> 8);
                ref[k + ch] = r > 255 ? 255 : (uint8_t)r;
            }
        }
    }
    uint32_t sum = 0;
    for (int k = 0; k < n * 4; k++) sum = sum * 31 + dst[k];
    printf("checksum %u\n", sum);
    printf(memcmp(dst, ref, n * 4) == 0 ? "blend ok\n" : "blend MISMATCH\n");
    return 0;
}
