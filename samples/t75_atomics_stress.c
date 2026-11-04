// Contention on atomics: fetch_add, a CAS spinlock protecting a plain counter, a ticket lock, and a futex-less mutex handoff with ulock-style waiting (pthread cond); all must add up exactly.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#define NT 8
#define N 200000
static atomic_long fa; static atomic_int spin; static long plain; static atomic_uint ticket_next, ticket_now; static long plain2;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static long plain3;
static void *worker(void *a) {
    for (int i = 0; i < N; i++) {
        atomic_fetch_add(&fa, 1);
        int exp = 0; while (!atomic_compare_exchange_weak_explicit(&spin, &exp, 1, memory_order_acquire, memory_order_relaxed)) { exp = 0; }
        plain++; atomic_store_explicit(&spin, 0, memory_order_release);
        unsigned my = atomic_fetch_add(&ticket_next, 1); while (atomic_load_explicit(&ticket_now, memory_order_acquire) != my) {}
        plain2++; atomic_fetch_add_explicit(&ticket_now, 1, memory_order_release);
        pthread_mutex_lock(&mu); plain3++; pthread_mutex_unlock(&mu);
    }
    return NULL;
}
int main() {
    pthread_t t[NT]; for (int k = 0; k < NT; k++) pthread_create(&t[k], NULL, worker, NULL);
    for (int k = 0; k < NT; k++) pthread_join(t[k], NULL);
    printf("fetch_add %ld cas-lock %ld ticket %ld mutex %ld (expected %d)\n", (long)fa, plain, plain2, plain3, NT * N);
    return 0;
}
