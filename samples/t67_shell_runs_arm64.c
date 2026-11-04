// A host program the guest starts (a login shell, as VS Code and Zed do to learn the user's environment) runs an arm64-only program: the shell cannot exec it by itself
// ("bad CPU type in executable"), rebar's exec shim makes it work. The guest passes the arm64 program's path as argv[1].
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char cmd[1024]; snprintf(cmd, sizeof cmd, "'%s' one two", argv[1]);
    int p[2]; pipe(p);
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1); posix_spawn_file_actions_addclose(&fa, p[0]);
    char *args[] = {"zsh", "-l", "-c", cmd, NULL};
    pid_t pid; int r = posix_spawn(&pid, "/bin/zsh", &fa, NULL, args, environ);
    close(p[1]);
    char buf[2048]; size_t n = 0; ssize_t k;
    while ((k = read(p[0], buf + n, sizeof buf - 1 - n)) > 0) n += (size_t)k;
    buf[n] = 0; int st = 0; waitpid(pid, &st, 0);
    printf("spawn %d\n", r);
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) if (strncmp(l, "nvrm:", 5)) printf("shell said: %s\n", l);
    printf("status %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
