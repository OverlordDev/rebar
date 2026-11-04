// Go's forkExec handshake: a close-on-exec pipe is created, the child dup2s its stdio and execve's an arm64-only program; if exec succeeds the pipe closes by itself and the parent's
// read returns 0, if not the child writes errno. The parent must not hang on the read.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv, char **envp) {
    if (argc < 2) return 2;
    int sp[2], op[2];
    pipe(sp); fcntl(sp[0], F_SETFD, FD_CLOEXEC); fcntl(sp[1], F_SETFD, FD_CLOEXEC);
    pipe(op);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(op[1], 1); dup2(op[1], 2); close(op[0]); close(op[1]);
        char *args[] = {argv[1], "x", NULL};
        execve(argv[1], args, envp);
        int e = errno; write(sp[1], &e, sizeof e); _exit(253);
    }
    close(sp[1]); close(op[1]);
    int err = 0; ssize_t n = read(sp[0], &err, sizeof err);
    printf("status pipe: read %zd err %d\n", n, err);
    char buf[512]; ssize_t r = read(op[0], buf, sizeof buf - 1); buf[r > 0 ? r : 0] = 0;
    for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) if (strncmp(l, "nvrm:", 5)) printf("child: %s\n", l);
    int st = 0; waitpid(pid, &st, 0);
    printf("exit %d\n", WEXITSTATUS(st));
    return 0;
}
