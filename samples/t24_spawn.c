// An arm64 program that starts other arm64 programs: posix_spawn, posix_spawnp and fork+execvp.
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

int main(int argc, char **argv) {
    if (argc > 1) { printf("child %s arg=%s\n", argv[0] ? "ok" : "?", argv[1]); return 7; }
    char *args[] = {argv[0], "via-spawn", NULL};
    pid_t pid; int st;
    int r = posix_spawn(&pid, argv[0], NULL, NULL, args, environ);
    waitpid(pid, &st, 0);
    printf("posix_spawn r=%d exit=%d\n", r, WEXITSTATUS(st));
    char *args2[] = {"t24_arm", "via-spawnp", NULL};
    setenv("PATH", ".:/usr/bin:/bin", 1);
    r = posix_spawnp(&pid, "t24_arm", NULL, NULL, args2, environ);
    waitpid(pid, &st, 0);
    printf("posix_spawnp r=%d exit=%d\n", r, WEXITSTATUS(st));
    fflush(stdout);
    pid = fork();
    if (pid == 0) { char *a[] = {"t24_arm", "via-execvp", NULL}; execvp("t24_arm", a); _exit(99); }
    waitpid(pid, &st, 0);
    printf("fork+execvp exit=%d\n", WEXITSTATUS(st));
    return 0;
}
