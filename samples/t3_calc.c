// libc-free C test: syscalls via inline asm, loops, recursion, division, arrays.
typedef unsigned long u64;
static long sys3(long n, long a, long b, long c) {
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x16 __asm__("x16") = n;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
    return x0;
}
static void put(const char *s, u64 n) { sys3(4, 1, (long)s, n); }
static void putnum(u64 v) {
    char buf[24]; int i = 24;
    buf[--i] = '\n';
    do { buf[--i] = '0' + v % 10; v /= 10; } while (v);
    put(buf + i, 24 - i);
}
static u64 fib(u64 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int cmp_arr[16];
void start(void) {
    putnum(fib(20));                       // 6765
    u64 s = 0;
    for (u64 i = 1; i <= 1000; i++) s += i * i; // 333833500
    putnum(s);
    for (int i = 0; i < 16; i++) cmp_arr[i] = (i * 7919) % 31;
    int mx = 0; for (int i = 0; i < 16; i++) if (cmp_arr[i] > mx) mx = cmp_arr[i];
    putnum(mx);
    putnum((u64)-17 / 3 % 1000);
    sys3(1, (long)(s % 256), 0, 0);
}
