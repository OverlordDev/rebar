// The imports that translated code calls itself (memcpy, strlen, strcmp, pthread_getspecific, os_unfair_lock, mutex, ...) and the division
// instructions (a zero divisor, INT_MIN / -1), with every result printed: the same output natively and under rebar.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <os/lock.h>
#include <mach/mach_time.h>

static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

int main(void) {
    uint64_t h = 1469598103934665603ull;
    char a[256], b[256];
    for (int round = 0; round < 3000; round++) {
        for (int k = 0; k < 200; k++) a[k] = (char)('a' + (k * 7 + round) % 26);
        a[199 - round % 50] = 0;
        size_t n = strlen(a);
        h = mix(h, n);
        memcpy(b, a, n + 1);
        h = mix(h, (uint64_t)strcmp(a, b) + 5);
        b[n / 2] ^= 1;
        h = mix(h, (uint64_t)(strcmp(a, b) > 0) + 7);
        h = mix(h, (uint64_t)(strncmp(a, b, n / 2) == 0) + 9);
        h = mix(h, (uint64_t)(memcmp(a, b, n) != 0));
        memmove(b + 3, b, n - 2);
        memset(b, 'z', 3);
        h = mix(h, (uint64_t)(unsigned char)b[2] + (uint64_t)(unsigned char)b[5]);
        bzero(b + 10, 20);
        h = mix(h, (uint64_t)b[12] + 1);
        const char *p = strchr(a, 'q'); h = mix(h, p ? (uint64_t)(p - a) + 1 : 0);
        p = strrchr(a, 'q'); h = mix(h, p ? (uint64_t)(p - a) + 1 : 0);
        p = memchr(a, 'm', n); h = mix(h, p ? (uint64_t)(p - a) + 1 : 0);
    }
    static pthread_key_t key; pthread_key_create(&key, NULL);
    pthread_setspecific(key, (void *)0x1234);
    uint64_t sp = 0; for (int k = 0; k < 1000; k++) sp += (uint64_t)pthread_getspecific(key);
    h = mix(h, sp);
    os_unfair_lock lk = OS_UNFAIR_LOCK_INIT; int got = 0;
    for (int k = 0; k < 1000; k++) { if (os_unfair_lock_trylock(&lk)) { got++; os_unfair_lock_unlock(&lk); } }
    h = mix(h, (uint64_t)got);
    os_unfair_lock_lock(&lk); int again = os_unfair_lock_trylock(&lk); os_unfair_lock_unlock(&lk);
    h = mix(h, (uint64_t)again + 3);
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; int mgot = 0, mfail = 0;
    for (int k = 0; k < 500; k++) { if (pthread_mutex_trylock(&mu) == 0) { mgot++; if (pthread_mutex_trylock(&mu) != 0) mfail++; pthread_mutex_unlock(&mu); } }
    h = mix(h, (uint64_t)mgot * 1000 + (uint64_t)mfail);
    uint64_t t0 = mach_absolute_time(), t1 = mach_absolute_time(); h = mix(h, t1 >= t0);
    // division
    volatile int64_t num[] = {INT64_MIN, INT64_MAX, -7, 7, 0, 1000003, -1000003};
    volatile int64_t den[] = {-1, 0, 1, -2, 3, INT64_MIN, 7};
    for (int x = 0; x < 7; x++) for (int y = 0; y < 7; y++) {
        int64_t n = num[x], d = den[y];
        int64_t q = d == 0 ? 0 : (d == -1 ? (int64_t)(0 - (uint64_t)n) : n / d);
        volatile int64_t vd = d; int64_t r64 = 0;
        if (vd != 0 && vd != -1) r64 = n / vd; else r64 = q;
        uint64_t un = (uint64_t)n, ud = (uint64_t)d; uint64_t uq = ud ? un / ud : 0;
        int32_t n32 = (int32_t)n, d32 = (int32_t)d; int32_t q32 = d32 == 0 ? 0 : (d32 == -1 ? (int32_t)(0 - (uint32_t)n32) : n32 / d32);
        uint32_t uq32 = (uint32_t)d ? (uint32_t)n / (uint32_t)d : 0;
        h = mix(h, (uint64_t)q ^ (uint64_t)r64 ^ uq ^ (uint64_t)q32 ^ uq32);
    }
    printf("t51 leaf imports: %016llx\n", (unsigned long long)h);
    return 0;
}
