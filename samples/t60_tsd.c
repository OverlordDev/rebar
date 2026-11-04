// Thread-specific data the way Go's runtime and Swift use it: a key from pthread_key_create, a value set and read back, per thread, with a destructor.
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static pthread_key_t key;
static int destroyed;
static void dtor(void *p) { __atomic_add_fetch(&destroyed, p ? 1 : 0, __ATOMIC_SEQ_CST); }
static void *worker(void *a) { int r = pthread_setspecific(key, (void *)(uintptr_t)a); void *v = pthread_getspecific(key); printf("worker set %d get %lu\n", r, (unsigned long)(uintptr_t)v); return NULL; }
int main() {
    int r = pthread_key_create(&key, dtor);
    printf("create %d\n", r);
    r = pthread_setspecific(key, (void *)0xc476c475c47957ull);
    printf("set %d get %lx\n", r, (unsigned long)(uintptr_t)pthread_getspecific(key));
    pthread_t t; pthread_create(&t, NULL, worker, (void *)7); pthread_join(t, NULL);
    printf("destroyed %d\n", destroyed);
    r = pthread_setspecific(key, (void *)0);
    printf("main again %lx\n", (unsigned long)(uintptr_t)pthread_getspecific(key));
    return 0;
}
