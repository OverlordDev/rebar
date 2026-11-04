// C++ exceptions through a frame that compact unwind cannot describe (it saves x19 and x21 alone): the unwind entry says "see the FDE", and the unwinder must restore
// the callee-saved registers from the DWARF rules for the catching frame to see its own x19/x21 again. Cleanups, catch and nested frames included.
#include <cstdio>
#include <stdexcept>
#include <string>
struct Guard { const char *name; ~Guard() { std::printf("  ~Guard(%s)\n", name); } };
extern "C" void wrapper(void (*fn)(int), int n);
#if defined(__aarch64__)
__asm__(
    ".text\n.globl _wrapper\n.p2align 2\n_wrapper:\n.cfi_startproc\n"
    "stp x29, x30, [sp, #-32]!\n.cfi_def_cfa_offset 32\n.cfi_offset 29, -32\n.cfi_offset 30, -24\n"
    "mov x29, sp\n.cfi_def_cfa_register 29\n"
    "str x19, [sp, #16]\n.cfi_offset 19, -16\n"
    "str x21, [sp, #24]\n.cfi_offset 21, -8\n"
    "mov x19, #111\nmov x21, #222\n"
    "mov x8, x0\nmov x0, x1\nblr x8\n"
    "ldr x21, [sp, #24]\nldr x19, [sp, #16]\nldp x29, x30, [sp], #32\nret\n.cfi_endproc\n");
#else
extern "C" void wrapper(void (*fn)(int), int n) { fn(n); }
#endif
extern "C" __attribute__((noinline)) void thrower(int n) {
    Guard g{"thrower"};
    if (n % 2) throw std::runtime_error("odd " + std::to_string(n));
}
__attribute__((noinline)) int viaWrapper(int n) {
    Guard g{"viaWrapper"};
    wrapper(thrower, n);
    return n * 10;
}
int main() {
    long total = 0, seen = 0;
    for (int n = 1; n <= 6; n++) {
        try { total += viaWrapper(n); std::printf("n=%d ok\n", n); }
        catch (const std::exception &e) { seen += n; std::printf("n=%d caught: %s (total %ld, seen %ld)\n", n, e.what(), total, seen); }
    }
    std::printf("total %ld seen %ld\n", total, seen);
    return 0;
}
