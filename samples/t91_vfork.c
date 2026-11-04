// vfork + exec in the child, waitpid in the parent (GIMP starts its plug-ins this way). Also a vfork child that exits without exec.
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int st;
    pid_t p = vfork();
    if (p == 0) { execl("/usr/bin/true", "true", (char *)0); _exit(127); }
    waitpid(p, &st, 0);
    printf("exec child status %d\n", WEXITSTATUS(st));
    p = vfork();
    if (p == 0) _exit(7);
    waitpid(p, &st, 0);
    printf("exit child status %d\n", WEXITSTATUS(st));
    return 0;
}
