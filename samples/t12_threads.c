// Guest threads hammering shared atomics and a mutex: counts must be exact.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
static atomic_long counter, cas_counter;
static long plain_locked;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
    for (int i = 0; i < 20000; i++) {
        atomic_fetch_add(&counter, 1);
        long v = atomic_load(&cas_counter);
        while (!atomic_compare_exchange_weak(&cas_counter, &v, v + 2)) {}
        pthread_mutex_lock(&mu); plain_locked += 3; pthread_mutex_unlock(&mu);
    }
    return (void *)(long)(*(int *)arg * 10);
}
int main(void) {
    pthread_t t[4]; int ids[4]; long total = 0;
    for (int i = 0; i < 4; i++) { ids[i] = i + 1; pthread_create(&t[i], NULL, worker, &ids[i]); }
    for (int i = 0; i < 4; i++) { void *r; pthread_join(t[i], &r); total += (long)r; }
    printf("counter=%ld cas=%ld locked=%ld ret=%ld\n", (long)counter, (long)cas_counter, plain_locked, total);
    return 0;
}
