// Self-modifying code the way a JIT engine writes it: store instructions, clean the data cache (dc cvau), invalidate the instruction cache (ic ivau), barriers, run.
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>
#include <libkern/OSCacheControl.h>
typedef int (*fn)(void);
static void flush(void *p, size_t n) {
    uintptr_t a = (uintptr_t)p & ~63ull, e = (uintptr_t)p + n;
    for (; a < e; a += 64) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uintptr_t)p & ~63ull; a < e; a += 64) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}
int main() {
    uint32_t *m = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
    if (m == MAP_FAILED) { puts("mmap failed"); return 1; }
    int sum = 0;
    for (int round = 1; round <= 200; round++) {
        m[0] = 0x52800000u | ((uint32_t)round << 5);   // mov w0, #round
        m[1] = 0xd65f03c0u;                              // ret
        flush(m, 8);
        sum += ((fn)m)();
    }
    printf("sum %d\n", sum);   // 1 + 2 + ... + 200
    __asm__ volatile("ic iallu\n dsb ish\n isb" ::: "memory");
    m[0] = 0x528000a0u; flush(m, 4);   // mov w0, #5
    printf("after iallu %d\n", ((fn)m)());
    return 0;
}
