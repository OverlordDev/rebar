// libc conformance: built for arm64 (under rebar) and for x86_64 (native); outputs must match.
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int ctor_ran;
__attribute__((constructor)) static void ctor(void) { ctor_ran = 42; }
static __thread int tls_counter = 7;
static __thread char tls_buf[64];
static atomic_int acount;

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void at1(void) { printf("atexit 1\n"); }
static void at2(void) { printf("atexit 2\n"); }

static int sum_varargs(int n, ...) {
    va_list ap; va_start(ap, n);
    int s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap); return s;
}
static void fmt_va(char *out, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(out, n, fmt, ap); va_end(ap);
}
static jmp_buf jb;
static void jumper(int v) { longjmp(jb, v); }

int main(int argc, char **argv) {
    printf("argc=%d ctor=%d tls=%d\n", argc, ctor_ran, tls_counter);
    printf("ints: %d %u %ld %lld %x %05d|%-5d|%+d\n", -5, 7u, -1234567890123L, 99999999999LL, 255, 42, 42, 42);
    printf("floats: %f %.3f %e %g %10.2f|\n", 3.14159, 2.0 / 3.0, 12345.678, 0.0001234, -1.5);
    printf("strs: %s %10s|%-10s| %c %%\n", "abc", "right", "left", 'z');

    char buf[128];
    snprintf(buf, sizeof buf, "%d-%s-%.2f", 17, "x", 1.005);
    printf("snprintf: %s (%zu)\n", buf, strlen(buf));
    fmt_va(buf, sizeof buf, "va %d %s %f %c", 9, "str", 2.5, 'q');
    printf("%s\n", buf);
    printf("sum_varargs=%d\n", sum_varargs(5, 1, 2, 3, 4, 5));

    // heap
    int *a = malloc(100 * sizeof(int));
    for (int i = 0; i < 100; i++) a[i] = (i * 7919 + 13) % 1000;
    qsort(a, 100, sizeof(int), cmp_int);
    int ok = 1; for (int i = 1; i < 100; i++) if (a[i - 1] > a[i]) ok = 0;
    int key = a[37];
    int *found = bsearch(&key, a, 100, sizeof(int), cmp_int);
    printf("qsort ok=%d first=%d last=%d bsearch=%d\n", ok, a[0], a[99], found ? (int)(found - a) >= 0 : -1);
    a = realloc(a, 200 * sizeof(int)); a[199] = 5; printf("realloc %d\n", a[199]); free(a);
    char *cp = calloc(16, 1); strcpy(cp, "calloc"); printf("%s %d\n", cp, (int)strlen(cp)); free(cp);

    char *words[] = {"pear", "apple", "fig", "banana", "cherry"};
    qsort(words, 5, sizeof(char *), cmp_str);
    for (int i = 0; i < 5; i++) printf("%s ", words[i]);
    printf("\n");

    // strings & conversions
    printf("%d %ld %.4f %lu\n", atoi("1234xyz"), strtol("-0x1f", NULL, 16), strtod("2.718281828", NULL), strtoul("4000000000", NULL, 10));
    char tok[] = "a,b;c d"; for (char *t = strtok(tok, ",; "); t; t = strtok(NULL, ",; ")) printf("[%s]", t);
    printf("\n");
    int x, y; char w[16];
    int n = sscanf("12 34 hello", "%d %d %15s", &x, &y, w);
    printf("sscanf n=%d %d %d %s\n", n, x, y, w);
    printf("memcmp=%d strstr=%s toupper=%c\n", memcmp("abc", "abd", 3), strstr("hello world", "wor"), toupper('q'));

    // math (libm)
    printf("math %.6f %.6f %.6f %.6f %.1f %.1f\n", sqrt(2.0), sin(1.0), pow(2.0, 0.5), exp(1.0), floor(-2.5), ceil(2.1));
    printf("fmod %.3f hypot %.3f atan2 %.5f\n", fmod(10.5, 3), hypot(3, 4), atan2(1, 2));

    // files, errno, stat
    int fd = open("/nonexistent/file", O_RDONLY);
    printf("open fail fd=%d errno=%d (%s)\n", fd, errno, strerror(errno));
    char path[] = "/tmp/rebar_t6_XXXXXX";
    int tfd = mkstemp(path);
    FILE *f = fdopen(tfd, "w+");
    fprintf(f, "line1\nline two\n%d\n", 12345);
    fflush(f); rewind(f);
    char line[64];
    while (fgets(line, sizeof line, f)) printf("read: %s", line);
    struct stat st; fstat(tfd, &st);
    printf("stat size=%lld isreg=%d\n", (long long)st.st_size, S_ISREG(st.st_mode));
    fclose(f);
    struct stat st2; int sr = stat(path, &st2); printf("stat() = %d size=%lld\n", sr, (long long)st2.st_size);
    unlink(path);
    int wfd = open("/tmp/rebar_t6_open", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(wfd, "hi\n", 3); close(wfd);
    struct stat st3; stat("/tmp/rebar_t6_open", &st3); printf("mode=%o\n", st3.st_mode & 0777); unlink("/tmp/rebar_t6_open");

    // env, setjmp, atomics, tls
    setenv("REBAR_T6", "yes", 1); printf("getenv=%s\n", getenv("REBAR_T6"));
    int jv = setjmp(jb);
    if (jv == 0) { printf("before longjmp\n"); jumper(7); }
    else printf("after longjmp %d\n", jv);
    for (int i = 0; i < 10; i++) atomic_fetch_add(&acount, i);
    int expected = 45; atomic_compare_exchange_strong(&acount, &expected, 100);
    printf("atomic=%d\n", atomic_load(&acount));
    tls_counter += 5; snprintf(tls_buf, sizeof tls_buf, "tls%d", tls_counter); printf("%s\n", tls_buf);
    fprintf(stderr, "to stderr\n");
    fputs("puts via fputs\n", stdout);
    puts("puts");
    atexit(at1); atexit(at2);
    return 3;
}
