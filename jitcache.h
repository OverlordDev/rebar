// The translation cache.  What is cached is the list of guest addresses the JIT translated (start pc of every block, in translation order), per program, in
// ~/Library/Caches/rebar/jit/<name>.rbc.  The machine code itself is not stored (it holds host addresses that change with every start), the list is: the next
// run translates the same blocks on a background thread while the guest is still starting, so the translation cost (the micro-freezes when new code is reached,
// the long start of big programs) is paid out of the guest's way.  A list is tied to the program's image files (path, size, modification time) and holds
// offsets from the image base, so it is valid for as long as those files are, whatever rebar version translates them.
// REBAR_JIT_CACHE=0 turns it off, REBAR_JIT_CACHE_DIR=<dir> moves it, REBAR_JIT_CACHE_DEBUG=1 prints what it did at exit.
//
// Nothing here uses malloc or stdio after the guest has started: a guest that makes its own allocator the process's default malloc zone (jemalloc in rustc, Chromium's
// allocator shim) would be handed these threads' allocations, and its zone functions are guest code.  Memory comes from mmap (a bump arena), files are read and written with
// open / read / write.
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

typedef struct { const char *path; uint64_t size; int64_t mtime; } JCImg;
typedef struct { uint32_t img, off; } JCEnt;
typedef struct { uint8_t *base; size_t cap, used; } JCArena;
static JCImg *g_jci; static uint32_t g_jci_n;   // the images of the cache file as loaded
static JCEnt *g_jce; static size_t g_jce_n;     // its entries
static JCArena g_jc_ar;                         // what they live in (for the life of the process)
static char g_jc_file[PATH_MAX];
static bool g_jc_on, g_jc_debug;
static pthread_mutex_t g_jc_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_jc_cv = PTHREAD_COND_INITIALIZER;
static int g_jc_ready;            // images [0, g_jc_ready) have been through their fixups: their code is final
static size_t g_jc_saved_n;       // blocks recorded when the file was last written
static uint8_t *g_jce_done;       // 1: this entry has been translated (or must not be)
static int g_jc_img_of_cache[MAX_IMAGES];   // cache image index -> loaded image index (-1 not looked at yet, -2 not loaded / a different file)

static bool jc_arena(JCArena *a, size_t cap) { a->base = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0); a->cap = cap; a->used = 0; if (a->base == MAP_FAILED) { a->base = NULL; return false; } return true; }
static void jc_arena_free(JCArena *a) { if (a->base) munmap(a->base, a->cap); a->base = NULL; }
static void *jc_alloc(JCArena *a, size_t n) { n = (n + 15) & ~(size_t)15; if (!a->base || a->used + n > a->cap) return NULL; void *p = a->base + a->used; a->used += n; return p; }

static bool jc_stat(const char *path, uint64_t *size, int64_t *mtime) { struct stat st; if (stat(path, &st) != 0) return false; *size = (uint64_t)st.st_size; *mtime = (int64_t)st.st_mtimespec.tv_sec * 1000000000ll + st.st_mtimespec.tv_nsec; return true; }
static uint64_t jc_fnv(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; } return h; }

static bool jc_path(const char *exe, char *out, size_t sz) {
    const char *dir = getenv("REBAR_JIT_CACHE_DIR"), *home = getenv("HOME"); char d[PATH_MAX];
    if (dir && *dir) snprintf(d, sizeof d, "%s", dir);
    else if (home && *home) snprintf(d, sizeof d, "%s/Library/Caches/rebar/jit", home);
    else return false;
    for (char *q = d + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(d, 0755); *q = '/'; }   // mkdir -p
    mkdir(d, 0755);
    uint64_t size; int64_t mt; if (!jc_stat(exe, &size, &mt)) return false;
    snprintf(out, sz, "%s/%016llx-%llx.rbc", d, (unsigned long long)jc_fnv(exe), (unsigned long long)size);
    return true;
}

// file: "RBJC1\n", u32 images, per image { u16 len, path, u64 size, i64 mtime }, u32 entries, entries { u32 image, u32 offset }.  The whole file is read into the arena and used in place.
static bool jc_read(const char *file, JCArena *ar, JCImg **imgs, uint32_t *nimg, JCEnt **ents, size_t *nent) {
    int fd = open(file, O_RDONLY); if (fd < 0) return false;
    struct stat st; if (fstat(fd, &st) != 0 || st.st_size < 14 || st.st_size > (512 << 20)) { close(fd); return false; }
    uint8_t *buf = jc_alloc(ar, (size_t)st.st_size + 16); if (!buf) { close(fd); return false; }
    size_t got = 0; while (got < (size_t)st.st_size) { ssize_t r = read(fd, buf + got, (size_t)st.st_size - got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (got != (size_t)st.st_size || memcmp(buf, "RBJC1\n", 6)) return false;
    size_t p = 6; uint32_t n; memcpy(&n, buf + p, 4); p += 4;
    if (n > MAX_IMAGES) return false;
    JCImg *im = jc_alloc(ar, (n ? n : 1) * sizeof *im); if (!im) return false;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t len; if (p + 2 > got) return false; memcpy(&len, buf + p, 2); p += 2;
        if (p + len + 16 > got) return false;
        char *path = jc_alloc(ar, (size_t)len + 1); if (!path) return false; memcpy(path, buf + p, len); path[len] = 0; p += len;
        im[k].path = path; memcpy(&im[k].size, buf + p, 8); memcpy(&im[k].mtime, buf + p + 8, 8); p += 16;
    }
    uint32_t ne; if (p + 4 > got) return false; memcpy(&ne, buf + p, 4); p += 4;
    if (ne > (4u << 20) || p + (size_t)ne * sizeof(JCEnt) > got) return false;
    JCEnt *en = jc_alloc(ar, (ne ? ne : 1) * sizeof *en); if (!en) return false; memcpy(en, buf + p, (size_t)ne * sizeof *en);
    *imgs = im; *nimg = n; *ents = en; *nent = ne; return true;
}

// the loaded image that holds pc (and the offset), or -1
static int jc_image_of(uint64_t pc, uint32_t *off) {
    int n = g_nimages;
    for (int k = 0; k < n; k++) { GImage *g = g_images[k]; if (g && pc >= g->base && pc < g->end) { *off = (uint32_t)(pc - g->base); return k; } }
    return -1;
}

static bool jc_write_all(int fd, const void *p, size_t n) { const uint8_t *q = p; while (n) { ssize_t w = write(fd, q, n); if (w <= 0) return false; q += w; n -= (size_t)w; } return true; }

static void jc_save(void) {
    if (!g_jc_on || g_jc_file[0] == 0) return;
    pthread_mutex_lock(&g_jc_mu);
    if (t_jit_busy) { pthread_mutex_unlock(&g_jc_mu); return; }   // (called from inside the translator: the next save will do)
    size_t snap_n = g_jc_n;
    JCArena ar; size_t need = ((size_t)snap_n * 8 * 3 + 64) * 4 + ((size_t)1 << 20);   // generous (mapped lazily): the snapshot, the entries, the duplicate table, the old file
    need += (size_t)64 << 20;
    if (!jc_arena(&ar, need)) { pthread_mutex_unlock(&g_jc_mu); return; }
    uint64_t *snap = jc_alloc(&ar, (snap_n ? snap_n : 1) * sizeof *snap);
    t_jit_busy = true; pthread_mutex_lock(&g_jit_mu); memcpy(snap, g_jc_pcs, snap_n * sizeof *snap); t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu);
    if (snap_n == g_jc_saved_n && g_jc_saved_n) { jc_arena_free(&ar); pthread_mutex_unlock(&g_jc_mu); return; }
    // merge with what is on disk now (another process of the same program may have written meanwhile): its entries first, ours after, no duplicates
    JCImg *oi = NULL; uint32_t on = 0; JCEnt *oe = NULL; size_t one = 0; jc_read(g_jc_file, &ar, &oi, &on, &oe, &one);
    uint32_t cap_i = on + (uint32_t)(g_nimages < MAX_IMAGES ? g_nimages : MAX_IMAGES) + 1; JCImg *ni = jc_alloc(&ar, cap_i * sizeof *ni); uint32_t nn = 0;
    size_t cap_e = one + snap_n + 16; JCEnt *ne = jc_alloc(&ar, cap_e * sizeof *ne); size_t nne = 0;
    size_t tab = 1; while (tab < (one + snap_n) * 2 + 16) tab <<= 1; uint64_t *seen = jc_alloc(&ar, tab * 8);
    if (!ni || !ne || !seen) { jc_arena_free(&ar); pthread_mutex_unlock(&g_jc_mu); return; }
    memset(seen, 0, tab * 8);
    int map_old[MAX_IMAGES]; for (uint32_t k = 0; k < on && k < MAX_IMAGES; k++) {   // the files of the old table that still match
        uint64_t sz; int64_t mt; map_old[k] = -1;
        if (jc_stat(oi[k].path, &sz, &mt) && sz == oi[k].size && mt == oi[k].mtime) { ni[nn] = oi[k]; map_old[k] = (int)nn++; }
    }
    int map_new[MAX_IMAGES]; int nimg = g_nimages; if (nimg > MAX_IMAGES) nimg = MAX_IMAGES;
    for (int k = 0; k < nimg; k++) {   // then the loaded images
        map_new[k] = -1; uint64_t sz; int64_t mt; if (!g_images[k] || !g_images[k]->path || !jc_stat(g_images[k]->path, &sz, &mt)) continue;
        for (uint32_t q = 0; q < nn; q++) if (!strcmp(ni[q].path, g_images[k]->path) && ni[q].size == sz && ni[q].mtime == mt) { map_new[k] = (int)q; break; }
        if (map_new[k] < 0 && nn < cap_i) {
            size_t len = strlen(g_images[k]->path); char *cp = jc_alloc(&ar, len + 1); if (!cp) continue; memcpy(cp, g_images[k]->path, len + 1);
            ni[nn].path = cp; ni[nn].size = sz; ni[nn].mtime = mt; map_new[k] = (int)nn++;
        }
    }
    #define JC_ADD(I, O) do { uint64_t key = ((uint64_t)(I) << 32) | (O); size_t h = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 20) & (tab - 1), dup = 0; \
        while (seen[h]) { if (seen[h] == key + 1) { dup = 1; break; } h = (h + 1) & (tab - 1); } \
        if (!dup && nne < cap_e) { seen[h] = key + 1; ne[nne].img = (I); ne[nne].off = (O); nne++; } } while (0)
    for (size_t k = 0; k < one; k++) if (oe[k].img < on && map_old[oe[k].img] >= 0) JC_ADD((uint32_t)map_old[oe[k].img], oe[k].off);
    for (size_t k = 0; k < snap_n; k++) { uint32_t off; int li = jc_image_of(snap[k], &off); if (li >= 0 && map_new[li] >= 0) JC_ADD((uint32_t)map_new[li], off); }
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s.%d", g_jc_file, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        bool ok = jc_write_all(fd, "RBJC1\n", 6) && jc_write_all(fd, &nn, 4);
        for (uint32_t k = 0; ok && k < nn; k++) { uint16_t len = (uint16_t)strlen(ni[k].path); ok = jc_write_all(fd, &len, 2) && jc_write_all(fd, ni[k].path, len) && jc_write_all(fd, &ni[k].size, 8) && jc_write_all(fd, &ni[k].mtime, 8); }
        uint32_t cnt = (uint32_t)nne; ok = ok && jc_write_all(fd, &cnt, 4) && jc_write_all(fd, ne, nne * sizeof *ne);
        close(fd);
        if (ok && rename(tmp, g_jc_file) == 0) g_jc_saved_n = snap_n; else unlink(tmp);
    }
    jc_arena_free(&ar);
    pthread_mutex_unlock(&g_jc_mu);
}

static void jc_flush_map(void) { if (g_jit_map) fflush(g_jit_map); }
static void jc_atexit(void) {
    if (g_jit_map) fflush(g_jit_map);
    jc_save();
    if (g_jc_debug) { char m[PATH_MAX + 160]; int n = snprintf(m, sizeof m, "[jitcache] pid %d: %llu blocks translated by the guest, %llu in the background (%zu cached), file %s\n", (int)getpid(), (unsigned long long)g_jc_main_compiles, (unsigned long long)g_jc_bg_compiles, g_jce_n, g_jc_file); if (n > 0) (void)!write(2, m, (size_t)n); }
}

// the cache's images that are loaded now (same path, size and modification time)
static void jc_resolve(void) {
    int n = g_jc_ready;
    for (uint32_t k = 0; k < g_jci_n; k++) {
        if (g_jc_img_of_cache[k] != -1) continue;
        for (int q = 0; q < n; q++) if (g_images[q] && g_images[q]->path && !strcmp(g_images[q]->path, g_jci[k].path)) {
            uint64_t sz; int64_t mt; if (jc_stat(g_jci[k].path, &sz, &mt) && sz == g_jci[k].size && mt == g_jci[k].mtime) g_jc_img_of_cache[k] = q; else g_jc_img_of_cache[k] = -2;
            break;
        }
    }
}

static void jc_block_signals(void) { sigset_t all; sigfillset(&all); pthread_sigmask(SIG_BLOCK, &all, NULL); }   // (a process-directed signal must reach a guest thread, whose handler this thread cannot run)
static void *jc_thread(void *arg) {
    (void)arg; t_jc_bg = true; jc_block_signals();
    for (;;) {
        pthread_mutex_lock(&g_jc_mu);
        pthread_cond_wait(&g_jc_cv, &g_jc_mu);
        pthread_mutex_unlock(&g_jc_mu);
        jc_resolve();
        for (size_t k = 0; k < g_jce_n; k++) {
            if (g_jce_done[k]) continue;
            int li = g_jce[k].img < g_jci_n ? g_jc_img_of_cache[g_jce[k].img] : -2;
            if (li == -2) { g_jce_done[k] = 1; continue; }
            if (li < 0) continue;   // its image is not loaded yet: a later kick
            g_jce_done[k] = 1;
            if ((size_t)(g_jit_top - g_jit_arena) > JIT_ARENA * 4 / 10) return NULL;   // enough pre-translated code: the arena is for what the program does next
            uint64_t pc = g_images[li]->base + g_jce[k].off;
            if (pc >= g_images[li]->end) continue;
            JEnt *e = jit_probe(pc);   // (a racy peek: jit_compile looks again under the lock)
            if (!e) jit_compile(pc);
        }
    }
    return NULL;
}
static void *jc_saver(void *arg) {
    (void)arg; jc_block_signals();
    for (;;) { sleep(5); if (g_jc_n >= g_jc_saved_n + 256 || (g_jc_n && !g_jc_saved_n)) jc_save(); }
    return NULL;
}

// Called when images [.., upto) have been through their fixups (start_images): the cache's blocks in them can be translated now.
static void jitcache_kick(int upto) {
    if (!g_jc_on) return;
    pthread_mutex_lock(&g_jc_mu); g_jc_ready = upto; pthread_cond_signal(&g_jc_cv); pthread_mutex_unlock(&g_jc_mu);
}
// Called once the executable is loaded (its path names the cache file).
static void jitcache_init(const char *exe) {
    if (g_jit_map) atexit(jc_flush_map);
    const char *e = getenv("REBAR_JIT_CACHE");
    if (!g_jit_on || (e && !strcmp(e, "0")) || getenv("REBAR_JIT_ONLY") || getenv("REBAR_JIT_SKIP")) return;
    if (!jc_path(exe, g_jc_file, sizeof g_jc_file)) return;
    g_jc_debug = getenv("REBAR_JIT_CACHE_DEBUG") != NULL;
    for (int k = 0; k < MAX_IMAGES; k++) g_jc_img_of_cache[k] = -1;
    g_jc_on = true; g_jc_rec = true;
    if (jc_arena(&g_jc_ar, 512u << 20) && jc_read(g_jc_file, &g_jc_ar, &g_jci, &g_jci_n, &g_jce, &g_jce_n) && g_jce_n) {
        g_jce_done = jc_alloc(&g_jc_ar, g_jce_n);
        if (g_jce_done) { memset(g_jce_done, 0, g_jce_n); pthread_t th; pthread_create(&th, NULL, jc_thread, NULL); pthread_detach(th); }
    }
    pthread_t sv; pthread_create(&sv, NULL, jc_saver, NULL); pthread_detach(sv);
    atexit(jc_atexit);
}
