// ARMv8 crypto extension known-answer tests (AES-128, SHA-1, SHA-256, CRC32, PMULL), compiled with the
// extensions enabled; the expected values are standard test vectors.
#include <arm_acle.h>
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void hex(const uint8_t *p, int n) { for (int i = 0; i < n; i++) printf("%02x", p[i]); printf("\n"); }

__attribute__((target("aes"))) static void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    static const uint8_t rcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36};
    uint8_t rk[11][16]; memcpy(rk[0], key, 16);
    for (int r = 1; r <= 10; r++) {  // key schedule with the AES instructions' help
        uint8_t t[4] = {rk[r-1][13], rk[r-1][14], rk[r-1][15], rk[r-1][12]};
        uint8x16_t v = vdupq_n_u8(0); v = vsetq_lane_u8(t[0], v, 0); v = vsetq_lane_u8(t[1], v, 1); v = vsetq_lane_u8(t[2], v, 2); v = vsetq_lane_u8(t[3], v, 3);
        uint8x16_t sub = vaeseq_u8(v, vdupq_n_u8(0));  // SubBytes (ShiftRows keeps lane 0..3 of a uniform-shift pattern in column 0 only)
        uint8_t sb[16]; vst1q_u8(sb, sub);
        uint8_t w[4] = {sb[0] ^ rcon[r-1], sb[1], sb[2], sb[3]};
        // ShiftRows moves bytes between columns, so recompute the four SubBytes outputs from lane 0 of four separate runs
        uint8_t o[4];
        for (int k = 0; k < 4; k++) { uint8x16_t u = vsetq_lane_u8(t[k], vdupq_n_u8(0), 0); uint8_t q[16]; vst1q_u8(q, vaeseq_u8(u, vdupq_n_u8(0))); o[k] = q[0]; }
        w[0] = o[0] ^ rcon[r-1]; w[1] = o[1]; w[2] = o[2]; w[3] = o[3];
        for (int k = 0; k < 4; k++) rk[r][k] = rk[r-1][k] ^ w[k];
        for (int k = 4; k < 16; k++) rk[r][k] = rk[r-1][k] ^ rk[r][k-4];
    }
    uint8x16_t s = vld1q_u8(in);
    for (int r = 0; r < 9; r++) s = vaesmcq_u8(vaeseq_u8(s, vld1q_u8(rk[r])));
    s = vaeseq_u8(s, vld1q_u8(rk[9]));
    s = veorq_u8(s, vld1q_u8(rk[10]));
    vst1q_u8(out, s);
}

__attribute__((target("sha2"))) static void sha256_block(uint32_t st[8], const uint8_t blk[64]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32x4_t s0 = vld1q_u32(st), s1 = vld1q_u32(st + 4), a0 = s0, a1 = s1;
    uint32x4_t m0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blk))), m1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blk + 16)));
    uint32x4_t m2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blk + 32))), m3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blk + 48)));
    uint32x4_t w[4] = {m0, m1, m2, m3};
    for (int r = 0; r < 16; r++) {
        uint32x4_t cur = w[r & 3];
        if (r >= 4) {
            uint32x4_t prev0 = w[r & 3], prev1 = w[(r + 1) & 3], prev2 = w[(r + 2) & 3], prev3 = w[(r + 3) & 3];
            cur = vsha256su1q_u32(vsha256su0q_u32(prev0, prev1), prev2, prev3);
            w[r & 3] = cur;
        }
        uint32x4_t wk = vaddq_u32(cur, vld1q_u32(&K[r * 4]));
        uint32x4_t t = s0;
        s0 = vsha256hq_u32(s0, s1, wk);
        s1 = vsha256h2q_u32(s1, t, wk);
    }
    vst1q_u32(st, vaddq_u32(s0, a0)); vst1q_u32(st + 4, vaddq_u32(s1, a1));
}

__attribute__((target("sha2"))) static void sha1_block(uint32_t st[5], const uint8_t blk[64]) {
    uint32x4_t abcd = vld1q_u32(st), abcd0 = abcd; uint32_t e = st[4], e0 = e;
    uint32x4_t w[4];
    for (int k = 0; k < 4; k++) w[k] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blk + 16 * k)));
    static const uint32_t K[4] = {0x5a827999, 0x6ed9eba1, 0x8f1bbcdc, 0xca62c1d6};
    for (int r = 0; r < 20; r++) {
        uint32x4_t cur;
        if (r < 4) cur = w[r];
        else { cur = vsha1su1q_u32(veorq_u32(vsha1su0q_u32(w[(r) & 3], w[(r + 1) & 3], w[(r + 2) & 3]), vdupq_n_u32(0)), w[(r + 3) & 3]); w[r & 3] = cur; }
        uint32x4_t wk = vaddq_u32(cur, vdupq_n_u32(K[r / 5]));
        uint32_t e1 = vsha1h_u32(vgetq_lane_u32(abcd, 0));
        if (r < 5) abcd = vsha1cq_u32(abcd, e, wk); else if (r < 10) abcd = vsha1pq_u32(abcd, e, wk);
        else if (r < 15) abcd = vsha1mq_u32(abcd, e, wk); else abcd = vsha1pq_u32(abcd, e, wk);
        e = e1;
    }
    vst1q_u32(st, vaddq_u32(abcd, abcd0)); st[4] = e + e0;
}

__attribute__((target("crc,aes"))) int main(void) {
    uint8_t key[16], pt[16], ct[16];
    for (int i = 0; i < 16; i++) { key[i] = (uint8_t)i; pt[i] = (uint8_t)(i * 0x11); }
    aes128_encrypt(key, pt, ct);                       // FIPS-197 appendix C.1: 69c4e0d86a7b0430d8cdb78070b4c55a
    printf("aes128 "); hex(ct, 16);

    uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t blk[64] = {0}; blk[0] = 'a'; blk[1] = 'b'; blk[2] = 'c'; blk[3] = 0x80; blk[63] = 24;
    sha256_block(st, blk);                              // sha256("abc") = ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad
    printf("sha256 "); for (int i = 0; i < 8; i++) printf("%08x", st[i]); printf("\n");

    uint32_t s1[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    sha1_block(s1, blk);                                // sha1("abc") = a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
    printf("sha1   "); for (int i = 0; i < 5; i++) printf("%08x", s1[i]); printf("\n");

    uint32_t crc = 0xffffffff;
    for (const char *p = "123456789"; *p; p++) crc = __crc32b(crc, (uint8_t)*p);
    printf("crc32  %08x\n", ~crc);                      // 0xcbf43926
    uint32_t crcc = 0xffffffff;
    for (const char *p = "123456789"; *p; p++) crcc = __crc32cb(crcc, (uint8_t)*p);
    printf("crc32c %08x\n", ~crcc);                     // 0xe3069283
    uint64_t data = 0x3837363534333231ull; uint32_t cx = __crc32d(0xffffffff, data); cx = __crc32b(cx, '9');
    printf("crc32d %08x\n", ~cx);
    poly128_t pm = vmull_p64(0x87, 0x1234567812345678ull);
    printf("pmull  %016llx%016llx\n", (unsigned long long)(pm >> 64), (unsigned long long)pm);
    return 0;
}
