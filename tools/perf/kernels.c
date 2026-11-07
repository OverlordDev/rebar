// kernels: a set of small compute kernels for A/B work on the JIT and for the "how far from native" table.
// Built twice (x86_64 natively, arm64 for rebar) by tools/perf/build_kernels.sh; `kernels <name> [scale]` runs one kernel and prints
// "<name> <ms> <checksum>": the checksum must be equal in both builds (the table script checks it).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t g_sink;
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }

// ---- floating point, scalar
static uint64_t k_nbody(int scale) {
    enum { N = 5 };
    double x[N], y[N], z[N], vx[N], vy[N], vz[N], m[N];
    for (int i = 0; i < N; i++) { x[i] = i * 1.3 + 0.1; y[i] = i * 0.7 - 0.4; z[i] = i * 0.2 + 0.5; vx[i] = 0.01 * i; vy[i] = 0.02 * i; vz[i] = -0.01 * i; m[i] = 1.0 + i; }
    long steps = 4000000L * scale;
    for (long s = 0; s < steps; s++) {
        for (int i = 0; i < N; i++)
            for (int j = i + 1; j < N; j++) {
                double dx = x[i] - x[j], dy = y[i] - y[j], dz = z[i] - z[j];
                double d2 = dx * dx + dy * dy + dz * dz + 0.01, mag = 0.001 / (d2 * sqrt(d2));
                vx[i] -= dx * m[j] * mag; vy[i] -= dy * m[j] * mag; vz[i] -= dz * m[j] * mag;
                vx[j] += dx * m[i] * mag; vy[j] += dy * m[i] * mag; vz[j] += dz * m[i] * mag;
            }
        for (int i = 0; i < N; i++) { x[i] += 0.001 * vx[i]; y[i] += 0.001 * vy[i]; z[i] += 0.001 * vz[i]; }
    }
    double e = 0; for (int i = 0; i < N; i++) e += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
    return (uint64_t)(e * 1e9);
}
static uint64_t k_mandel(int scale) {
    uint64_t sum = 0; int W = 300, H = 200;
    for (int rep = 0; rep < 30 * scale; rep++)
        for (int py = 0; py < H; py++)
            for (int px = 0; px < W; px++) {
                double cr = -2.0 + 3.0 * px / W, ci = -1.0 + 2.0 * py / H, zr = 0, zi = 0; int it = 0;
                while (it < 200 && zr * zr + zi * zi < 4.0) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; it++; }
                sum += (uint64_t)it;
            }
    return sum;
}
static uint64_t k_spectral(int scale) {
    int n = 300; double *u = malloc(n * sizeof(double)), *v = malloc(n * sizeof(double)), *t = malloc(n * sizeof(double));
    for (int i = 0; i < n; i++) u[i] = 1.0;
#define A(i, j) (1.0 / ((((i) + (j)) * ((i) + (j) + 1) / 2) + (i) + 1))
    for (int rep = 0; rep < 1200 * scale; rep++) {
        for (int i = 0; i < n; i++) { double s = 0; for (int j = 0; j < n; j++) s += A(i, j) * u[j]; t[i] = s; }
        for (int i = 0; i < n; i++) { double s = 0; for (int j = 0; j < n; j++) s += A(j, i) * t[j]; v[i] = s; }
        for (int i = 0; i < n; i++) u[i] = v[i] / (1.0 + fabs(v[i]));
    }
    double s = 0; for (int i = 0; i < n; i++) s += u[i]; free(u); free(v); free(t);
    return (uint64_t)(s * 1e9);
}
// ---- integer / memory
static uint64_t k_fannkuch(int scale) {
    int n = 10; int perm[16], perm1[16], count[16]; long maxflips = 0, checksum = 0, permcount = 0;
    for (int rep = 0; rep < scale; rep++) {
        for (int i = 0; i < n; i++) perm1[i] = i;
        int r = n;
        for (;;) {
            while (r != 1) { count[r - 1] = r; r--; }
            for (int i = 0; i < n; i++) perm[i] = perm1[i];
            int flips = 0, k;
            while ((k = perm[0]) != 0) { int k2 = (k + 1) >> 1; for (int i = 0; i < k2; i++) { int t = perm[i]; perm[i] = perm[k - i]; perm[k - i] = t; } flips++; }
            if (flips > maxflips) maxflips = flips;
            checksum += (permcount % 2 == 0) ? flips : -flips; permcount++;
            for (;;) {
                if (r == n) return (uint64_t)(checksum * 1000 + maxflips);
                int p0 = perm1[0]; for (int i = 0; i < r; i++) perm1[i] = perm1[i + 1]; perm1[r] = p0;
                if (--count[r] > 0) break; r++;
            }
        }
    }
    return 0;
}
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint64_t k_sha256(int scale) {
    static const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; uint32_t w[64]; uint8_t block[64];
    for (int i = 0; i < 64; i++) block[i] = (uint8_t)(i * 7 + 3);
    for (long rep = 0; rep < 1200000L * scale; rep++) {
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)block[4 * i] << 24 | block[4 * i + 1] << 16 | block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 64; i++) { uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10); w[i] = w[i - 16] + s0 + w[i - 7] + s1; }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i], S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh; block[rep & 63] ^= (uint8_t)h[0];
    }
    return h[0] ^ ((uint64_t)h[1] << 32);
}
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint64_t k_qsort(int scale) {
    size_t n = 200000; uint64_t *a = malloc(n * 8); uint64_t acc = 0;
    for (int rep = 0; rep < 8 * scale; rep++) { for (size_t i = 0; i < n; i++) a[i] = rnd() >> 20; qsort(a, n, 8, cmp_u64); acc += a[n / 2] + a[n - 1]; }
    free(a); return acc;
}
typedef struct Node { struct Node *next; uint64_t v; } Node;
static uint64_t k_chase(int scale) {   // pointer chasing through a shuffled list: memory latency bound, like most of V8's heap walks
    size_t n = 1 << 18; Node *nodes = malloc(n * sizeof(Node)); size_t *idx = malloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) idx[i] = i;
    for (size_t i = n - 1; i > 0; i--) { size_t j = rnd() % (i + 1), t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    for (size_t i = 0; i < n; i++) { nodes[idx[i]].next = &nodes[idx[(i + 1) % n]]; nodes[idx[i]].v = i; }
    uint64_t acc = 0; Node *p = &nodes[idx[0]];
    for (long k = 0; k < 8000000L * scale; k++) { acc += p->v; p = p->next; }
    free(nodes); free(idx); return acc;
}
static uint64_t k_hash(int scale) {   // open-addressing hash table insert/lookup
    size_t cap = 1 << 20; uint64_t *keys = calloc(cap, 8), acc = 0;
    for (long i = 0; i < 600000L * 12 * scale; i++) {
        if ((i % 600000L) == 0) memset(keys, 0, cap * 8);
        uint64_t k = (rnd() | 1), h = (k * 0x9e3779b97f4a7c15ull) >> 44;
        while (keys[h & (cap - 1)] && keys[h & (cap - 1)] != k) h++;
        if (!keys[h & (cap - 1)]) keys[h & (cap - 1)] = k; else acc++;
        acc += keys[(h + 7) & (cap - 1)] & 3;
    }
    free(keys); return acc;
}
// ---- calls
struct Shape { double (*area)(const struct Shape *); double a, b; };
static double area_rect(const struct Shape *s) { return s->a * s->b; }
static double area_tri(const struct Shape *s) { return 0.5 * s->a * s->b; }
static double area_circ(const struct Shape *s) { return 3.14159 * s->a * s->a; }
static uint64_t k_virt(int scale) {
    struct Shape sh[64]; for (int i = 0; i < 64; i++) { sh[i].area = (i % 3 == 0) ? area_rect : (i % 3 == 1) ? area_tri : area_circ; sh[i].a = 1 + i * 0.01; sh[i].b = 2 + i * 0.02; }
    double acc = 0; for (long k = 0; k < 48000000L * scale; k++) acc += sh[k & 63].area(&sh[(k * 7) & 63]);
    return (uint64_t)(acc);
}
static uint64_t fibr(int n) { return n < 2 ? n : fibr(n - 1) + fibr(n - 2); }
static uint64_t k_fib(int scale) { return fibr(37 + (scale > 1 ? 1 : 0)); }
// ---- strings / memory ops
static uint64_t k_strings(int scale) {
    char *buf = malloc(1 << 16); uint64_t acc = 0;
    for (int i = 0; i < (1 << 16); i++) buf[i] = 'a' + (char)(rnd() % 26);
    for (int i = 0; i < (1 << 16); i += 97) buf[i] = ' ';
    for (long rep = 0; rep < 18000L * scale; rep++) {
        size_t off = (size_t)(rep * 131) & 4095; acc += strlen(buf + off) & 0xff;
        char tmp[256]; memcpy(tmp, buf + off, 255); tmp[255] = 0; acc += (uint64_t)(strchr(tmp, ' ') ? strchr(tmp, ' ') - tmp : 0);
        for (int i = 0; i < 1 << 16; i++) acc += (uint8_t)buf[i] == ' ';
        memmove(buf + 1, buf, 1 << 12);
    }
    free(buf); return acc;
}
// ---- NEON-friendly loops (auto-vectorised at -O3)
static uint64_t k_matmul(int scale) {
    int n = 96; float *A = malloc(n * n * 4), *B = malloc(n * n * 4), *C = malloc(n * n * 4);
    for (int i = 0; i < n * n; i++) { A[i] = (float)(i % 17) * 0.1f; B[i] = (float)(i % 13) * 0.2f; }
    for (int rep = 0; rep < 400 * scale; rep++)
        for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) { float s = 0; for (int k = 0; k < n; k++) s += A[i * n + k] * B[k * n + j]; C[i * n + j] = s; }
    double s = 0; for (int i = 0; i < n * n; i++) s += C[i]; free(A); free(B); free(C); return (uint64_t)s;
}
static uint64_t k_saxpy(int scale) {
    size_t n = 1 << 16; float *x = malloc(n * 4), *y = malloc(n * 4); for (size_t i = 0; i < n; i++) { x[i] = (float)i; y[i] = 1.0f; }
    for (long rep = 0; rep < 9000L * scale; rep++) for (size_t i = 0; i < n; i++) y[i] = y[i] * 0.999f + x[i] * 0.001f;
    double s = 0; for (size_t i = 0; i < n; i += 61) s += y[i]; free(x); free(y); return (uint64_t)(s * 100);
}
static uint64_t k_bytes(int scale) {   // byte-wise loop with table lookups (CRC-like)
    static uint32_t T[256]; for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; T[i] = c; }
    size_t n = 1 << 20; uint8_t *d = malloc(n); for (size_t i = 0; i < n; i++) d[i] = (uint8_t)rnd();
    uint32_t crc = 0xffffffffu; for (long rep = 0; rep < 120L * scale; rep++) for (size_t i = 0; i < n; i++) crc = T[(crc ^ d[i]) & 255] ^ (crc >> 8);
    free(d); return crc;
}

typedef struct { const char *name; uint64_t (*fn)(int); } Kernel;
static const Kernel kernels[] = {{"nbody", k_nbody}, {"mandel", k_mandel}, {"spectral", k_spectral}, {"fannkuch", k_fannkuch}, {"sha256", k_sha256}, {"qsort", k_qsort}, {"chase", k_chase}, {"hash", k_hash},
    {"virt", k_virt}, {"fib", k_fib}, {"strings", k_strings}, {"matmul", k_matmul}, {"saxpy", k_saxpy}, {"bytes", k_bytes}};

int main(int argc, char **argv) {
    int scale = argc > 2 ? atoi(argv[2]) : 1; if (scale < 1) scale = 1;
    if (argc > 1 && !strcmp(argv[1], "list")) { for (size_t i = 0; i < sizeof kernels / sizeof *kernels; i++) printf("%s\n", kernels[i].name); return 0; }
    for (size_t i = 0; i < sizeof kernels / sizeof *kernels; i++) {
        if (argc > 1 && strcmp(argv[1], "all") && strcmp(argv[1], kernels[i].name)) continue;
        rng_state = 0x9e3779b97f4a7c15ull; double t0 = now_ms(); uint64_t r = kernels[i].fn(scale); double t1 = now_ms(); g_sink += r;
        printf("%s %.1f %llu\n", kernels[i].name, t1 - t0, (unsigned long long)r); fflush(stdout);
    }
    return 0;
}
