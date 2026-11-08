// An outlined function that saves only x30, calls something and returns the flags of its last compare (`cmp w0, #1; ldr x30, [sp], #16; ret`, as the compiler's machine outliner emits):
// the JIT must not take the load into x30 for a frame restore and drop the flags (Logic Pro compared an Optional<URL> with stale flags).
#include <stdio.h>
__asm__(
    ".text\n.p2align 2\n"
    ".globl _outl_is_one\n_outl_is_one:\n"
    "  str x30, [sp, #-16]!\n"
    "  bl _outl_nop\n"
    "  cmp w0, #1\n"
    "  ldr x30, [sp], #16\n"
    "  ret\n"
    ".p2align 2\n_outl_nop:\n  ret\n");
extern void outl_is_one(void);
static int is_one(int v) {
    int r;
    __asm__ volatile("mov w0, %w1\n bl _outl_is_one\n cset %w0, eq\n" : "=&r"(r) : "r"(v) : "x0", "x30", "cc", "memory");
    return r;
}
int main(void) {
    int hits = 0, bad = 0;
    for (int k = 0; k < 20000; k++) { int v = (k % 3) ? k : 1; int e = (v == 1); int r = is_one(v); hits += r; if (r != e) bad++; }
    printf(bad ? "BAD %d\n" : "all ok\n", bad);
    (void)hits;
    return 0;
}
