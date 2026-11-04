// SHA-512 with the ARMv8.2 SHA512 instructions (as BoringSSL does), the ADDHN/RADDHN/SUBHN/RSUBHN narrowing adds,
// and the SDOT/UDOT dot products.
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint64_t K[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec, 0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

#define ROR(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define SIG0(x) (ROR(x, 1) ^ ROR(x, 8) ^ ((x) >> 7))
#define SIG1(x) (ROR(x, 19) ^ ROR(x, 61) ^ ((x) >> 6))
static void load_block(uint64_t w[80], const uint8_t *p) {
    for (int k = 0; k < 16; k++) { uint64_t v = 0; for (int j = 0; j < 8; j++) v = v << 8 | p[8 * k + j]; w[k] = v; }
}

// the plain algorithm
static void block_ref(uint64_t st[8], const uint8_t *p) {
    uint64_t w[80];
    load_block(w, p);
    for (int k = 16; k < 80; k++) w[k] = w[k - 16] + SIG0(w[k - 15]) + w[k - 7] + SIG1(w[k - 2]);
    uint64_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int k = 0; k < 80; k++) {
        uint64_t t1 = h + (ROR(e, 14) ^ ROR(e, 18) ^ ROR(e, 41)) + ((e & f) ^ (~e & g)) + K[k] + w[k];
        uint64_t t2 = (ROR(a, 28) ^ ROR(a, 34) ^ ROR(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

// the same with SHA512SU0/SU1 for the message schedule and SHA512H/H2 for the rounds
static void block_ce(uint64_t st[8], const uint8_t *p) {
    uint64_t w[80];
    load_block(w, p);
    for (int k = 16; k < 80; k += 2) {
        uint64x2_t t = vsha512su0q_u64(vld1q_u64(&w[k - 16]), vld1q_u64(&w[k - 14]));
        t = vsha512su1q_u64(t, vld1q_u64(&w[k - 2]), vld1q_u64(&w[k - 7]));
        vst1q_u64(&w[k], t);
    }
    uint64x2_t ab = vld1q_u64(&st[0]), cd = vld1q_u64(&st[2]), ef = vld1q_u64(&st[4]), gh = vld1q_u64(&st[6]);
    uint64x2_t ab0 = ab, cd0 = cd, ef0 = ef, gh0 = gh;
    for (int k = 0; k < 80; k += 2) {
        uint64x2_t kw = vaddq_u64(vld1q_u64(&K[k]), vld1q_u64(&w[k]));
        uint64x2_t fg = vextq_u64(ef, gh, 1), de = vextq_u64(cd, ef, 1);
        uint64x2_t t = vaddq_u64(gh, vextq_u64(kw, kw, 1));
        t = vsha512hq_u64(t, fg, de);
        uint64x2_t new_ef = vaddq_u64(cd, t);
        t = vsha512h2q_u64(t, cd, ab);
        gh = ef; cd = ab; ab = t; ef = new_ef;
    }
    vst1q_u64(&st[0], vaddq_u64(ab, ab0)); vst1q_u64(&st[2], vaddq_u64(cd, cd0));
    vst1q_u64(&st[4], vaddq_u64(ef, ef0)); vst1q_u64(&st[6], vaddq_u64(gh, gh0));
}

static void sha512(const uint8_t *msg, size_t n, uint8_t out[64], void (*blk)(uint64_t[8], const uint8_t *)) {
    uint64_t st[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1, 0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
    uint8_t buf[256]; memset(buf, 0, sizeof buf);
    memcpy(buf, msg, n); buf[n] = 0x80;
    size_t total = (n + 1 + 16 <= 128) ? 128 : 256;
    uint64_t bits = (uint64_t)n * 8;
    for (int j = 0; j < 8; j++) buf[total - 1 - j] = (uint8_t)(bits >> (8 * j));
    for (size_t o = 0; o < total; o += 128) blk(st, buf + o);
    for (int k = 0; k < 8; k++) for (int j = 0; j < 8; j++) out[8 * k + j] = (uint8_t)(st[k] >> (56 - 8 * j));
}

static void hex(const uint8_t *d, int n, char *out) { for (int k = 0; k < n; k++) sprintf(out + 2 * k, "%02x", d[k]); }

static int fails;
#define CHECK(name, cond) do { printf("%s %s\n", (cond) ? "ok  " : "FAIL", name); if (!(cond)) fails++; } while (0)

int main(void) {
    char h1[130], h2[130];
    uint8_t d1[64], d2[64];
    sha512((const uint8_t *)"abc", 3, d1, block_ce); hex(d1, 64, h1);
    CHECK("sha512(abc) = ddaf35a1...", !strncmp(h1, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 128));
    sha512((const uint8_t *)"", 0, d1, block_ce); hex(d1, 64, h1);
    CHECK("sha512() = cf83e135...", !strncmp(h1, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", 128));
    uint8_t m[200]; for (int k = 0; k < 200; k++) m[k] = (uint8_t)(k * 7 + 1);
    sha512(m, 200, d1, block_ce); sha512(m, 200, d2, block_ref);
    CHECK("sha512 of 200 bytes, two blocks, equals the plain algorithm", !memcmp(d1, d2, 64));

    // narrowing adds, against scalar code
    uint16_t av[8] = {0x1234, 0xffff, 0x00ff, 0x8000, 0x7fff, 0x0100, 0xabcd, 0x0001}, bv[8] = {0x0101, 0x0001, 0x0001, 0x8000, 0x0001, 0x00ff, 0x1234, 0xffff};
    uint16x8_t a = vld1q_u16(av), b = vld1q_u16(bv);
    uint8_t r[16], e[16];
    int ok;
    vst1_u8(r, vaddhn_u16(a, b));  ok = 1; for (int k = 0; k < 8; k++) ok &= r[k] == (uint8_t)((uint16_t)(av[k] + bv[k]) >> 8); CHECK("addhn", ok);
    vst1_u8(r, vraddhn_u16(a, b)); ok = 1; for (int k = 0; k < 8; k++) ok &= r[k] == (uint8_t)((uint16_t)(av[k] + bv[k] + 0x80) >> 8); CHECK("raddhn", ok);
    vst1_u8(r, vsubhn_u16(a, b));  ok = 1; for (int k = 0; k < 8; k++) ok &= r[k] == (uint8_t)((uint16_t)(av[k] - bv[k]) >> 8); CHECK("subhn", ok);
    vst1_u8(r, vrsubhn_u16(a, b)); ok = 1; for (int k = 0; k < 8; k++) ok &= r[k] == (uint8_t)((uint16_t)(av[k] - bv[k] + 0x80) >> 8); CHECK("rsubhn", ok);
    vst1q_u8(r, vaddhn_high_u16(vdup_n_u8(0x55), a, b)); ok = 1;
    for (int k = 0; k < 8; k++) ok &= r[k] == 0x55 && r[8 + k] == (uint8_t)((uint16_t)(av[k] + bv[k]) >> 8);
    CHECK("addhn2 keeps the low half", ok);
    uint32_t cv[4] = {0x12345678, 0xffffffff, 0x80000000, 7}, dv[4] = {0x0000abcd, 1, 0x80000000, 0xffffffff};
    uint16_t o16[4];
    vst1_u16(o16, vaddhn_u32(vld1q_u32(cv), vld1q_u32(dv)));  ok = 1; for (int k = 0; k < 4; k++) ok &= o16[k] == (uint16_t)((uint32_t)(cv[k] + dv[k]) >> 16); CHECK("addhn 32", ok);
    vst1_u16(o16, vrsubhn_u32(vld1q_u32(cv), vld1q_u32(dv))); ok = 1; for (int k = 0; k < 4; k++) ok &= o16[k] == (uint16_t)((uint32_t)(cv[k] - dv[k] + 0x8000) >> 16); CHECK("rsubhn 32", ok);
    // dot products
    int8_t sa[16], sb[16]; uint8_t ua[16], ub[16];
    for (int k = 0; k < 16; k++) { sa[k] = (int8_t)(k * 29 - 100); sb[k] = (int8_t)(77 - k * 13); ua[k] = (uint8_t)(k * 31 + 200); ub[k] = (uint8_t)(255 - k * 17); }
    int32_t acc0[4] = {1000, -2000, 30000, -400000}, got[4], want[4];
    uint32_t uacc0[4] = {5, 4000000000u, 77, 0}, ugot[4], uwant[4];
    vst1q_s32(got, vdotq_s32(vld1q_s32(acc0), vld1q_s8(sa), vld1q_s8(sb)));
    for (int k = 0; k < 4; k++) { want[k] = acc0[k]; for (int j = 0; j < 4; j++) want[k] += sa[4 * k + j] * sb[4 * k + j]; }
    CHECK("sdot", !memcmp(got, want, 16));
    vst1q_u32(ugot, vdotq_u32(vld1q_u32(uacc0), vld1q_u8(ua), vld1q_u8(ub)));
    for (int k = 0; k < 4; k++) { uwant[k] = uacc0[k]; for (int j = 0; j < 4; j++) uwant[k] += (uint32_t)ua[4 * k + j] * ub[4 * k + j]; }
    CHECK("udot", !memcmp(ugot, uwant, 16));
    vst1q_s32(got, vdotq_laneq_s32(vld1q_s32(acc0), vld1q_s8(sa), vld1q_s8(sb), 2));
    for (int k = 0; k < 4; k++) { want[k] = acc0[k]; for (int j = 0; j < 4; j++) want[k] += sa[4 * k + j] * sb[8 + j]; }
    CHECK("sdot by element", !memcmp(got, want, 16));
    vst1q_u32(ugot, vdotq_laneq_u32(vld1q_u32(uacc0), vld1q_u8(ua), vld1q_u8(ub), 3));
    for (int k = 0; k < 4; k++) { uwant[k] = uacc0[k]; for (int j = 0; j < 4; j++) uwant[k] += (uint32_t)ua[4 * k + j] * ub[12 + j]; }
    CHECK("udot by element", !memcmp(ugot, uwant, 16));
    int32_t got2[2];
    vst1_s32(got2, vdot_s32(vld1_s32(acc0), vld1_s8(sa), vld1_s8(sb)));
    CHECK("sdot, 64-bit form", got2[0] == acc0[0] + sa[0]*sb[0] + sa[1]*sb[1] + sa[2]*sb[2] + sa[3]*sb[3] && got2[1] == acc0[1] + sa[4]*sb[4] + sa[5]*sb[5] + sa[6]*sb[6] + sa[7]*sb[7]);
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails != 0;
}
