// ARMv8.2-8.6 extension instructions the interpreter has to implement: FCMA (FCADD, FCMLA), FHM (FMLAL family), I8MM (SMMLA UMMLA USMMLA USDOT SUDOT), BF16 (BFDOT BFMMLA BFMLALB/T BFCVT BFCVTN),
// RDM (SQRDMLAH / SQRDMLSH), FlagM (RMIF, SETF8, SETF16), unprivileged loads (LDTR / STTR), pointer-authenticated loads (LDRAA / LDRAB), PACGA.
// Every result is compared with a plain C model of the instruction on random inputs; the program prints the number of mismatches per instruction (all must be 0).
#include <arm_neon.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static uint64_t rs = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static float rf(void) { return (float)((int)(rnd() % 20001) - 10000) / (float)(1 + rnd() % 64); }
static void fillf(float32x4_t *v) { float a[4]; for (int k = 0; k < 4; k++) a[k] = rf(); memcpy(v, a, 16); }
static void filld(float64x2_t *v) { double a[2]; for (int k = 0; k < 2; k++) a[k] = (double)rf() * 1.37; memcpy(v, a, 16); }
static void fillh(float16x8_t *v) { _Float16 a[8]; for (int k = 0; k < 8; k++) a[k] = (_Float16)((float)((int)(rnd() % 401) - 200) / (float)(1 + rnd() % 8)); memcpy(v, a, 16); }
static void fillb(uint8x16_t *v) { uint8_t a[16]; for (int k = 0; k < 16; k++) a[k] = (uint8_t)rnd(); memcpy(v, a, 16); }
static void fill16(int16x8_t *v) { int16_t a[8]; for (int k = 0; k < 8; k++) a[k] = (int16_t)rnd(); memcpy(v, a, 16); }
static void fill32(int32x4_t *v) { int32_t a[4]; for (int k = 0; k < 4; k++) a[k] = (int32_t)rnd(); memcpy(v, a, 16); }
static void fillbf(uint16x8_t *v) { uint16_t a[8]; for (int k = 0; k < 8; k++) { float f = rf(); uint32_t u; memcpy(&u, &f, 4); a[k] = (uint16_t)(u >> 16); } memcpy(v, a, 16); }
static float bf(uint16_t h) { uint32_t u = (uint32_t)h << 16; float f; memcpy(&f, &u, 4); return f; }
static uint16_t tobf(float f) { uint32_t u; memcpy(&u, &f, 4); u += 0x7fff + ((u >> 16) & 1); return (uint16_t)(u >> 16); }
#define N 3000
#define CHECK(name, cond) do { if (!(cond)) bad_##name++; } while (0)
#define VEC(T, name) T name
static int same(const void *a, const void *b) { return memcmp(a, b, 16) == 0; }

int main(void) {
  int b_fcadd = 0, b_fcmla = 0, b_fcmlax = 0, b_fmlal = 0, b_fmlalx = 0, b_i8 = 0, b_dot = 0, b_bf = 0, b_bfx = 0, b_bfmmla = 0, b_bfmlal = 0, b_bfcvt = 0, b_rdm = 0, b_rdmx = 0, b_flag = 0, b_ldtr = 0, b_ra = 0, b_pacga = 0;
  for (int it = 0; it < N; it++) {
    float32x4_t a, b, d, r; fillf(&a); fillf(&b); fillf(&d);
    float fa[4], fb[4], fd[4], e[4]; memcpy(fa, &a, 16); memcpy(fb, &b, 16); memcpy(fd, &d, 16);
    // FCADD .4s
    r = a; __asm__("fcadd %0.4s, %1.4s, %2.4s, #90" : "=w"(r) : "w"(a), "w"(b));
    for (int k = 0; k < 4; k += 2) { e[k] = fa[k] - fb[k + 1]; e[k + 1] = fa[k + 1] + fb[k]; } if (!same(&r, e)) b_fcadd++;
    __asm__("fcadd %0.4s, %1.4s, %2.4s, #270" : "=w"(r) : "w"(a), "w"(b));
    for (int k = 0; k < 4; k += 2) { e[k] = fa[k] + fb[k + 1]; e[k + 1] = fa[k + 1] - fb[k]; } if (!same(&r, e)) b_fcadd++;
    // FCMLA .4s, the four rotations
    r = d; __asm__("fcmla %0.4s, %1.4s, %2.4s, #0" : "+w"(r) : "w"(a), "w"(b));
    for (int k = 0; k < 4; k += 2) { e[k] = fmaf(fa[k], fb[k], fd[k]); e[k + 1] = fmaf(fa[k], fb[k + 1], fd[k + 1]); } if (!same(&r, e)) b_fcmla++;
    r = d; __asm__("fcmla %0.4s, %1.4s, %2.4s, #90" : "+w"(r) : "w"(a), "w"(b));
    for (int k = 0; k < 4; k += 2) { e[k] = fmaf(-fa[k + 1], fb[k + 1], fd[k]); e[k + 1] = fmaf(fa[k + 1], fb[k], fd[k + 1]); } if (!same(&r, e)) b_fcmla++;
    r = d; __asm__("fcmla %0.4s, %1.4s, %2.4s, #180" : "+w"(r) : "w"(a), "w"(b));
    for (int k = 0; k < 4; k += 2) { e[k] = fmaf(-fa[k], fb[k], fd[k]); e[k + 1] = fmaf(-fa[k], fb[k + 1], fd[k + 1]); } if (!same(&r, e)) b_fcmla++;
    r = d; __asm__("fcmla %0.4s, %1.4s, %2.4s, #270" : "+w"(r) : "w"(a), "w"(b));
    for (int k = 0; k < 4; k += 2) { e[k] = fmaf(fa[k + 1], fb[k + 1], fd[k]); e[k + 1] = fmaf(-fa[k + 1], fb[k], fd[k + 1]); } if (!same(&r, e)) b_fcmla++;
    // FCMLA by element (.s[1]: the second complex number of Vm)
    r = d; __asm__("fcmla %0.4s, %1.4s, %2.s[1], #90" : "+w"(r) : "w"(a), "w"(b));
    for (int k = 0; k < 4; k += 2) { e[k] = fmaf(-fa[k + 1], fb[3], fd[k]); e[k + 1] = fmaf(fa[k + 1], fb[2], fd[k + 1]); } if (!same(&r, e)) b_fcmlax++;
    // FCADD / FCMLA .2d and .8h
    float64x2_t da, db, dd, dr; filld(&da); filld(&db); filld(&dd); double ga[2], gb[2], xd[2], xe[2]; memcpy(ga, &da, 16); memcpy(gb, &db, 16); memcpy(xd, &dd, 16);
    __asm__("fcadd %0.2d, %1.2d, %2.2d, #90" : "=w"(dr) : "w"(da), "w"(db)); xe[0] = ga[0] - gb[1]; xe[1] = ga[1] + gb[0]; if (!same(&dr, xe)) b_fcadd++;
    dr = dd; __asm__("fcmla %0.2d, %1.2d, %2.2d, #180" : "+w"(dr) : "w"(da), "w"(db)); xe[0] = fma(-ga[0], gb[0], xd[0]); xe[1] = fma(-ga[0], gb[1], xd[1]); if (!same(&dr, xe)) b_fcmla++;
    float16x8_t ha, hb, hd, hr; fillh(&ha); fillh(&hb); fillh(&hd); _Float16 pa[8], pb[8], pd[8], pe[8]; memcpy(pa, &ha, 16); memcpy(pb, &hb, 16); memcpy(pd, &hd, 16);
    __asm__("fcadd %0.8h, %1.8h, %2.8h, #270" : "=w"(hr) : "w"(ha), "w"(hb)); for (int k = 0; k < 8; k += 2) { pe[k] = (_Float16)((float)pa[k] + (float)pb[k + 1]); pe[k + 1] = (_Float16)((float)pa[k + 1] - (float)pb[k]); } if (!same(&hr, pe)) b_fcadd++;
    hr = hd; __asm__("fcmla %0.8h, %1.8h, %2.8h, #90" : "+w"(hr) : "w"(ha), "w"(hb)); for (int k = 0; k < 8; k += 2) { pe[k] = (_Float16)((double)(-(float)pa[k + 1]) * (float)pb[k + 1] + (float)pd[k]); pe[k + 1] = (_Float16)((double)(float)pa[k + 1] * (float)pb[k] + (float)pd[k + 1]); } if (!same(&hr, pe)) b_fcmla++;
    { float16x8_t xm = hb; hr = hd; __asm__("fcmla %0.8h, %1.8h, %2.h[2], #0" : "+w"(hr) : "w"(ha), "x"(xm)); for (int k = 0; k < 8; k += 2) { pe[k] = (_Float16)((double)(float)pa[k] * (float)pb[4] + (float)pd[k]); pe[k + 1] = (_Float16)((double)(float)pa[k] * (float)pb[5] + (float)pd[k + 1]); } if (!same(&hr, pe)) b_fcmlax++; }
    // FMLAL family (vector and by element)
    r = d; __asm__("fmlal %0.4s, %1.4h, %2.4h" : "+w"(r) : "w"(ha), "w"(hb)); for (int k = 0; k < 4; k++) e[k] = fmaf((float)pa[k], (float)pb[k], fd[k]); if (!same(&r, e)) b_fmlal++;
    r = d; __asm__("fmlsl %0.4s, %1.4h, %2.4h" : "+w"(r) : "w"(ha), "w"(hb)); for (int k = 0; k < 4; k++) e[k] = fmaf(-(float)pa[k], (float)pb[k], fd[k]); if (!same(&r, e)) b_fmlal++;
    r = d; __asm__("fmlal2 %0.4s, %1.4h, %2.4h" : "+w"(r) : "w"(ha), "w"(hb)); for (int k = 0; k < 4; k++) e[k] = fmaf((float)pa[4 + k], (float)pb[4 + k], fd[k]); if (!same(&r, e)) b_fmlal++;
    r = d; __asm__("fmlsl2 %0.4s, %1.4h, %2.4h" : "+w"(r) : "w"(ha), "w"(hb)); for (int k = 0; k < 4; k++) e[k] = fmaf(-(float)pa[4 + k], (float)pb[4 + k], fd[k]); if (!same(&r, e)) b_fmlal++;
    { float16x8_t xm = hb; r = d; __asm__("fmlal %0.4s, %1.4h, %2.h[5]" : "+w"(r) : "w"(ha), "x"(xm)); for (int k = 0; k < 4; k++) e[k] = fmaf((float)pa[k], (float)pb[5], fd[k]); if (!same(&r, e)) b_fmlalx++;
      r = d; __asm__("fmlsl2 %0.4s, %1.4h, %2.h[3]" : "+w"(r) : "w"(ha), "x"(xm)); for (int k = 0; k < 4; k++) e[k] = fmaf(-(float)pa[4 + k], (float)pb[3], fd[k]); if (!same(&r, e)) b_fmlalx++; }
    // I8MM
    uint8x16_t ua, ub; int32x4_t acc, ir; fillb(&ua); fillb(&ub); fill32(&acc); uint8_t ba[16], bb[16]; int32_t ac[4], ie[4]; memcpy(ba, &ua, 16); memcpy(bb, &ub, 16); memcpy(ac, &acc, 16);
    ir = acc; __asm__("smmla %0.4s, %1.16b, %2.16b" : "+w"(ir) : "w"(ua), "w"(ub)); for (int x = 0; x < 2; x++) for (int y = 0; y < 2; y++) { int32_t s = ac[2 * x + y]; for (int k = 0; k < 8; k++) s += (int8_t)ba[8 * x + k] * (int8_t)bb[8 * y + k]; ie[2 * x + y] = s; } if (!same(&ir, ie)) b_i8++;
    ir = acc; __asm__("ummla %0.4s, %1.16b, %2.16b" : "+w"(ir) : "w"(ua), "w"(ub)); for (int x = 0; x < 2; x++) for (int y = 0; y < 2; y++) { int32_t s = ac[2 * x + y]; for (int k = 0; k < 8; k++) s += ba[8 * x + k] * bb[8 * y + k]; ie[2 * x + y] = s; } if (!same(&ir, ie)) b_i8++;
    ir = acc; __asm__("usmmla %0.4s, %1.16b, %2.16b" : "+w"(ir) : "w"(ua), "w"(ub)); for (int x = 0; x < 2; x++) for (int y = 0; y < 2; y++) { int32_t s = ac[2 * x + y]; for (int k = 0; k < 8; k++) s += ba[8 * x + k] * (int8_t)bb[8 * y + k]; ie[2 * x + y] = s; } if (!same(&ir, ie)) b_i8++;
    ir = acc; __asm__("usdot %0.4s, %1.16b, %2.16b" : "+w"(ir) : "w"(ua), "w"(ub)); for (int k = 0; k < 4; k++) { int32_t s = ac[k]; for (int j = 0; j < 4; j++) s += ba[4 * k + j] * (int8_t)bb[4 * k + j]; ie[k] = s; } if (!same(&ir, ie)) b_dot++;
    ir = acc; __asm__("usdot %0.4s, %1.16b, %2.4b[2]" : "+w"(ir) : "w"(ua), "w"(ub)); for (int k = 0; k < 4; k++) { int32_t s = ac[k]; for (int j = 0; j < 4; j++) s += ba[4 * k + j] * (int8_t)bb[8 + j]; ie[k] = s; } if (!same(&ir, ie)) b_dot++;
    ir = acc; __asm__("sudot %0.4s, %1.16b, %2.4b[1]" : "+w"(ir) : "w"(ua), "w"(ub)); for (int k = 0; k < 4; k++) { int32_t s = ac[k]; for (int j = 0; j < 4; j++) s += (int8_t)ba[4 * k + j] * bb[4 + j]; ie[k] = s; } if (!same(&ir, ie)) b_dot++;
    // BF16
    uint16x8_t xa, xb; fillbf(&xa); fillbf(&xb); uint16_t ya[8], yb[8]; memcpy(ya, &xa, 16); memcpy(yb, &xb, 16);
    r = d; __asm__("bfdot %0.4s, %1.8h, %2.8h" : "+w"(r) : "w"(xa), "w"(xb)); for (int k = 0; k < 4; k++) e[k] = (float)((double)fd[k] + (double)bf(ya[2 * k]) * bf(yb[2 * k]) + (double)bf(ya[2 * k + 1]) * bf(yb[2 * k + 1])); if (!same(&r, e)) b_bf++;
    r = d; __asm__("bfdot %0.4s, %1.8h, %2.2h[1]" : "+w"(r) : "w"(xa), "w"(xb)); for (int k = 0; k < 4; k++) e[k] = (float)((double)fd[k] + (double)bf(ya[2 * k]) * bf(yb[2]) + (double)bf(ya[2 * k + 1]) * bf(yb[3])); if (!same(&r, e)) b_bfx++;
    r = d; __asm__("bfmmla %0.4s, %1.8h, %2.8h" : "+w"(r) : "w"(xa), "w"(xb)); for (int x = 0; x < 2; x++) for (int y = 0; y < 2; y++) { double s = fd[2 * x + y]; for (int k = 0; k < 4; k++) s += (double)bf(ya[4 * x + k]) * bf(yb[4 * y + k]); e[2 * x + y] = (float)s; } if (!same(&r, e)) b_bfmmla++;
    r = d; __asm__("bfmlalb %0.4s, %1.8h, %2.8h" : "+w"(r) : "w"(xa), "w"(xb)); for (int k = 0; k < 4; k++) e[k] = fmaf(bf(ya[2 * k]), bf(yb[2 * k]), fd[k]); if (!same(&r, e)) b_bfmlal++;
    r = d; __asm__("bfmlalt %0.4s, %1.8h, %2.8h" : "+w"(r) : "w"(xa), "w"(xb)); for (int k = 0; k < 4; k++) e[k] = fmaf(bf(ya[2 * k + 1]), bf(yb[2 * k + 1]), fd[k]); if (!same(&r, e)) b_bfmlal++;
    { uint16x8_t xm = xb; r = d; __asm__("bfmlalt %0.4s, %1.8h, %2.h[6]" : "+w"(r) : "w"(xa), "x"(xm)); for (int k = 0; k < 4; k++) e[k] = fmaf(bf(ya[2 * k + 1]), bf(yb[6]), fd[k]); if (!same(&r, e)) b_bfmlal++; }
    { uint16x8_t xr; __asm__("bfcvtn %0.4h, %1.4s" : "=w"(xr) : "w"(a)); uint16_t g[8] = {0}; for (int k = 0; k < 4; k++) g[k] = tobf(fa[k]); if (!same(&xr, g)) b_bfcvt++;
      xr = xb; __asm__("bfcvtn2 %0.8h, %1.4s" : "+w"(xr) : "w"(a)); uint16_t h2[8]; memcpy(h2, &xb, 16); for (int k = 0; k < 4; k++) h2[4 + k] = tobf(fa[k]); if (!same(&xr, h2)) b_bfcvt++;
      uint16_t sc; float sf = fa[0]; __asm__("bfcvt h0, %s1\n fmov %w0, s0" : "=r"(sc) : "w"(sf) : "v0"); if ((uint16_t)sc != tobf(fa[0])) b_bfcvt++; }
    // RDM
    int16x8_t s16a, s16b, s16d, s16r; fill16(&s16a); fill16(&s16b); fill16(&s16d); int16_t qa[8], qb[8], qd[8], qe[8]; memcpy(qa, &s16a, 16); memcpy(qb, &s16b, 16); memcpy(qd, &s16d, 16);
    s16r = s16d; __asm__("sqrdmlah %0.8h, %1.8h, %2.8h" : "+w"(s16r) : "w"(s16a), "w"(s16b));
    for (int k = 0; k < 8; k++) { long long v = (((long long)qd[k] << 16) + 2LL * qa[k] * qb[k] + (1 << 15)) >> 16; qe[k] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; } if (!same(&s16r, qe)) b_rdm++;
    s16r = s16d; __asm__("sqrdmlsh %0.8h, %1.8h, %2.8h" : "+w"(s16r) : "w"(s16a), "w"(s16b));
    for (int k = 0; k < 8; k++) { long long v = (((long long)qd[k] << 16) - 2LL * qa[k] * qb[k] + (1 << 15)) >> 16; qe[k] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; } if (!same(&s16r, qe)) b_rdm++;
    int32x4_t i32a, i32b, i32d, i32r; fill32(&i32a); fill32(&i32b); fill32(&i32d); int32_t ra[4], rb[4], rd[4], re[4]; memcpy(ra, &i32a, 16); memcpy(rb, &i32b, 16); memcpy(rd, &i32d, 16);
    i32r = i32d; __asm__("sqrdmlah %0.4s, %1.4s, %2.4s" : "+w"(i32r) : "w"(i32a), "w"(i32b));
    for (int k = 0; k < 4; k++) { __int128 v = (((__int128)rd[k] << 32) + (__int128)2 * ra[k] * rb[k] + ((__int128)1 << 31)) >> 32; re[k] = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; } if (!same(&i32r, re)) b_rdm++;
    { int32_t sres, sd = rd[0]; __asm__("fmov s1, %w1\n fmov s2, %w2\n fmov s3, %w3\n sqrdmlsh s1, s2, s3\n fmov %w0, s1" : "=r"(sres) : "r"(sd), "r"(ra[0]), "r"(rb[0]) : "v1", "v2", "v3");
      __int128 v = (((__int128)sd << 32) - (__int128)2 * ra[0] * rb[0] + ((__int128)1 << 31)) >> 32; int32_t ex = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; if (sres != ex) b_rdm++; }
    { int16x8_t m = s16b; s16r = s16d; __asm__("sqrdmlah %0.8h, %1.8h, %2.h[3]" : "+w"(s16r) : "w"(s16a), "x"(m));
      for (int k = 0; k < 8; k++) { long long v = (((long long)qd[k] << 16) + 2LL * qa[k] * qb[3] + (1 << 15)) >> 16; qe[k] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; } if (!same(&s16r, qe)) b_rdmx++; }
    // FlagM: SETF8 / SETF16 / RMIF
    { uint32_t v = (uint32_t)rnd(); uint32_t nzcv; __asm__("msr nzcv, xzr\n mov w9, %w1\n setf8 w9\n mrs %0, nzcv" : "=r"(nzcv) : "r"(v) : "x9", "cc");
      uint32_t n = (v >> 7) & 1, z = (v & 0xff) == 0, vv = ((v >> 8) ^ (v >> 7)) & 1; if ((nzcv >> 28) != ((n << 3) | (z << 2) | vv)) b_flag++;
      __asm__("msr nzcv, xzr\n mov w9, %w1\n setf16 w9\n mrs %0, nzcv" : "=r"(nzcv) : "r"(v) : "x9", "cc");
      n = (v >> 15) & 1; z = (v & 0xffff) == 0; vv = ((v >> 16) ^ (v >> 15)) & 1; if ((nzcv >> 28) != ((n << 3) | (z << 2) | vv)) b_flag++;
      uint64_t x = rnd(); __asm__("msr nzcv, xzr\n mov x9, %1\n rmif x9, #13, #0xa\n mrs %0, nzcv" : "=r"(nzcv) : "r"(x) : "x9", "cc");
      uint64_t t = (x >> 13) | (x << 51); uint32_t ef = (((t >> 3) & 1) << 3) | (((t >> 1) & 1) << 1); if ((nzcv >> 28) != ef) b_flag++; }
    // LDTR / STTR, LDRAA / LDRAB (plain loads here), PACGA
    { uint64_t buf[4] = {0x1122334455667788ull, 0x99aabbccddeeff00ull, 0, 0}; uint64_t o1, o2, o3, o4, o5; uint32_t w1; int64_t s1;
      __asm__ volatile("ldtr %0, [%5, #8]\n ldtrb %w1, [%5, #1]\n ldtrsh %2, [%5, #2]\n ldtrsw %3, [%5, #4]\n ldtr %w4, [%5]" : "=r"(o1), "=r"(o2), "=r"(o3), "=r"(o4), "=r"(o5) : "r"(buf) : "memory");
      if (o1 != buf[1] || o2 != 0x77 || (int64_t)o3 != (int16_t)0x5566 || (int64_t)o4 != (int32_t)0x11223344 || (uint32_t)o5 != 0x55667788u) b_ldtr++;
      __asm__ volatile("mov x10, #0x1234\n sttr x10, [%0, #16]\n mov w10, #0x55\n sttrb w10, [%0, #24]" : : "r"(buf) : "x10", "memory"); if (buf[2] != 0x1234 || (buf[3] & 0xff) != 0x55) b_ldtr++;
      uint64_t arr[4] = {1, 2, 3, 4}; uint64_t la, lb, lc; const uint64_t *p = &arr[1];
      __asm__ volatile("mov x11, %3\n ldraa %0, [x11, #8]\n ldrab %1, [x11, #-8]\n ldraa %2, [x11, #8]!\n mov %3, x11" : "=r"(la), "=r"(lb), "=r"(lc), "+r"(p) : : "x11", "memory");
      if (la != 3 || lb != 1 || lc != 3 || p != &arr[2]) b_ra++;
      uint64_t pg1, pg2; __asm__("mov x12, %2\n mov x13, %3\n pacga %0, x12, x13\n pacga %1, x12, x13" : "=r"(pg1), "=r"(pg2) : "r"(rnd()), "r"(rnd()) : "x12", "x13"); if (pg1 != pg2 || (pg1 & 0xffffffffull)) b_pacga++; }
  }
  printf("fcadd %d fcmla %d fcmla-elem %d\nfmlal %d fmlal-elem %d\ni8mm %d i8dot %d\nbfdot %d bfdot-elem %d bfmmla %d bfmlal %d bfcvt %d\nrdm %d rdm-elem %d\nflagm %d ldtr %d ldra %d pacga %d\n",
         b_fcadd, b_fcmla, b_fcmlax, b_fmlal, b_fmlalx, b_i8, b_dot, b_bf, b_bfx, b_bfmmla, b_bfmlal, b_bfcvt, b_rdm, b_rdmx, b_flag, b_ldtr, b_ra, b_pacga);
  return 0;
}
