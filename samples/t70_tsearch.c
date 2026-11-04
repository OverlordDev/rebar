// tsearch / tfind / tdelete / twalk with a comparator in the program (the host's libc calls it): results and timing of the callbacks
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static long ncmp;
static int cmp(const void *a, const void *b) { ncmp++; long x = *(const long *)a, y = *(const long *)b; return x < y ? -1 : x > y; }
static long sum, cnt;
static void walk(const void *node, VISIT v, int level) { (void)level; if (v == postorder || v == leaf) { sum += **(long **)node; cnt++; } }
int main(int argc, char **argv) {
    void *root = NULL; long *keys = malloc(20000 * sizeof(long));
    for (long i = 0; i < 20000; i++) keys[i] = (i * 7919) % 20011;
    for (int i = 0; i < 20000; i++) tsearch(&keys[i], &root, cmp);
    long found = 0; for (long k = 0; k < 20011; k++) if (tfind(&k, &root, cmp)) found++;
    for (long k = 0; k < 20011; k += 3) tdelete(&k, &root, cmp);
    twalk(root, walk);
    printf("found %ld walked %ld sum %ld\n", found, cnt, sum);
    if (argc > 1) printf("comparisons %ld\n", ncmp);
    return 0;
}
