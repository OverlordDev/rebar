// malloc_size() of a block from an allocator that registered its own malloc zone (Firefox's mozjemalloc): libmalloc asks every zone, so the program's zone must answer.
// (without it malloc_size returned 0 and Firefox looped for ever in code that grows a buffer until its usable size is big enough)
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char arena[1 << 16]; static size_t used;
static size_t z_size(malloc_zone_t *z, const void *p) { (void)z; return (const char *)p >= arena && (const char *)p < arena + sizeof arena ? *(const size_t *)((const char *)p - 16) : 0; }
static void *z_malloc(malloc_zone_t *z, size_t n) { (void)z; n = (n + 15) & ~15ull; char *p = arena + used + 16; *(size_t *)(p - 16) = n; used += n + 16; return p; }
static void *z_calloc(malloc_zone_t *z, size_t a, size_t b) { void *p = z_malloc(z, a * b); memset(p, 0, a * b); return p; }
static void *z_valloc(malloc_zone_t *z, size_t n) { return z_malloc(z, n); }
static void z_free(malloc_zone_t *z, void *p) { (void)z; (void)p; }
static void *z_realloc(malloc_zone_t *z, void *p, size_t n) { void *q = z_malloc(z, n); if (p) memcpy(q, p, z_size(z, p) < n ? z_size(z, p) : n); return q; }
int main(void) {
    static malloc_zone_t zone; memset(&zone, 0, sizeof zone);
    zone.size = z_size; zone.malloc = z_malloc; zone.calloc = z_calloc; zone.valloc = z_valloc; zone.free = z_free; zone.realloc = z_realloc; zone.zone_name = "t90"; zone.version = 8;
    malloc_zone_register(&zone);
    void *own = malloc_zone_malloc(&zone, 100), *sys = malloc(100);
    printf("own %zu system %d unknown %zu\n", malloc_size(own), malloc_size(sys) >= 100, malloc_size((void *)8));
    return 0;
}
