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


// ---- the code snapshot.  Besides the list of blocks the cache keeps (file .rbs next to the .rbc) the translated code itself: the used part of the JIT arena and the indirect-branch cells,
// written as they are, and a record per block.  A run whose environment is the same (the same rebar binary, the arena, the indirect-branch cache and the stub table at the same addresses,
// the same images at the same addresses, the same host functions: all of it is the case for every start since the machine was booted) maps the file over its fresh arena with mmap, which takes
// no time, and enters the blocks into its tables: the program starts with its code translated, and a page of it is read from disk only when it is reached.  Anything that differs
// (a new rebar, another boot, a changed image) makes the snapshot unusable and the run falls back to the list of blocks (translating them in the background).
// Only blocks of the images that were loaded at start are kept, and only if they are still valid: code a program generates itself differs from run to run.
typedef struct { char magic[8]; uint64_t rebar_size; int64_t rebar_mtime; uint64_t arena, ic, stub_base, stub_size, guest_lo, guest_size, stubs_hash, knobs_hash, cpu_hash;
                 uint64_t thunk_len, top_off, cell_len, code_file_off, cell_file_off, rec_file_off, img_bytes; uint32_t nimg, nrec, npend, nsites, leaf_malloc, pad; uint64_t nblocks, ninsns; } SnHdr;
typedef struct { uint64_t pc; uint32_t code_off, len, exit_off, sites_idx; uint16_t nsites; uint8_t n, precise; } SnRec;
typedef struct { uint64_t target; uint32_t site_off, pad; } SnPend;
#define SN_PAGE 16384ull
#define SN_ALIGN(x) (((x) + SN_PAGE - 1) & ~(SN_PAGE - 1))
static char g_sn_file[PATH_MAX];
static bool g_sn_on, g_sn_restored; static int g_sn_nimg0 = -1; static uint64_t g_sn_stubs0; static size_t g_sn_saved_blocks; static uint64_t g_sn_restored_blocks;

static uint64_t sn_knobs_hash(void) {   // the JIT's own settings (anything that changes the code it writes)
    uint64_t sum = 0;
    for (char **e = environ; e && *e; e++) if (!strncmp(*e, "REBAR_JIT", 9) && strncmp(*e, "REBAR_JIT_CACHE", 15) && strncmp(*e, "REBAR_JIT_SNAPSHOT", 18)) sum += jc_fnv(*e);
    return sum;
}
static uint64_t sn_stubs_hash(void) { uint64_t h = 1469598103934665603ull; for (int k = 0; k < g_nstubs; k++) h = (h ^ (uint64_t)g_stubs[k].host) * 1099511628211ull; return h ^ (uint64_t)g_nstubs; }
static uint64_t sn_cpu_hash(void) { return (uint64_t)g_have_f16c | (uint64_t)g_have_sse41 << 1 | (uint64_t)g_have_ssse3 << 2 | (uint64_t)g_have_fma << 3 | (uint64_t)g_have_avx2 << 4; }
static bool sn_debug_modes(void) { return g_jit_count || g_jit_map || g_jit_dump || g_jit_nonly || g_jit_nskip; }
static void sn_env(SnHdr *h) {
    memset(h, 0, sizeof *h); memcpy(h->magic, "RBJS1\n\0\0", 8);
    uint64_t sz; int64_t mt; if (jc_stat(g_self_path, &sz, &mt)) { h->rebar_size = sz; h->rebar_mtime = mt; }
    h->arena = (uint64_t)g_jit_arena; h->ic = (uint64_t)g_jit_ic; h->stub_base = g_stub_base; h->stub_size = g_stub_size; h->guest_lo = g_guest_lo; h->guest_size = g_guest_size;
    h->stubs_hash = g_sn_stubs0; h->knobs_hash = sn_knobs_hash(); h->cpu_hash = sn_cpu_hash(); h->thunk_len = (uint64_t)(g_jit_thunk_end - g_jit_arena);
}
static bool sn_env_same(const SnHdr *a, const SnHdr *b) {
    return a->rebar_size != 0 && a->rebar_size == b->rebar_size && a->rebar_mtime == b->rebar_mtime && a->arena == b->arena && a->ic == b->ic && a->stub_base == b->stub_base && a->stub_size == b->stub_size &&
           a->guest_lo == b->guest_lo && a->guest_size == b->guest_size && a->stubs_hash == b->stubs_hash && a->knobs_hash == b->knobs_hash && a->cpu_hash == b->cpu_hash && a->thunk_len == b->thunk_len;
}
static bool sn_pad(int fd, size_t n) { char z[4096]; memset(z, 0, sizeof z); while (n) { size_t k = n < sizeof z ? n : sizeof z; if (!jc_write_all(fd, z, k)) return false; n -= k; } return true; }

static void sn_save_now(void);
static void sn_save_if(size_t min_new) {
    if (g_sn_saved_blocks && g_jit_reg_n < g_sn_saved_blocks + min_new) return;
    if (!g_sn_saved_blocks && g_jit_reg_n < 32) return;
    sn_save_now();
}
static void sn_save_now(void) {
    if (!g_sn_on || !g_jit_on || g_sn_nimg0 <= 0 || sn_debug_modes() || g_sn_file[0] == 0) return;
    pthread_mutex_lock(&g_jc_mu);
    if (t_jit_busy || g_jit_reg_n == 0) { pthread_mutex_unlock(&g_jc_mu); return; }   // (not enough new code to write it all again)
    JCArena ar; size_t cap = (size_t)g_jit_reg_n * (sizeof(SnRec) + 16 * sizeof(JSite)) + ((size_t)g_jit_cell_top - (size_t)g_jit_end) + ((size_t)1 << 20) * sizeof(SnPend) + (size_t)(g_jit_top - g_jit_arena) + ((size_t)64 << 20);
    if (!jc_arena(&ar, cap)) { pthread_mutex_unlock(&g_jc_mu); return; }
    t_jit_busy = true; pthread_mutex_lock(&g_jit_mu);   // no translation and no patching while the state is read
    SnHdr h; sn_env(&h);
    size_t top = (size_t)(g_jit_top - g_jit_arena), cells = (size_t)(g_jit_cell_top - g_jit_end);
    uint8_t *cellcopy = jc_alloc(&ar, cells ? cells : 8); if (cellcopy) memcpy(cellcopy, g_jit_end, cells);
    uint8_t *codecopy = jc_alloc(&ar, top); if (codecopy) memcpy(codecopy, g_jit_arena, top);   // (a copy, so that the file is written without holding the translator's lock)
    SnRec *recs = jc_alloc(&ar, g_jit_reg_n * sizeof *recs);
    size_t cap_sites = (size_t)g_jit_reg_n * 16; JSite *sites = jc_alloc(&ar, cap_sites * sizeof *sites);
    SnPend *pend = jc_alloc(&ar, ((size_t)1 << 20) * sizeof *pend);
    bool ok = cellcopy && codecopy && recs && sites && pend; uint32_t nrec = 0, nsites = 0, npend = 0;
    int nimg = g_sn_nimg0 < g_nimages ? g_sn_nimg0 : g_nimages;
    for (size_t k = 0; ok && k < g_jit_reg_n; k++) {
        const JReg *r = &g_jit_reg[k];
        JEnt *e = jit_probe(r->pc); if (!e || e->code != r->code) continue;     // translated and still valid
        bool inimg = false; for (int q = 0; q < nimg; q++) if (g_images[q] && r->pc >= g_images[q]->base && r->pc < g_images[q]->end) { inimg = true; break; }
        if (!inimg || nsites + r->nsites > cap_sites) continue;
        recs[nrec++] = (SnRec){r->pc, (uint32_t)(r->code - g_jit_arena), r->len, r->exit_off, nsites, (uint16_t)r->nsites, (uint8_t)r->n, (uint8_t)r->precise};
        if (r->nsites) memcpy(&sites[nsites], r->sites, r->nsites * sizeof(JSite));
        nsites += r->nsites;
    }
    for (size_t k = 0; ok && k + 8 <= cells; k += 8) {   // an indirect-branch cell that points at a block that is not kept goes back to the dummy block
        uint64_t v; memcpy(&v, cellcopy + k, 8);
        if (v == (uint64_t)g_jit_dummy || v == 0) continue;
        bool keep = false;
        if (v >= (uint64_t)g_jit_arena && v < (uint64_t)g_jit_top) { uint32_t off = (uint32_t)(v - (uint64_t)g_jit_arena); size_t lo = 0, hi = nrec; while (lo < hi) { size_t mid = (lo + hi) / 2; if (recs[mid].code_off < off) lo = mid + 1; else hi = mid; } keep = lo < nrec && recs[lo].code_off == off; }
        if (!keep) { uint64_t d = (uint64_t)g_jit_dummy; memcpy(cellcopy + k, &d, 8); }
    }
    for (size_t b = 0; ok && b < (1u << JIT_PEND_BITS); b++) for (JPend *w = g_jit_pend[b]; w && npend < (1u << 20); w = w->next) {   // the blocks that wait for a target
        uint32_t off = (uint32_t)(w->site - g_jit_arena); size_t l = 0, u = nrec;
        while (l < u) { size_t mid = (l + u) / 2; if (recs[mid].code_off + recs[mid].len <= off) l = mid + 1; else u = mid; }
        if (l < nrec && off >= recs[l].code_off) { pend[npend].target = w->target; pend[npend].site_off = off; pend[npend].pad = 0; npend++; }
    }
    uint8_t *imgs = jc_alloc(&ar, (size_t)(nimg > 0 ? nimg : 1) * (PATH_MAX + 40)); size_t ip = 0;
    for (int q = 0; ok && q < nimg; q++) {   // the images of the snapshot: path, size, modification time, base
        uint64_t sz = 0, base = g_images[q]->base; int64_t mt = 0; const char *path = g_images[q]->path; uint16_t len = (uint16_t)strlen(path);
        if (!imgs || !jc_stat(path, &sz, &mt)) { ok = false; break; }
        memcpy(imgs + ip, &len, 2); ip += 2; memcpy(imgs + ip, path, len); ip += len; memcpy(imgs + ip, &sz, 8); memcpy(imgs + ip + 8, &mt, 8); memcpy(imgs + ip + 16, &base, 8); ip += 24;
    }
    h.leaf_malloc = g_leaf_malloc_used; h.nblocks = g_jit_nblocks; h.ninsns = g_jit_ninsns;
    h.top_off = top; h.cell_len = cells; h.nimg = (uint32_t)nimg; h.nrec = nrec; h.npend = npend; h.nsites = nsites; h.img_bytes = ip;
    h.code_file_off = SN_PAGE; h.cell_file_off = h.code_file_off + SN_ALIGN(top); h.rec_file_off = h.cell_file_off + SN_ALIGN(cells);
    size_t reg_n_now = g_jit_reg_n;
    t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu);   // everything needed has been copied
    if (ok) {
        char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s.%d", g_sn_file, (int)getpid());
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            bool w = jc_write_all(fd, &h, sizeof h) && sn_pad(fd, SN_PAGE - sizeof h) && jc_write_all(fd, codecopy, top) && sn_pad(fd, SN_ALIGN(top) - top) &&
                     jc_write_all(fd, cellcopy, cells) && sn_pad(fd, SN_ALIGN(cells) - cells) && jc_write_all(fd, imgs, ip) && jc_write_all(fd, recs, nrec * sizeof *recs) &&
                     jc_write_all(fd, pend, npend * sizeof *pend) && jc_write_all(fd, sites, nsites * sizeof *sites);
            close(fd);
            if (w && rename(tmp, g_sn_file) == 0) g_sn_saved_blocks = reg_n_now; else unlink(tmp);
        }
    }
    jc_arena_free(&ar);
    pthread_mutex_unlock(&g_jc_mu);
}

// At the first kick, before the guest has run anything: map the snapshot over the arena when everything matches.
static void sn_restore(void) {
    if (!g_sn_on || !g_jit_on || g_jit_nblocks != 0 || sn_debug_modes() || g_sn_file[0] == 0) return;
    int fd = open(g_sn_file, O_RDONLY); if (fd < 0) return;
    struct stat st; SnHdr h, cur;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof h || pread(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) { close(fd); return; }
    sn_env(&cur);
    size_t need = (size_t)h.rec_file_off + h.img_bytes + (size_t)h.nrec * sizeof(SnRec) + (size_t)h.npend * sizeof(SnPend) + (size_t)h.nsites * sizeof(JSite);
    if (memcmp(h.magic, cur.magic, 8) || !sn_env_same(&h, &cur) || (size_t)st.st_size < need || h.top_off > (size_t)(g_jit_end - g_jit_arena) || h.top_off < h.thunk_len ||
        h.cell_len > (size_t)(g_jit_cell_end - g_jit_end) || h.nimg == 0 || (int)h.nimg != g_sn_nimg0 || h.nimg > MAX_IMAGES || h.nrec > (4u << 20) || h.npend > (1u << 20)) { close(fd); return; }
    JCArena ar; if (!jc_arena(&ar, (size_t)need + ((size_t)16 << 20))) { close(fd); return; }
    // the thunks this run generated must be what the snapshot starts with (the strongest check of every setting that shapes the code)
    uint8_t *tbuf = jc_alloc(&ar, h.thunk_len);
    bool good = tbuf && pread(fd, tbuf, h.thunk_len, (off_t)h.code_file_off) == (ssize_t)h.thunk_len && !memcmp(tbuf, g_jit_arena, h.thunk_len);
    size_t tail_len = need - (size_t)h.rec_file_off; uint8_t *tail = good ? jc_alloc(&ar, tail_len) : NULL;
    good = good && tail && pread(fd, tail, tail_len, (off_t)h.rec_file_off) == (ssize_t)tail_len;
    size_t tp = 0;
    for (uint32_t q = 0; good && q < h.nimg; q++) {   // the images: the same files at the same addresses
        uint16_t len; if (tp + 2 > h.img_bytes) { good = false; break; } memcpy(&len, tail + tp, 2); tp += 2;
        if (len >= PATH_MAX || tp + len + 24 > h.img_bytes) { good = false; break; }
        char path[PATH_MAX]; memcpy(path, tail + tp, len); path[len] = 0; tp += len;
        uint64_t sz, base, csz; int64_t mt, cmt; memcpy(&sz, tail + tp, 8); memcpy(&mt, tail + tp + 8, 8); memcpy(&base, tail + tp + 16, 8); tp += 24;
        if (!g_images[q] || strcmp(g_images[q]->path, path) || g_images[q]->base != base || !jc_stat(path, &csz, &cmt) || csz != sz || cmt != mt) good = false;
    }
    if (!good) { jc_arena_free(&ar); close(fd); return; }
    // everything matches: the code and the cells are mapped over the arena
    void *m1 = mmap(g_jit_arena, SN_ALIGN(h.top_off), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, (off_t)h.code_file_off);
    if (m1 == MAP_FAILED) { jc_arena_free(&ar); close(fd); return; }
    if (h.cell_len && mmap(g_jit_end, SN_ALIGN(h.cell_len), PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, (off_t)h.cell_file_off) == MAP_FAILED) abort();   // (the arena is half replaced: nothing to go back to)
    close(fd);
    const SnRec *rb = (const SnRec *)(tail + h.img_bytes); const SnPend *pb = (const SnPend *)(rb + h.nrec); const JSite *sb_src = (const JSite *)(pb + h.npend);
    JSite *sb = h.nsites ? jgrow(NULL, 0, (size_t)h.nsites * sizeof(JSite)) : NULL;   // (the blocks' records point into it for the life of the process)
    if (h.nsites) memcpy(sb, sb_src, (size_t)h.nsites * sizeof(JSite));
    t_jit_busy = true; pthread_mutex_lock(&g_jit_mu);
    g_jit_top = g_jit_arena + h.top_off; g_jit_cell_top = g_jit_end + h.cell_len;
    if (g_jit_reg_cap < h.nrec + 65536) { size_t nc = (size_t)h.nrec + 65536; g_jit_reg = jgrow(g_jit_reg, g_jit_reg_cap * sizeof(JReg), nc * sizeof(JReg)); g_jit_reg_cap = nc; }
    for (uint32_t k = 0; k < h.nrec; k++) {
        const SnRec *r = &rb[k]; uint8_t *code = g_jit_arena + r->code_off;
        jit_insert(r->pc, code);
        JBlk *b = jpool_alloc(1, sizeof *b); b->pc = r->pc; b->bytes = (uint32_t)r->n * 4; b->exit_off = r->exit_off; b->code = code;
        unsigned ph = (unsigned)((r->pc >> 12) & ((1 << JIT_PAGE_BITS) - 1)); b->next = g_jit_pages[ph]; g_jit_pages[ph] = b;
        g_jit_reg[g_jit_reg_n++] = (JReg){code, r->len, r->n, r->pc, r->precise != 0, r->nsites ? &sb[r->sites_idx] : NULL, r->nsites, r->exit_off};
    }
    for (uint32_t k = 0; k < h.npend; k++) {
        JPend *w = jpool_alloc(0, sizeof *w); w->target = pb[k].target; w->site = g_jit_arena + pb[k].site_off;
        unsigned qh = (unsigned)((w->target >> 2) * 0x9E3779B97F4A7C15ull >> (64 - JIT_PEND_BITS)); w->next = g_jit_pend[qh]; g_jit_pend[qh] = w;
    }
    g_jit_nblocks = h.nblocks; g_jit_ninsns = h.ninsns; if (h.leaf_malloc) g_leaf_malloc_used = true;
    g_sn_restored = true; g_sn_restored_blocks = h.nrec; g_sn_saved_blocks = g_jit_reg_n;
    t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu);
    jc_arena_free(&ar);
}

static void jc_flush_map(void) { if (g_jit_map) fflush(g_jit_map); }
static void jc_atexit(void) {
    if (getenv("REBAR_DBG_ADDR")) { char m[512]; int n = snprintf(m, sizeof m, "[addr] arena=%p stubbase=%llx guestlo=%llx strlen=%p sin=%p malloc=%p dyldslide=%p main_base=%llx nimg=%d img1=%llx\n", (void *)g_jit_arena, (unsigned long long)g_stub_base, (unsigned long long)g_guest_lo, dlsym(RTLD_DEFAULT, "strlen"), dlsym(RTLD_DEFAULT, "sin"), dlsym(RTLD_DEFAULT, "malloc"), (void *)&environ, (unsigned long long)g_main_image->base, g_nimages, (unsigned long long)(g_nimages > 1 ? g_images[1]->base : 0)); (void)!write(2, m, (size_t)n); }
    if (g_jit_map) fflush(g_jit_map);
    jc_save();
    sn_save_if(1000);
    if (g_jc_debug) { char m[PATH_MAX + 160]; int n = snprintf(m, sizeof m, "[jitcache] pid %d: %llu blocks translated by the guest, %llu in the background (%zu cached), %llu restored from the snapshot, file %s\n", (int)getpid(), (unsigned long long)g_jc_main_compiles, (unsigned long long)g_jc_bg_compiles, g_jce_n, (unsigned long long)g_sn_restored_blocks, g_jc_file); if (n > 0) (void)!write(2, m, (size_t)n); }
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
    for (int tick = 0;; tick++) { sleep(5); if (g_jc_n >= g_jc_saved_n + 256 || (g_jc_n && !g_jc_saved_n)) jc_save(); if (tick % 6 == 5) sn_save_if(30000); }
    return NULL;
}

// Called when images [.., upto) have been through their fixups (start_images): the cache's blocks in them can be translated now.
static void jitcache_kick(int upto) {
    if (!g_jc_on) return;
    if (g_sn_nimg0 < 0) { g_sn_nimg0 = upto; g_sn_stubs0 = sn_stubs_hash(); sn_restore(); }   // the first kick: the images loaded at start are the ones a snapshot can hold
    pthread_mutex_lock(&g_jc_mu); g_jc_ready = upto; pthread_cond_signal(&g_jc_cv); pthread_mutex_unlock(&g_jc_mu);
}
// The snapshots are big (tens to hundreds of megabytes of code per program): when the cache directory holds more than REBAR_JIT_CACHE_MAX_MB (default 6000), the least recently written
// snapshots go.  Done once at start, before the guest runs (this uses malloc).
#include <dirent.h>
static void jc_evict(void) {
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", g_jc_file); char *sl = strrchr(dir, '/'); if (!sl) return; *sl = 0;
    const char *e = getenv("REBAR_JIT_CACHE_MAX_MB"); uint64_t limit = (uint64_t)(e ? atoll(e) : 6000) << 20;
    DIR *d = opendir(dir); if (!d) return;
    struct { int64_t mt; uint64_t sz; char name[64]; } *f = malloc(sizeof(*f) * 2048); size_t n = 0; uint64_t total = 0;
    struct dirent *de;
    while (f && (de = readdir(d)) && n < 2048) {
        size_t l = strlen(de->d_name); if (l < 5 || strcmp(de->d_name + l - 4, ".rbs") || l >= 64) continue;
        char path[PATH_MAX + 80]; snprintf(path, sizeof path, "%s/%s", dir, de->d_name); struct stat st; if (stat(path, &st) != 0) continue;
        f[n].mt = (int64_t)st.st_mtimespec.tv_sec; f[n].sz = (uint64_t)st.st_size; memcpy(f[n].name, de->d_name, l + 1); total += f[n].sz; n++;
    }
    closedir(d);
    while (f && total > limit && n > 0) {   // the oldest first
        size_t k = 0; for (size_t q = 1; q < n; q++) if (f[q].mt < f[k].mt) k = q;
        char path[PATH_MAX + 80]; snprintf(path, sizeof path, "%s/%s", dir, f[k].name);
        if (strcmp(path, g_sn_file) != 0) unlink(path);
        total -= f[k].sz; f[k] = f[--n];
    }
    free(f);
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
    { const char *sn = getenv("REBAR_JIT_SNAPSHOT"); size_t n = strlen(g_jc_file);
      if (!(sn && !strcmp(sn, "0")) && n > 4 && !strcmp(g_jc_file + n - 4, ".rbc")) { snprintf(g_sn_file, sizeof g_sn_file, "%.*s.rbs", (int)(n - 4), g_jc_file); g_sn_on = true; } }
    jc_evict();
    if (jc_arena(&g_jc_ar, 512u << 20) && jc_read(g_jc_file, &g_jc_ar, &g_jci, &g_jci_n, &g_jce, &g_jce_n) && g_jce_n) {
        g_jce_done = jc_alloc(&g_jc_ar, g_jce_n);
        if (g_jce_done) { memset(g_jce_done, 0, g_jce_n); pthread_t th; pthread_create(&th, NULL, jc_thread, NULL); pthread_detach(th); }
    }
    pthread_t sv; pthread_create(&sv, NULL, jc_saver, NULL); pthread_detach(sv);
    atexit(jc_atexit);
}
