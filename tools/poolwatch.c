#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
static void *my_push(void) { void *t = objc_autoreleasePoolPush(); fprintf(stderr, "[pool] push tid=%llx token=%p from %p\n", (unsigned long long)(uintptr_t)pthread_self(), t, __builtin_return_address(0)); return t; }
static void my_pop(void *t) { fprintf(stderr, "[pool] pop  tid=%llx token=%p from %p\n", (unsigned long long)(uintptr_t)pthread_self(), t, __builtin_return_address(0)); objc_autoreleasePoolPop(t); }
__attribute__((used)) static struct { void *r, *o; } interposers[] __attribute__((section("__DATA,__interpose"))) = { { (void *)my_push, (void *)objc_autoreleasePoolPush }, { (void *)my_pop, (void *)objc_autoreleasePoolPop } };
