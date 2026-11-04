// Go's runtime installs a SIGURG handler (goroutine preemption) that works on the arm64 ucontext; rebar does not deliver it (the host's frame is x86). The program must go on.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
static void handler(int sig, siginfo_t *si, void *ctx) { (void)sig; (void)si; (void)ctx; }
int main() {
    struct sigaction sa = {0}, old;
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    int r = sigaction(SIGURG, &sa, &old);
    pthread_kill(pthread_self(), SIGURG);
    pthread_kill(pthread_self(), SIGURG);
    printf("sigaction %d survived\n", r);
    return 0;
}
