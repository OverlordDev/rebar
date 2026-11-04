// The ordered and atomic instructions: every size of the fetch-and-op family, swap, compare-and-swap and the acquire/release accesses,
// single-threaded against known results (printed as a checksum) and from four threads against a known total.
// Built for arm64 with -march=armv8.1-a the compiler emits the LSE instructions (ldadd, ldclr, ldset, ldeor, swp, cas).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t seed = 88172645463325252ull;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
static uint64_t hash = 1469598103934665603ull;
static void mix(uint64_t v) { hash = (hash ^ v) * 1099511628211ull; }

#define OPS(T, NAME)                                                                                                    \
static void single_##NAME(void) {                                                                                       \
    for (int n = 0; n < 4000; n++) {                                                                                    \
        T cell = (T)rnd(), arg = (T)rnd(), old;                                                                         \
        old = __atomic_fetch_add(&cell, arg, __ATOMIC_SEQ_CST); mix(old); mix(cell);                                   \
        old = __atomic_fetch_and(&cell, arg, __ATOMIC_ACQ_REL); mix(old); mix(cell);                                   \
        old = __atomic_fetch_or(&cell, (T)rnd(), __ATOMIC_RELAXED); mix(old); mix(cell);                               \
        old = __atomic_fetch_xor(&cell, arg, __ATOMIC_ACQUIRE); mix(old); mix(cell);                                   \
        old = __atomic_exchange_n(&cell, arg, __ATOMIC_RELEASE); mix(old); mix(cell);                                  \
        T expect = (rnd() & 1) ? cell : (T)rnd(), want = (T)rnd();                                                      \
        bool ok = __atomic_compare_exchange_n(&cell, &expect, want, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);        \
        mix(ok); mix(expect); mix(cell);                                                                                \
        __atomic_store_n(&cell, arg, __ATOMIC_RELEASE); mix(__atomic_load_n(&cell, __ATOMIC_ACQUIRE));                 \
        (void)__atomic_fetch_add(&cell, 1, __ATOMIC_RELAXED); mix(cell);                                                \
        (void)__atomic_fetch_or(&cell, arg, __ATOMIC_RELAXED); mix(cell);                                               \
    }                                                                                                                   \
}
#include <stdbool.h>
OPS(uint8_t, b) OPS(uint16_t, h) OPS(uint32_t, w) OPS(uint64_t, x)

static volatile uint64_t total_add, total_cas, bits, spin_guard, spin_count;
static volatile uint32_t flags_or, flags_and = 0xffffffffu;
static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    for (int n = 0; n < 200000; n++) {
        __atomic_fetch_add(&total_add, 3, __ATOMIC_SEQ_CST);
        uint64_t cur = __atomic_load_n(&total_cas, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&total_cas, &cur, cur + 1, true, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
        while (__atomic_exchange_n(&spin_guard, 1, __ATOMIC_ACQUIRE)) {}   // a spin lock around a plain counter
        spin_count = spin_count + 1;
        __atomic_store_n(&spin_guard, 0, __ATOMIC_RELEASE);
    }
    for (int b = 0; b < 8; b++) {
        __atomic_fetch_or(&flags_or, 1u << (id * 8 + b), __ATOMIC_SEQ_CST);
        __atomic_fetch_and(&flags_and, ~(1u << (id * 8 + b)), __ATOMIC_SEQ_CST);
    }
    __atomic_fetch_xor(&bits, 1ull << id, __ATOMIC_SEQ_CST);
    return NULL;
}

int main(void) {
    single_b(); single_h(); single_w(); single_x();
    printf("single-thread checksum %016llx\n", (unsigned long long)hash);
    pthread_t th[4];
    for (long k = 0; k < 4; k++) pthread_create(&th[k], NULL, worker, (void *)k);
    for (int k = 0; k < 4; k++) pthread_join(th[k], NULL);
    printf("add %llu cas %llu spin %llu or %08x and %08x xor %llx\n", (unsigned long long)total_add, (unsigned long long)total_cas,
           (unsigned long long)spin_count, flags_or, flags_and, (unsigned long long)bits);
    return 0;
}
