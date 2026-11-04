// Flags handed from a callee to its caller (what LLVM's outlined functions do: "cmp x10, #0x3f; ret", then the caller's csel reads them) and
// flags that must survive until a conditional select after a call.  The translator must not treat a return or a call as the end of the flags' life.
#include <stdio.h>
#include <stdint.h>
__asm__(".text\n.globl _flagfn\n.p2align 2\n_flagfn:\n    cmp x0, x1\n    ret\n"
        ".globl _flagfn_pass\n.p2align 2\n_flagfn_pass:\n    add x9, x0, x1\n    ret\n");
__attribute__((noinline)) static long via_callee(long a, long b, long c, long d) {   // the callee compares, the caller selects
    long r;
    __asm__ volatile("cmp %3, %4\n mov x0, %1\n mov x1, %2\n bl _flagfn\n csel %0, %3, %4, ne\n" : "=r"(r) : "r"(a), "r"(b), "r"(c), "r"(d) : "x0", "x1", "x30", "cc", "memory");
    return r;
}
__attribute__((noinline)) static long across_call(long a, long b, long c, long d) {   // the caller compares, a callee that does not touch the flags runs, the caller selects
    long r;
    __asm__ volatile("cmp %1, %2\n mov x0, #1\n mov x1, #2\n bl _flagfn_pass\n csel %0, %3, %4, lt\n" : "=r"(r) : "r"(a), "r"(b), "r"(c), "r"(d) : "x0", "x1", "x9", "x30", "cc", "memory");
    return r;
}
int main(void) {
    long bad = 0, sum = 0;
    for (long i = 0; i < 200000; i++) {
        long a = (i * 7) % 11, b = (i * 5) % 11, c = (i * 3) % 7, d = (i * 2) % 5 + 1;
        long a2 = a ^ 3, b2 = (b + (i & 1)) % 11;   // (other operands than the ones the loop compared itself, so that old flags cannot give the right answer)
        long e1 = a2 != b2 ? c : d, e2 = a2 < b2 ? c : d;
        long g1 = via_callee(a2, b2, c, d), g2 = across_call(a2, b2, c, d);
        if (g1 != e1) bad++;
        if (g2 != e2) bad++;
        sum += g1 + g2;
    }
    printf("t50 flags across calls: bad %ld sum %ld\n", bad, sum);
    return 0;
}
