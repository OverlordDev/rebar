// `environ` must be the live variable: setenv / unsetenv reallocate the array (tmux's client walked a stale copy and called strlen on freed memory)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char **environ;
int main(void) {
    for (int i = 0; i < 200; i++) { char k[32]; snprintf(k, sizeof k, "REBAR_T93_%d", i); setenv(k, "v", 1); }
    int n = 0, bad = 0; for (char **e = environ; *e; e++) { n++; if (strlen(*e) > 1000) bad++; }
    unsetenv("REBAR_T93_7"); int m = 0; for (char **e = environ; *e; e++) { m++; if (!strncmp(*e, "REBAR_T93_7=", 12)) bad++; }
    printf("count %s, after unsetenv %d fewer, bad %d\n", n >= 200 ? "ok" : "BAD", n - m, bad); return 0; }
