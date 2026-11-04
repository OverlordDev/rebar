// A replacement allocator in its own dylib (the way Firefox's libmozglue replaces malloc and operator new / delete): memory is carved out of a private arena, and free() of anything that did not come from there aborts
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static uint8_t *arena; static size_t used; static const size_t ARENA = 64u << 20;
extern "C" { int g_own_allocs, g_own_frees; }
extern "C" {
static void init(void) { if (!arena) arena = (uint8_t *)mmap(NULL, ARENA, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); }
void *malloc(size_t n) { init(); n = (n + 15) & ~15ull; uint64_t *p = (uint64_t *)(arena + used); used += n + 16; p[0] = 0x4d594d414c4c4f43ull; p[1] = n; g_own_allocs++; return p + 2; }
void free(void *q) { if (!q) return; uint64_t *p = (uint64_t *)q - 2; if (!((uint8_t *)p >= arena && (uint8_t *)p < arena + ARENA) || p[0] != 0x4d594d414c4c4f43ull) { fprintf(stderr, "free of a foreign pointer %p\n", q); abort(); } g_own_frees++; }
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); memset(p, 0, a * b); return p; }
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = (char *)malloc(n); memcpy(p, s, n); return p; }
void *realloc(void *q, size_t n) { if (!q) return malloc(n); uint64_t old = ((uint64_t *)q)[-1]; void *p = malloc(n); memcpy(p, q, old < n ? old : n); free(q); return p; }
}
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
