// Newer A64 extensions the interpreter implements: CSSC (SMAX UMAX SMIN UMIN ABS CNT CTZ, register and immediate), HBC (BC.cond), CPA (ADDPT SUBPT MADDPT MSUBPT), LSE (CASP), LSE128 (SWPP LDCLRP LDSETP),
// THE (RCWCAS RCWCLR RCWSET RCWSWP and the pair forms), LRCPC2 (LDAPUR / STLUR of SIMD registers), LRCPC3 (STLR / LDAPR with writeback, LDIAPP / STILP), MOPS (CPYF*, CPY*, SET*), MTE arithmetic,
// the register forms of SQSHL UQSHL SQRSHL UQRSHL, SQDMLAL / SQDMLSL / SQDMULL (vector and scalar), USQADD / SUQADD, URECPE / URSQRTE, FRECPX.  Each result is compared with a C model; the program prints the
// number of mismatches per family (all 0).
#include <arm_neon.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint64_t rs_ = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rs_ ^= rs_ << 13; rs_ ^= rs_ >> 7; rs_ ^= rs_ << 17; return rs_; }
static int same(const void *a, const void *b, int n) { return memcmp(a, b, n) == 0; }
static int64_t sat64(__int128 v, int bits) { __int128 mx = ((__int128)1 << (bits - 1)) - 1, mn = -mx - 1; return v > mx ? (int64_t)mx : v < mn ? (int64_t)mn : (int64_t)v; }
static uint64_t usat(__int128 v, int bits) { __int128 mx = ((__int128)1 << bits) - 1; return v < 0 ? 0 : v > mx ? (uint64_t)mx : (uint64_t)v; }
#define N 2000
int main(void) {
  int b_cssc = 0, b_bc = 0, b_cpa = 0, b_casp = 0, b_lse128 = 0, b_rcw = 0, b_ldapur = 0, b_rcpc3 = 0, b_mops = 0, b_mte = 0, b_shift = 0, b_sqd = 0, b_usq = 0, b_est = 0, b_recpx = 0, b_retpc = 0;
  for (int it = 0; it < N; it++) {
    uint64_t a = rnd(), b = rnd(), r;
    // CSSC
    __asm__("smax %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); if ((int64_t)r != ((int64_t)a > (int64_t)b ? (int64_t)a : (int64_t)b)) b_cssc++;
    __asm__("umin %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); if (r != (a < b ? a : b)) b_cssc++;
    { uint32_t w; uint32_t wa = (uint32_t)a, wb = (uint32_t)b; __asm__("smin %w0, %w1, %w2" : "=r"(w) : "r"(wa), "r"(wb)); if ((int32_t)w != ((int32_t)wa < (int32_t)wb ? (int32_t)wa : (int32_t)wb)) b_cssc++;
      __asm__("umax %w0, %w1, %w2" : "=r"(w) : "r"(wa), "r"(wb)); if (w != (wa > wb ? wa : wb)) b_cssc++;
      __asm__("abs %w0, %w1" : "=r"(w) : "r"(wa)); if (w != (uint32_t)((int32_t)wa < 0 ? -(int64_t)(int32_t)wa : (int32_t)wa)) b_cssc++;
      __asm__("cnt %w0, %w1" : "=r"(w) : "r"(wa)); if (w != (uint32_t)__builtin_popcount(wa)) b_cssc++;
      __asm__("ctz %w0, %w1" : "=r"(w) : "r"(wa)); if (w != (wa ? (uint32_t)__builtin_ctz(wa) : 32u)) b_cssc++; }
    __asm__("abs %0, %1" : "=r"(r) : "r"(a)); if (r != ((int64_t)a < 0 ? (uint64_t)0 - a : a)) b_cssc++;
    __asm__("cnt %0, %1" : "=r"(r) : "r"(a)); if (r != (uint64_t)__builtin_popcountll(a)) b_cssc++;
    __asm__("ctz %0, %1" : "=r"(r) : "r"(a)); if (r != (a ? (uint64_t)__builtin_ctzll(a) : 64u)) b_cssc++;
    __asm__("smax %0, %1, #-77" : "=r"(r) : "r"(a)); if ((int64_t)r != ((int64_t)a > -77 ? (int64_t)a : -77)) b_cssc++;
    __asm__("smin %0, %1, #99" : "=r"(r) : "r"(a)); if ((int64_t)r != ((int64_t)a < 99 ? (int64_t)a : 99)) b_cssc++;
    __asm__("umax %0, %1, #200" : "=r"(r) : "r"(a)); if (r != (a > 200 ? a : 200)) b_cssc++;
    __asm__("umin %0, %1, #17" : "=r"(r) : "r"(a)); if (r != (a < 17 ? a : 17)) b_cssc++;
    { uint32_t w, wa = (uint32_t)a; __asm__("smax %w0, %w1, #-100" : "=r"(w) : "r"(wa)); if ((int32_t)w != ((int32_t)wa > -100 ? (int32_t)wa : -100)) b_cssc++; }
    // BC.cond (taken and not taken), RETAASPPC is exercised below
    { int x = 0; __asm__ volatile("cmp %1, %2\n bc.lt 1f\n mov %0, #1\n b 2f\n1: mov %0, #2\n2:" : "=&r"(x) : "r"(a), "r"(b) : "cc"); if (x != ((int64_t)a < (int64_t)b ? 2 : 1)) b_bc++; }
    // CPA
    { uint64_t p = a, o = b & 0xffff; __asm__("addpt %0, %1, %2, lsl #3" : "=r"(r) : "r"(p), "r"(o)); if (r != p + (o << 3)) b_cpa++;
      __asm__("subpt %0, %1, %2" : "=r"(r) : "r"(p), "r"(o)); if (r != p - o) b_cpa++;
      uint64_t m = b, q = rnd(); __asm__("maddpt %0, %1, %2, %3" : "=r"(r) : "r"(p), "r"(m), "r"(q)); if (r != q + p * m) b_cpa++;
      __asm__("msubpt %0, %1, %2, %3" : "=r"(r) : "r"(p), "r"(m), "r"(q)); if (r != q - p * m) b_cpa++; }
    // CASP, 64-bit and 32-bit pairs
    { static __int128 mem __attribute__((aligned(16))); uint64_t *m = (uint64_t *)&mem; m[0] = a; m[1] = b; uint64_t old0, old1;
      register uint64_t x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = 11, x3 __asm__("x3") = 22;
      __asm__ volatile("casp x0, x1, x2, x3, [%2]" : "+r"(x0), "+r"(x1) : "r"(&mem), "r"(x2), "r"(x3) : "memory"); old0 = x0; old1 = x1;
      if (old0 != a || old1 != b || m[0] != 11 || m[1] != 22) b_casp++;
      x0 = 5; x1 = 6; __asm__ volatile("casp x0, x1, x2, x3, [%2]" : "+r"(x0), "+r"(x1) : "r"(&mem), "r"(x2), "r"(x3) : "memory"); if (x0 != 11 || x1 != 22 || m[0] != 11 || m[1] != 22) b_casp++;
      uint32_t *w = (uint32_t *)&mem; w[0] = (uint32_t)a; w[1] = (uint32_t)b;
      register uint32_t y0 __asm__("w0") = (uint32_t)a, y1 __asm__("w1") = (uint32_t)b, y2 __asm__("w2") = 7, y3 __asm__("w3") = 8;
      __asm__ volatile("casp w0, w1, w2, w3, [%2]" : "+r"(y0), "+r"(y1) : "r"(&mem), "r"(y2), "r"(y3) : "memory"); if (y0 != (uint32_t)a || y1 != (uint32_t)b || w[0] != 7 || w[1] != 8) b_casp++; }
    // LSE128
    { static __int128 mem __attribute__((aligned(16))); uint64_t *m = (uint64_t *)&mem; uint64_t v0 = rnd(), v1 = rnd(); m[0] = a; m[1] = b;
      register uint64_t x4 __asm__("x4") = v0, x5 __asm__("x5") = v1;
      __asm__ volatile("swpp x4, x5, [%2]" : "+r"(x4), "+r"(x5) : "r"(&mem) : "memory"); if (x4 != a || x5 != b || m[0] != v0 || m[1] != v1) b_lse128++;
      uint64_t c0 = rnd(), c1 = rnd(); register uint64_t x6 __asm__("x6") = c0, x7 __asm__("x7") = c1;
      __asm__ volatile("ldclrp x6, x7, [%2]" : "+r"(x6), "+r"(x7) : "r"(&mem) : "memory"); if (x6 != v0 || x7 != v1 || m[0] != (v0 & ~c0) || m[1] != (v1 & ~c1)) b_lse128++;
      uint64_t o0 = m[0], o1 = m[1]; register uint64_t x8 __asm__("x8") = c0, x9 __asm__("x9") = c1;
      __asm__ volatile("ldsetpal x8, x9, [%2]" : "+r"(x8), "+r"(x9) : "r"(&mem) : "memory"); if (x8 != o0 || x9 != o1 || m[0] != (o0 | c0) || m[1] != (o1 | c1)) b_lse128++; }
    // RCW (FEAT_THE)
    { static uint64_t mem[2] __attribute__((aligned(16))); mem[0] = a; uint64_t e = a, nv = b, old;
      __asm__ volatile("rcwcas %0, %2, [%1]" : "+r"(e) : "r"(mem), "r"(nv) : "memory", "cc"); if (e != a || mem[0] != b) b_rcw++;
      uint64_t v = rnd(); old = 0; __asm__ volatile("rcwclr %2, %0, [%1]" : "=r"(old) : "r"(mem), "r"(v) : "memory", "cc"); if (old != b || mem[0] != (b & ~v)) b_rcw++;
      uint64_t cur = mem[0]; __asm__ volatile("rcwset %2, %0, [%1]" : "=r"(old) : "r"(mem), "r"(v) : "memory", "cc"); if (old != cur || mem[0] != (cur | v)) b_rcw++;
      cur = mem[0]; __asm__ volatile("rcwswp %2, %0, [%1]" : "=r"(old) : "r"(mem), "r"(a) : "memory", "cc"); if (old != cur || mem[0] != a) b_rcw++;
      cur = mem[0]; __asm__ volatile("rcwsswp %2, %0, [%1]" : "=r"(old) : "r"(mem), "r"(b) : "memory", "cc"); if (old != cur || mem[0] != b) b_rcw++; }
    // LDAPUR / STLUR of SIMD registers
    { uint8_t buf[48]; for (int k = 0; k < 48; k++) buf[k] = (uint8_t)rnd(); uint8_t sv[48]; memcpy(sv, buf, 48);
      uint64x2_t q; uint64_t d; uint32_t s; uint16_t h; uint8_t by;
      __asm__ volatile("ldapur q0, [%1, #3]\n mov %0.16b, v0.16b" : "=w"(q) : "r"(buf) : "v0", "memory"); if (!same(&q, buf + 3, 16)) b_ldapur++;
      __asm__ volatile("ldapur d0, [%1, #-5]\n fmov %0, d0" : "=r"(d) : "r"(buf + 20) : "v0", "memory"); if (!same(&d, buf + 15, 8)) b_ldapur++;
      __asm__ volatile("ldapur s0, [%1, #1]\n fmov %w0, s0" : "=r"(s) : "r"(buf) : "v0", "memory"); if (!same(&s, buf + 1, 4)) b_ldapur++;
      __asm__ volatile("ldapur h0, [%1, #2]\n umov %w0, v0.h[0]" : "=r"(h) : "r"(buf) : "v0", "memory"); if (!same(&h, buf + 2, 2)) b_ldapur++;
      __asm__ volatile("ldapur b0, [%1, #7]\n umov %w0, v0.b[0]" : "=r"(by) : "r"(buf) : "v0", "memory"); if (by != buf[7]) b_ldapur++;
      uint64x2_t sq = {a, b}; __asm__ volatile("mov v1.16b, %0.16b\n stlur q1, [%1, #5]" : : "w"(sq), "r"(buf) : "v1", "memory"); if (!same(buf + 5, &sq, 16) || !same(buf, sv, 5) || !same(buf + 21, sv + 21, 27)) b_ldapur++;
      __asm__ volatile("fmov d2, %0\n stlur d2, [%1, #30]" : : "r"(a), "r"(buf) : "v2", "memory"); if (!same(buf + 30, &a, 8)) b_ldapur++; }
    // LDAPR / STLR with writeback, LDIAPP / STILP
    { uint64_t buf[8]; for (int k = 0; k < 8; k++) buf[k] = rnd(); uint64_t *p = &buf[4], v = rnd(), l1, l2;
      __asm__ volatile("stlr %2, [%0, #-8]!" : "+r"(p) : "r"(p), "r"(v) : "memory"); if (p != &buf[3] || buf[3] != v) b_rcpc3++;
      uint64_t *q = &buf[1]; uint64_t got; __asm__ volatile("ldapr %1, [%0], #8" : "+r"(q), "=r"(got) : : "memory"); if (q != &buf[2] || got != buf[1]) b_rcpc3++;
      uint64_t *z = &buf[2]; __asm__ volatile("ldiapp %1, %2, [%0], #16" : "+r"(z), "=r"(l1), "=r"(l2) : : "memory"); if (z != &buf[4] || l1 != buf[2] || l2 != buf[3]) b_rcpc3++;
      uint64_t *y = &buf[6], x1 = rnd(), x2 = rnd(); __asm__ volatile("stilp %2, %3, [%0, #-16]!" : "+r"(y) : "r"(y), "r"(x1), "r"(x2) : "memory"); if (y != &buf[4] || buf[4] != x1 || buf[5] != x2) b_rcpc3++;
      uint64_t k1, k2; __asm__ volatile("ldiapp %0, %1, [%2]" : "=r"(k1), "=r"(k2) : "r"(&buf[4]) : "memory"); if (k1 != x1 || k2 != x2) b_rcpc3++; }
    // MOPS: copy forward / may-overlap / set
    { uint8_t src[300], dst[300]; for (int k = 0; k < 300; k++) { src[k] = (uint8_t)rnd(); dst[k] = 0xcc; }
      uint8_t *d = dst + 7, *s = src + 3; uint64_t n = 200 + (rnd() & 63), n0 = n;
      __asm__ volatile("cpyfp [%0]!, [%1]!, %2!\n cpyfm [%0]!, [%1]!, %2!\n cpyfe [%0]!, [%1]!, %2!" : "+r"(d), "+r"(s), "+r"(n) : : "memory", "cc");
      if (memcmp(dst + 7, src + 3, 200) != 0 || dst[6] != 0xcc || n != 0 || d != dst + 7 + n0 || s != src + 3 + n0) b_mops++;
      uint8_t buf[200]; for (int k = 0; k < 200; k++) buf[k] = (uint8_t)k; uint8_t *d2 = buf + 20, *s2 = buf + 5; uint64_t n2 = 100; uint8_t ref[200]; memcpy(ref, buf, 200); memmove(ref + 20, ref + 5, 100);
      __asm__ volatile("cpyp [%0]!, [%1]!, %2!\n cpym [%0]!, [%1]!, %2!\n cpye [%0]!, [%1]!, %2!" : "+r"(d2), "+r"(s2), "+r"(n2) : : "memory", "cc"); if (memcmp(buf, ref, 200) != 0) b_mops++;
      uint8_t st[100]; memset(st, 0x11, 100); uint8_t *sp = st + 10; uint64_t sn = 50, val = 0xa5;
      __asm__ volatile("setp [%0]!, %1!, %2\n setm [%0]!, %1!, %2\n sete [%0]!, %1!, %2" : "+r"(sp), "+r"(sn) : "r"(val) : "memory", "cc");
      for (int k = 0; k < 100; k++) if (st[k] != ((k >= 10 && k < 60) ? 0xa5 : 0x11)) { b_mops++; break; } if (sn != 0 || sp != st + 60) b_mops++; }
    // MTE arithmetic (tags are not kept)
    { uint64_t p = a & 0x00ffffffffffff00ull, q, t; __asm__("addg %0, %1, #32, #3" : "=r"(q) : "r"(p)); if ((q & 0x00ffffffffffffffull) != p + 32) b_mte++;
      __asm__("subp %0, %1, %2" : "=r"(t) : "r"(p), "r"(p - 40)); if (t != 40) b_mte++;
      __asm__("irg %0, %1" : "=r"(q) : "r"(p)); if ((q & 0x00ffffffffffffffull) != p) b_mte++;
      uint64_t blk[4] __attribute__((aligned(16))) = {1, 2, 3, 4}; __asm__ volatile("stzg %0, [%0]" : : "r"(blk) : "memory"); if (blk[0] != 0 || blk[1] != 0 || blk[2] != 3) b_mte++;
      __asm__ volatile("stg %0, [%0, #16]" : : "r"(blk) : "memory"); if (blk[2] != 3) b_mte++; }
    // SQSHL / UQSHL / SQRSHL / UQRSHL by register
    { int8x16_t va, vb; int8_t xa[16], xb[16]; for (int k = 0; k < 16; k++) { xa[k] = (int8_t)rnd(); xb[k] = (int8_t)((int)(rnd() % 19) - 9); } memcpy(&va, xa, 16); memcpy(&vb, xb, 16);
      int8x16_t o; int8_t e[16];
      __asm__("sqshl %0.16b, %1.16b, %2.16b" : "=w"(o) : "w"(va), "w"(vb)); for (int k = 0; k < 16; k++) { int s = xb[k]; __int128 x = xa[k]; e[k] = (int8_t)sat64(s >= 0 ? x << s : x >> -s, 8); } if (!same(&o, e, 16)) b_shift++;
      __asm__("uqshl %0.16b, %1.16b, %2.16b" : "=w"(o) : "w"(va), "w"(vb)); for (int k = 0; k < 16; k++) { int s = xb[k]; __int128 x = (uint8_t)xa[k]; e[k] = (int8_t)usat(s >= 0 ? x << s : x >> -s, 8); } if (!same(&o, e, 16)) b_shift++;
      __asm__("sqrshl %0.16b, %1.16b, %2.16b" : "=w"(o) : "w"(va), "w"(vb)); for (int k = 0; k < 16; k++) { int s = xb[k]; __int128 x = xa[k]; e[k] = (int8_t)sat64(s >= 0 ? x << s : (x + ((__int128)1 << (-s - 1))) >> -s, 8); } if (!same(&o, e, 16)) b_shift++;
      __asm__("uqrshl %0.16b, %1.16b, %2.16b" : "=w"(o) : "w"(va), "w"(vb)); for (int k = 0; k < 16; k++) { int s = xb[k]; __int128 x = (uint8_t)xa[k]; e[k] = (int8_t)usat(s >= 0 ? x << s : (x + ((__int128)1 << (-s - 1))) >> -s, 8); } if (!same(&o, e, 16)) b_shift++;
      int64_t sa = (int64_t)a >> (rnd() % 40), sh = (int)(rnd() % 11) - 5, so; __asm__("fmov d1, %1\n fmov d2, %2\n sqshl d1, d1, d2\n fmov %0, d1" : "=r"(so) : "r"(sa), "r"(sh) : "v1", "v2");
      __int128 x = sa; if (so != sat64(sh >= 0 ? x << sh : x >> -sh, 64)) b_shift++; }
    // SQDMLAL / SQDMLSL / SQDMULL
    { int16x4_t h1, h2; int16_t u[4], w[4]; for (int k = 0; k < 4; k++) { u[k] = (int16_t)rnd(); w[k] = (int16_t)rnd(); } memcpy(&h1, u, 8); memcpy(&h2, w, 8); int32x4_t acc, o; int32_t ac[4], e[4]; for (int k = 0; k < 4; k++) ac[k] = (int32_t)rnd(); memcpy(&acc, ac, 16);
      o = acc; __asm__("sqdmlal %0.4s, %1.4h, %2.4h" : "+w"(o) : "w"(h1), "w"(h2)); for (int k = 0; k < 4; k++) e[k] = (int32_t)sat64((__int128)ac[k] + sat64((__int128)2 * u[k] * w[k], 32), 32); if (!same(&o, e, 16)) b_sqd++;
      o = acc; __asm__("sqdmlsl %0.4s, %1.4h, %2.4h" : "+w"(o) : "w"(h1), "w"(h2)); for (int k = 0; k < 4; k++) e[k] = (int32_t)sat64((__int128)ac[k] - sat64((__int128)2 * u[k] * w[k], 32), 32); if (!same(&o, e, 16)) b_sqd++;
      __asm__("sqdmull %0.4s, %1.4h, %2.4h" : "=w"(o) : "w"(h1), "w"(h2)); for (int k = 0; k < 4; k++) e[k] = (int32_t)sat64((__int128)2 * u[k] * w[k], 32); if (!same(&o, e, 16)) b_sqd++;
      int32_t x = (int32_t)rnd(), y = (int32_t)rnd(); int64_t so, sacc = (int64_t)rnd(); __asm__("fmov s1, %w1\n fmov s2, %w2\n fmov d3, %3\n sqdmlal d3, s1, s2\n fmov %0, d3" : "=r"(so) : "r"(x), "r"(y), "r"(sacc) : "v1", "v2", "v3");
      if (so != sat64((__int128)sacc + sat64((__int128)2 * x * y, 64), 64)) b_sqd++;
      int16_t x16 = (int16_t)rnd(), y16 = (int16_t)rnd(); int32_t so32; __asm__("fmov h1, %w1\n fmov h2, %w2\n sqdmull s3, h1, h2\n fmov %w0, s3" : "=r"(so32) : "r"((uint32_t)(uint16_t)x16), "r"((uint32_t)(uint16_t)y16) : "v1", "v2", "v3");
      if (so32 != (int32_t)sat64((__int128)2 * x16 * y16, 32)) b_sqd++; }
    // USQADD / SUQADD
    { uint8x16_t ua; int8x16_t sb; uint8_t xa[16]; int8_t xb[16]; for (int k = 0; k < 16; k++) { xa[k] = (uint8_t)rnd(); xb[k] = (int8_t)rnd(); } memcpy(&ua, xa, 16); memcpy(&sb, xb, 16); uint8x16_t o = ua; uint8_t e[16];
      __asm__("usqadd %0.16b, %1.16b" : "+w"(o) : "w"(sb)); for (int k = 0; k < 16; k++) e[k] = (uint8_t)usat((__int128)xa[k] + xb[k], 8); if (!same(&o, e, 16)) b_usq++;
      int8x16_t so, sa2; memcpy(&sa2, xa, 16); so = sa2; int8_t e2[16]; __asm__("suqadd %0.16b, %1.16b" : "+w"(so) : "w"(ua)); (void)ua; for (int k = 0; k < 16; k++) e2[k] = (int8_t)sat64((__int128)(int8_t)xa[k] + xa[k], 8); if (!same(&so, e2, 16)) b_usq++; }
    // URECPE / URSQRTE / FRECPX
    { uint32x4_t v; uint32_t xs[4]; for (int k = 0; k < 4; k++) xs[k] = (uint32_t)rnd(); memcpy(&v, xs, 16); uint32x4_t o; uint32_t e[4];
      __asm__("urecpe %0.4s, %1.4s" : "=w"(o) : "w"(v)); for (int k = 0; k < 4; k++) { uint32_t x = xs[k]; if (!(x >> 31)) e[k] = 0xffffffffu; else { unsigned t = (x >> 23) * 2 + 1, bq = (1u << 19) / t; e[k] = ((bq + 1) / 2 & 0x1ff) << 23; } } if (!same(&o, e, 16)) b_est++;
      __asm__("ursqrte %0.4s, %1.4s" : "=w"(o) : "w"(v)); for (int k = 0; k < 4; k++) { uint32_t x = xs[k]; if ((x >> 30) == 0) e[k] = 0xffffffffu; else { unsigned t = x >> 23; unsigned long long a2 = t < 256 ? t * 2 + 1 : (((t >> 1) << 1) + 1) * 2, bq = 512; while (a2 * (bq + 1) * (bq + 1) < (1ull << 28)) bq++; e[k] = (uint32_t)(((bq + 1) / 2) & 0x1ff) << 23; } } if (!same(&o, e, 16)) b_est++;
      float f = (float)((int)(rnd() % 20001) - 10000) / 7.0f + 0.001f, g; uint32_t fb, gb; memcpy(&fb, &f, 4); __asm__("fmov s1, %s1\n frecpx s1, s1\n fmov %s0, s1" : "=w"(g) : "w"(f) : "v1"); memcpy(&gb, &g, 4);
      uint32_t exp = (fb >> 23) & 0xff, want = (fb & 0x80000000u) | ((exp == 0 ? 0xfe : (~exp & 0xff)) << 23); if (gb != want) b_recpx++; }
  }
  // RETAASPPC: returns to x30 like RET
  { uint64_t out = 0; __asm__ volatile("3: adr x30, 1f\n retaasppc 3b\n mov %0, #1\n b 2f\n1: mov %0, #2\n2:" : "=&r"(out) : : "x30"); if (out != 2) b_retpc++; }
  printf("cssc %d bc %d cpa %d casp %d lse128 %d rcw %d\nldapur %d rcpc3 %d mops %d mte %d\nshift %d sqdmlal %d usqadd %d estimate %d frecpx %d retaasppc %d\n",
         b_cssc, b_bc, b_cpa, b_casp, b_lse128, b_rcw, b_ldapur, b_rcpc3, b_mops, b_mte, b_shift, b_sqd, b_usq, b_est, b_recpx, b_retpc);
  return 0;
}
