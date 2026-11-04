// posix_spawnp finds a program through PATH (Rust's rustc runs `xcrun` this way and warned that it did not exist).
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
extern char **environ;
int main(void) {
    pid_t pid;
    char *argv[] = {"echo", "found through PATH", NULL};
    int r = posix_spawnp(&pid, "echo", NULL, NULL, argv, environ);
    printf("spawnp echo: %d\n", r);
    int st = 0; if (!r) waitpid(pid, &st, 0);
    printf("status %d\n", WEXITSTATUS(st));
    char *bad[] = {"no_such_program_here", NULL};
    r = posix_spawnp(&pid, "no_such_program_here", NULL, NULL, bad, environ);
    printf("spawnp missing: %d\n", r);
    return 0;
}
