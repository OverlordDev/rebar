// Zig's and Go-like futexes on macOS: __ulock_wait2 / __ulock_wake (UL_COMPARE_AND_WAIT, 32-bit; and the 64-bit compare form) used for a ping-pong between two threads.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
extern int __ulock_wait2(uint32_t op, void *addr, uint64_t value, uint64_t timeout_ns, uint64_t value2);
extern int __ulock_wake(uint32_t op, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT 1
#define UL_COMPARE_AND_WAIT64 5
#define ULF_NO_ERRNO 0x1000000
static _Atomic uint32_t turn32; static _Atomic uint64_t turn64;
static int rounds = 20000;
static void *ping32(void *p) { int who = (int)(long)p; for (int i = 0; i < rounds; i++) {
    while (atomic_load(&turn32) != (uint32_t)who) __ulock_wait2(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &turn32, (uint64_t)(1 - who), 0, 0);
    atomic_store(&turn32, 1 - who); __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, &turn32, 0); } return NULL; }
static void *ping64(void *p) { int who = (int)(long)p; for (int i = 0; i < rounds; i++) {
    while (atomic_load(&turn64) != (uint64_t)who) __ulock_wait2(UL_COMPARE_AND_WAIT64 | ULF_NO_ERRNO, &turn64, (uint64_t)(1 - who), 0, 8);
    atomic_store(&turn64, 1 - who); __ulock_wake(UL_COMPARE_AND_WAIT64 | ULF_NO_ERRNO, &turn64, 0); } return NULL; }
int main() {
    pthread_t a, b;
    pthread_create(&a, NULL, ping32, (void *)0); pthread_create(&b, NULL, ping32, (void *)1); pthread_join(a, NULL); pthread_join(b, NULL);
    printf("ulock32 ok %u\n", (unsigned)turn32);
    pthread_create(&a, NULL, ping64, (void *)0); pthread_create(&b, NULL, ping64, (void *)1); pthread_join(a, NULL); pthread_join(b, NULL);
    printf("ulock64 ok %llu\n", (unsigned long long)turn64);
    return 0;
}
