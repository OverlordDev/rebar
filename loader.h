// Guest image loader: the main executable plus the arm64 dylibs / frameworks it depends on
// (and anything the guest dlopens). It plays the part of dyld for arm64 images:
//   - every guest image lives inside one reserved address range [g_guest_lo, +g_guest_size), so
//     "is this pc guest code?" stays a single range check;
//   - dylibs are placed after the executable and slid; segments are mapped copy-on-write
//     straight from the file (the 278 MB Electron Framework costs nothing until touched);
//   - install names are resolved through @rpath / @executable_path / @loader_path; system
//     libraries (/usr/lib, /System, ...) are the host's x86_64 builds, loaded with dlopen;
//   - binds resolve against the exports of the guest images first, then the host.

#include <limits.h>
#include <mach-o/nlist.h>
#include <sys/stat.h>

// ------------------------------------------------------------------------ file helpers

static const uint8_t *arm64_slice_or_null(const uint8_t *file, size_t len) {
    if (len < 32) return NULL;
    uint32_t magic = *(const uint32_t *)file;
    if (magic == MH_MAGIC_64) return ((const struct mach_header_64 *)file)->cputype == CPU_TYPE_ARM64 ? file : NULL;
    if (magic == FAT_CIGAM) {
        const struct fat_header *fh = (const void *)file;
        const struct fat_arch *fa = (const void *)(fh + 1);
        const uint8_t *arme = NULL;   // (an arm64e slice is used only when there is no plain arm64 one)
        for (uint32_t k = 0; k < __builtin_bswap32(fh->nfat_arch); k++)
            if ((cpu_type_t)__builtin_bswap32(fa[k].cputype) == CPU_TYPE_ARM64) {
                uint32_t off = __builtin_bswap32(fa[k].offset);
                if (off >= len) continue;
                if ((__builtin_bswap32(fa[k].cpusubtype) & 0xff) == 2) { if (!arme) arme = file + off; continue; }
                return file + off;
            }
        if (arme) return arme;
    }
    return NULL;
}

static GImage *find_image(const char *real) {
    for (int k = 0; k < g_nimages; k++) if (!strcmp(g_images[k]->path, real)) return g_images[k];
    return NULL;
}

static bool system_path(const char *p) {
    return !strncmp(p, "/usr/lib/", 9) || !strncmp(p, "/System/", 8) || !strncmp(p, "/Library/Apple/", 15);
}

static char *expand_origin(const char *tmpl, const GImage *loader) {  // @executable_path / @loader_path
    char buf[PATH_MAX];
    const char *rest;
    const char *src = NULL;
    if (!strncmp(tmpl, "@executable_path", 16)) { src = g_main_image->path; rest = tmpl + 16; }
    else if (!strncmp(tmpl, "@loader_path", 12)) { src = (loader ? loader : g_main_image)->path; rest = tmpl + 12; }
    else return strdup(tmpl);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", src);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    snprintf(buf, sizeof buf, "%s%s", dir, rest);
    return strdup(buf);
}

// Returns a malloc'd real path of the guest file for this install name, or NULL when the
// dependency is a host (system) library.
static char *resolve_guest_dep(const GImage *loader, const char *name) {
    char cand[PATH_MAX], real[PATH_MAX];
    if (!strncmp(name, "@rpath/", 7)) {
        const GImage *chain[2] = {loader, g_main_image};
        for (int c = 0; c < 2; c++)
            for (int r = 0; r < chain[c]->nrpath; r++) {
                char *rp = expand_origin(chain[c]->rpath[r], chain[c]);
                snprintf(cand, sizeof cand, "%s/%s", rp, name + 7);
                free(rp);
                if (realpath(cand, real)) return strdup(real);
            }
        // No rpath leads to it (a binary with no LC_RPATH at all, like the python3 launcher of GIMP's bundle, whose Python library sits several levels up): look for the rest of the
        // name below each directory above the loader, as a bundle would have it laid out
        if (loader && loader->path) {
            char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", loader->path);
            for (int up = 0; up < 8; up++) {
                char *slash = strrchr(dir, '/'); if (!slash || slash == dir) break; *slash = 0;
                snprintf(cand, sizeof cand, "%s/%s", dir, name + 7);
                if (realpath(cand, real)) return strdup(real);
            }
        }
        return NULL;
    }
    if (name[0] == '@') {
        char *e = expand_origin(name, loader);
        char *r = realpath(e, real) ? strdup(real) : NULL;
        free(e);
        return r;
    }
    if (system_path(name)) return NULL;
    return realpath(name, real) ? strdup(real) : NULL;  // a non-system absolute path: guest if the file is there
}

// ------------------------------------------------------------------------ mapping

static void reserve_guest_region(void) {
    const uint64_t base = 0x100000000ull, size = 8ull << 30;
    void *p = mmap((void *)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p != (void *)base) { fprintf(stderr, "rebar: cannot reserve the guest address range (got %p)\n", p); exit(1); }
    g_guest_lo = base; g_guest_size = size;
    g_next_free = base;
}

static void parse_load_commands(GImage *img) {
    const struct mach_header_64 *mh = img->mh;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        switch (lc->cmd) {
        case LC_MAIN:
            img->entryoff = ((const struct entry_point_command *)lc)->entryoff;
            img->have_entry = true;
            break;
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB: case LC_LOAD_UPWARD_DYLIB: {
            const struct dylib_command *dc = (const void *)lc;
            if (img->ndeps < 128) {
                img->dep_path[img->ndeps] = strdup((const char *)dc + dc->dylib.name.offset);
                img->dep_reexport[img->ndeps] = lc->cmd == LC_REEXPORT_DYLIB;
                img->ndeps++;
            }
            break;
        }
        case LC_RPATH: {
            const struct rpath_command *rc = (const void *)lc;
            if (img->nrpath < 16) img->rpath[img->nrpath++] = strdup((const char *)rc + rc->path.offset);
            break;
        }
        case LC_SYMTAB: { const struct symtab_command *st = (const void *)lc; img->symoff = st->symoff; img->nsyms = st->nsyms; img->stroff = st->stroff; img->strsize = st->strsize; break; }
        case LC_DYLD_CHAINED_FIXUPS: img->chained = (const void *)lc; break;
        case LC_DYLD_EXPORTS_TRIE: {
            const struct linkedit_data_command *ld = (const void *)lc;
            img->export_trie = img->slice + ld->dataoff; img->export_size = ld->datasize;
            break;
        }
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY:
            img->dyldinfo = (const void *)lc;
            if (!img->export_trie && img->dyldinfo->export_size) {
                img->export_trie = img->slice + img->dyldinfo->export_off; img->export_size = img->dyldinfo->export_size;
            }
            break;
        }
    }
}

// Maps the segments (copy-on-write from the file) and records the runtime layout.
static void map_segments(GImage *img, int fd, uint64_t slice_off) {
    const struct mach_header_64 *mh = img->mh;
    uint64_t lo = ~0ull, hi = 0;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)lc;
            if (!strcmp(sg->segname, "__PAGEZERO")) continue;
            if (sg->vmaddr < lo) lo = sg->vmaddr;
            if (sg->vmaddr + sg->vmsize > hi) hi = sg->vmaddr + sg->vmsize;
        }
    uint64_t span = (hi - lo + 0x3fff) & ~0x3fffull;
    if (img->is_main) {
        img->slide = 0;
        if (lo < g_guest_lo || lo + span > g_guest_lo + g_guest_size) {
            fprintf(stderr, "rebar: the executable's preferred base 0x%llx is outside the guest range\n", (unsigned long long)lo);
            exit(1);
        }
        if (lo + span > g_next_free) g_next_free = lo + span;
    } else {
        if (g_next_free + span > g_guest_lo + g_guest_size) { fprintf(stderr, "rebar: out of guest address space loading %s\n", img->path); exit(1); }
        img->slide = g_next_free - lo;
        g_next_free += span + 0x4000;
    }
    img->base = lo + img->slide;
    img->end = img->base + span;
    img->nseg = 0;
    lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        uint64_t addr = sg->vmaddr + img->slide;
        if (img->nseg < 64) img->segaddr[img->nseg++] = addr;
        if (!strcmp(sg->segname, "__PAGEZERO")) continue;
        uint64_t fsz = sg->filesize, vsz = (sg->vmsize + 0xfff) & ~0xfffull, fpg = (fsz + 0xfff) & ~0xfffull;
        if (fsz) {
            void *p = mmap((void *)addr, fpg, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, fd, (off_t)(slice_off + sg->fileoff));
            if (p == MAP_FAILED) { fprintf(stderr, "rebar: cannot map %s of %s: %s\n", sg->segname, img->path, strerror(errno)); exit(1); }
            if (fsz & 0xfff) memset((void *)(addr + fsz), 0, 0x1000 - (fsz & 0xfff));  // bss tail of the last file page
        }
        if (vsz > fpg) {
            void *p = mmap((void *)(addr + fpg), vsz - fpg, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
            if (p == MAP_FAILED) { fprintf(stderr, "rebar: cannot map bss of %s: %s\n", img->path, strerror(errno)); exit(1); }
        }
        if (!strcmp(sg->segname, "__TEXT")) { /* header lives here */ }
    }
}

// ------------------------------------------------------------------------ loading

static GImage *load_image(const char *real_path, GImage *loader, bool is_main) {
    GImage *have = find_image(real_path);
    if (have) return have;
    int fd = open(real_path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "rebar: cannot open %s: %s\n", real_path, strerror(errno)); exit(1); }
    size_t len = (size_t)lseek(fd, 0, SEEK_END);
    uint8_t *file = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) { perror("mmap"); exit(1); }
    const uint8_t *slice = arm64_slice_or_null(file, len);
    if (!slice) { fprintf(stderr, "rebar: %s has no arm64 slice\n", real_path); exit(1); }
    if (g_nimages >= MAX_IMAGES) { fprintf(stderr, "rebar: too many images\n"); exit(1); }

    GImage *img = calloc(1, sizeof *img);
    img->path = strdup(real_path);
    img->file = file; img->filelen = len; img->slice = slice;
    img->mh = (const struct mach_header_64 *)slice;
    img->is_main = is_main;
    img->index = g_nimages;
    g_images[g_nimages++] = img;
    if (is_main) g_main_image = img;
    if (is_main && img->mh->filetype != MH_EXECUTE) { fprintf(stderr, "rebar: not an executable\n"); exit(1); }
    parse_load_commands(img);
    map_segments(img, fd, (uint64_t)(slice - file));
    close(fd);
    if ((strstr(img->path, "/libjvm.dylib") || strstr(img->path, "/libcoreclr.dylib")) && g_jit_nskip < 16 && !getenv("REBAR_JIT_JAVA")) {   // HotSpot's generated code relies on faults (null checks, safepoint polls) that need the exact register state of the interpreter
        if (getenv("REBAR_JIT_JAVA_SKIP")) { g_jit_skip[g_jit_nskip].lo = 0x10000; g_jit_skip[g_jit_nskip].hi = 0x100000000ull; g_jit_nskip++; }   // (not translated at all)
        else { g_jit_precise_lo = 0x10000; g_jit_precise_hi = 0x100000000ull; }   // translated in the precise form
    }
    if (getenv("REBAR_JIT_PRECISE")) { g_jit_precise_lo = 0x10000; g_jit_precise_hi = ~0ull; }   // REBAR_JIT_PRECISE=1: every block stores the pc and registers before a load / store (faults, and the signal handlers that see them, are exact everywhere; slower)
    { static int show = -1; if (show < 0) show = getenv("REBAR_IMAGES") != NULL; if (show && !is_main) fprintf(stderr, "rebar: loaded %s base=0x%llx slide=0x%llx\n", img->path, (unsigned long long)img->base, (unsigned long long)img->slide); }   // (the images of the main program are listed at start)

    // thread-local template (runtime addresses)
    const struct load_command *lc = (const void *)(img->mh + 1);
    for (uint32_t k = 0; k < img->mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        const struct section_64 *sec = (const void *)(sg + 1);
        for (uint32_t j = 0; j < sg->nsects; j++, sec++) {
            uint32_t type = sec->flags & SECTION_TYPE;
            if (type != S_THREAD_LOCAL_REGULAR && type != S_THREAD_LOCAL_ZEROFILL) continue;
            uint64_t a = sec->addr + img->slide;
            if (!img->tls_start || a < img->tls_start) img->tls_start = a;
            if (a + sec->size > img->tls_start + img->tls_size) img->tls_size = a + sec->size - img->tls_start;
            if (type == S_THREAD_LOCAL_REGULAR && a + sec->size - img->tls_start > img->tls_data_size)
                img->tls_data_size = a + sec->size - img->tls_start;
        }
    }

    // dependencies
    for (int k = 0; k < img->ndeps; k++) {
        char *gp = resolve_guest_dep(img, img->dep_path[k]);
        if (gp) {
            const uint8_t *dfile; size_t dlen; int dfd = open(gp, O_RDONLY);
            bool arm = false;
            if (dfd >= 0) {
                dlen = (size_t)lseek(dfd, 0, SEEK_END);
                dfile = mmap(NULL, dlen, PROT_READ, MAP_PRIVATE, dfd, 0);
                if (dfile != MAP_FAILED) { arm = arm64_slice_or_null(dfile, dlen) != NULL; munmap((void *)dfile, dlen); }
                close(dfd);
            }
            if (arm) { img->dep_img[k] = load_image(gp, img, false); free(gp); continue; }
            free(gp);
        }
        img->dep_host[k] = true;
    }
    return img;
}

// ------------------------------------------------------------------------ symbols

static uint64_t uleb(const uint8_t **p);

// Looks a symbol up in one image's export trie. Returns the runtime address or 0.
static uint64_t image_export_lookup(const GImage *img, const char *name) {
    if (!img->export_trie) {   // no export trie (a dylib made only of data and code, like a Dart AOT snapshot): the external symbols of the symbol table
        if (!img->nsyms || !img->symoff) return 0;
        const struct nlist_64 *nl = (const struct nlist_64 *)(img->slice + img->symoff);
        uint64_t text_vm = 0; const struct load_command *lc = (const void *)(img->mh + 1);
        for (uint32_t k = 0; k < img->mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize))
            if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) { text_vm = ((const struct segment_command_64 *)lc)->vmaddr; break; }
        for (uint32_t k = 0; k < img->nsyms; k++)
            if ((nl[k].n_type & N_EXT) && (nl[k].n_type & N_TYPE) == N_SECT && nl[k].n_un.n_strx < img->strsize && !strcmp((const char *)(img->slice + img->stroff + nl[k].n_un.n_strx), name))
                return img->base + (nl[k].n_value - text_vm);
        return 0;
    }
    const uint8_t *node = img->export_trie, *end = img->export_trie + img->export_size;
    const char *rest = name;
    for (;;) {
        const uint8_t *p = node;
        uint64_t terminal = uleb(&p);
        if (!*rest && terminal) {
            uint64_t flags = uleb(&p);
            if (flags & 0x8) return 0;  // re-export: resolved by name through the dependency search
            return img->base + uleb(&p);
        }
        const uint8_t *q = node;
        uleb(&q);
        q += terminal;
        uint8_t nchildren = *q++;
        bool matched = false;
        for (int k = 0; k < nchildren && q < end; k++) {
            const char *edge = (const char *)q;
            size_t el = strlen(edge);
            q += el + 1;
            uint64_t child = uleb(&q);
            if (!strncmp(rest, edge, el)) { rest += el; node = img->export_trie + child; matched = true; break; }
        }
        if (!matched) return 0;
    }
}

static uint64_t find_guest_symbol(const char *name) {
    for (int k = 0; k < g_nimages; k++) {
        uint64_t a = image_export_lookup(g_images[k], name);
        if (a) return a;
    }
    return 0;
}

// ------------------------------------------------------------------------ fixups / init

static void rebase_opcodes(GImage *img) {  // classic images: slide every rebased pointer
    if (!img->dyldinfo || !img->dyldinfo->rebase_size || !img->slide) return;
    const uint8_t *p = img->slice + img->dyldinfo->rebase_off, *end = p + img->dyldinfo->rebase_size;
    uint64_t addr = 0;
    while (p < end) {
        uint8_t op = *p++, imm = op & 0x0f;
        switch (op & 0xf0) {
        case 0x00: p = end; break;
        case 0x10: break;
        case 0x20: addr = img->segaddr[imm] + uleb(&p); break;
        case 0x30: addr += uleb(&p); break;
        case 0x40: addr += (uint64_t)imm * 8; break;
        case 0x50: for (int t = 0; t < imm; t++) { *(uint64_t *)addr += img->slide; addr += 8; } break;
        case 0x60: { uint64_t n = uleb(&p); for (uint64_t t = 0; t < n; t++) { *(uint64_t *)addr += img->slide; addr += 8; } break; }
        case 0x70: *(uint64_t *)addr += img->slide; addr += 8 + uleb(&p); break;
        case 0x80: { uint64_t n = uleb(&p), skip = uleb(&p); for (uint64_t t = 0; t < n; t++) { *(uint64_t *)addr += img->slide; addr += 8 + skip; } break; }
        default: fprintf(stderr, "rebar: bad rebase opcode 0x%02x in %s\n", op, img->path); exit(1);
        }
    }
}

static void apply_chained_fixups(GImage *img);
static void apply_classic_binds(GImage *img);

static void fixup_image(GImage *img) {
    if (img->fixed) return;
    img->fixed = true;
    if (img->chained) apply_chained_fixups(img);
    else if (img->dyldinfo) { rebase_opcodes(img); apply_classic_binds(img); }
}

static void collect_init_order(GImage *img, GImage **order, int *n, bool *seen) {
    if (seen[img->index]) return;
    seen[img->index] = true;
    for (int k = 0; k < img->ndeps; k++) if (img->dep_img[k]) collect_init_order(img->dep_img[k], order, n, seen);
    order[(*n)++] = img;
}

// Runs one image's static initializers (dyld order: dependencies first).
static void run_image_initializers(CPU *c, GImage *img, char **apple) {
    if (img->inited) return;
    img->inited = true;
    const struct load_command *lc = (const void *)(img->mh + 1);
    for (uint32_t k = 0; k < img->mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const void *)lc;
        const struct section_64 *sec = (const void *)(sg + 1);
        for (uint32_t j = 0; j < sg->nsects; j++, sec++) {
            uint32_t type = sec->flags & SECTION_TYPE;
            uint64_t a = sec->addr + img->slide;
            if (type == S_MOD_INIT_FUNC_POINTERS) {
                for (uint64_t o = 0; o < sec->size; o += 8)
                    call_guest(c, *(uint64_t *)(a + o), (uint64_t)g_argc, (uint64_t)g_argv, (uint64_t)g_environ, (uint64_t)apple);
            } else if (type == 0x16 /* S_INIT_FUNC_OFFSETS */) {
                for (uint64_t o = 0; o < sec->size; o += 4)
                    call_guest(c, img->base + *(uint32_t *)(a + o), (uint64_t)g_argc, (uint64_t)g_argv, (uint64_t)g_environ, (uint64_t)apple);
            }
        }
    }
}

// Brings images [first, g_nimages) to the running state: fixups, unwind info, ObjC, +load, initializers.
static char **g_apple;
static void install_host_bundle_hook(void);
static void start_images(CPU *c, int first) {
    for (int k = first; k < g_nimages; k++) fixup_image(g_images[k]);
    for (int k = first; k < g_nimages; k++) { unwind_setup_image(g_images[k]); objc_prepare_image(g_images[k]); }
    for (int k = first; k < g_nimages; k++) objc_map_image(g_images[k]);
    install_host_bundle_hook();
    for (int k = first; k < g_nimages; k++) objc_attach_categories(g_images[k]);
    for (int k = first; k < g_nimages; k++) swift_register_image(g_images[k]);
    jitcache_kick(g_nimages);   // the code of these images is final: the translator may start on the blocks it remembers
    GImage *order[MAX_IMAGES]; bool seen[MAX_IMAGES] = {0};
    int n = 0;
    for (int k = 0; k < first; k++) seen[k] = true;
    for (int k = first; k < g_nimages; k++) collect_init_order(g_images[k], order, &n, seen);
    for (int k = 0; k < n; k++) {
        if (order[k]->is_main) continue;  // the executable's initializers run last, right before main
        objc_load_image(c, order[k]);
        run_image_initializers(c, order[k], g_apple);
    }
}

// ------------------------------------------------------------------------ dlopen & friends

static GImage *image_from_handle(uint64_t h) {
    for (int k = 0; k < g_nimages; k++) if ((uint64_t)g_images[k] == h) return g_images[k];
    return NULL;
}

static char g_dlerr[700];
static bool g_dlerr_pending;
H(h_dlerror) {
    if (g_dlerr_pending) { g_dlerr_pending = false; RET((uint64_t)g_dlerr); return; }
    host_call(c, dlsym(RTLD_DEFAULT, "dlerror"), false);
}
H(h_dlopen) {
    const char *path = S(0);
    if (!path) { c->x[0] = (uint64_t)g_main_image; return; }
    char *gp = NULL;
    if (path[0] == '/' || path[0] == '@' || strchr(path, '/')) gp = resolve_guest_dep(g_main_image, path);
    else {  // bare name: look next to the executable's Frameworks / rpaths
        char cand[PATH_MAX];
        for (int r = 0; r < g_main_image->nrpath && !gp; r++) {
            char *rp = expand_origin(g_main_image->rpath[r], g_main_image);
            snprintf(cand, sizeof cand, "%s/%s", rp, path); free(rp);
            char real[PATH_MAX];
            if (realpath(cand, real)) gp = strdup(real);
        }
    }
    if (!gp && path[0] != '/' && path[0] != '@' && strchr(path, '/') && g_main_image && g_main_image->path) {   // a relative path (GTK's pixbuf loaders cache lists "lib/gdk-pixbuf-2.0/..."): the app's launcher
        char dir[PATH_MAX], cand[PATH_MAX], real[PATH_MAX];                                                    // would have been started with the bundle's Resources as the working directory
        snprintf(dir, sizeof dir, "%s", g_main_image->path);
        for (int up = 0; up < 8 && !gp; up++) {
            char *slash = strrchr(dir, '/'); if (!slash || slash == dir) break; *slash = 0;
            snprintf(cand, sizeof cand, "%s/%s", dir, path);
            if (realpath(cand, real)) gp = strdup(real);
            else { snprintf(cand, sizeof cand, "%s/Resources/%s", dir, path); if (realpath(cand, real)) gp = strdup(real); }   // (<bundle>/Contents/Resources is the usual working directory)
        }
    }
    if (!gp && !strncmp(path, "@rpath/", 7)) {   // dlopen("@rpath/x") searches the rpaths of the images that are loaded (the caller's, and the executable's): the caller is not known here, so all of them
        char cand[PATH_MAX], real[PATH_MAX];
        for (int k = 0; k < g_nimages && !gp; k++)
            for (int r = 0; r < g_images[k]->nrpath && !gp; r++) {
                char *rp = expand_origin(g_images[k]->rpath[r], g_images[k]);
                snprintf(cand, sizeof cand, "%s/%s", rp, path + 7); free(rp);
                if (realpath(cand, real)) gp = strdup(real);
            }
    }
    if (gp) {
        const uint8_t *f; size_t l; int fd = open(gp, O_RDONLY); bool arm = false;
        if (fd >= 0) {
            l = (size_t)lseek(fd, 0, SEEK_END); f = mmap(NULL, l, PROT_READ, MAP_PRIVATE, fd, 0);
            if (f != MAP_FAILED) { arm = arm64_slice_or_null(f, l) != NULL; munmap((void *)f, l); }
            close(fd);
        }
        if (arm) {
            int first = g_nimages;
            GImage *img = load_image(gp, NULL, false);
            for (int k = first; k < g_nimages; k++) {
                const char *bad = image_stl_import(g_images[k]);
                if (!bad) continue;
                snprintf(g_dlerr, sizeof g_dlerr, "rebar: %s calls the host's libc++ with an object (%s) whose arm64 layout rebar does not convert (wide strings, filesystem directory/path iterators, moved stringstreams)", gp, bad);
                g_dlerr_pending = true;
                fprintf(stderr, "%s\n", g_dlerr);
                free(gp);
                c->x[0] = 0;
                return;
            }
            free(gp);
            start_images(c, first);
            c->x[0] = (uint64_t)img;
            return;
        }
        free(gp);
    }
    void *h = dlopen(path, (int)c->x[1]);  // a system library
    if (!h) { const char *e = dlerror(); snprintf(g_dlerr, sizeof g_dlerr, "%s", e ? e : "image not found"); g_dlerr_pending = true; }   // (kept for the guest's dlerror: the message is delivered once, and a second failed dlopen of the same name used to leave the host with none: JNA then took strlen of NULL)
    c->x[0] = (uint64_t)h;
}

// +[NSBundle bundleForClass:] answered by the host's own code (-[NSWindowController windowNibPath], NSBundle's localization, an NSImage lookup by name) knows nothing of the guest's images: a class of
// Xcode's IDEKit gave the main bundle, and "IDEWorkspaceWindow" was searched there ("failed to load window nib file"). The method is replaced by one that answers for a class inside a guest image.
static id (*g_orig_bundleForClass)(id, SEL, Class);
static id rebar_bundleForClass(id self, SEL cmd, Class cls) {
    if (cls && in_guest((uint64_t)cls)) {
        GImage *im = image_by_addr((uint64_t)cls); char root[PATH_MAX];
        if (im && im->path && im != g_main_image && guest_bundle_root(im->path, root, sizeof root)) {
            id str = send_h((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), root);
            id b = str ? send_h(self, sel_registerName("bundleWithPath:"), str) : NULL;
            if (b) return b;
        }
    }
    return g_orig_bundleForClass(self, cmd, cls);
}
static void install_host_bundle_hook(void) {
    static bool done; if (done || getenv("REBAR_NO_BUNDLE_HOOK")) return; done = true;
    Class meta = object_getClass((id)objc_getClass("NSBundle")); if (!meta) return;
    Method m = class_getInstanceMethod(meta, sel_registerName("bundleForClass:")); if (!m) return;
    g_orig_bundleForClass = (id (*)(id, SEL, Class))method_getImplementation(m);
    method_setImplementation(m, (IMP)rebar_bundleForClass);
}

// A plug-in bundle (Xcode's .dvtplugin / .ideplugin, a framework a program loads with NSBundle) holds arm64 code that the host's NSBundle cannot dlopen. Its load, loadAndReturnError:,
// preflightAndReturnError: and isLoaded are answered here: the executable goes through the guest loader, as dlopen does. A bundle with an executable of the host's kind is left to the host.
static GImage *g_bundle_img[256]; static char *g_bundle_path[256]; static int g_nbundle;
static bool bundle_load_hook(CPU *c, id self, SEL sel) {
    static SEL s_load, s_lre, s_pre, s_il, s_ep; static Class cB;
    if (!s_load) { s_load = sel_registerName("load"); s_lre = sel_registerName("loadAndReturnError:"); s_pre = sel_registerName("preflightAndReturnError:"); s_il = sel_registerName("isLoaded"); s_ep = sel_registerName("executablePath"); cB = objc_getClass("NSBundle"); }
    static SEL s_bwi, s_bi, s_bwp, s_ies;
    if (!s_bwi) { s_bwi = sel_registerName("bundleWithIdentifier:"); s_bi = sel_registerName("bundleIdentifier"); s_bwp = sel_registerName("bundleWithPath:"); s_ies = sel_registerName("isEqualToString:"); }
    if (sel == s_bwi && cB && self == (id)cB && c->x[2] > 0x10000) {   // +bundleWithIdentifier: knows the bundles of the images the host loaded; the guest's frameworks are looked up here
        id r = send_h(self, sel, (id)c->x[2]);
        if (!r) {
            id want = (id)c->x[2]; char done[PATH_MAX * 0 + 1]; (void)done;
            for (int k = 0; k < g_nimages && !r; k++) {
                char root[PATH_MAX]; if (!g_images[k]->path || !guest_bundle_root(g_images[k]->path, root, sizeof root)) continue;
                id str = send_h((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), root);
                id b = str ? send_h(self, s_bwp, str) : NULL; if (!b) continue;
                id ident = send_h(b, s_bi);
                if (ident && ((uint64_t)send_h(ident, s_ies, want) & 0xff)) r = b;
            }
        }
        c->x[0] = (uint64_t)r; c->x[1] = 0;
        return true;
    }
    if (sel != s_load && sel != s_lre && sel != s_pre && sel != s_il) return false;
    if (!cB) return false;
    Class k = object_getClass(self); while (k && k != cB) k = class_getSuperclass(k);
    if (!k) return false;
    id ep = send_h(self, s_ep);
    const char *path = ep ? (const char *)send_h(ep, sel_registerName("UTF8String")) : NULL;
    if (!path || !*path) return false;
    char real[PATH_MAX]; if (!realpath(path, real)) return false;
    int idx = -1; for (int i = 0; i < g_nbundle; i++) if (!strcmp(g_bundle_path[i], real)) idx = i;
    if (idx < 0) {
        bool arm = false; int fd = open(real, O_RDONLY);
        if (fd >= 0) { size_t l = (size_t)lseek(fd, 0, SEEK_END); const uint8_t *f = mmap(NULL, l, PROT_READ, MAP_PRIVATE, fd, 0); if (f != MAP_FAILED) { arm = arm64_slice_or_null(f, l) != NULL; munmap((void *)f, l); } close(fd); }
        if (!arm) return false;
    }
    if (sel == s_il) { c->x[0] = idx >= 0 && g_bundle_img[idx] ? 1 : 0; c->x[1] = 0; return true; }
    if (sel == s_pre) { c->x[0] = 1; c->x[1] = 0; return true; }
    if (idx < 0 && g_nbundle < 256) {
        int first = g_nimages;
        GImage *img = load_image(real, NULL, false);
        if (getenv("REBAR_TRACE_BUNDLES")) fprintf(stderr, "[bundle] load %s -> %s\n", real, img ? "ok" : "FAILED");
        if (img) start_images(c, first);
        g_bundle_img[g_nbundle] = img; g_bundle_path[g_nbundle] = strdup(real); idx = g_nbundle++;
    }
    bool ok = idx >= 0 && g_bundle_img[idx];
    c->x[0] = ok ? 1 : 0; c->x[1] = 0;
    if (sel == s_lre && !ok && c->x[2] > 0x10000) *(uint64_t *)c->x[2] = 0;   // (no NSError is built here: the caller sees NO)
    return true;
}

static bool host_libsystem_symbol(const char *sym) {
    void *p = dlsym(RTLD_DEFAULT, sym); Dl_info di;
    return p && dladdr(p, &di) && di.dli_fname && (!strncmp(di.dli_fname, "/usr/lib/system/", 16) || !strcmp(di.dli_fname, "/usr/lib/libSystem.B.dylib") || !strcmp(di.dli_fname, "/usr/lib/libc++.1.dylib"));
}
H(h_dlsym) {
    uint64_t handle = c->x[0];
    char name[512];
    snprintf(name, sizeof name, "_%s", S(1));
    GImage *img = image_from_handle(handle);
    uint64_t a = img ? image_export_lookup(img, name) : 0;
    if (!a && img && !getenv("REBAR_NO_DLSYM_DEPS")) {   // dlsym(handle, name) also looks into what the library depends on (JBR's libawt_lwawt and the natives of AWTThreading in libosxapp): its guest dependencies, breadth first
        static __thread GImage *queue[512]; int qh = 0, qt = 0; queue[qt++] = img;
        for (; qh < qt && !a; qh++)
            for (int d = 0; d < queue[qh]->ndeps && !a; d++) {
                GImage *dep = queue[qh]->dep_img[d]; if (!dep) continue;
                bool seen = false; for (int q = 0; q < qt; q++) if (queue[q] == dep) { seen = true; break; }
                if (seen) continue;
                a = image_export_lookup(dep, name);
                if (!a && qt < 512) queue[qt++] = dep;
            }
    }
    bool deps = false;   // dlsym(handle of a guest library, name) also finds what the library's dependencies export (Julia asks libjulia-internal for strlen): the libraries here are the host's
    if (!a && img && !img->is_main && name[1] && !strchr(name + 1, '$') && host_libsystem_symbol(name + 1)) deps = true;   // (only what libSystem really has: every guest library depends on it)
    if (!a && (!img || img->is_main || deps)) {       // RTLD_DEFAULT / RTLD_NEXT / the executable: every guest image, then the host
        a = deps ? 0 : find_guest_symbol(name);   // (a library's dependencies that are guest images are not searched: lbt asks OpenBLAS for names it does not have)
        if (!a) a = host_data_addr(name);        // a host variable: its address, as the guest will read it
        if (!a) a = stub_for(name, NULL, true);  // a host function: through our stub so special handlers apply
    }
    if (!a && getenv("REBAR_TRACE_DLSYM")) fprintf(stderr, "[dlsym miss: handle %llx img %p path %s trie %p size %u main %d]\n", (unsigned long long)handle, (void *)img, img ? img->path : "-", img ? (void *)img->export_trie : NULL, img ? (unsigned)img->export_size : 0, img ? (int)img->is_main : 0);
    if (!a) { snprintf(g_dlerr, sizeof g_dlerr, "dlsym(%p, %s): symbol not found", (void *)handle, S(1)); g_dlerr_pending = true; }   // callers decide whether a symbol exists by asking dlerror (GTK 3 looking for GTK 2)
    if (g_trace_stubs && trace_stub_wanted("_dlsym")) { GImage *in = a ? image_by_addr(a) : NULL; fprintf(stderr, "   -> %llx%s%s\n", (unsigned long long)a, in ? " in " : (a >= g_stub_base && a - g_stub_base < g_stub_size ? " (stub " : ""), in ? in->path : (a >= g_stub_base && a - g_stub_base < g_stub_size ? g_stubs[(a - g_stub_base) / 16].name : "")); }
    c->x[0] = a;
}

H(h_dlclose) { c->x[0] = 0; }

H(h_dladdr) {
    uint64_t addr = c->x[0];
    Dl_info *info = P(1);
    GImage *img = NULL;
    for (int k = 0; k < g_nimages; k++) if (addr >= g_images[k]->base && addr < g_images[k]->end) img = g_images[k];
    if (img) {
        info->dli_fname = img->path; info->dli_fbase = (void *)img->base; info->dli_sname = NULL; info->dli_saddr = NULL;
        c->x[0] = 1;
    } else c->x[0] = (uint64_t)dladdr((void *)addr, info);
}

H(h_dyld_image_count) { c->x[0] = (uint64_t)g_nimages; }
H(h_dyld_get_image_name) { c->x[0] = c->x[0] < (uint64_t)g_nimages ? (uint64_t)g_images[c->x[0]]->path : 0; }
H(h_dyld_get_image_header) { c->x[0] = c->x[0] < (uint64_t)g_nimages ? g_images[c->x[0]]->base : 0; }
H(h_dyld_get_image_vmaddr_slide) { c->x[0] = c->x[0] < (uint64_t)g_nimages ? g_images[c->x[0]]->slide : 0; }
H(h_dyld_register_func_for_add_image) {  // notify about the images that already exist
    uint64_t fn = c->x[0];
    for (int k = 0; k < g_nimages; k++) call_guest(c, fn, g_images[k]->base, g_images[k]->slide, 0, 0);
}
