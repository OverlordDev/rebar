// Go's runtime (and Swift's) read thread-specific values straight from the thread pointer: TPIDRRO_EL0 + key * 8 holds what pthread_setspecific stored.
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static pthread_key_t key;
static uint64_t direct(unsigned k) { uint64_t base, v; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base)); v = ((uint64_t *)base)[k]; return v; }
static void *worker(void *a) { pthread_setspecific(key, a); printf("worker direct %lu\n", (unsigned long)direct((unsigned)key)); return NULL; }
int main() {
    pthread_key_create(&key, NULL);
    pthread_setspecific(key, (void *)0x1234);
    printf("main direct %lx\n", (unsigned long)direct((unsigned)key));
    pthread_t t; pthread_create(&t, NULL, worker, (void *)7); pthread_join(t, NULL);
    return 0;
}
