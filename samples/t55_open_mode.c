// open / openat through the $NOCANCEL entry points that Bun, Zig and Rust call directly: the mode is a variadic argument (on the stack on arm64), and the file
// must get it.  opencode's server created its own registration file with mode 0000 and then could not read it.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
int open_nc(const char *p, int fl, ...) __asm("_open$NOCANCEL");
int openat_nc(int fd, const char *p, int fl, ...) __asm("_openat$NOCANCEL");
static void show(const char *what, const char *path) { struct stat st; if (stat(path, &st) == 0) printf("%s mode=%03o\n", what, st.st_mode & 0777); else printf("%s: no file\n", what); }
int main() {
    char a[] = "/tmp/rebar_t55_a", b[] = "/tmp/rebar_t55_b", c[] = "/tmp/rebar_t55_c";
    unlink(a); unlink(b); unlink(c);
    int fd = open_nc(a, O_WRONLY | O_CREAT | O_CLOEXEC, 0640); if (fd >= 0) close(fd);
    show("open$NOCANCEL", a);
    fd = openat_nc(AT_FDCWD, b, O_WRONLY | O_CREAT | O_CLOEXEC, 0600); if (fd >= 0) close(fd);
    show("openat$NOCANCEL", b);
    fd = open(c, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd);
    show("open", c);
    unlink(a); unlink(b); unlink(c);
    return 0;
}
