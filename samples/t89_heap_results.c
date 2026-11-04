// realpath(NULL), strdup, getcwd(NULL) and asprintf return malloc'd memory that the program frees with its own free: with a replacement allocator (t88's libt89) the result must come from it
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
extern int g_own_allocs, g_own_frees;
int main(void) {
    char *a = realpath("/tmp/../tmp", NULL);
    char *b = strdup("duplicate");
    char *c = getcwd(NULL, 0);
    char *d = NULL; asprintf(&d, "%d-%s", 42, "formatted");
    printf("%s %s %d %s\n", a, b, c != NULL, d);
    free(a); free(b); free(c); free(d);
    printf("freed ok, own allocations %s\n", g_own_allocs >= 4 ? "used" : "partly host");
    return 0;
}
