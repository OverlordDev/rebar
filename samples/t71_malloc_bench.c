// malloc / calloc / realloc / free in a loop (translated code calls the host's allocator directly while nothing has patched the default zone)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main() {
    void *p[256]; unsigned long sum = 0;
    for (int round = 0; round < 40000; round++) {
        for (int k = 0; k < 64; k++) { p[k] = malloc(16 + (k * 7 + round) % 200); memset(p[k], k, 16); }
        for (int k = 0; k < 16; k++) { p[k] = realloc(p[k], 300 + k); sum += ((unsigned char *)p[k])[0]; }
        for (int k = 64; k < 80; k++) { p[k] = calloc(3, 11 + k); sum += ((unsigned char *)p[k])[5]; }
        for (int k = 0; k < 80; k++) free(p[k]);
    }
    printf("sum %lu\n", sum);
    return 0;
}
