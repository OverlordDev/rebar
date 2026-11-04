// A forked child runs code that was never translated before (so its JIT must work: allocate, patch, chain) while the parent's threads were busy compiling at the moment of
// the fork.  rebar's JIT bookkeeping lists must survive that (OpenCode's server forks at start-up and its child died in jpool_alloc).
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile int stop;
__attribute__((noinline)) static unsigned f1(unsigned x) { for (int i = 0; i < 40; i++) x = x * 1664525u + 1013904223u + (x >> 7); return x; }
__attribute__((noinline)) static unsigned f2(unsigned x) { return x % 7 ? f1(x) + 3 : f1(x ^ 5) - 1; }
__attribute__((noinline)) static unsigned f3(unsigned x) { switch (x & 7) { case 0: return f2(x); case 1: return f1(x) ^ 9; case 2: return f2(x + 1); case 3: return x * 3; case 4: return f1(x + 4); default: return f2(x ^ 77); } }
static void *busy(void *a) {   // a thread that keeps reaching new code: compiles blocks all the time
    unsigned x = 1; char buf[256];
    while (!stop) { x = f3(x); snprintf(buf, sizeof buf, "%u %s %d", x, "abc", (int)(x & 255)); if (strlen(buf) > 1000) break; }
    return NULL;
}
int main() {
    pthread_t t[3]; for (int k = 0; k < 3; k++) pthread_create(&t[k], NULL, busy, NULL);
    int bad = 0;
    for (int round = 0; round < 40; round++) {
        pid_t p = fork();
        if (p == 0) {
            unsigned x = (unsigned)round; char buf[128];
            for (int i = 0; i < 300; i++) { x = f3(x + i); snprintf(buf, sizeof buf, "%u:%d", x, i % 13); if (buf[0] == 'z') x++; }
            _exit((int)(x & 0) );
        }
        int st = 0; waitpid(p, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad++;
    }
    stop = 1; for (int k = 0; k < 3; k++) pthread_join(t[k], NULL);
    printf("children failed: %d\n", bad);
    return 0;
}
