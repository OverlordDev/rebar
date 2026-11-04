// A handler installed with SA_ONSTACK runs on the alternate stack the thread registered with sigaltstack, and sigaltstack(NULL, &old) reports that stack back.
// (Go's signal handler refuses to run anywhere else and `go build` hung on a SIGCHLD that arrived while a goroutine was running.)
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *alt;
static const size_t alt_size = 64 * 1024;
static volatile int on_alt = -1, saw_signal;

static void handler(int sig, siginfo_t *si, void *uc) {
    (void)si; (void)uc;
    char local;
    on_alt = (&local >= alt && &local < alt + alt_size);
    saw_signal = sig;
}

static void *worker(void *arg) {
    (void)arg;
    char *own = malloc(alt_size);
    stack_t ss = { .ss_sp = own, .ss_size = alt_size, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    char *keep = alt; alt = own;
    pthread_kill(pthread_self(), SIGUSR1);
    usleep(1000);
    printf("thread: signal %d on its own altstack %d\n", saw_signal, on_alt);
    alt = keep;
    return NULL;
}

int main(void) {
    alt = malloc(alt_size);
    stack_t ss = { .ss_sp = alt, .ss_size = alt_size, .ss_flags = 0 };
    if (sigaltstack(&ss, NULL)) { perror("sigaltstack"); return 1; }
    stack_t old; memset(&old, 0, sizeof old);
    sigaltstack(NULL, &old);
    printf("reported back: %d\n", old.ss_sp == alt && old.ss_size == alt_size && !(old.ss_flags & SS_DISABLE));
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    pthread_kill(pthread_self(), SIGUSR1);
    usleep(1000);
    printf("main: signal %d on altstack %d\n", saw_signal, on_alt);
    on_alt = -1; saw_signal = 0;
    pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_join(t, NULL);
    return 0;
}
