// ___chkstk_darwin (called for run-time sized stack frames) must keep the caller's registers: the host's x86_64 function of that name left 8 in x0.
#include <stdio.h>
#include <string.h>
__attribute__((noinline)) long frame(const long *p, int n) {
    char buf[n];                       // a variable-length array: sized at run time, so the frame is probed
    memset(buf, 3, (size_t)n);
    return p[3] + buf[n - 1] + n;      // p must survive the probe
}
int main(void) {
    long data[8] = {1, 2, 3, 40, 5, 6, 7, 8};
    long sum = 0;
    for (int i = 1; i < 300; i += 7) sum += frame(data, i * 50);
    printf("sum %ld\n", sum);
    return 0;
}
