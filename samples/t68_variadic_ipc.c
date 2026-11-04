// Variadic libc functions whose extra arguments are on the stack on Apple arm64: semctl(SETVAL), sem_open(mode, value), shm_open(mode), execl.
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
    key_t key = ftok("/tmp", 'R');
    int id = semget(key, 1, IPC_CREAT | 0600);
    semctl(id, 0, SETVAL, 3);
    printf("semval %d\n", semctl(id, 0, GETVAL));
    struct sembuf op = {0, -1, 0};
    printf("semop %d\n", semop(id, &op, 1));
    printf("semval %d\n", semctl(id, 0, GETVAL));
    semctl(id, 0, IPC_RMID);
    sem_unlink("/rebar_t68"); sem_t *s = sem_open("/rebar_t68", O_CREAT, 0600, 2);
    int v = -1; sem_getvalue(s, &v); printf("sem_open value %d\n", v);
    sem_close(s); sem_unlink("/rebar_t68");
    shm_unlink("/rebar_t68shm");
    int fd = shm_open("/rebar_t68shm", O_CREAT | O_RDWR, 0600);
    struct stat st; fstat(fd, &st); printf("shm_open mode %o\n", st.st_mode & 0777);
    close(fd); shm_unlink("/rebar_t68shm");
    fflush(stdout);
    { pid_t p = fork(); if (p == 0) { execl("/bin/echo", "/bin/echo", "execl", "ok", (char *)NULL); _exit(127); } int sts = 0; waitpid(p, &sts, 0); printf("execl status %d\n", WEXITSTATUS(sts)); }
    (void)argc; (void)argv;
    return 0;
}
