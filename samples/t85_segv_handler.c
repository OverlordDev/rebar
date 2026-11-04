// A SIGSEGV handler with SA_SIGINFO that reads the fault address from siginfo and moves the pc in the ucontext to a recovery function: what HotSpot does for implicit null
// checks and V8 / wasmtime for traps. The handler's ucontext must have the platform's layout; the program continues at the new pc.
#define _XOPEN_SOURCE 700
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ucontext.h>

static volatile int hits;
static void recover(void) { printf("recovered after fault %d\n", hits); fflush(stdout); _exit(0); }
static void handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    hits++;
    printf("SIGSEGV %d at %p\n", sig == SIGSEGV, si->si_addr);
#if defined(__aarch64__)
    uc->uc_mcontext->__ss.__pc = (uint64_t)recover;
#else
    uc->uc_mcontext->__ss.__rip = (uint64_t)recover;
#endif
}
int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO | SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    volatile int *p = (volatile int *)(uintptr_t)0x10;
    int v = *p;   // faults
    printf("not reached %d\n", v);
    return 0;
}
