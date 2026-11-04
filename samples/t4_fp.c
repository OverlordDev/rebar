// Scalar FP conformance: built for arm64 (run under rebar) and for x86_64
// (native); the two outputs must be identical.
typedef unsigned long u64; typedef long i64; typedef unsigned u32; typedef int i32;
#ifdef __aarch64__
static long sys3(long n, long a, long b, long c) {
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x16 __asm__("x16") = n;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    return x0;
}
#define EXIT(c) sys3(1, c, 0, 0)
#define WRITE(p, n) sys3(4, 1, (long)(p), n)
void start(void);
#else
long write(int, const void *, unsigned long); void _exit(int);
#define EXIT(c) _exit(c)
#define WRITE(p, n) write(1, p, n)
#endif
static void puts_(const char *s) { int n = 0; while (s[n]) n++; WRITE(s, n); }
static void puth(u64 v) { char b[17]; for (int i = 15; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; } b[16] = '\n'; WRITE(b, 17); }

static u64 rng_s = 0x9e3779b97f4a7c15ull;
static u64 rnd(void) { rng_s ^= rng_s >> 12; rng_s ^= rng_s << 25; rng_s ^= rng_s >> 27; return rng_s * 0x2545F4914F6CDD1Dull; }
static double rd_(void) { i64 m = (i64)(rnd() % 4000001) - 2000000; i64 d = (i64)(rnd() % 1999) + 1; return (double)m / (double)d * ((rnd() & 7) == 0 ? 1e-30 : (rnd() & 7) == 1 ? 1e30 : 1.0); }
static u64 bits_d(double x) { u64 r; __builtin_memcpy(&r, &x, 8); if (x != x) return 0x7ff8000000000000ull; return r; }
static u64 bits_f(float x) { u32 r; __builtin_memcpy(&r, &x, 4); if (x != x) return 0x7fc00000u; return r; }
static u64 h;
static void mix(u64 v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; }
#define GROUP(name) puts_(name); puth(h); h = 0xcbf29ce484222325ull;

void start(void) {
    h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2000; i++) {
        double a = rd_(), b = rd_(); float fa = (float)a, fb = (float)b;
        mix(bits_d(a + b)); mix(bits_d(a - b)); mix(bits_d(a * b)); mix(bits_d(a / b));
        mix(bits_f(fa + fb)); mix(bits_f(fa - fb)); mix(bits_f(fa * fb)); mix(bits_f(fa / fb));
    }
    GROUP("arith ")
    for (int i = 0; i < 2000; i++) {
        double a = rd_(); float fa = (float)a;
        mix(bits_d(__builtin_sqrt(a < 0 ? -a : a))); mix(bits_f(__builtin_sqrtf(fa < 0 ? -fa : fa)));
        mix(bits_d(-a)); mix(bits_d(a < 0 ? -a : a)); mix(bits_f(-fa));
    }
    GROUP("sqrt/neg/abs ")
    for (int i = 0; i < 2000; i++) {
        double a = rd_(), b = rd_(), c = rd_(); float fa = (float)a, fb = (float)b, fc = (float)c;
        mix(bits_d(__builtin_fma(a, b, c))); mix(bits_f(__builtin_fmaf(fa, fb, fc)));
        mix(bits_d(__builtin_fma(a, b, -c))); mix(bits_d(__builtin_fma(-a, b, c))); mix(bits_d(__builtin_fma(-a, b, -c)));
    }
    GROUP("fma ")
    for (int i = 0; i < 2000; i++) {
        double a = rd_(), b = rd_(); float fa = (float)a, fb = (float)b;
        mix(a < b); mix(a <= b); mix(a == b); mix(a != b); mix(a > b); mix(a >= b);
        mix(fa < fb); mix(fa == fb); mix(fa >= fb);
        mix(bits_d(a < b ? a : b)); mix(bits_d(a > b ? a : b)); mix(bits_f(fa < fb ? fa : fb));
        mix(bits_d(__builtin_fmax(a, b))); mix(bits_d(__builtin_fmin(a, b)));
    }
    GROUP("compare/select/minmax ")
    for (int i = 0; i < 2000; i++) {
        double a = rd_(); float fa = (float)a; i64 s = (i64)rnd(); u64 u = rnd();
        mix(bits_f((float)a)); mix(bits_d((double)fa));
        mix(bits_d((double)s)); mix(bits_d((double)u)); mix(bits_f((float)s)); mix(bits_f((float)u));
        mix(bits_d((double)(i32)s)); mix(bits_d((double)(u32)u)); mix(bits_f((float)(i32)s)); mix(bits_f((float)(u32)u));
        double c = a; if (c > 1e9) c = 1e9; if (c < -1e9) c = -1e9;
        mix((u64)(i64)c); mix((u64)(i32)c); mix((u64)(u32)(c < 0 ? -c : c)); mix((u64)(c < 0 ? -c : c));
        mix((u64)(i64)(float)c);
    }
    GROUP("convert ")
    for (int i = 0; i < 2000; i++) {
        double a = rd_(); if (a > 1e15) a = 1e15; if (a < -1e15) a = -1e15;
        mix(bits_d(__builtin_floor(a))); mix(bits_d(__builtin_ceil(a))); mix(bits_d(__builtin_trunc(a)));
        mix(bits_d(__builtin_round(a))); mix(bits_d(__builtin_rint(a)));
        float f = (float)a; if (f > 1e7f) f = 1e7f; if (f < -1e7f) f = -1e7f;
        mix(bits_f(__builtin_floorf(f))); mix(bits_f(__builtin_roundf(f)));
        mix((u64)__builtin_llround(a)); mix((u64)__builtin_llrint(a));
    }
    GROUP("round ")
    mix(bits_d(1.0)); mix(bits_d(-0.5)); mix(bits_d(31.0)); mix(bits_d(0.125)); mix(bits_f(2.5f)); mix(bits_f(-7.75f));
    mix(bits_d(1.0 / 3.0)); mix(bits_d(0.0)); mix(bits_f(0.0f));
    { double z = 0.0, nz = -0.0; mix(bits_d(z / z)); mix(bits_d(1.0 / z)); mix(bits_d(1.0 / nz)); mix(bits_d(z * nz)); }
    GROUP("immediates/special ")
    EXIT(0);
}
#ifndef __aarch64__
int main(void) { start(); return 0; }
#endif
