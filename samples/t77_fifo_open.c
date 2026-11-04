// open() of a FIFO blocks until the other end is opened. The other end is opened by another thread of the same process, so the blocking open
// must not hold the bridge lock (Zig's parallel build stalled on this).
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *path = "/tmp/rebar_t77_fifo";

static void *writer(void *a) {
    (void)a;
    usleep(200000);
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror("writer open"); return NULL; }
    write(fd, "through the fifo\n", 17);
    close(fd);
    return NULL;
}

int main(void) {
    unlink(path);
    if (mkfifo(path, 0600)) { perror("mkfifo"); return 1; }
    pthread_t t;
    pthread_create(&t, NULL, writer, NULL);
    int fd = open(path, O_RDONLY);           // blocks until the writer thread opens its end
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    printf("read %zd: %s", n, buf);
    close(fd);
    pthread_join(t, NULL);
    unlink(path);
    printf("done\n");
    return 0;
}
