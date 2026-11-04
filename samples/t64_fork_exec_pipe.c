// What Go's os/exec does: pipe, fork, in the child dup2 the pipe over stdout/stderr and execve an arm64-only program (given as argv[1]); the parent reads to EOF and waits.
// A threaded parent (Go has many threads) forks while they are busy.
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile int stop;
static void *busy(void *a) { volatile unsigned x = 1; while (!stop) x = x * 1664525u + 1013904223u; return NULL; }
static int g_argc; static char **g_argv, **g_envp;
static int run(void) {
    char **argv = g_argv, **envp = g_envp;
    pthread_t t[3]; for (int k = 0; k < 3; k++) pthread_create(&t[k], NULL, busy, NULL);
    int p[2]; if (pipe(p)) return 3;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1); dup2(p[1], 2); close(p[0]); close(p[1]);
        char *args[] = {argv[1], "one", "two", NULL};
        execve(argv[1], args, envp);
        _exit(127);
    }
    close(p[1]);
    char buf[4096]; size_t n = 0; ssize_t r;
    while ((r = read(p[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)r;
    buf[n] = 0;
    int st = 0; waitpid(pid, &st, 0);
    stop = 1; for (int k = 0; k < 3; k++) pthread_join(t[k], NULL);
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) if (strncmp(l, "nvrm:", 5)) printf("child: %s\n", l);
    printf("status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
static void *runner(void *a) { (void)a; run(); return NULL; }
int main(int argc, char **argv, char **envp) {   // the fork happens on a thread that is not the main one, as it does in Go
    if (argc < 2) return 2;
    g_argc = argc; g_argv = argv; g_envp = envp;
    pthread_t t; pthread_create(&t, NULL, runner, NULL); pthread_join(t, NULL);
    return 0;
}
