// Bridge between the interpreted arm64 guest and the host's x86_64 libSystem.
//
// Imports of the guest (chained fixups or classic bind opcodes) are resolved
// to "stubs": addresses in a reserved region that the interpreter intercepts
// instead of executing. A stub either forwards to the host function of the same
// name through a small SysV trampoline, or to a handler here that deals with an
// ABI difference (variadics, callbacks, setjmp, ...).
//
// Apple arm64 vs x86_64 SysV differences that matter for a forwarder:
//  - variadic arguments are passed on the stack on arm64 (so printf & co are
//    re-routed to their v* forms over a va_list that points at the guest stack);
//  - function pointers handed to the host would be called as x86 code (qsort,
//    bsearch, atexit are wrapped; signal handlers and threads are not yet).

#include <dlfcn.h>
#include <search.h>
#include <wchar.h>
#include <semaphore.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <syslog.h>
#include <execinfo.h>
#include <mach-o/fixup-chains.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <sys/ucontext.h>
#include "csigs.h"

// ---- guest images (the executable and the dylibs/frameworks it loads; see loader.h)
typedef struct GImage {
    char *path;
    const uint8_t *file; size_t filelen;
    const uint8_t *slice;                       // the arm64 Mach-O inside the file
    const struct mach_header_64 *mh;            // == slice
    uint64_t base, slide, end;                  // runtime header address, slide, end of the last segment
    int index;
    bool is_main, fixed, objc_mapped, inited, have_entry;
    uint64_t entryoff;
    const uint8_t *export_trie; uint32_t export_size;
    const uint8_t *unwind_info;
    uint64_t eh_frame, eh_size;                 // __TEXT,__eh_frame (runtime address): the FDEs that DWARF-mode compact unwind entries point to
    const struct linkedit_data_command *chained;
    const struct dyld_info_command *dyldinfo;
    uint64_t segaddr[64]; int nseg;             // runtime segment addresses, load-command order
    int ndeps; char *dep_path[128]; struct GImage *dep_img[128]; bool dep_host[128], dep_reexport[128];
    char *rpath[16]; int nrpath;
    uint64_t tls_start, tls_size, tls_data_size; // thread-local template (runtime addresses)
} GImage;
#define MAX_IMAGES 512
// This thread's thread-local-variable blocks, by image index + 1 (0 stays empty: a descriptor whose key is 0 has not been seen yet). The guest's TLV thunk reads it through TPIDRRO_EL0.
// The same table is what a program that reads its thread-specific values straight off the thread pointer sees (Go's runtime keeps its g there: [TPIDRRO_EL0 + key * 8]
// after pthread_setspecific(key, g)), so h_pthread_setspecific mirrors every value into the slot of its key; the host's keys are below 512.
#define TLV_BASE 600   // the image slots start above the host's pthread keys (0x100..0x1ff)
static __thread uint64_t t_tlv_tab[TLV_BASE + MAX_IMAGES + 1];
static GImage *g_images[MAX_IMAGES];
static int g_nimages;
static GImage *g_main_image;
static uint64_t g_next_free;                    // next free load address inside the guest reservation

static inline bool in_guest(uint64_t a) { return a - g_guest_lo < g_guest_size; }

static GImage *image_by_addr(uint64_t a) {
    for (int k = 0; k < g_nimages; k++) if (a >= g_images[k]->base && a < g_images[k]->end) return g_images[k];
    return NULL;
}

// Frame-pointer walk of the guest stack, printed with image-relative offsets so a crash can be located
// with otool/lldb in the original binary.
static bool own_check(uint64_t lo, uint64_t len);
// the last imports this thread called, shown by the crash reports (noisy ones are skipped when printing)
typedef struct { const char *name; uint64_t x0, x1, x2, lr, ret; } RingEnt;
static __thread RingEnt t_ring[128];
static __thread unsigned t_ring_n;
static void dump_recent_imports(void) {
    static const char *noise[] = {"__tlv_bootstrap", "_pthread_getspecific", "_os_unfair_lock_trylock", "_os_unfair_lock_unlock", "_bzero", "_memcpy",
                                  "_memmove", "___error", "_malloc", "_calloc", "_free", "_malloc_size", "_realloc", "_malloc_zone_malloc", "_malloc_zone_free",
                                  "_pthread_mutex_lock", "_pthread_mutex_unlock", "_pthread_setspecific", "_memset_pattern16", "_strlen", "_memcmp", NULL};
    int shown = 0;
    for (unsigned k = 1; k <= 128 && shown < 16; k++) {
        const RingEnt *e = &t_ring[(t_ring_n - k) & 127];
        if (t_ring_n < k || !e->name) break;
        bool skip = false;
        for (const char **nz = noise; *nz; nz++) if (!strcmp(e->name, *nz)) skip = true;
        if (skip) continue;
        if (!shown++) fprintf(stderr, "  recent imports on this thread, newest first:\n");
        fprintf(stderr, "    %s(0x%llx, 0x%llx, 0x%llx) = 0x%llx from 0x%llx\n", e->name, (unsigned long long)e->x0, (unsigned long long)e->x1, (unsigned long long)e->x2, (unsigned long long)e->ret, (unsigned long long)e->lr);
    }
}
static void dump_backtrace(CPU *c) {
    uint64_t pcs[24], n = 0, fp = c->x[29];
    pcs[n++] = c->pc;
    pcs[n++] = c->x[30];
    while (n < 24 && fp && !(fp & 7) && own_check(fp, 16)) {
        uint64_t lr = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];
        if (!lr) break;
        pcs[n++] = lr;
        if (next <= fp) break;
        fp = next;
    }
    for (uint64_t k = 0; k < n; k++) {
        GImage *im = image_by_addr(pcs[k]);
        const char *name = im ? strrchr(im->path, '/') : NULL;
        fprintf(stderr, "  #%llu 0x%llx  %s+0x%llx\n", (unsigned long long)k, (unsigned long long)pcs[k],
                im ? (name ? name + 1 : im->path) : "?", im ? (unsigned long long)(pcs[k] - im->base) : 0ull);
    }
    dump_recent_imports();
}

// ---- exception-handling state (see unwind.h)
typedef struct { uint64_t pc, sp, fp, lr; uint64_t x[10]; uint64_t d[8]; } UFrame;  // callee-saved view of a guest frame
typedef struct HostCallRec { struct HostCallRec *prev; CPU *cpu; UFrame f; } HostCallRec;  // one per guest->host call
typedef struct Act { struct Act *prev; CPU *cpu, *saved; HostCallRec *hc_at_entry; bool own_gil; } Act;   // one per host->guest entry
static __thread HostCallRec *t_hc;
static __thread Act *t_act;
static __thread bool t_landed;                  // an exception was delivered to a guest handler during this host call
static __thread uint64_t t_pend_x0, t_pend_x1;  // exception pointer / selector for the handler, applied by stub_call

static void uframe_from_cpu(const CPU *c, UFrame *f) {
    f->pc = c->x[30]; f->lr = c->x[30]; f->sp = c->sp; f->fp = c->x[29];
    for (int k = 0; k < 10; k++) f->x[k] = c->x[19 + k];
    for (int k = 0; k < 8; k++) f->d[k] = c->vr[8 + k].d[0];
}

typedef void (*StubHandler)(CPU *);
typedef struct { uint8_t mask, wb, uninit, wide; bool dtor, sret, sb, sretw; } StrSpec;  // which argument registers hold std::string objects (see string_call)
typedef struct { char *name, *lib; void *host; StubHandler handler; bool sret; const char *enc; char narrow; const char *stk; StrSpec *str; bool maps, thisret, swift; } Stub;  // maps: (task, *address, size, ...) creates memory the guest owns
static __thread Stub *t_last_stub;   // the import this thread called last, for the crash report
static __thread uint64_t t_last_lr;  // and where it was called from

#define MAX_STUBS 8192
#define STUB_RET 0   // pc here means "return to the host" (see call_guest)
#define STUB_EXIT 1  // main() returned here
static Stub g_stubs[MAX_STUBS];
static int g_nstubs;

static inline uint64_t stub_addr(int idx) { return g_stub_base + 16 * (uint64_t)idx; }

// The imports that translated code calls itself (jit.h: emit_leaf_call): plain host functions of up to six integer arguments that never throw,
// never block (the thread may hold the interpreter lock, which the generic path drops around a host call) and never call back into the guest.
static bool g_zone_cloned;       // the guest asked for the default zone (to patch it, as Chromium's allocator shim does): its malloc and free must go through the handlers from now on
static bool g_leaf_malloc_used;  // translated code calls the host's malloc and free directly
static bool jit_leaf_import(uint64_t target, void **fn, int *nargs, int *zext8) {
    uint64_t off = target - g_stub_base;
    if (off >= g_stub_size || (off & 15) || off / 16 < 2 || off / 16 >= (uint64_t)g_nstubs) return false;
    const Stub *s = &g_stubs[off / 16];
    // malloc, calloc, realloc, free: while the guest has not got hold of the default zone nothing can have patched it, so the call goes straight to the host (the handler path costs
    // ~140 ns a call and takes the interpreter lock); the moment it asks for the zone every translation is thrown away (h_malloc_default_zone) and these calls take the handlers
    if (s->name && !g_zone_cloned && !getenv("REBAR_NO_LEAF_MALLOC") && (!strcmp(s->name, "_malloc") || !strcmp(s->name, "_free") || !strcmp(s->name, "_calloc") || !strcmp(s->name, "_realloc"))) {
        *fn = dlsym(RTLD_DEFAULT, s->name + 1); *zext8 = 0;
        *nargs = !strcmp(s->name, "_malloc") || !strcmp(s->name, "_free") ? 1 : 2;
        if (!*fn) return false;
        g_leaf_malloc_used = true;
        return true;
    }
    if (!s->host || !s->name || s->handler || s->enc || s->str || s->sret || s->maps || s->thisret || s->swift || s->stk) return false;
    static const struct { const char *name; int nargs; bool zext8; } ok[] = {
        {"_memcpy", 3, 0}, {"_memmove", 3, 0}, {"_memset", 3, 0}, {"_bzero", 2, 0}, {"_memcmp", 3, 0}, {"_bcmp", 3, 0}, {"_memchr", 3, 0}, {"_memset_pattern16", 3, 0},
        {"_strlen", 1, 0}, {"_strcmp", 2, 0}, {"_strncmp", 3, 0}, {"_strchr", 2, 0}, {"_strrchr", 2, 0}, {"_strcpy", 2, 0}, {"_strncpy", 3, 0},
        {"_pthread_getspecific", 1, 0}, {"_mach_absolute_time", 0, 0}, {"_os_unfair_lock_trylock", 1, 1}, {"_os_unfair_lock_unlock", 1, 0},
        {"_pthread_mutex_unlock", 1, 0}, {"_pthread_mutex_trylock", 1, 0},
        {"_pthread_self", 0, 0}, {"_pthread_threadid_np", 2, 0}, {"_clock_gettime", 2, 0}, {"_strnlen", 2, 0}, {"_malloc_size", 1, 0},
    };
    for (size_t k = 0; k < sizeof ok / sizeof ok[0]; k++)
        if (!strcmp(s->name, ok[k].name)) {
            if (s->narrow && !ok[k].zext8) return false;
            *fn = s->host; *nargs = ok[k].nargs; *zext8 = ok[k].zext8;
            return true;
        }
    return false;
}

static int g_argc;
static char **g_argv, **g_environ, *g_progname, g_exe_path[1024];
extern char **environ;

// ---------------------------------------------------------- host call trampoline

// rebar_host_call(fn, ints[6], dbls[8], out[4], stack, nstack)
//   Calls an x86_64 function with six integer args, eight double args and `nstack`
//   (even) extra 8-byte stack words; returns rax, xmm0, rdx, xmm1, r12, rcx, r8 in out[0..6] (out[0], out[1] are also inputs: swiftself, rax).
extern void rebar_host_call(void *fn, const uint64_t *ints, const uint64_t *dbls, uint64_t *out,
                              const uint64_t *stack, uint64_t nstack);
extern void *rebar_host_landing_addr(void);
// The CFI below names rebar_personality (unwind.h): C++/ObjC exceptions that propagate out of
// host code into this frame are searched for in the guest frames that made the call. When the
// unwinder lands at Lhc_landing (rax = exception, rdx = 1 for the handler frame), rebar_arrive
// runs the guest cleanups; it returns 1 if a guest handler was installed (we then return
// normally, and the interpreter continues at the handler), 0 to keep unwinding.
__asm__(
    ".text\n"
    ".globl _rebar_host_call\n"
    "_rebar_host_call:\n"
    ".cfi_startproc\n"
    ".cfi_personality 155, _rebar_personality\n"
    "  pushq %rbp\n  .cfi_def_cfa_offset 16\n  .cfi_offset %rbp, -16\n"
    "  movq %rsp, %rbp\n  .cfi_def_cfa_register %rbp\n"
    "  pushq %rbx\n  .cfi_offset %rbx, -24\n"
    "  pushq %r12\n  .cfi_offset %r12, -32\n"
    "  pushq %r13\n  .cfi_offset %r13, -40\n"
    "  subq $8, %rsp\n"
    "  movq %rcx, %rbx\n  movq %rdi, %r11\n  movq %rsi, %r10\n  movq %rdx, %r12\n"
    "  movq %r9, %rax\n"
    "1:\n  testq %rax, %rax\n  jz 2f\n  decq %rax\n  pushq (%r8,%rax,8)\n  jmp 1b\n"
    "2:\n"
    "  movq 0(%r10), %rdi\n  movq 8(%r10), %rsi\n  movq 16(%r10), %rdx\n"
    "  movq 24(%r10), %rcx\n  movq 32(%r10), %r8\n  movq 40(%r10), %r9\n"
    "  movsd 0(%r12), %xmm0\n  movsd 8(%r12), %xmm1\n  movsd 16(%r12), %xmm2\n  movsd 24(%r12), %xmm3\n"
    "  movsd 32(%r12), %xmm4\n  movsd 40(%r12), %xmm5\n  movsd 48(%r12), %xmm6\n  movsd 56(%r12), %xmm7\n"
    "  movq 0(%rbx), %r13\n"        // out[0]: swiftself (x20 of a Swift caller; callee-saved, so a C function does not mind)
    "  movq 8(%rbx), %rax\n"        // out[1]: %al = the vector registers of a variadic C function (8), or the address of the indirect result of a Swift function
    "  xorl %r12d, %r12d\n"         // swifterror starts out clear
    "  call *%r11\n"
    "  movq %rax, 0(%rbx)\n  movq %xmm0, 8(%rbx)\n  movq %rdx, 16(%rbx)\n  movq %xmm1, 24(%rbx)\n"
    "  movq %r12, 32(%rbx)\n  movq %rcx, 40(%rbx)\n  movq %r8, 48(%rbx)\n"   // the error, and the 3rd and 4th result registers of Swift
    "Lhc_epilogue:\n"
    "  leaq -24(%rbp), %rsp\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n"
    "Lhc_landing:\n"
    "  movq %rax, %r13\n  movq %rax, %rdi\n  movq %rdx, %rsi\n  call _rebar_arrive\n"
    "  testl %eax, %eax\n  jnz Lhc_epilogue\n"
    "  call _rebar_gil_drop\n"
    "  movq %r13, %rdi\n  call __Unwind_Resume\n"
    ".cfi_endproc\n"
    ".globl _rebar_host_landing_addr\n"
    "_rebar_host_landing_addr:\n  leaq Lhc_landing(%rip), %rax\n  ret\n");

// Calls a host function with a call record so exceptions can find the calling guest frames.
static void host_invoke(CPU *c, void *fn, const uint64_t *ints, const uint64_t *dbls,
                        const uint64_t *stack, uint64_t nstack, uint64_t *out) {
    HostCallRec rec;
    rec.prev = t_hc; rec.cpu = c;
    uframe_from_cpu(c, &rec.f);
    t_hc = &rec;
    bool had_gil = gil_release();            // the host call may block; other guest threads run meanwhile
    rebar_host_call(fn, ints, dbls, out, stack, nstack);
    if (had_gil) gil_acquire();
    t_hc = rec.prev;
}

// Swift's own calling convention (imports named _$s...): like the C one for the arguments, but self travels in x20 (r13 on x86_64), a thrown error comes back in
// x21 (r12), the address of an indirect result is in x8 (rax) and up to four result registers are used (x0-x3, rax rdx rcx r8).
static __thread bool t_swift;
static void host_call_full(CPU *c, void *fn, const uint64_t *ints, const uint64_t *dbls,
                           const uint64_t *stack, uint64_t nstack) {
    uint64_t out[8] = {0, 8};
    bool swift = t_swift; t_swift = false;
    if (swift) { out[0] = c->x[20]; out[1] = c->x[8]; }
    host_invoke(c, fn, ints, dbls, stack, nstack, out);
    if (t_landed) return;  // an exception was delivered to a guest handler; stub_call finishes the hand-off
    c->x[0] = out[0];  // rax:rdx and xmm0:xmm1 carry two-register results (NSRange, NSPoint, ...)
    c->x[1] = out[2];
    memset(&c->vr[0], 0, 32);
    c->vr[0].d[0] = out[1];
    c->vr[1].d[0] = out[3];
    if (swift) { c->x[2] = out[5]; c->x[3] = out[6]; if (out[4]) c->x[21] = out[4]; }
}

// sret: the callee returns a class by value. arm64 passes the result address in x8,
// SysV in the first integer register, shifting every other argument by one.
// The integer arguments are x0..x7 followed by the words on the guest's stack (arm64 passes the
// 9th and later arguments there); the first six go to registers, the rest to the SysV stack.
static __thread const char *t_stk;  // sizes of the stack-passed arguments of the call being made (see k_stackargs)
static void host_call(CPU *c, void *fn, bool sret) {
    uint64_t dbls[8], seq[24], stack[12];
    int n = 0;
    for (int k = 0; k < 8; k++) dbls[k] = c->vr[k].d[0];
    if (sret) seq[n++] = c->x[8];
    for (int k = 0; k < 8; k++) seq[n++] = c->x[k];
    const uint64_t *gs = (const uint64_t *)c->sp;
    for (int k = 0; k < 10; k++) seq[n + k] = gs[k];
    if (t_stk) {  // arm64 packs stack arguments by natural size, x86_64 gives each an 8-byte slot
        uint64_t off = 0;
        int k = 0;
        for (const char *z = t_stk; *z && k < 10; z++, k++) {
            unsigned sz = (unsigned)(*z - '0');
            off = (off + sz - 1) & ~(uint64_t)(sz - 1);
            uint64_t v = 0;
            memcpy(&v, (const char *)gs + off, sz);
            seq[n + k] = v;
            off += sz;
        }
        t_stk = NULL;
    }
    n += 10;
    for (int k = 0; k < 12; k++) stack[k] = seq[6 + k];
    host_call_full(c, fn, seq, dbls, stack, 12);
}

// ---------------------------------------------------- running guest code from C

// One CPU per host thread: the main thread owns the loader's CPU, any other thread
// that calls into guest code (GCD workers, NSThread, ...) gets one on first use.
// Stacks (guest stacks, signal stacks) are mapped with unmapped-by-default guard zones on both sides.
// Guest code such as Chromium's probes the memory around its stack with vm_region/vm_protect and
// changes the protection of the neighbouring pages; whatever lies there must not be something the
// emulator needs (a read-only alternate signal stack makes the kernel kill the process with SIGILL).
static void own_add(uint64_t lo, uint64_t len);
static void *alloc_guarded(size_t size) {
    const size_t guard = 1 << 20;
    uint8_t *p = mmap(NULL, size + 2 * guard, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("rebar: mmap stack"); exit(1); }
    if (mprotect(p + guard, size, PROT_READ | PROT_WRITE) != 0) { perror("rebar: mprotect stack"); exit(1); }
    return p + guard;
}

// The guest runs on stacks of its own, not on the host thread's. V8 and Chromium compare their stack pointer
// with pthread_get_stackaddr_np/pthread_get_stacksize_np, so those answer for the guest stack of the thread.
typedef struct { pthread_t th; uint64_t hi, size; } GStack;
static GStack g_gstacks[256];
static unsigned g_gstack_next;
static pthread_mutex_t g_gstack_mu = PTHREAD_MUTEX_INITIALIZER;
static void gstack_register(uint64_t lo, uint64_t size) {
    pthread_t me = pthread_self();
    pthread_mutex_lock(&g_gstack_mu);
    unsigned slot = g_gstack_next++ % 256;
    for (unsigned k = 0; k < 256; k++) if (g_gstacks[k].th == me) { slot = k; break; }
    g_gstacks[slot] = (GStack){me, lo + size, size};
    pthread_mutex_unlock(&g_gstack_mu);
}
static const GStack *gstack_of(pthread_t th) {
    const GStack *r = NULL;
    pthread_mutex_lock(&g_gstack_mu);
    for (unsigned k = 0; k < 256; k++) if (g_gstacks[k].th == th) { r = &g_gstacks[k]; break; }
    pthread_mutex_unlock(&g_gstack_mu);
    return r;
}

static CPU *cur_cpu(void) {
    if (!t_cpu) {
        t_cpu = calloc(1, sizeof(CPU));
        t_cpu->tpidrro = (uint64_t)t_tlv_tab;
        prof_register(t_cpu);
        size_t sz = 4u << 20;
        void *st = alloc_guarded(sz);
        own_add((uint64_t)st, sz);
        gstack_register((uint64_t)st, sz);
        t_cpu->sp = ((uint64_t)st + sz - 256) & ~0xfull;
    }
    return t_cpu;
}

static uint64_t call_guest(CPU *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    CPU saved = *c;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    c->x[30] = stub_addr(STUB_RET);
    c->pc = fn;
    bool held = gil_release();  // a callback into the guest runs without the bridge lock, so other guest threads are not held up
    run_until(c, stub_addr(STUB_RET));
    if (held) gil_acquire(); else if (t_have_gil) gil_release();
    uint64_t r = c->x[0];
    *c = saved;
    return r;
}

typedef struct { uint64_t sret; bool mem; int hfa; uint64_t tmp[64]; } EntryState;
static const char *imp_encoding(uint64_t imp);
static void entry_marshal(CPU *c, uint64_t *f, const char *enc, EntryState *st);
static void entry_finish(CPU *c, uint64_t *f, EntryState *st);
static void forward_enc(CPU *c, void *fn, void *stret_fn, const char *enc, const char *name);

// Host code calling a guest address: the exec-fault handler below redirects the faulting
// thread to rebar_guest_entry, which looks like an ordinary callee of the x86 caller.
// Frame (8-byte slots): 0-5 rdi..r9, 6-13 xmm0-7, 14 target, 15 stack args, 16-19 results, 20 rax, 21 r13, 22 r12 (in and out), 23 rcx, 24 r8 (out).
// A Swift caller (a closure handed to DispatchQueue.sync, say) passes the closure's context in r13 (x20 for the guest), the address of an indirect
// result in rax (x8), and takes a thrown error back in r12 (x21) and more than two result words in rcx and r8 (x2, x3); for a C caller these are
// callee-saved or scratch registers and the translation changes nothing.
extern void rebar_guest_entry(void);
__asm__(
    ".text\n"
    ".globl _rebar_guest_entry\n"
    "_rebar_guest_entry:\n"
    ".cfi_startproc\n"
    ".cfi_personality 155, _rebar_entry_personality\n"
    "  pushq %rbp\n  .cfi_def_cfa_offset 16\n  .cfi_offset %rbp, -16\n"
    "  movq %rsp, %rbp\n  .cfi_def_cfa_register %rbp\n  subq $208, %rsp\n"
    "  movq %rax, 160(%rsp)\n  movq %r13, 168(%rsp)\n  movq %r12, 176(%rsp)\n"
    "  movq %rdi, 0(%rsp)\n  movq %rsi, 8(%rsp)\n  movq %rdx, 16(%rsp)\n"
    "  movq %rcx, 24(%rsp)\n  movq %r8, 32(%rsp)\n  movq %r9, 40(%rsp)\n"
    "  movsd %xmm0, 48(%rsp)\n  movsd %xmm1, 56(%rsp)\n  movsd %xmm2, 64(%rsp)\n  movsd %xmm3, 72(%rsp)\n"
    "  movsd %xmm4, 80(%rsp)\n  movsd %xmm5, 88(%rsp)\n  movsd %xmm6, 96(%rsp)\n  movsd %xmm7, 104(%rsp)\n"
    "  movq %r11, 112(%rsp)\n"
    "  leaq 16(%rbp), %rax\n  movq %rax, 120(%rsp)\n"
    "  movq %rsp, %rdi\n"
    "  call _rebar_bridge_enter\n"
    "  movq 128(%rsp), %rax\n  movq 136(%rsp), %rdx\n  movsd 144(%rsp), %xmm0\n  movsd 152(%rsp), %xmm1\n"
    "  movq 176(%rsp), %r12\n  movq 184(%rsp), %rcx\n  movq 192(%rsp), %r8\n"
    "  leave\n  .cfi_def_cfa %rsp, 8\n  ret\n"
    ".cfi_endproc\n");

static __thread int t_cb_stackargs;   // inside a host call whose callbacks have more than eight integer arguments (see rebar_bridge_enter)

// A guest signal handler runs where the kernel would run it: on the alternate stack its thread registered with sigaltstack, when the handler asked for SA_ONSTACK. The host's
// own alternate stack belongs to the emulator (the guest's is only recorded, see h_sigaltstack); on_segv notes that the guest function it redirects is a signal handler.
// Without this the handler ran on whatever stack the interrupted code used, and Go's (adjustSignalStack: "not on the signal stack, not on g0") then waited for a cgo
// thread to borrow forever: `go build` hung when a child exited (SIGCHLD) while a goroutine was running.
static __thread uint64_t t_gss_sp, t_gss_size;
static __thread bool t_gss_on;
static __thread int t_sig_entry;   // the number of the signal whose handler the guest entry in progress is, 0 for any other host call into the guest
static uint32_t g_sig_onstack;     // bit n: the guest's handler for signal n was installed with SA_ONSTACK
static uint64_t g_sigtramp_ra;     // where the host's signal trampoline returns to after calling a handler (found once at start)
static void sigtramp_probe(int sig, siginfo_t *si, void *uc) { (void)sig; (void)si; (void)uc; g_sigtramp_ra = (uint64_t)__builtin_return_address(0); }
static void find_sigtramp(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa); sa.sa_sigaction = sigtramp_probe; sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR2, &sa, &old)) return;
    raise(SIGUSR2);
    sigaction(SIGUSR2, &old, NULL);
}

// The host's istream/ostream seek and tell functions (and streambuf::pubseekoff / pubseekpos) call the virtual seekoff / seekpos of a stream buffer that is the guest's own
// (every ifstream / ofstream of arm64 code: the file buffers are not in libc++.dylib). Both return a 136-byte std::fpos by value, and seekpos takes one: SysV passes the
// result address in rdi (this in rsi) and the argument on the stack, arm64 the result address in x8, this in x0 and a pointer to a copy of the argument in x1.
// Told apart by where in the host the callback comes from.
static uint64_t g_fpos_cb_seekoff[8], g_fpos_cb_seekpos[8];
static int g_fpos_cb_n_off, g_fpos_cb_n_pos, g_fpos_ready;
static void fpos_cb_init(void) {
    static const char *off[] = {"__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5tellgEv", "__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5seekgExNS_8ios_base7seekdirE",
        "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5tellpEv", "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5seekpExNS_8ios_base7seekdirE",
        "__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE10pubseekoffExNS_8ios_base7seekdirEj", NULL};
    static const char *pos[] = {"__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5seekgENS_4fposI11__mbstate_tEE", "__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5seekpENS_4fposI11__mbstate_tEE",
        "__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE10pubseekposENS_4fposI11__mbstate_tEEj", NULL};
    for (int k = 0; off[k]; k++) { void *a = dlsym(RTLD_DEFAULT, off[k] + 1); if (a) g_fpos_cb_seekoff[g_fpos_cb_n_off++] = (uint64_t)a; }
    for (int k = 0; pos[k]; k++) { void *a = dlsym(RTLD_DEFAULT, pos[k] + 1); if (a) g_fpos_cb_seekpos[g_fpos_cb_n_pos++] = (uint64_t)a; }
    g_fpos_ready = 1;
}
static int fpos_callback_kind(uint64_t ra) {   // 1: seekoff, 2: seekpos, 0: neither
    if (!g_fpos_ready) return 0;   // (set up once at start: looking symbols up from a guest thread that has just begun broke the thread tests)
    // (the host's functions lie close together, so a return address near one of them is only a candidate: the function it is in has to be exactly that one)
    bool near = false;
    for (int k = 0; k < g_fpos_cb_n_off && !near; k++) near = ra - g_fpos_cb_seekoff[k] < 0x800;
    for (int k = 0; k < g_fpos_cb_n_pos && !near; k++) near = ra - g_fpos_cb_seekpos[k] < 0x800;
    if (!near) return 0;
    Dl_info di;
    if (!dladdr((void *)ra, &di) || !di.dli_saddr) return 0;
    uint64_t fn = (uint64_t)di.dli_saddr;
    for (int k = 0; k < g_fpos_cb_n_off; k++) if (g_fpos_cb_seekoff[k] == fn) return 1;
    for (int k = 0; k < g_fpos_cb_n_pos; k++) if (g_fpos_cb_seekpos[k] == fn) return 2;
    return 0;
}

void rebar_bridge_enter(uint64_t *f) {
    CPU *c = cur_cpu();
    CPU saved = *c;
    int sigent = t_sig_entry; t_sig_entry = 0;
    const uint64_t *stk = (const uint64_t *)f[15];
    const char *enc = imp_encoding(f[14]);
    static int trace_entry = -1; if (trace_entry < 0) trace_entry = getenv("REBAR_TRACE_ENTRY") != NULL;   // REBAR_TRACE_ENTRY=1: every host call into a guest function (methods: self, selector)
    if (trace_entry) fprintf(stderr, "[entry] %llx%s%s self=%llx sel=%s a2=%llx a3=%llx\n", (unsigned long long)f[14], enc ? " enc=" : "", enc ? enc : "", (unsigned long long)f[0], (f[1] >> 36) == 0x7ff && ((int (*)(void *))dlsym(RTLD_DEFAULT, "sel_isMapped"))((void *)f[1]) ? ((const char *(*)(void *))dlsym(RTLD_DEFAULT, "sel_getName"))((void *)f[1]) : "-", (unsigned long long)f[2], (unsigned long long)f[3]);
    if (trace_entry) { void *ra = ((void **)f[15])[-1]; Dl_info di; fprintf(stderr, "        called from host %p %s+0x%llx (%s)\n", ra, dladdr(ra, &di) && di.dli_sname ? di.dli_sname : "?", dladdr(ra, &di) ? (unsigned long long)((char *)ra - (char *)di.dli_saddr) : 0ull, di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?"); }
    EntryState st;
    int fpos_kind = 0;
    bool held = gil_release();               // entering the guest from host code: the bridge lock is not held while it runs
    Act act = {t_act, c, &saved, t_hc, false};  // lets rebar_entry_personality restore the CPU if an exception leaves
    t_act = &act;
    if (enc) entry_marshal(c, f, enc, &st);
    else if ((fpos_kind = fpos_callback_kind(((uint64_t *)f[15])[-1])) != 0) {
        c->x[8] = f[0]; c->x[0] = f[1];
        if (fpos_kind == 1) { c->x[1] = f[2]; c->x[2] = f[3]; c->x[3] = f[4]; }   // seekoff(this, off, dir, which)
        else { c->x[1] = (uint64_t)stk; c->x[2] = f[2]; }                           // seekpos(this, pos (a copy on the stack), which)
    }
    else {
        for (int k = 0; k < 6; k++) c->x[k] = f[k];
        c->x[6] = stk[0]; c->x[7] = stk[1];
        // the ninth and later integer arguments of an arm64 callee are on its stack, in the order the x86 caller pushed them after the eighth (a DNSService
        // callback gets its rdata, ttl and context there). Only done while a host call that is known to make such callbacks runs: for any other callback the
        // copy below sp broke Bionic (Electron) at start, for a reason not found yet. Eight slots above the caller's own are safe to read.
        if (t_cb_stackargs > 0) { c->sp = (c->sp - 64) & ~0xfull; memcpy((void *)c->sp, stk + 2, 64); }
        for (int k = 0; k < 8; k++) { memset(&c->vr[k], 0, 16); c->vr[k].d[0] = f[6 + k]; }
        c->x[8] = f[20]; c->x[20] = f[21]; c->x[21] = f[22];   // Swift: indirect result address, context, error (all but the error are discarded when the CPU is restored below)
    }
    if (sigent > 0 && sigent < 32 && ((g_sig_onstack >> sigent) & 1) && t_gss_on && !(c->sp > t_gss_sp && c->sp <= t_gss_sp + t_gss_size)) c->sp = (t_gss_sp + t_gss_size) & ~0xfull;
    c->x[30] = stub_addr(STUB_RET);
    c->pc = f[14];
    run_until(c, stub_addr(STUB_RET));
    if (enc) entry_finish(c, f, &st);
    else { f[16] = c->x[0]; f[17] = c->x[1]; f[18] = c->vr[0].d[0]; f[19] = c->vr[1].d[0]; f[22] = c->x[21]; f[23] = c->x[2]; f[24] = c->x[3]; }
    if (!enc && fpos_kind) f[16] = f[0];   // (SysV returns the result address in rax)
    t_act = act.prev;
    *c = saved;
    if (held) gil_acquire(); else if (t_have_gil) gil_release();  // an exception that arrived from the host may have taken the lock
}

static struct { uint64_t fn, arg; } g_atexit[256];
static int g_natexit;

[[noreturn]] static void run_exit(CPU *c, int code) {
    while (g_natexit > 0) {
        g_natexit--;
        call_guest(c, g_atexit[g_natexit].fn, g_atexit[g_natexit].arg, 0, 0, 0);
    }
    exit(code);
}

// --------------------------------------------------------------------- handlers

#define H(name) static void name(CPU *c)
// a host function looked up once per call site (dlsym walks dyld's tries: far too slow for malloc and free)
#define HOSTFN(name) ({ static void *hostfn_; if (!hostfn_) hostfn_ = dlsym(RTLD_DEFAULT, name); hostfn_; })
#define VA_INIT(ap, ptr) \
    va_list ap; ap[0].gp_offset = 48; ap[0].fp_offset = 176; \
    ap[0].overflow_arg_area = (void *)(ptr); ap[0].reg_save_area = NULL
#define RET(v) (c->x[0] = (uint64_t)(int64_t)(v))
#define S(n) ((const char *)c->x[n])
#define P(n) ((void *)c->x[n])

// Variadic arguments start at sp on Apple arm64; a va_list is a pointer to them.
H(h_printf)   { VA_INIT(ap, c->sp); RET(vprintf(S(0), ap)); }
H(h_fprintf)  { VA_INIT(ap, c->sp); RET(vfprintf(P(0), S(1), ap)); }
H(h_sprintf)  { VA_INIT(ap, c->sp); RET(vsprintf(P(0), S(1), ap)); }
H(h_snprintf) { VA_INIT(ap, c->sp); RET(vsnprintf(P(0), c->x[1], S(2), ap)); }
H(h_dprintf)  { VA_INIT(ap, c->sp); RET(vdprintf((int)c->x[0], S(1), ap)); }
H(h_asprintf) { VA_INIT(ap, c->sp); RET(vasprintf(P(0), S(1), ap)); }
H(h_scanf)    { VA_INIT(ap, c->sp); RET(vscanf(S(0), ap)); }
H(h_sscanf)   { VA_INIT(ap, c->sp); RET(vsscanf(S(0), S(1), ap)); }
H(h_fscanf)   { VA_INIT(ap, c->sp); RET(vfscanf(P(0), S(1), ap)); }
H(h_printf_chk)    { VA_INIT(ap, c->sp); RET(vprintf(S(1), ap)); }
H(h_fprintf_chk)   { VA_INIT(ap, c->sp); RET(vfprintf(P(0), S(2), ap)); }
H(h_sprintf_chk)   { VA_INIT(ap, c->sp); RET(vsnprintf(P(0), c->x[2], S(3), ap)); }
H(h_snprintf_chk)  { VA_INIT(ap, c->sp); RET(vsnprintf(P(0), c->x[1], S(4), ap)); }
H(h_vprintf)   { VA_INIT(ap, c->x[1]); RET(vprintf(S(0), ap)); }
H(h_vfprintf)  { VA_INIT(ap, c->x[2]); RET(vfprintf(P(0), S(1), ap)); }
H(h_vsprintf)  { VA_INIT(ap, c->x[2]); RET(vsprintf(P(0), S(1), ap)); }
H(h_vsnprintf) { VA_INIT(ap, c->x[3]); RET(vsnprintf(P(0), c->x[1], S(2), ap)); }
// the wide-character printf family (wchar_t is 32 bits on both architectures)
H(h_swprintf)   { VA_INIT(ap, c->sp); RET(vswprintf((wchar_t *)P(0), c->x[1], (const wchar_t *)P(2), ap)); }
H(h_vswprintf)  { VA_INIT(ap, c->x[3]); RET(vswprintf((wchar_t *)P(0), c->x[1], (const wchar_t *)P(2), ap)); }
H(h_wprintf)    { VA_INIT(ap, c->sp); RET(vwprintf((const wchar_t *)P(0), ap)); }
H(h_vwprintf)   { VA_INIT(ap, c->x[1]); RET(vwprintf((const wchar_t *)P(0), ap)); }
H(h_fwprintf)   { VA_INIT(ap, c->sp); RET(vfwprintf((FILE *)P(0), (const wchar_t *)P(1), ap)); }
H(h_vfwprintf)  { VA_INIT(ap, c->x[2]); RET(vfwprintf((FILE *)P(0), (const wchar_t *)P(1), ap)); }
H(h_vdprintf)  { VA_INIT(ap, c->x[2]); RET(vdprintf((int)c->x[0], S(1), ap)); }
H(h_vasprintf) { VA_INIT(ap, c->x[2]); RET(vasprintf(P(0), S(1), ap)); }
H(h_vsscanf)   { VA_INIT(ap, c->x[2]); RET(vsscanf(S(0), S(1), ap)); }
H(h_vfscanf)   { VA_INIT(ap, c->x[2]); RET(vfscanf(P(0), S(1), ap)); }
// The _l variants take a locale after the fixed arguments.
H(h_printf_l)    { VA_INIT(ap, c->sp); RET(vprintf_l(P(0), S(1), ap)); }
H(h_fprintf_l)   { VA_INIT(ap, c->sp); RET(vfprintf_l(P(0), P(1), S(2), ap)); }
H(h_sprintf_l)   { VA_INIT(ap, c->sp); RET(vsprintf_l(P(0), P(1), S(2), ap)); }
H(h_snprintf_l)  { VA_INIT(ap, c->sp); RET(vsnprintf_l(P(0), c->x[1], P(2), S(3), ap)); }
H(h_dprintf_l)   { VA_INIT(ap, c->sp); RET(vdprintf_l((int)c->x[0], P(1), S(2), ap)); }
H(h_asprintf_l)  { VA_INIT(ap, c->sp); RET(vasprintf_l(P(0), P(1), S(2), ap)); }
H(h_scanf_l)     { VA_INIT(ap, c->sp); RET(vscanf_l(P(0), S(1), ap)); }
H(h_sscanf_l)    { VA_INIT(ap, c->sp); RET(vsscanf_l(S(0), P(1), S(2), ap)); }
H(h_fscanf_l)    { VA_INIT(ap, c->sp); RET(vfscanf_l(P(0), P(1), S(2), ap)); }
H(h_vprintf_l)   { VA_INIT(ap, c->x[2]); RET(vprintf_l(P(0), S(1), ap)); }
H(h_vfprintf_l)  { VA_INIT(ap, c->x[3]); RET(vfprintf_l(P(0), P(1), S(2), ap)); }
H(h_vsprintf_l)  { VA_INIT(ap, c->x[3]); RET(vsprintf_l(P(0), P(1), S(2), ap)); }
H(h_vsnprintf_l) { VA_INIT(ap, c->x[4]); RET(vsnprintf_l(P(0), c->x[1], P(2), S(3), ap)); }
H(h_vdprintf_l)  { VA_INIT(ap, c->x[3]); RET(vdprintf_l((int)c->x[0], P(1), S(2), ap)); }
H(h_vasprintf_l) { VA_INIT(ap, c->x[3]); RET(vasprintf_l(P(0), P(1), S(2), ap)); }
H(h_vsscanf_l)   { VA_INIT(ap, c->x[3]); RET(vsscanf_l(S(0), P(1), S(2), ap)); }
H(h_vfscanf_l)   { VA_INIT(ap, c->x[3]); RET(vfscanf_l(P(0), P(1), S(2), ap)); }
H(h_vsnprintf_chk) { VA_INIT(ap, c->x[5]); RET(vsnprintf(P(0), c->x[1], S(4), ap)); }
H(h_vsprintf_chk)  { VA_INIT(ap, c->x[4]); RET(vsnprintf(P(0), c->x[2], S(3), ap)); }

// The emulator itself depends on SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP reaching on_segv (host code
// jumping into guest code is delivered as an instruction-fetch fault), so the guest may neither
// replace those handlers nor block those signals.
#define FATAL_SIGS ((1u << (SIGILL - 1)) | (1u << (SIGTRAP - 1)) | (1u << (SIGFPE - 1)) | (1u << (SIGBUS - 1)) | (1u << (SIGSEGV - 1)))
static bool fatal_sig(int sig) { return sig == SIGILL || sig == SIGTRAP || sig == SIGFPE || sig == SIGBUS || sig == SIGSEGV; }
// SIGURG is what Go's runtime sends itself to preempt a goroutine: its handler reads and rewrites the arm64 ucontext, which the host does not have (it would corrupt the x86 frame
// and the program spins in signal handlers), so the handler is never installed and the signal stays ignored by default; the scheduler then preempts cooperatively.
static bool guest_cannot_handle(int sig) { return sig == SIGURG; }
H(h_sigaction) {
    int sig = (int)c->x[0];
    if (guest_cannot_handle(sig)) {
        if (c->x[2]) memset(P(2), 0, sizeof(struct sigaction));
        RET(0);
        return;
    }
    if (fatal_sig(sig)) {
        if (c->x[2]) memset(P(2), 0, sizeof(struct sigaction));
        RET(0);
        return;
    }
    // a handler that is a guest function starts with an instruction-fetch fault, which is a SIGSEGV; if the guest's mask (Go blocks every signal while its handlers run)
    // held SIGSEGV back, the thread would fault again and again
    struct sigaction sa; const struct sigaction *sap = (const struct sigaction *)c->x[1];
    if (sap) { sa = *sap; sa.sa_mask &= ~FATAL_SIGS; sap = &sa; if (sig > 0 && sig < 32) { if (sa.sa_flags & SA_ONSTACK) __atomic_or_fetch(&g_sig_onstack, 1u << sig, __ATOMIC_RELAXED); else __atomic_and_fetch(&g_sig_onstack, ~(1u << sig), __ATOMIC_RELAXED); } }
    RET(sigaction(sig, sap, (struct sigaction *)c->x[2]));
}
H(h_signal) {
    int sig = (int)c->x[0];
    if (fatal_sig(sig) || guest_cannot_handle(sig)) { c->x[0] = 0; return; }  // SIG_DFL
    c->x[0] = (uint64_t)signal(sig, (void (*)(int))c->x[1]);
}
H(h_sigprocmask) {
    sigset_t set = c->x[1] ? *(const sigset_t *)c->x[1] : 0, *setp = c->x[1] ? &set : NULL;
    if (setp) set &= ~FATAL_SIGS;
    RET(sigprocmask((int)c->x[0], setp, (sigset_t *)c->x[2]));
}
// The alternate signal stack of every thread belongs to the emulator (x86 signal frames are far larger
// than the stacks arm64 crash handlers allocate; a frame that does not fit kills the process with SIGILL).
H(h_sigaltstack) {
    if (c->x[1]) { stack_t *old = P(1); memset(old, 0, sizeof *old); if (t_gss_on) { old->ss_sp = (void *)t_gss_sp; old->ss_size = t_gss_size; } else old->ss_flags = SS_DISABLE; }
    if (c->x[0]) { const stack_t *ns = P(0); if (ns->ss_flags & SS_DISABLE) t_gss_on = false; else { t_gss_sp = (uint64_t)ns->ss_sp; t_gss_size = ns->ss_size; t_gss_on = true; } }
    RET(0);
}
H(h_pthread_sigmask) {
    sigset_t set = c->x[1] ? *(const sigset_t *)c->x[1] : 0, *setp = c->x[1] ? &set : NULL;
    if (setp) set &= ~FATAL_SIGS;
    RET(pthread_sigmask((int)c->x[0], setp, (sigset_t *)c->x[2]));
}

// Crash reporters (Crashpad in Chromium) register Mach exception ports, which would receive our
// instruction-fetch faults before the BSD signal handler does; refuse silently.
H(h_set_exception_ports) { c->x[0] = 0; }
H(h_swap_exception_ports) { if (c->x[6]) *(uint32_t *)c->x[6] = 0; c->x[0] = 0; }

// ---- malloc zones
// Chromium's allocator shim replaces the function pointers of the process' default malloc zone (after
// making its read-only page writable) and registers its own zones; on a real Mac that sends every malloc
// in the process to PartitionAlloc. Here the host's libraries must keep the host's allocator (a host
// malloc that calls guest code recurses into the emulator's own thread-local setup), so the guest gets a
// writable *clone* of the default zone to patch, its own zones are only recorded, and the guest's calls
// (malloc, free, malloc_zone_*) are routed through the clone's possibly patched function pointers.
#define ZONE_BYTES 256
typedef struct { uint64_t *clone, *real, orig[ZONE_BYTES / 8]; } ZoneClone;
static ZoneClone g_def_zone, g_purge_zone;
static uint64_t g_guest_zones[8];
static int g_nguest_zones;
static bool is_guest_zone(const uint64_t *zone) { return zone && (in_guest(zone[2]) || in_guest(zone[3])); }
static void own_add(uint64_t lo, uint64_t len);
static void *zone_clone_of(ZoneClone *zc, const char *host_fn) {
    if (!zc->clone) {
        if (!g_zone_cloned) { g_zone_cloned = true; if (g_leaf_malloc_used) jit_invalidate(0, ~0ull); }   // (see jit_leaf_import)
        void *(*real)(void) = dlsym(RTLD_DEFAULT, host_fn);
        zc->real = real();
        uint64_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        memcpy(page, zc->real, ZONE_BYTES);
        memcpy(zc->orig, page, ZONE_BYTES);
        own_add((uint64_t)page, 4096);
        zc->clone = page;
    }
    return zc->clone;
}
H(h_pthread_get_stackaddr_np) {
    const GStack *g = gstack_of((pthread_t)c->x[0]);
    if (g) RET(g->hi); else host_call(c, HOSTFN("pthread_get_stackaddr_np"), false);
}
H(h_pthread_get_stacksize_np) {
    const GStack *g = gstack_of((pthread_t)c->x[0]);
    if (g) RET(g->size); else host_call(c, HOSTFN("pthread_get_stacksize_np"), false);
}
H(h_malloc_default_zone) { c->x[0] = (uint64_t)zone_clone_of(&g_def_zone, "malloc_default_zone"); }
H(h_malloc_default_purgeable_zone) { c->x[0] = (uint64_t)zone_clone_of(&g_purge_zone, "malloc_default_purgeable_zone"); }
static ZoneClone *clone_for(const uint64_t *zone) {
    if (g_def_zone.clone == zone) return &g_def_zone;
    if (g_purge_zone.clone == zone) return &g_purge_zone;
    return NULL;
}
static bool def_patched(int slot) { return g_def_zone.clone && g_def_zone.clone[slot] != g_def_zone.orig[slot]; }

H(h_malloc_zone_register) {
    const uint64_t *zone = P(0);  // malloc_zone_t: reserved1, reserved2, size, malloc, calloc, ...
    if (is_guest_zone(zone)) {
        for (int k = 0; k < g_nguest_zones; k++) if (g_guest_zones[k] == (uint64_t)zone) return;
        if (g_nguest_zones < 8) g_guest_zones[g_nguest_zones++] = (uint64_t)zone;
        return;
    }
    if (clone_for(zone)) return;
    void (*reg)(void *) = HOSTFN("malloc_zone_register");
    reg(P(0));
}
H(h_malloc_zone_unregister) {
    const uint64_t *zone = P(0);
    if (is_guest_zone(zone)) {
        for (int k = 0; k < g_nguest_zones; k++)
            if (g_guest_zones[k] == (uint64_t)zone) { g_guest_zones[k] = g_guest_zones[--g_nguest_zones]; break; }
        return;
    }
    if (clone_for(zone)) return;
    void (*unreg)(void *) = HOSTFN("malloc_zone_unregister");
    unreg(P(0));
}

// Calls slot `slot` of a zone: a guest function pointer runs in the interpreter, an untouched host one is
// called on the real host zone.
static bool zone_slot_call(CPU *c, uint64_t *zone, int slot, int nargs, uint64_t *args) {
    ZoneClone *zc = clone_for(zone);
    uint64_t fn = zone[slot];
    if (zc && fn == zc->orig[slot]) return false;     // untouched: the caller uses the host entry point
    if (!zc && !is_guest_zone(zone)) return false;
    c->x[0] = call_guest(c, fn, (uint64_t)zone, nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0, nargs > 2 ? args[2] : 0);
    return true;
}
#define ZONE_FN(name, slot, nargs, hostsym)                                                             \
    H(name) {                                                                                           \
        uint64_t *zone = P(0), a[3] = {c->x[1], c->x[2], c->x[3]};                                      \
        if (zone_slot_call(c, zone, slot, nargs, a)) return;                                            \
        ZoneClone *zc = clone_for(zone);                                                                \
        if (zc) c->x[0] = (uint64_t)zc->real;                                                           \
        host_call(c, HOSTFN(hostsym), false);                                              \
    }
ZONE_FN(h_malloc_zone_malloc, 3, 1, "malloc_zone_malloc")
ZONE_FN(h_malloc_zone_calloc, 4, 2, "malloc_zone_calloc")
ZONE_FN(h_malloc_zone_valloc, 5, 1, "malloc_zone_valloc")
ZONE_FN(h_malloc_zone_free, 6, 1, "malloc_zone_free")
ZONE_FN(h_malloc_zone_realloc, 7, 2, "malloc_zone_realloc")

// Plain malloc/free from the guest: libSystem sends them to the default zone, which the guest may have patched.
H(h_malloc)  { uint64_t a[3] = {c->x[0], 0, 0}; if (def_patched(3) && zone_slot_call(c, g_def_zone.clone, 3, 1, a)) return; host_call(c, HOSTFN("malloc"), false); }
H(h_calloc)  { uint64_t a[3] = {c->x[0], c->x[1], 0}; if (def_patched(4) && zone_slot_call(c, g_def_zone.clone, 4, 2, a)) return; host_call(c, HOSTFN("calloc"), false); }
H(h_valloc)  { uint64_t a[3] = {c->x[0], 0, 0}; if (def_patched(5) && zone_slot_call(c, g_def_zone.clone, 5, 1, a)) return; host_call(c, HOSTFN("valloc"), false); }
H(h_free)    { uint64_t a[3] = {c->x[0], 0, 0}; if (def_patched(6) && zone_slot_call(c, g_def_zone.clone, 6, 1, a)) return; host_call(c, HOSTFN("free"), false); }
H(h_realloc) { uint64_t a[3] = {c->x[0], c->x[1], 0}; if (def_patched(7) && zone_slot_call(c, g_def_zone.clone, 7, 2, a)) return; host_call(c, HOSTFN("realloc"), false); }
H(h_malloc_size) { uint64_t a[3] = {c->x[0], 0, 0}; if (def_patched(2) && zone_slot_call(c, g_def_zone.clone, 2, 1, a)) return; host_call(c, HOSTFN("malloc_size"), false); }

H(h_malloc_get_all_zones) {  // (task, reader, **addresses, *count): the guest sees its own zones first, the default zone as its clone
    int (*real)(uint64_t, void *, uint64_t **, uint32_t *) = HOSTFN("malloc_get_all_zones");
    uint64_t *host_zones = NULL; uint32_t nhost = 0;
    int r = real(0, NULL, &host_zones, &nhost);
    uint64_t *all = malloc((nhost + g_nguest_zones + 1) * 8);
    int n = 0;
    for (int k = 0; k < g_nguest_zones; k++) all[n++] = g_guest_zones[k];
    for (uint32_t k = 0; k < nhost; k++)
        all[n++] = (g_def_zone.clone && host_zones[k] == (uint64_t)g_def_zone.real) ? (uint64_t)g_def_zone.clone
                 : (g_purge_zone.clone && host_zones[k] == (uint64_t)g_purge_zone.real) ? (uint64_t)g_purge_zone.clone : host_zones[k];
    if (c->x[2]) *(uint64_t **)c->x[2] = all;
    if (c->x[3]) *(uint32_t *)c->x[3] = (uint32_t)n;
    RET(r);
}

// Memory the guest owns: its images (and the reservation around them), the stacks the emulator gives it,
// and every mapping it makes itself. The guest probes and re-protects memory around its stack (Chromium
// does), and must not be able to touch pages that belong to the host or to the emulator: protection
// changes, unmaps and advice outside guest-owned ranges are silently accepted without being done.
typedef struct { uint64_t lo, hi; } Range;
// A sorted array of disjoint, non-adjacent ranges: mappings next to each other (a reservation and the pieces of it
// that get committed, say) are one range, and an mprotect that spans several of them is still the guest's own.
#define MAX_OWNED 65536
static Range g_owned[MAX_OWNED];
static int g_nowned;
static pthread_mutex_t g_owned_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_owned_seq;  // odd while the table is being changed: lock-free readers (guest_owned_code) retry
static int own_first_ending_at_or_after(uint64_t x) {  // first range with hi >= x
    int a = 0, b = g_nowned;
    while (a < b) { int m = (a + b) / 2; if (g_owned[m].hi < x) a = m + 1; else b = m; }
    return a;
}
static void own_add(uint64_t lo, uint64_t len) {
    jit_invalidate(lo, lo + len);   // (new contents may have been mapped over code that was translated)
    if (!len) return;
    uint64_t hi = lo + len;
    pthread_mutex_lock(&g_owned_lock);
    __atomic_add_fetch(&g_owned_seq, 1, __ATOMIC_SEQ_CST);
    int a = own_first_ending_at_or_after(lo), b = a;
    while (b < g_nowned && g_owned[b].lo <= hi) {
        if (g_owned[b].lo < lo) lo = g_owned[b].lo;
        if (g_owned[b].hi > hi) hi = g_owned[b].hi;
        b++;
    }
    if (b == a) {
        if (g_nowned < MAX_OWNED) {
            memmove(&g_owned[a + 1], &g_owned[a], (size_t)(g_nowned - a) * sizeof(Range));
            g_owned[a] = (Range){lo, hi};
            g_nowned++;
        }
    } else {
        g_owned[a] = (Range){lo, hi};
        memmove(&g_owned[a + 1], &g_owned[b], (size_t)(g_nowned - b) * sizeof(Range));
        g_nowned -= b - a - 1;
    }
    __atomic_add_fetch(&g_owned_seq, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_owned_lock);
}
static void own_remove(uint64_t lo, uint64_t len) {
    __atomic_add_fetch(&g_code_epoch, 1, __ATOMIC_RELAXED);
    jit_invalidate(lo, lo + len);
    uint64_t hi = lo + len;
    pthread_mutex_lock(&g_owned_lock);
    __atomic_add_fetch(&g_owned_seq, 1, __ATOMIC_SEQ_CST);
    int k = own_first_ending_at_or_after(lo + 1);  // first range with hi > lo
    while (k < g_nowned && g_owned[k].lo < hi) {
        Range r = g_owned[k];
        if (r.lo >= lo && r.hi <= hi) {  // all of it goes
            memmove(&g_owned[k], &g_owned[k + 1], (size_t)(g_nowned - k - 1) * sizeof(Range));
            g_nowned--;
        } else if (r.lo < lo && r.hi > hi) {  // a hole in the middle
            if (g_nowned < MAX_OWNED) {
                memmove(&g_owned[k + 2], &g_owned[k + 1], (size_t)(g_nowned - k - 1) * sizeof(Range));
                g_owned[k] = (Range){r.lo, lo};
                g_owned[k + 1] = (Range){hi, r.hi};
                g_nowned++;
            } else g_owned[k].hi = lo;
            break;
        } else if (r.lo < lo) { g_owned[k].hi = lo; k++; }  // the tail goes
        else { g_owned[k].lo = hi; break; }                 // the head goes
    }
    __atomic_add_fetch(&g_owned_seq, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&g_owned_lock);
}
static bool own_check(uint64_t lo, uint64_t len) {
    if (in_guest(lo) && in_guest(lo + len - 1)) return true;
    bool ok = false;
    pthread_mutex_lock(&g_owned_lock);
    int k = own_first_ending_at_or_after(lo + len);  // first range that ends at or after the end of the span
    if (k < g_nowned) ok = g_owned[k].lo <= lo && lo + len <= g_owned[k].hi;
    pthread_mutex_unlock(&g_owned_lock);
    return ok;
}
// Code the guest generated into memory it mapped (V8 copies its builtins into a code range): the interpreter runs
// it like any other guest code. Host functions never live in such mappings. Also reached from the fault handler, so
// the scan takes no lock.
static __thread Range t_code_hit;
static void guest_code_range(uint64_t *lo, uint64_t *len) { *lo = t_code_hit.lo; *len = t_code_hit.hi - t_code_hit.lo; }
static __thread uint32_t t_code_hit_epoch;
static bool guest_owned_code(uint64_t pc) {
    if (t_code_hit_epoch != g_code_epoch) { t_code_hit = (Range){0, 0}; t_code_hit_epoch = g_code_epoch; }
    if (pc - t_code_hit.lo < t_code_hit.hi - t_code_hit.lo) return true;
    for (;;) {  // no lock (this is also reached from the fault handler): a seqlock makes the scan see a consistent table
        uint32_t seq = __atomic_load_n(&g_owned_seq, __ATOMIC_SEQ_CST);
        if (seq & 1) continue;
        int n = g_nowned;
        Range hit = {0, 0};
        for (int k = 0; k < n && k < MAX_OWNED; k++)
            if (pc - g_owned[k].lo < g_owned[k].hi - g_owned[k].lo) { hit = g_owned[k]; break; }
        if (__atomic_load_n(&g_owned_seq, __ATOMIC_SEQ_CST) != seq) continue;
        if (hit.hi) t_code_hit = hit;
        return hit.hi != 0;
    }
}
// arm64 macOS has 16KB pages and its programs rely on it: PartitionAlloc and other allocators assume what mmap
// returns is 16KB-aligned and that lengths round up to 16KB. The x86_64 host maps in 4KB pages, so the
// guest's view is adjusted here.
#define GUEST_PAGE 0x4000ull
static uint64_t g_trace_mem;  // REBAR_TRACE_MEM=<hex addr>: log the mapping calls whose range contains it
static void trace_mem(const char *what, uint64_t a, uint64_t len, uint64_t x, uint64_t y, uint64_t ret, uint64_t lr) {
    if (!g_trace_mem || g_trace_mem - a >= (len ? len : 1)) return;
    fprintf(stderr, "[mem] %s(0x%llx, 0x%llx, 0x%llx, 0x%llx) = 0x%llx from 0x%llx\n", what, (unsigned long long)a, (unsigned long long)len,
            (unsigned long long)x, (unsigned long long)y, (unsigned long long)ret, (unsigned long long)lr);
}
H(h_mmap) {  // (addr, len, prot, flags, fd, off)
    int flags = (int)c->x[3], prot = (int)c->x[2], fd = (int)c->x[4];
    off_t off = (off_t)c->x[5];
    uint64_t len = (c->x[1] + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    void *r = mmap(P(0), len, prot, flags, fd, off);
    if (r == MAP_FAILED && (flags & 0x800)) {  // MAP_JIT
        flags &= ~0x800;
        r = mmap(P(0), len, prot, flags, fd, off);
    }
    if (r != MAP_FAILED && !(flags & MAP_FIXED) && ((uint64_t)r & (GUEST_PAGE - 1))) {
        munmap(r, len);  // not 16KB aligned: reserve extra room, place the mapping on a 16KB boundary and give the rest back
        uint8_t *base = mmap(NULL, len + GUEST_PAGE, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
        if (base == MAP_FAILED) { c->x[0] = (uint64_t)MAP_FAILED; return; }
        uint8_t *al = (uint8_t *)(((uint64_t)base + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1));
        r = mmap(al, len, prot, (flags & ~0x800) | MAP_FIXED, fd, off);  // (MAP_JIT and MAP_FIXED together are refused on x86_64)
        if (al > base) munmap(base, (size_t)(al - base));
        uint8_t *end = al + len, *top = base + len + GUEST_PAGE;
        if (top > end) munmap(end, (size_t)(top - end));
    }
    if (r != MAP_FAILED) own_add((uint64_t)r, len);
    if (g_trace_mem && ((uint64_t)r != (uint64_t)-1 ? g_trace_mem - (uint64_t)r < len : g_trace_mem - c->x[0] < len))
        fprintf(stderr, "[mem] mmap(hint 0x%llx, len 0x%llx, prot %d, flags 0x%x) = %p from 0x%llx\n", (unsigned long long)c->x[0], (unsigned long long)c->x[1], prot, flags, r, (unsigned long long)c->x[30]);
    c->x[0] = (uint64_t)r;
}
// ---- running other arm64 programs
// A program that starts helper processes (Chromium's GPU, network and crashpad helpers, Node's child_process)
// asks the kernel to execute an arm64-only executable, which an x86_64 host refuses ("Bad CPU type").
// posix_spawn / exec of such a file is turned into running it under rebar.
static char g_self_path[PATH_MAX];
static bool needs_rebar(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    uint8_t buf[4096];
    ssize_t n = pread(fd, buf, sizeof buf, 0);
    close(fd);
    if (n < 32) return false;
    uint32_t magic = *(uint32_t *)buf;
    if (magic == MH_MAGIC_64) return ((struct mach_header_64 *)buf)->cputype == CPU_TYPE_ARM64;
    if (magic == FAT_CIGAM) {
        const struct fat_header *fh = (const void *)buf;
        const struct fat_arch *fa = (const void *)(fh + 1);
        bool arm = false, x86 = false;
        uint32_t na = __builtin_bswap32(fh->nfat_arch);
        for (uint32_t k = 0; k < na && (const uint8_t *)(fa + k + 1) <= buf + n; k++) {
            cpu_type_t t = (cpu_type_t)__builtin_bswap32(fa[k].cputype);
            if (t == CPU_TYPE_ARM64 && (__builtin_bswap32(fa[k].cpusubtype) & 0xff) != 2) arm = true;   // (an arm64e slice, as in the system tools, needs pointer authentication: rebar cannot run it)
            if (t == CPU_TYPE_X86_64) x86 = true;
        }
        static int all_arm = -1; if (all_arm < 0) all_arm = getenv("REBAR_ALL_ARM64") != NULL;   // REBAR_ALL_ARM64=1: a universal binary that is started runs its arm64 slice under rebar too (to test an app's arm64 build on a Mac that could run the x86_64 one)
        return arm && (!x86 || all_arm);
    }
    return false;
}
static char *resolve_exec_path(const char *file, char *out) {   // execvp-style lookup
    if (strchr(file, '/')) { snprintf(out, PATH_MAX, "%s", file); return out; }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    while (*path) {
        const char *e = strchr(path, ':');
        size_t l = e ? (size_t)(e - path) : strlen(path);
        snprintf(out, PATH_MAX, "%.*s/%s", (int)l, path, file);
        if (access(out, X_OK) == 0) return out;
        path += l + (e ? 1 : 0);
    }
    return NULL;
}
static char **rebar_argv(const char *path, char *const *argv) {  // [rebar, path, argv[1..]]
    int n = 0;
    while (argv && argv[n]) n++;
    char **v = calloc((size_t)n + 3, sizeof(char *));
    v[0] = g_self_path;
    v[1] = (char *)path;
    for (int k = 1; k < n; k++) v[k + 1] = argv[k];
    return v;
}
// A program that is not arm64 and not rebar (a login shell, env, git) gets librebar_exec.dylib (execshim.c) injected, so that what it starts can be arm64 too: the shim turns an
// exec of an arm64-only file into `rebar file ...`. The variables are added to the environment of that one child only, the guest never sees them.
static char g_shim_path[PATH_MAX];   // next to rebar; empty: no shim
static char **env_with_shim(char *const *envp) {
    char *const *base = envp ? envp : environ;
    if (!g_shim_path[0]) return (char **)base;
    int n = 0;
    for (; base && base[n]; n++) if (!strncmp(base[n], "DYLD_INSERT_LIBRARIES=", 22)) return (char **)base;
    char **v = calloc((size_t)n + 3, sizeof *v);
    for (int k = 0; k < n; k++) v[k] = base[k];
    char *a = NULL, *b = NULL;
    if (asprintf(&a, "DYLD_INSERT_LIBRARIES=%s", g_shim_path) < 0 || asprintf(&b, "REBAR_BIN=%s", g_self_path) < 0) { free(v); return (char **)base; }
    v[n] = a; v[n + 1] = b;
    return v;
}
static void env_free_shim(char **v, char *const *orig) { if (v != (char **)orig && v != environ) { int n = 0; while (v[n]) n++; free(v[n - 2]); free(v[n - 1]); free(v); } }
static bool exec_shim(const char *file, bool search, char *const *argv, char **new_argv, char *resolved) {
    if (!g_self_path[0] || !file) return false;
    const char *path = search ? resolve_exec_path(file, resolved) : file;
    if (!path || !needs_rebar(path)) return false;
    if (path != resolved) snprintf(resolved, PATH_MAX, "%s", path);
    char **v = rebar_argv(resolved, argv);
    *(char ***)new_argv = v;
    return true;
}
static void do_spawn(CPU *c, bool search) {  // (pid*, path, file_actions, attr, argv, envp)
    char resolved[PATH_MAX];
    char **v = NULL;
    int (*spawn)(pid_t *, const char *, const void *, const void *, char *const *, char *const *) = HOSTFN("posix_spawn");
    if (exec_shim((const char *)c->x[1], search, (char *const *)c->x[4], (char **)&v, resolved)) {
        int r = spawn((pid_t *)c->x[0], g_self_path, (const void *)c->x[2], (const void *)c->x[3], v, (char *const *)c->x[5]);
        free(v);
        RET(r);
        return;
    }
    char **env = env_with_shim((char *const *)c->x[5]);
    if (search) spawn = HOSTFN("posix_spawnp");   // (a program that is not arm64 is found by the host's own PATH search)
    int r = spawn((pid_t *)c->x[0], (const char *)c->x[1], (const void *)c->x[2], (const void *)c->x[3], (char *const *)c->x[4], env);
    env_free_shim(env, (char *const *)c->x[5]);
    RET(r);
}
H(h_posix_spawn)  { do_spawn(c, false); }
H(h_posix_spawnp) { do_spawn(c, true); }
static void do_exec(CPU *c, const char *file, char *const *argv, char *const *envp, bool search) {
    char resolved[PATH_MAX];
    char **v = NULL;
    if (exec_shim(file, search, argv, (char **)&v, resolved)) {
        execve(g_self_path, v, envp ? envp : environ);
        free(v);
        RET(-1);
        return;
    }
    char **env = env_with_shim(envp);
    if (search) {
        char path2[PATH_MAX];
        if (!resolve_exec_path(file, path2)) { errno = ENOENT; env_free_shim(env, envp); RET(-1); return; }
        execve(path2, argv, env);
    } else execve(file, argv, env);
    env_free_shim(env, envp);
    RET(-1);
}
H(h_execve)  { do_exec(c, (const char *)c->x[0], (char *const *)c->x[1], (char *const *)c->x[2], false); }
H(h_execv)   { do_exec(c, (const char *)c->x[0], (char *const *)c->x[1], NULL, false); }
H(h_execvp)  { do_exec(c, (const char *)c->x[0], (char *const *)c->x[1], NULL, true); }
// mincore reports one byte per page: the host's pages are 4KB, the guest counts 16KB ones, so fold four into one
H(h_mincore) {  // (addr, len, vec)
    uint64_t addr = c->x[0], len = c->x[1];
    uint8_t *vec = P(2);
    uint64_t gpages = (len + GUEST_PAGE - 1) / GUEST_PAGE, hpages = (len + 4095) / 4096;
    uint8_t *tmp = malloc(hpages + 4);
    int r = mincore(P(0), (size_t)len, (char *)tmp);
    if (r == 0) {
        for (uint64_t g = 0; g < gpages; g++) {
            uint8_t v = 0;
            for (uint64_t k = g * 4; k < g * 4 + 4 && k < hpages; k++) v |= tmp[k];
            vec[g] = v;
        }
    }
    free(tmp);
    (void)addr;
    RET(r);
}
H(h_getpagesize) { RET(GUEST_PAGE); }
H(h_sysconf) {
    if ((int)c->x[0] == 29) { RET(GUEST_PAGE); return; }  // _SC_PAGESIZE
    host_call(c, HOSTFN("sysconf"), false);
}
H(h_sys_icache_invalidate) { jit_invalidate(c->x[0], c->x[0] + c->x[1]); }
// A guest that suspends another thread (Chromium's sampling profiler suspends the browser's main thread every few seconds) must not freeze it inside the
// translator: the suspender translates blocks too and would wait for the lock that the frozen thread holds, for ever. With the lock held across the
// suspension the target cannot be inside jit_compile / jit_invalidate when it stops.
H(h_thread_suspend) {
    pthread_mutex_lock(&g_jit_mu);
    c->x[0] = (uint32_t)thread_suspend((thread_act_t)c->x[0]);
    pthread_mutex_unlock(&g_jit_mu);
}
H(h_munmap) { bool ok = own_check(c->x[0], c->x[1]); trace_mem(ok ? "munmap" : "munmap (ignored)", c->x[0], c->x[1], 0, 0, 0, c->x[30]); if (!ok) { RET(0); return; } own_remove(c->x[0], c->x[1]); RET(munmap(P(0), c->x[1])); }
H(h_mprotect) { bool ok = own_check(c->x[0], c->x[1]); trace_mem(ok ? "mprotect" : "mprotect (ignored)", c->x[0], c->x[1], c->x[2], 0, 0, c->x[30]); if (!ok) { RET(0); return; } RET(mprotect(P(0), c->x[1], (int)c->x[2])); }
H(h_madvise) { bool ok = own_check(c->x[0], c->x[1]); trace_mem(ok ? "madvise" : "madvise (ignored)", c->x[0], c->x[1], c->x[2], 0, 0, c->x[30]); if (!ok) { RET(0); return; } RET(madvise(P(0), c->x[1], (int)c->x[2])); }
H(h_vm_protect) {  // (task, addr, size, set_maximum, prot) - also used for mach_vm_protect
    if (!own_check(c->x[1], c->x[2])) { RET(0); return; }
    kern_return_t (*f)(uint64_t, uint64_t, uint64_t, int, int) = HOSTFN("mach_vm_protect");
    RET(f(c->x[0], c->x[1], c->x[2], (int)c->x[3], (int)c->x[4]));
}
H(h_vm_allocate) {  // (task, *addr, size, flags)
    kern_return_t (*f)(uint64_t, uint64_t *, uint64_t, int) = HOSTFN("mach_vm_allocate");
    int r = f(c->x[0], (uint64_t *)c->x[1], c->x[2], (int)c->x[3]);
    if (r == 0) own_add(*(uint64_t *)c->x[1], c->x[2]);
    RET(r);
}
H(h_vm_deallocate) {  // (task, addr, size)
    if (!own_check(c->x[1], c->x[2])) { RET(0); return; }
    own_remove(c->x[1], c->x[2]);
    kern_return_t (*f)(uint64_t, uint64_t, uint64_t) = HOSTFN("mach_vm_deallocate");
    RET(f(c->x[0], c->x[1], c->x[2]));
}

// open of a FIFO blocks until the other end is opened, and F_SETLKW until the lock is free: the other end is often another guest thread, which needs the bridge lock to get there (Zig's parallel build)
#define NOGIL(expr) ({ bool _g = gil_release(); __typeof__(expr) _r = (expr); int _e = errno; if (_g) gil_acquire(); errno = _e; _r; })
H(h_open)   { RET(NOGIL(open(S(0), (int)c->x[1], *(uint32_t *)c->sp))); }
H(h_openat) { RET(NOGIL(openat((int)c->x[0], S(1), (int)c->x[2], *(uint32_t *)c->sp))); }
H(h_fcntl)  { if (c->x[1] == F_SETLKW) RET(NOGIL(fcntl((int)c->x[0], (int)c->x[1], *(uint64_t *)c->sp))); else RET(fcntl((int)c->x[0], (int)c->x[1], *(uint64_t *)c->sp)); }
H(h_ioctl)  { RET(ioctl((int)c->x[0], c->x[1], *(uint64_t *)c->sp)); }
// More variadic libc functions: their variadic arguments are on the guest's stack on Apple arm64 (a call through the host's registers passes junk: Audacity's single-instance
// semaphore was set to 0 by semctl(SETVAL, 1) and the first semop blocked for ever).
H(h_semctl)  { RET(semctl((int)c->x[0], (int)c->x[1], (int)c->x[2], *(uint64_t *)c->sp)); }
H(h_shm_open) { RET(shm_open(S(0), (int)c->x[1], (mode_t)*(uint64_t *)c->sp)); }
H(h_sem_open) { RET((uint64_t)sem_open(S(0), (int)c->x[1], (mode_t)((uint64_t *)c->sp)[0], (unsigned)((uint64_t *)c->sp)[1])); }
H(h_syslog)  { VA_INIT(ap, c->sp); vsyslog((int)c->x[0], S(1), ap); c->x[0] = 0; }
static void exec_list(CPU *c, bool search, bool with_env) {   // execl / execlp / execle: arg0 in x1, the rest of the list on the stack, up to a NULL (and then the environment for execle)
    char *argv[64]; int n = 0; argv[n++] = (char *)c->x[1];
    const uint64_t *st = (const uint64_t *)c->sp;
    while (n < 63 && st[n - 1]) { argv[n] = (char *)st[n - 1]; n++; }
    char *const *envp = with_env ? (char *const *)st[n] : NULL;
    argv[n] = NULL;
    do_exec(c, (const char *)c->x[0], argv, envp, search);
}
// Variadic functions of other libraries that the host forwards: the first `fixed` arguments stay in x0.., the variadic ones (one 8-byte slot each) come from the guest's stack into the registers
// that follow, where the x86 callee's va_arg finds them (libcurl's option setters, sqlite's configuration calls).
#define VARIADIC_FWD(handler, host, fixed, nvar) \
    H(handler) { const uint64_t *st = (const uint64_t *)c->sp; for (int k = 0; k < (nvar); k++) c->x[(fixed) + k] = st[k]; host_call(c, HOSTFN(host), false); }
VARIADIC_FWD(h_curl_easy_setopt, "curl_easy_setopt", 2, 1)
VARIADIC_FWD(h_curl_easy_getinfo, "curl_easy_getinfo", 2, 1)
VARIADIC_FWD(h_curl_multi_setopt, "curl_multi_setopt", 2, 1)
VARIADIC_FWD(h_curl_share_setopt, "curl_share_setopt", 2, 1)
VARIADIC_FWD(h_sqlite3_db_config, "sqlite3_db_config", 2, 3)
VARIADIC_FWD(h_sqlite3_config, "sqlite3_config", 1, 3)
H(h_execl)   { exec_list(c, false, false); }
H(h_execlp)  { exec_list(c, true, false); }
H(h_execle)  { exec_list(c, false, true); }

H(h_exit)       { run_exit(c, (int)c->x[0]); }
H(h_nop)        { c->x[0] = 0; }
H(h_atexit)     { if (g_natexit < 256) { g_atexit[g_natexit].fn = c->x[0]; g_atexit[g_natexit].arg = 0; g_natexit++; } c->x[0] = 0; }
H(h_cxa_atexit) { if (g_natexit < 256) { g_atexit[g_natexit].fn = c->x[0]; g_atexit[g_natexit].arg = c->x[1]; g_natexit++; } c->x[0] = 0; }

typedef struct { CPU *c; uint64_t fn, thunk; bool has_thunk; } CbCtx;
static int qsort_cb(void *t, const void *a, const void *b) {
    CbCtx *x = t;
    return (int)call_guest(x->c, x->fn, x->has_thunk ? x->thunk : (uint64_t)a, x->has_thunk ? (uint64_t)a : (uint64_t)b,
                           x->has_thunk ? (uint64_t)b : 0, 0);
}
H(h_qsort)   { CbCtx x = {c, c->x[3], 0, false}; qsort_r(P(0), c->x[1], c->x[2], &x, qsort_cb); }
H(h_qsort_r) { CbCtx x = {c, c->x[4], c->x[3], true}; qsort_r(P(0), c->x[1], c->x[2], &x, qsort_cb); }
H(h_bsearch) {
    uint64_t key = c->x[0], base = c->x[1], n = c->x[2], w = c->x[3], fn = c->x[4];
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2, el = base + mid * w;
        int r = (int)call_guest(c, fn, key, el, 0, 0);
        if (r == 0) { c->x[0] = el; return; }
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    c->x[0] = 0;
}

// std::filesystem::directory_iterator and recursive_directory_iterator keep their state (a std::filesystem::directory_entry: the path as a std::string, then plain data) on the host's
// heap, in the host's std::string layout; the guest's inline code reads entry.path() in the arm64 layout. __dereference hands out a converted copy of the current entry (one per host
// entry address, refreshed at every call: the iterator moves on, the address stays).
static void str_to_guest(uint8_t *g, const uint8_t *d);
static struct { uint64_t host; uint8_t *guest; } g_dentry[4096];
static uint8_t *dir_entry_guest(uint64_t host) {
    uint32_t h = (uint32_t)((host >> 4) * 2654435761u) & 4095;
    for (int n = 0; n < 4096 && g_dentry[h].host && g_dentry[h].host != host; n++) h = (h + 1) & 4095;
    if (!g_dentry[h].host) { g_dentry[h].host = host; g_dentry[h].guest = calloc(1, 80); }
    uint8_t *g = g_dentry[h].guest;
    memcpy(g, (const void *)host, 80);
    str_to_guest(g, (const uint8_t *)host);
    return g;
}
H(h_dir_deref)      { host_call(c, HOSTFN("_ZNKSt3__14__fs10filesystem18directory_iterator13__dereferenceEv"), false); if (c->x[0]) c->x[0] = (uint64_t)dir_entry_guest(c->x[0]); }
H(h_rdir_deref)     { host_call(c, HOSTFN("_ZNKSt3__14__fs10filesystem28recursive_directory_iterator13__dereferenceEv"), false); if (c->x[0]) c->x[0] = (uint64_t)dir_entry_guest(c->x[0]); }

// tsearch / tfind / tdelete / twalk / lfind / lsearch: the host's libc calls the guest's comparator, which would be one instruction-fetch fault (a signal, ~6 microseconds) per
// comparison; these call it with call_guest instead. The comparator takes no context argument, so the guest function travels in a thread-local.
static __thread struct { CPU *c; uint64_t fn; } t_cmp;
static int cmp_cb(const void *a, const void *b) { return (int)call_guest(t_cmp.c, t_cmp.fn, (uint64_t)a, (uint64_t)b, 0, 0); }
static void walk_cb(const void *node, VISIT which, int level) { call_guest(t_cmp.c, t_cmp.fn, (uint64_t)node, (uint64_t)which, (uint64_t)level, 0); }
#define WITH_CMP(CPUARG, FN, EXPR) ({ typeof(t_cmp) saved_ = t_cmp; t_cmp.c = (CPUARG); t_cmp.fn = (FN); typeof(EXPR) r_ = (EXPR); t_cmp = saved_; r_; })
H(h_tsearch) { c->x[0] = (uint64_t)WITH_CMP(c, c->x[2], tsearch(P(0), (void **)P(1), cmp_cb)); }
H(h_tfind)   { c->x[0] = (uint64_t)WITH_CMP(c, c->x[2], tfind(P(0), (void *const *)P(1), cmp_cb)); }
H(h_tdelete) { c->x[0] = (uint64_t)WITH_CMP(c, c->x[2], tdelete(P(0), (void **)P(1), cmp_cb)); }
H(h_twalk)   { typeof(t_cmp) saved = t_cmp; t_cmp.c = c; t_cmp.fn = c->x[1]; twalk(P(0), walk_cb); t_cmp = saved; }
H(h_lfind)   { c->x[0] = (uint64_t)WITH_CMP(c, c->x[4], lfind(P(0), P(1), (size_t *)P(2), c->x[3], cmp_cb)); }
H(h_lsearch) { c->x[0] = (uint64_t)WITH_CMP(c, c->x[4], lsearch(P(0), P(1), (size_t *)P(2), c->x[3], cmp_cb)); }

// jmp_buf layout (ours): x19..x28, fp, lr, sp, d8..d15
H(h_setjmp) {
    uint64_t *jb = P(0);
    for (int k = 0; k < 10; k++) jb[k] = c->x[19 + k];
    jb[10] = c->x[29]; jb[11] = c->x[30]; jb[12] = c->sp;
    for (int k = 0; k < 8; k++) jb[13 + k] = c->vr[8 + k].d[0];
    c->x[0] = 0;
}
H(h_longjmp) {
    uint64_t *jb = P(0);
    int val = (int)c->x[1] ? (int)c->x[1] : 1;
    for (int k = 0; k < 10; k++) c->x[19 + k] = jb[k];
    c->x[29] = jb[10]; c->pc = jb[11]; c->sp = jb[12];
    for (int k = 0; k < 8; k++) c->vr[8 + k].d[0] = jb[13 + k];
    c->x[0] = (uint64_t)val;
}

// SSLCipherSuite is a uint32_t in the x86_64 SDK that the host's Security framework was built for and a uint16_t in the arm64 SDK the guest was built
// with: a guest that sized a buffer for N suites of 2 bytes had 4 bytes per suite written into it (a Rust program's TLS setup corrupted the heap this way).
static int ssl_get_ciphers(CPU *c, const char *fn) {
    int (*f)(void *, uint32_t *, size_t *) = dlsym(RTLD_DEFAULT, fn);
    uint16_t *out = P(1); size_t *num = P(2);
    size_t cap = *num, n = cap;
    uint32_t *tmp = calloc(cap ? cap : 1, sizeof(uint32_t));
    int r = f(P(0), tmp, &n);
    if (r == 0) { for (size_t k = 0; k < n && k < cap; k++) out[k] = (uint16_t)tmp[k]; *num = n; }
    free(tmp);
    return r;
}
H(h_SSLGetEnabledCiphers) { c->x[0] = (uint32_t)ssl_get_ciphers(c, "SSLGetEnabledCiphers"); }
H(h_SSLGetSupportedCiphers) { c->x[0] = (uint32_t)ssl_get_ciphers(c, "SSLGetSupportedCiphers"); }
H(h_SSLSetEnabledCiphers) {
    int (*f)(void *, const uint32_t *, size_t) = dlsym(RTLD_DEFAULT, "SSLSetEnabledCiphers");
    const uint16_t *in = P(1); size_t n = c->x[2];
    uint32_t *tmp = malloc((n ? n : 1) * sizeof(uint32_t));
    for (size_t k = 0; k < n; k++) tmp[k] = in[k];
    c->x[0] = (uint32_t)f(P(0), tmp, n);
    free(tmp);
}
H(h_SSLGetNegotiatedCipher) {
    int (*f)(void *, uint32_t *) = dlsym(RTLD_DEFAULT, "SSLGetNegotiatedCipher");
    uint32_t v = 0; int r = f(P(0), &v);
    if (r == 0) *(uint16_t *)P(1) = (uint16_t)v;
    c->x[0] = (uint32_t)r;
}

// ___chkstk_darwin probes the stack pages a big frame is about to use (x9 = size) and keeps every other register: Swift code that sizes a frame at run time calls it
// with live values in x0-x8 and goes on with them. The host's x86_64 function of that name takes its size in rax and leaves rax (8, from the call trampoline)
// in x0, which turned a pointer the caller still needed into 8 (a null-ish load in the Swift Decodable init(from:) of a node addon). The guest stacks are
// fully committed, so there is nothing to probe.
H(h_chkstk_darwin) { (void)c; }
// The CPU feature flags an arm64 program asks for (Qt refuses to start without neon and crc32, Rust and JavaScript engines look for crypto and atomics): the host is an
// x86_64 Mac, so it knows none of these names. The interpreter implements these features, so they are reported; every other name goes to the host.
H(h_sysctlbyname) {
    static const char *yes[] = {"hw.optional.neon", "hw.optional.neon_hpfp", "hw.optional.neon_fp16", "hw.optional.AdvSIMD", "hw.optional.AdvSIMD_HPFPCvt", "hw.optional.armv8_crc32",
        "hw.optional.armv8_1_atomics", "hw.optional.arm.AdvSIMD", "hw.optional.arm.AdvSIMD_HPFPCvt", "hw.optional.arm.FEAT_AES", "hw.optional.arm.FEAT_SHA1", "hw.optional.arm.FEAT_SHA256",
        "hw.optional.arm.FEAT_CRC32", "hw.optional.arm.FEAT_LSE", "hw.optional.arm.FEAT_FP16", NULL};
    const char *name = S(0);
    for (const char **y = yes; name && *y; y++)
        if (!strcmp(name, *y)) {
            size_t *lenp = (size_t *)c->x[3 - 1];   // oldlenp
            if (c->x[1] && lenp && *lenp >= 4) *(int *)c->x[1] = 1;
            if (lenp) *lenp = 4;
            RET(0);
            return;
        }
    host_call(c, HOSTFN("sysctlbyname"), false);
}
H(h_nsgetargv)    { c->x[0] = (uint64_t)&g_argv; }
H(h_nsgetargc)    { c->x[0] = (uint64_t)&g_argc; }
H(h_nsgetprogname){ c->x[0] = (uint64_t)&g_progname; }
H(h_getprogname)  { c->x[0] = (uint64_t)g_progname; }
H(h_nsgetexepath) {
    uint32_t *sz = P(1);
    uint32_t need = (uint32_t)strlen(g_exe_path) + 1;
    if (*sz < need) { *sz = need; RET(-1); return; }
    memcpy(P(0), g_exe_path, need);
    RET(0);
}

// Thread-local variables: every host thread gets its own block per image, initialised from that image's template.
// A descriptor is { thunk, key, offset-in-block }. The thunk is a few arm64 instructions in guest memory (tlv_thunk): when the descriptor's key (image index + 1) is set
// and this thread already has the image's block it returns block + offset without leaving the translated code; otherwise (first access of the descriptor, or of the thread)
// it falls through to this handler, which makes the block, fills in the key and answers.
H(h_DNSServiceProcessResult) { t_cb_stackargs++; host_call(c, HOSTFN("DNSServiceProcessResult"), false); t_cb_stackargs--; }
H(h_pthread_setspecific) {
    uint64_t key = c->x[0], val = c->x[1];
    host_call(c, HOSTFN("pthread_setspecific"), false);
    if (c->x[0] == 0 && key < TLV_BASE) t_tlv_tab[key] = val;
}
H(h_tlv_bootstrap) {
    uint64_t *desc = P(0);
    GImage *im = desc[1] > TLV_BASE && desc[1] - TLV_BASE <= (uint64_t)g_nimages ? g_images[desc[1] - TLV_BASE - 1] : image_by_addr((uint64_t)desc);
    if (!im) die(c, "thread-local descriptor outside the guest images", 0);
    int slot = TLV_BASE + im->index + 1;
    if (!t_tlv_tab[slot]) {
        uint8_t *blk = calloc(1, im->tls_size ? im->tls_size : 1);
        memcpy(blk, (void *)im->tls_start, im->tls_data_size);
        t_tlv_tab[slot] = (uint64_t)blk;
    }
    if (!desc[1] || desc[1] <= TLV_BASE) desc[1] = (uint64_t)slot;
    c->x[0] = t_tlv_tab[slot] + desc[2];
}
static uint64_t g_tlv_thunk;
static void own_add(uint64_t lo, uint64_t len);
static uint64_t stub_for(const char *name, const char *lib, bool weak);
static uint64_t tlv_thunk(void) {
    if (g_tlv_thunk) return g_tlv_thunk;
    uint64_t base = (g_next_free + 0x3fff) & ~0x3fffull;
    g_next_free = base + 0x8000;
    if (mmap((void *)base, 0x4000, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) == MAP_FAILED) { perror("rebar: thread-local thunk"); exit(1); }
    static const uint32_t code[10] = {
        0xf9400410,   // ldr x16, [x0, #8]            key
        0xd53bd071,   // mrs x17, tpidrro_el0         this thread's table
        0xf8707a31,   // ldr x17, [x17, x16, lsl #3]  the image's block
        0xb4000091,   // cbz x17, slow
        0xf9400810,   // ldr x16, [x0, #16]           offset
        0x8b100220,   // add x0, x17, x16
        0xd65f03c0,   // ret
        0x10000070,   // slow: adr x16, literal
        0xf9400210,   // ldr x16, [x16]
        0xd61f0200,   // br x16                        the import stub (x0 and lr are as they were)
    };
    memcpy((void *)base, code, sizeof code);
    uint64_t slow = stub_for("__tlv_bootstrap", NULL, false);
    memcpy((void *)(base + 40), &slow, 8);
    own_add(base, 0x4000);
    return g_tlv_thunk = base;
}

#include "objc.h"

H(h_Unwind_Resume);
H(h_Unwind_GetCFA); H(h_Unwind_SetGR); H(h_Unwind_SetIP); H(h_Unwind_GetIP); H(h_Unwind_GetGR);
H(h_dlopen); H(h_dlsym); H(h_dlclose); H(h_dladdr); H(h_dlerror);
H(h_dyld_image_count); H(h_dyld_get_image_name); H(h_dyld_get_image_header);
H(h_dyld_get_image_vmaddr_slide); H(h_dyld_register_func_for_add_image);
typedef struct { const char *name; StubHandler h; } Special;
static const Special k_special[] = {
    {"_printf", h_printf}, {"_fprintf", h_fprintf}, {"_sprintf", h_sprintf}, {"_snprintf", h_snprintf},
    {"_dprintf", h_dprintf}, {"_asprintf", h_asprintf}, {"_scanf", h_scanf}, {"_sscanf", h_sscanf}, {"_fscanf", h_fscanf},
    {"___printf_chk", h_printf_chk}, {"___fprintf_chk", h_fprintf_chk},
    {"___sprintf_chk", h_sprintf_chk}, {"___snprintf_chk", h_snprintf_chk},
    {"_vprintf", h_vprintf}, {"_vfprintf", h_vfprintf}, {"_vsprintf", h_vsprintf}, {"_vsnprintf", h_vsnprintf}, {"_swprintf", h_swprintf}, {"_vswprintf", h_vswprintf}, {"_wprintf", h_wprintf}, {"_vwprintf", h_vwprintf}, {"_fwprintf", h_fwprintf}, {"_vfwprintf", h_vfwprintf},
    {"_vdprintf", h_vdprintf}, {"_vasprintf", h_vasprintf}, {"_vsscanf", h_vsscanf}, {"_vfscanf", h_vfscanf},
    {"___vsnprintf_chk", h_vsnprintf_chk}, {"___vsprintf_chk", h_vsprintf_chk},
    {"_printf_l", h_printf_l}, {"_fprintf_l", h_fprintf_l}, {"_sprintf_l", h_sprintf_l}, {"_snprintf_l", h_snprintf_l},
    {"_dprintf_l", h_dprintf_l}, {"_asprintf_l", h_asprintf_l}, {"_scanf_l", h_scanf_l}, {"_sscanf_l", h_sscanf_l}, {"_fscanf_l", h_fscanf_l},
    {"_vprintf_l", h_vprintf_l}, {"_vfprintf_l", h_vfprintf_l}, {"_vsprintf_l", h_vsprintf_l}, {"_vsnprintf_l", h_vsnprintf_l},
    {"_vdprintf_l", h_vdprintf_l}, {"_vasprintf_l", h_vasprintf_l}, {"_vsscanf_l", h_vsscanf_l}, {"_vfscanf_l", h_vfscanf_l},
    {"_sigaction", h_sigaction}, {"_signal", h_signal}, {"_sigprocmask", h_sigprocmask}, {"_pthread_sigmask", h_pthread_sigmask}, {"_sigaltstack", h_sigaltstack},
    {"_task_set_exception_ports", h_set_exception_ports}, {"_thread_set_exception_ports", h_set_exception_ports},
    {"_host_set_exception_ports", h_set_exception_ports}, {"_task_swap_exception_ports", h_swap_exception_ports},
    {"_thread_swap_exception_ports", h_swap_exception_ports},
    {"_malloc_zone_register", h_malloc_zone_register}, {"_malloc_zone_unregister", h_malloc_zone_unregister},
    {"_malloc_get_all_zones", h_malloc_get_all_zones},
    {"_getpagesize", h_getpagesize}, {"_mincore", h_mincore}, {"_sysconf", h_sysconf}, {"_posix_spawn", h_posix_spawn}, {"_posix_spawnp", h_posix_spawnp},
    {"_execve", h_execve}, {"_execv", h_execv}, {"_execvp", h_execvp},
    {"_pthread_get_stackaddr_np", h_pthread_get_stackaddr_np}, {"_pthread_get_stacksize_np", h_pthread_get_stacksize_np}, {"_malloc_default_zone", h_malloc_default_zone},
    {"_malloc_default_purgeable_zone", h_malloc_default_purgeable_zone}, {"_malloc", h_malloc}, {"_calloc", h_calloc},
    {"_valloc", h_valloc}, {"_free", h_free}, {"_realloc", h_realloc}, {"_malloc_size", h_malloc_size}, {"_malloc_zone_malloc", h_malloc_zone_malloc},
    {"_malloc_zone_calloc", h_malloc_zone_calloc}, {"_malloc_zone_valloc", h_malloc_zone_valloc},
    {"_malloc_zone_free", h_malloc_zone_free}, {"_malloc_zone_realloc", h_malloc_zone_realloc},
    {"_mmap", h_mmap}, {"_sys_icache_invalidate", h_sys_icache_invalidate}, {"_thread_suspend", h_thread_suspend}, {"___chkstk_darwin", h_chkstk_darwin}, {"_sysctlbyname", h_sysctlbyname},
    {"_SSLGetEnabledCiphers", h_SSLGetEnabledCiphers}, {"_SSLGetSupportedCiphers", h_SSLGetSupportedCiphers}, {"_SSLSetEnabledCiphers", h_SSLSetEnabledCiphers}, {"_SSLGetNegotiatedCipher", h_SSLGetNegotiatedCipher}, {"_munmap", h_munmap}, {"_mprotect", h_mprotect}, {"_madvise", h_madvise},
    {"_vm_protect", h_vm_protect}, {"_mach_vm_protect", h_vm_protect}, {"_vm_allocate", h_vm_allocate},
    {"_mach_vm_allocate", h_vm_allocate}, {"_vm_deallocate", h_vm_deallocate}, {"_mach_vm_deallocate", h_vm_deallocate},
    {"_open", h_open}, {"_openat", h_openat}, {"_fcntl", h_fcntl}, {"_ioctl", h_ioctl}, {"_semctl", h_semctl}, {"_shm_open", h_shm_open}, {"_sem_open", h_sem_open}, {"_syslog", h_syslog}, {"_execl", h_execl}, {"_curl_easy_setopt", h_curl_easy_setopt}, {"_curl_easy_getinfo", h_curl_easy_getinfo}, {"_curl_multi_setopt", h_curl_multi_setopt}, {"_curl_share_setopt", h_curl_share_setopt}, {"_sqlite3_db_config", h_sqlite3_db_config}, {"_sqlite3_config", h_sqlite3_config}, {"_execlp", h_execlp}, {"_execle", h_execle},
    {"_open$NOCANCEL", h_open}, {"_openat$NOCANCEL", h_openat}, {"_fcntl$NOCANCEL", h_fcntl},   // (the mode / argument of these variadic functions is on the stack on arm64, not in x2 / x3: Bun, Zig and Rust call the $NOCANCEL forms directly)
    {"_exit", h_exit}, {"_atexit", h_atexit}, {"___cxa_atexit", h_cxa_atexit}, {"___cxa_finalize", h_nop},
    {"_qsort", h_qsort}, {"_qsort_r", h_qsort_r}, {"_bsearch", h_bsearch}, {"__ZNKSt3__14__fs10filesystem18directory_iterator13__dereferenceEv", h_dir_deref}, {"__ZNKSt3__14__fs10filesystem28recursive_directory_iterator13__dereferenceEv", h_rdir_deref}, {"_tsearch", h_tsearch}, {"_tfind", h_tfind}, {"_tdelete", h_tdelete}, {"_twalk", h_twalk}, {"_lfind", h_lfind}, {"_lsearch", h_lsearch},
    {"_setjmp", h_setjmp}, {"__setjmp", h_setjmp}, {"_sigsetjmp", h_setjmp},
    {"_longjmp", h_longjmp}, {"__longjmp", h_longjmp}, {"_siglongjmp", h_longjmp},
    {"__NSGetArgv", h_nsgetargv}, {"__NSGetArgc", h_nsgetargc}, {"__NSGetProgname", h_nsgetprogname},
    {"_getprogname", h_getprogname}, {"__NSGetExecutablePath", h_nsgetexepath},
    {"__tlv_bootstrap", h_tlv_bootstrap}, {"_pthread_setspecific", h_pthread_setspecific}, {"_swift_updateClassMetadata2", h_swift_updateClassMetadata2}, {"_swift_getInitializedObjCClass", h_swift_getInitializedObjCClass}, {"_DNSServiceProcessResult", h_DNSServiceProcessResult},
    {"_dlopen", h_dlopen}, {"_dlerror", h_dlerror}, {"_dlsym", h_dlsym}, {"_dlclose", h_dlclose}, {"_dladdr", h_dladdr},
    {"__dyld_image_count", h_dyld_image_count}, {"__dyld_get_image_name", h_dyld_get_image_name},
    {"__dyld_get_image_header", h_dyld_get_image_header}, {"__dyld_get_image_vmaddr_slide", h_dyld_get_image_vmaddr_slide},
    {"__dyld_register_func_for_add_image", h_dyld_register_func_for_add_image}, {"__Unwind_Resume", h_Unwind_Resume}, {"__Unwind_GetCFA", h_Unwind_GetCFA}, {"__Unwind_SetGR", h_Unwind_SetGR}, {"__Unwind_SetIP", h_Unwind_SetIP}, {"__Unwind_GetIP", h_Unwind_GetIP}, {"__Unwind_GetGR", h_Unwind_GetGR},
    {"_objc_msgSend", h_objc_msgSend}, {"_objc_msgSendSuper2", h_objc_msgSendSuper2},
    {"_CFRunLoopObserverCreate", h_CFRunLoopObserverCreate}, {"_CFRunLoopTimerCreate", h_CFRunLoopTimerCreate}, {"_CFRunLoopSourceCreate", h_CFRunLoopSourceCreate}, {"_CVDisplayLinkSetOutputCallback", h_CVDisplayLinkSetOutputCallback},
    {"_method_setImplementation", h_method_setImplementation}, {"_class_replaceMethod", h_class_replaceMethod}, {"_class_addMethod", h_class_addMethod},
    {"_method_getImplementation", h_method_getImplementation}, {"_class_getMethodImplementation", h_class_getMethodImplementation},
    {"_NSLog", h_NSLog}, {"_NSLogv", h_NSLogv},
    {"_CFStringCreateWithFormat", h_CFStringCreateWithFormat},
    {"_CFStringCreateWithFormatAndArguments", h_CFStringCreateWithFormatAndArguments},
    {NULL, NULL},
};

// ------------------------------------------------------------- fault handling

// Each host thread that can fault needs an alternate signal stack: when the guest has been running a
// while the thread's own stack may sit anywhere (guest-supplied stacks, guard pages), and a signal
// that cannot push its frame is turned into SIGILL by the kernel.
static __thread bool t_altstack;
static void ensure_altstack(void) {
    if (t_altstack) return;
    t_altstack = true;
    stack_t ss = {.ss_sp = alloc_guarded(1 << 17), .ss_size = 1 << 17, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    if (getenv("REBAR_IMAGES")) fprintf(stderr, "rebar: alt stack %p - %p\n", ss.ss_sp, (char *)ss.ss_sp + ss.ss_size);
}

#define r_rax(u) ((u)->uc_mcontext->__ss.__rax)
#define r_rbx(u) ((u)->uc_mcontext->__ss.__rbx)
#define r_rcx(u) ((u)->uc_mcontext->__ss.__rcx)
#define r_rdx(u) ((u)->uc_mcontext->__ss.__rdx)
#define r_rsi(u) ((u)->uc_mcontext->__ss.__rsi)
#define r_rdi(u) ((u)->uc_mcontext->__ss.__rdi)
#define r_r8(u) ((u)->uc_mcontext->__ss.__r8)
#define r_r9(u) ((u)->uc_mcontext->__ss.__r9)
#define r_r10(u) ((u)->uc_mcontext->__ss.__r10)
#define r_r11(u) ((u)->uc_mcontext->__ss.__r11)
#define r_r12(u) ((u)->uc_mcontext->__ss.__r12)
#define r_r13(u) ((u)->uc_mcontext->__ss.__r13)
#define r_r14(u) ((u)->uc_mcontext->__ss.__r14)
#define r_r15(u) ((u)->uc_mcontext->__ss.__r15)
// REBAR_ENTRYSTATS=<file>: how often host code jumps to a guest function (each costs a signal), by target; <file>.<pid> is rewritten every two seconds
static struct { uint64_t key, n, caller, arg0; } g_ent[8192];
static bool g_entry_stats;
static void ent_count(uint64_t rip, uint64_t caller, uint64_t arg0) {
    unsigned h = (unsigned)((rip * 0x9e3779b97f4a7c15ull) >> 51);
    for (int k = 0; k < 64; k++) {
        unsigned j = (h + k) & 8191; uint64_t cur = __atomic_load_n(&g_ent[j].key, __ATOMIC_RELAXED);
        if (!cur) { uint64_t z = 0; if (!__atomic_compare_exchange_n(&g_ent[j].key, &z, rip, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) cur = z; else cur = rip; }
        if (cur == rip) { __atomic_fetch_add(&g_ent[j].n, 1, __ATOMIC_RELAXED); g_ent[j].caller = caller; g_ent[j].arg0 = arg0; return; }
    }
}
#include <malloc/malloc.h>
static void *entstats_thread(void *arg) {
    for (;;) {
        sleep(2);
        char name[PATH_MAX]; snprintf(name, sizeof name, "%s.%d", (const char *)arg, (int)getpid());
        FILE *f = fopen(name, "w"); if (!f) continue;
        { malloc_zone_t *z = malloc_default_zone(); uint64_t *zz = (uint64_t *)z;
          fprintf(f, "defzone-clone %p purge-clone %p real-default %p slots:", (void *)g_def_zone.clone, (void *)g_purge_zone.clone, (void *)z);
          for (int q = 2; q < 12; q++) fprintf(f, " %llx", (unsigned long long)zz[q]);
          fprintf(f, "\n"); }
        for (int k = 0; k < 8192; k++) if (g_ent[k].key) {
            GImage *im = image_by_addr(g_ent[k].key);
            Dl_info di; const char *cn = dladdr((void *)g_ent[k].caller, &di) && di.dli_sname ? di.dli_sname : "?";
            fprintf(f, "%llx %llu arg0=%llx %s+0x%llx <- %s (%s)\n", (unsigned long long)g_ent[k].key, (unsigned long long)g_ent[k].n, (unsigned long long)g_ent[k].arg0, im && im->path ? im->path : "?", (unsigned long long)(im ? g_ent[k].key - im->base : 0), cn, dladdr((void *)g_ent[k].caller, &di) && di.dli_fname ? di.dli_fname : "?");
        }
        fclose(f);
    }
    return NULL;
}
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    if (sig == SIGILL || sig == SIGTRAP || sig == SIGFPE) {  // never an instruction-fetch bridge: report without touching any state
        { const JReg *jb = g_jit_on ? jit_block_of(uc->uc_mcontext->__ss.__rip) : NULL; if (jb) { char bb[160]; int nn = snprintf(bb, sizeof bb, "rebar: trap in translated code of the block at guest pc 0x%llx (%u instructions)\n", (unsigned long long)jb->pc, jb->n); write(2, bb, nn); } }
        char b[200];
        int n = snprintf(b, sizeof b, "rebar: signal %d code=%d at host pc %llx (si_addr %p), rsp %llx\n", sig, si->si_code,
                         (unsigned long long)uc->uc_mcontext->__ss.__rip, si->si_addr, (unsigned long long)uc->uc_mcontext->__ss.__rsp);
        write(2, b, n);
        if ((uint64_t)si->si_addr != uc->uc_mcontext->__ss.__rip) _exit(140 + sig);
    }
    if (!t_altstack) ensure_altstack();
    uint64_t rip = uc->uc_mcontext->__ss.__rip;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("REBAR_DEBUG_SIG") != NULL;
    if (dbg) { sigset_t cur; pthread_sigmask(SIG_BLOCK, NULL, &cur); char b[200]; int n = snprintf(b, sizeof b, "[sig %d code=%d si_addr=%p rip=%llx rsp=%llx mask=%x thr=%p]\n", sig, si->si_code, si->si_addr, (unsigned long long)rip, (unsigned long long)uc->uc_mcontext->__ss.__rsp, (unsigned)cur, (void *)pthread_self()); write(2, b, n); }
    if ((uint64_t)si->si_addr == rip) {
        if (rip - g_guest_lo < g_guest_size || guest_owned_code(rip)) {  // host code jumped to a guest function
            { uint64_t ra = *(uint64_t *)uc->uc_mcontext->__ss.__rsp; if (g_sigtramp_ra && ra - (g_sigtramp_ra - 0x80) < 0x100) t_sig_entry = (int)uc->uc_mcontext->__ss.__rdi; }   // called by the host's signal trampoline: a signal handler
            uc->uc_mcontext->__ss.__r11 = rip;
            uc->uc_mcontext->__ss.__rip = (uint64_t)rebar_guest_entry;
            if (g_entry_stats) {
                ent_count(rip, *(uint64_t *)uc->uc_mcontext->__ss.__rsp, uc->uc_mcontext->__ss.__rdi);
                static uint64_t want; static int shown; if (!want) { const char *w = getenv("REBAR_ENTRYSTATS_BT"); want = w ? strtoull(w, NULL, 16) : 1; }
                if (rip == want && shown < 6) {   // a few host stacks into that guest function (frame pointers)
                    shown++;
                    char buf[2000]; int n = snprintf(buf, sizeof buf, "ENTRY %llx rdi=%llx rsi=%llx sel=%s:", (unsigned long long)rip, (unsigned long long)uc->uc_mcontext->__ss.__rdi, (unsigned long long)uc->uc_mcontext->__ss.__rsi, sel_getName((SEL)uc->uc_mcontext->__ss.__rsi));
                    uint64_t *fp = (uint64_t *)uc->uc_mcontext->__ss.__rbp; uint64_t ra = *(uint64_t *)uc->uc_mcontext->__ss.__rsp;
                    for (int k = 0; k < 10 && n < 1800; k++) {
                        Dl_info di; const char *nm = dladdr((void *)ra, &di) && di.dli_sname ? di.dli_sname : "?";
                        n += snprintf(buf + n, sizeof buf - n, " <- %s", nm);
                        if (!fp || (uint64_t)fp < 0x10000) break;
                        ra = fp[1]; fp = (uint64_t *)fp[0];
                    }
                    buf[n++] = 10; write(2, buf, n);
                }
            }
            return;
        }
        if (rip - g_stub_base < g_stub_size) {  // host code jumped to an imported symbol's stub
            int idx = (int)((rip - g_stub_base) / 16);
            if (idx >= 2 && idx < g_nstubs && g_stubs[idx].host && !g_stubs[idx].handler) {
                uc->uc_mcontext->__ss.__rip = (uint64_t)g_stubs[idx].host;  // arguments are already x86
                return;
            }
            uc->uc_mcontext->__ss.__r11 = rip;
            uc->uc_mcontext->__ss.__rip = (uint64_t)rebar_guest_entry;
            return;
        }
    }
    { const char *lf = getenv("REBAR_FAULT_LOG"); if (lf) { int fd = open(lf, O_WRONLY | O_APPEND | O_CREAT, 0644); if (fd >= 0) { char tag[64]; int n = snprintf(tag, sizeof tag, "=== pid %d\n", getpid()); write(fd, tag, n); dup2(fd, 2); } } }  // REBAR_FAULT_LOG=<file>: fault reports of helper processes whose stderr is lost
    { const JReg *jb = g_jit_on ? jit_block_of(rip) : NULL;
      if (jb) {
          fprintf(stderr, "rebar: the fault is in translated code of the block at guest pc 0x%llx (%u instructions), %u bytes into it\n", (unsigned long long)jb->pc, jb->n, (unsigned)(rip - (uint64_t)jb->code));
          if (t_cpu) { fprintf(stderr, "  guest registers:"); for (int k = 0; k < 31; k++) fprintf(stderr, " x%d=%llx", k, (unsigned long long)t_cpu->x[k]); fprintf(stderr, " sp=%llx\n", (unsigned long long)t_cpu->sp); }
          fprintf(stderr, "  host code at the fault:"); for (int k = -16; k < 16; k++) fprintf(stderr, " %02x", ((uint8_t *)rip)[k]); fprintf(stderr, "\n  block code:"); for (uint32_t k = 0; k < jb->len && k < 400; k++) fprintf(stderr, "%s%02x", k % 32 ? " " : "\n   ", jb->code[k]); fprintf(stderr, "\n");
          fprintf(stderr, "  host registers: rax=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r15=%llx rsp=%llx\n", (unsigned long long)uc->uc_mcontext->__ss.__rax, (unsigned long long)uc->uc_mcontext->__ss.__rcx, (unsigned long long)uc->uc_mcontext->__ss.__rdx, (unsigned long long)uc->uc_mcontext->__ss.__rsi, (unsigned long long)uc->uc_mcontext->__ss.__rdi, (unsigned long long)uc->uc_mcontext->__ss.__r8, (unsigned long long)uc->uc_mcontext->__ss.__r9, (unsigned long long)uc->uc_mcontext->__ss.__r15, (unsigned long long)uc->uc_mcontext->__ss.__rsp);
      } }
    fprintf(stderr, "rebar: fault (signal %d) addr %p, host pc %p, last guest pc 0x%llx, last import %s called from 0x%llx\n",
            sig, si->si_addr, (void *)rip, (unsigned long long)(t_cpu ? t_cpu->pc : 0), t_last_stub ? t_last_stub->name : "-", (unsigned long long)t_last_lr);
    { const _STRUCT_X86_THREAD_STATE64 *r = &uc->uc_mcontext->__ss; fprintf(stderr, "  host registers: rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx rsp=%llx rbp=%llx\n",
        (unsigned long long)r->__rax, (unsigned long long)r->__rbx, (unsigned long long)r->__rcx, (unsigned long long)r->__rdx, (unsigned long long)r->__rsi, (unsigned long long)r->__rdi, (unsigned long long)r->__r8, (unsigned long long)r->__r9,
        (unsigned long long)r->__r10, (unsigned long long)r->__r11, (unsigned long long)r->__r12, (unsigned long long)r->__r13, (unsigned long long)r->__r14, (unsigned long long)r->__r15, (unsigned long long)r->__rsp, (unsigned long long)r->__rbp); }   // (the interpreter keeps the guest pc in one of them)
    { const uint64_t hv[] = {r_rax(uc), r_rbx(uc), r_rcx(uc), r_rdx(uc), r_rsi(uc), r_rdi(uc), r_r8(uc), r_r9(uc), r_r10(uc), r_r11(uc), r_r12(uc), r_r13(uc), r_r14(uc), r_r15(uc)};
      static const char *hn[] = {"rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
      for (int k = 0; k < 14; k++) { GImage *gi = image_by_addr(hv[k]); if (gi && gi->path) fprintf(stderr, "  %s = %llx is in %s +0x%llx\n", hn[k], (unsigned long long)hv[k], gi->path, (unsigned long long)(hv[k] - gi->base)); } }
    if (t_cpu) { fprintf(stderr, "  guest registers (pc %llx):", (unsigned long long)t_cpu->pc); for (int k = 0; k < 31; k++) fprintf(stderr, " x%d=%llx", k, (unsigned long long)t_cpu->x[k]); fprintf(stderr, " sp=%llx\n", (unsigned long long)t_cpu->sp); }
    if (t_ring_n) { const RingEnt *e = &t_ring[(t_ring_n - 1) & 127]; fprintf(stderr, "  last import: %s(0x%llx, 0x%llx, 0x%llx)\n", e->name, (unsigned long long)e->x0, (unsigned long long)e->x1, (unsigned long long)e->x2); }
    dump_recent_imports();
    {   // what the kernel says about the page that was touched
        mach_vm_address_t ra = (mach_vm_address_t)si->si_addr;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t bi;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t on = 0;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &on) == KERN_SUCCESS)
            fprintf(stderr, "  page: %s region 0x%llx-0x%llx prot %d/%d%s%s\n", (uint64_t)si->si_addr >= ra ? "in" : "below",
                    (unsigned long long)ra, (unsigned long long)(ra + rs), bi.protection, bi.max_protection, bi.shared ? " shared" : "", bi.reserved ? " reserved" : "");
        else fprintf(stderr, "  page: not mapped\n");
    }
    {   // host frames of the faulting thread, from the interrupted context's frame pointer chain
        uint64_t fp = uc->uc_mcontext->__ss.__rbp;
        for (int k = 0; k < 24 && fp && !(fp & 7); k++) {
            uint64_t *f = (uint64_t *)fp;
            Dl_info di;
            if (!f[1]) break;
            if (dladdr((void *)f[1], &di) && di.dli_sname)
                fprintf(stderr, "  host #%d %p %s+%ld (%s)\n", k, (void *)f[1], di.dli_sname, (long)((char *)f[1] - (char *)di.dli_saddr),
                        di.dli_fname ? strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname : "?");
            else fprintf(stderr, "  host #%d %p\n", k, (void *)f[1]);
            if (f[0] <= fp) break;
            fp = f[0];
        }
    }
    _exit(sig == SIGABRT ? 134 : 139);
}

// ---------------------------------------------------------------------- stubs

// ------------------------------------------------- libc++ std::string across the ABI boundary
//
// arm64 libc++ uses the "alternate" std::string layout, the x86_64 libc++ the host runs does not:
//   arm64   long {pointer, size, allocation | 1<<63}   short {chars (up to 22 + NUL) from byte 0, size in byte 23}
//   x86_64  long {allocation | 1, size, pointer}       short {size << 1, chars from byte 1}
// The guest keeps its strings in the arm64 layout (its inline code reads them), so a call into the host's libc++
// that takes or returns one gets host-layout copies of the strings on the C stack, and whatever the callee left
// in them is converted back afterwards. Which arguments are strings comes from the demangled import name
// (str_spec_for, once per import). std::filesystem::path is a std::string as far as layout goes.
static const char k_strT[] = "std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>";
static const char k_wstrT[] = "std::__1::basic_string<wchar_t, std::__1::char_traits<wchar_t>, std::__1::allocator<wchar_t>>";
static const char k_pathT[] = "std::__1::__fs::filesystem::path";
static char *(*p_demangle)(const char *, char *, size_t *, int *);

static void str_to_host(uint8_t *d, const uint8_t *g) {
    if (g[23] & 0x80) {
        uint64_t w[3];
        memcpy(w, g, 24);
        uint64_t o[3] = {(w[2] & ~(1ull << 63)) | 1, w[1], w[0]};
        memcpy(d, o, 24);
    } else {
        unsigned n = g[23] & 0x7f;
        if (n > 22) n = 22;
        memset(d, 0, 24);
        d[0] = (uint8_t)(n << 1);
        memcpy(d + 1, g, n);
    }
}

static void str_to_guest(uint8_t *g, const uint8_t *d) {
    if (d[0] & 1) {
        uint64_t w[3];
        memcpy(w, d, 24);
        uint64_t o[3] = {w[2], w[1], (w[0] & ~1ull) | (1ull << 63)};
        memcpy(g, o, 24);
    } else {
        unsigned n = d[0] >> 1;
        if (n > 22) n = 22;
        uint8_t o[24] = {0};
        memcpy(o, d + 1, n);
        o[23] = (uint8_t)n;
        memcpy(g, o, 24);
    }
}

// std::wstring: the long form is the same as std::string's; the short form holds up to 4 wchar_t (arm64: characters from byte 0 and the size in byte 23, x86_64: the size << 1 in
// the first 4 bytes and the characters from byte 4).
static void wstr_to_host(uint8_t *d, const uint8_t *g) {
    if (g[23] & 0x80) { str_to_host(d, g); return; }
    unsigned n = g[23] & 0x7f;
    if (n > 4) n = 4;
    memset(d, 0, 24);
    d[0] = (uint8_t)(n << 1);
    memcpy(d + 4, g, n * 4);
}
static void wstr_to_guest(uint8_t *g, const uint8_t *d) {
    if (d[0] & 1) { str_to_guest(g, d); return; }
    unsigned n = d[0] >> 1;
    if (n > 4) n = 4;
    uint8_t o[24] = {0};
    memcpy(o, d + 4, n * 4);
    o[23] = (uint8_t)n;
    memcpy(g, o, 24);
}

static size_t str_type_len(const char *p, bool *wide) {  // length of a "std::string" / "std::wstring" / "std::filesystem::path" type name at p, else 0
    if (wide) *wide = false;
    if (!strncmp(p, k_strT, sizeof k_strT - 1)) return sizeof k_strT - 1;
    if (!strncmp(p, k_wstrT, sizeof k_wstrT - 1)) { if (wide) *wide = true; return sizeof k_wstrT - 1; }
    if (!strncmp(p, k_pathT, sizeof k_pathT - 1)) return sizeof k_pathT - 1;
    return 0;
}

// Itanium mangling gives the return type of function templates only; for the rest, the ones that return a string.
static bool returns_string_by_value(const char *n) {
    if (strstr(n, "9to_stringE") || strstr(n, "6locale4nameEv") || strstr(n, "10error_code7messageEv")) return true;
    if (strstr(n, "12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6substrE")) return true;
    if ((strstr(n, "stringbufI") || strstr(n, "stringstreamI")) && strstr(n, "3strEv")) return true;
    if (strstr(n, "10filesystem")) {
        // (path's own __filename/__stem/... return a string_view in rax:rdx / x0:x1, which needs no conversion)
        static const char *fns[] = {"__absolute", "__canonical", "__read_symlink", "__temp_directory_path", "__weakly_canonical",
                                    "lexically_normal", "lexically_relative", NULL};
        for (const char **f = fns; *f; f++) if (strstr(n, *f)) return true;
        if (strstr(n, "14__current_pathEPNS_10error_codeE")) return true;   // current_path() (the setter, __current_path(const path&, error_code&), returns nothing)
    }
    return false;
}

static char *g_spec_note;  // --string-specs: what str_spec_for decided about the qualifier
static StrSpec *str_spec_for(const char *name) {
    if (strncmp(name, "__Z", 3)) return NULL;
    if (!strstr(name, "12basic_string") && !strstr(name, "10filesystem") && !strstr(name, "stringbuf") && !strstr(name, "stringstream") &&
        !strstr(name, "9to_stringE") && !strstr(name, "10to_wstringE") && !strstr(name, "6locale4nameEv") && !strstr(name, "10error_code7messageEv")) return NULL;
    if (!p_demangle) {
        p_demangle = HOSTFN("__cxa_demangle");
        if (!p_demangle) { void *h = dlopen("/usr/lib/libc++abi.dylib", RTLD_NOW); if (h) p_demangle = dlsym(h, "__cxa_demangle"); }
        if (!p_demangle) return NULL;
    }
    int st = 0;
    char *d = p_demangle(name + 1, NULL, NULL, &st);
    if (!d || st) { free(d); return NULL; }
    for (char *q = d; *q; q++) if (q[0] == '>' && q[1] == ' ' && q[2] == '>') memmove(q + 1, q + 2, strlen(q + 2) + 1);
    StrSpec sp = {0};
    char *rp = strrchr(d, ')'), *lp = rp;
    if (!rp) { free(d); return NULL; }
    for (int depth = 0; lp >= d; lp--) {
        if (*lp == ')') depth++;
        else if (*lp == '(' && --depth == 0) break;
    }
    if (lp < d) { free(d); return NULL; }
    bool const_fn = strstr(rp, "const") != NULL;
    *rp = 0;
    *lp = 0;
    int argi = 0;
    // d is "[return type ]qualified::name[<targs>]": fn is the qualified name, sep its last "::"
    char *fn = d, *sep = NULL;
    {
        int dp = 0;
        for (char *q = d; *q; q++) {
            if (dp == 0 && !strncmp(q, "operator", 8)) { q += 8; while (*q && strchr("<>=!+-*/%&|^~", *q)) q++; q--; continue; }
            if (*q == '<' || *q == '(') dp++;
            else if (*q == '>' || *q == ')') dp--;
            else if (dp == 0 && !strncmp(q, "std::", 5) && (q == d || q[-1] == ' ')) { fn = q; sep = NULL; }
            else if (dp == 0 && q[0] == ':' && q[1] == ':') { sep = q; q++; }
        }
    }
    bool wide0 = false;
    size_t tl = str_type_len(fn, &wide0);
    if (tl && fn[tl] == ':' && fn + tl == sep) {  // a member of std::string / std::wstring / path: this is a string
        const char *cls = (tl == sizeof k_strT - 1 || tl == sizeof k_wstrT - 1) ? "basic_string" : "path", *m = fn + tl + 2;
        bool dtor = m[0] == '~', ctor = !strcmp(m, cls);
        sp.mask |= 1; if (wide0) sp.wide |= 1;
        if (ctor || !strncmp(m, "__init", 6)) sp.uninit |= 1;
        if (!const_fn && !dtor) sp.wb |= 1;
        sp.dtor = dtor;
        argi = 1;
    } else if (sep) {  // a member of some other class takes this in x0, unless it is a namespace or a static function
        size_t ql = (size_t)(sep - fn);
        static const char *spaces[] = {"std", "std::__1", "std::__1::__fs", "std::__1::__fs::filesystem", "std::__1::chrono", NULL};
        bool ns = false;
        for (const char **n = spaces; *n; n++) if (strlen(*n) == ql && !strncmp(fn, *n, ql)) ns = true;
        if (!ns && !strstr(fn, "locale::global") && !strstr(fn, "sync_with_stdio")) argi = 1;
        if (g_spec_note) snprintf(g_spec_note, 400, "%s qualifier: %.*s", argi ? "this" : "no this,", (int)ql, fn);
    }
    {   // a member of std::stringbuf (other than a constructor/destructor) works on the string inside the object
        static const char sbT[] = "std::__1::basic_stringbuf<char, std::__1::char_traits<char>, std::__1::allocator<char>>::";
        if (sep && !strncmp(fn, sbT, sizeof sbT - 1) && fn + sizeof sbT - 3 == sep && strcmp(sep + 2, "basic_stringbuf") && sep[2] != '~')
            sp.sb = true;
    }
    bool retw = false;
    size_t rl = str_type_len(d, &retw);
    if (rl && d[rl] == ' ' && !strncmp(d + rl + 1, "std::", 5)) { sp.sret = true; sp.sretw = retw; }  // a function template returning a string
    bool bad = false;
    char *tok = lp + 1;
    int depth = 0;
    for (char *q = tok;; q++) {
        if (*q == '<' || *q == '(' || *q == '[') depth++;
        else if (*q == '>' || *q == ')' || *q == ']') depth--;
        if (!(depth == 0 && *q == ',') && *q) continue;
        bool last = !*q;
        *q = 0;
        while (*tok == ' ') tok++;
        bool argw = false;
        size_t pl = str_type_len(tok, &argw);
        const char *suf = tok + pl;
        while (*suf == ' ') suf++;
        if (!*tok || !strcmp(tok, "void")) {}
        else if (pl && (!*suf || *suf == '&' || *suf == '*' || !strncmp(suf, "const&", 6) || !strncmp(suf, "const*", 6))) {
            bool cref = !strncmp(suf, "const&", 6) || !strncmp(suf, "const*", 6);
            if (argi < 8) { sp.mask |= 1 << argi; if (argw) sp.wide |= 1 << argi; if (!cref) sp.wb |= 1 << argi; }
            argi++;
        } else if (!strcmp(tok, "float") || !strcmp(tok, "double")) {  // a vector register, not an integer one
        } else if (!strcmp(tok, "long double") || !strcmp(tok, "...")) { bad = true; break;
        } else if (!strpbrk(tok, "&*") && (!strncmp(tok, "std::__1::basic_string_view<", 28) || !strcmp(tok, "std::__1::error_code") ||
                                           !strcmp(tok, "std::__1::error_condition"))) argi += 2;  // two-register structs
        else argi++;
        if (last) break;
        tok = q + 1;
    }
    free(d);
    if (!sp.sret) sp.sret = returns_string_by_value(name);
    if (strstr(name, "10to_wstringE")) { sp.sret = true; sp.sretw = true; }   // std::to_wstring returns a std::wstring
    if (bad || (!sp.mask && !sp.sret && !sp.sb)) return NULL;
    StrSpec *r = malloc(sizeof *r);
    *r = sp;
    return r;
}

// ---- std::stringbuf
// A stringbuf holds a std::string that the guest's inline code (constructor, destructor) and the host's code
// (overflow, underflow, str(), ... most of them reached through the vtable by the host's own ostream/istream code)
// both work on. Pointer arguments cannot be converted for the latter, so the vtable that guest-built stringbufs
// get is a proxy: its stringbuf entries are stubs that convert the object to the host layout (the string, and the
// get/put pointers that point into its inline buffer), run the real function and convert it back.
// Everything else sees the object in the guest layout.
#define SB_STR 64  // offset of the std::string in a basic_stringbuf<char> (after the 64 bytes of basic_streambuf)
static const int k_sb_ptrs[] = {16, 24, 32, 40, 48, 56, 88};  // eback gptr egptr pbase pptr epptr __hm_
static uint64_t *g_sb_vt;
static __thread uint8_t *t_sb_host[4];  // stringbufs of this thread that are in the host layout right now
static __thread int t_sb_n;
static int stub_new(const char *name, const char *lib, void *host, StubHandler h);

static bool sb_is_proxy(const uint8_t *sb) { return g_sb_vt && *(const uint64_t *)sb == (uint64_t)(g_sb_vt + 2); }
static bool sb_in_host_layout(const uint8_t *sb) {
    for (int k = 0; k < t_sb_n; k++) if (t_sb_host[k] == sb) return true;
    return false;
}
static void sb_shift(uint8_t *sb, uint64_t lo, uint64_t hi, int delta) {
    for (int k = 0; k < 7; k++) {
        uint64_t *p = (uint64_t *)(sb + k_sb_ptrs[k]);
        if (*p >= lo && *p <= hi) *p += delta;
    }
}
static void sb_to_host(uint8_t *sb) {
    uint8_t *s = sb + SB_STR, tmp[24];
    bool is_short = !(s[23] & 0x80);
    str_to_host(tmp, s);
    memcpy(s, tmp, 24);
    if (is_short) sb_shift(sb, (uint64_t)s, (uint64_t)s + 23, +1);
}
static void sb_to_guest(uint8_t *sb) {
    uint8_t *s = sb + SB_STR, tmp[24];
    bool is_short = !(s[0] & 1);
    str_to_guest(tmp, s);
    memcpy(s, tmp, 24);
    if (is_short) sb_shift(sb, (uint64_t)s + 1, (uint64_t)s + 24, -1);
}
static bool sb_enter(uint8_t *sb) {  // true if the caller must convert back
    if (!sb_is_proxy(sb) || sb_in_host_layout(sb)) return false;
    sb_to_host(sb);
    if (t_sb_n < 4) t_sb_host[t_sb_n++] = sb;
    return true;
}
static void sb_leave(uint8_t *sb) {
    if (t_sb_n) t_sb_n--;
    sb_to_guest(sb);
}
static void h_sb_vcall(CPU *c) {
    Stub *s = t_last_stub;
    uint8_t *sb = (uint8_t *)c->x[0];
    bool conv = sb_enter(sb);
    t_stk = NULL;
    host_call(c, s->host, false);
    if (conv) sb_leave(sb);
}
static void h_sb_vdtor(CPU *c) {  // destructors: the object is gone afterwards
    Stub *s = t_last_stub;
    sb_enter((uint8_t *)c->x[0]);
    t_stk = NULL;
    host_call(c, s->host, false);
    if (t_sb_n) t_sb_n--;
}
static uint64_t sb_proxy_vtable(void) {
    if (g_sb_vt) return (uint64_t)g_sb_vt;
    uint64_t *real = HOSTFN("_ZTVNSt3__115basic_stringbufIcNS_11char_traitsIcEENS_9allocatorIcEEEE");
    if (!real) return 0;
    uint64_t *vt = calloc(16, 8);
    memcpy(vt, real, 16 * 8);
    for (int k = 2; k < 16; k++) {  // the entries that are stringbuf's own and so read its string
        Dl_info di;
        if (!dladdr((void *)real[k], &di) || !di.dli_sname || !strstr(di.dli_sname, "basic_stringbuf")) continue;
        char nm[64];
        snprintf(nm, sizeof nm, "<stringbuf vtable %d>", k - 2);
        vt[k] = stub_addr(stub_new(nm, NULL, (void *)real[k], k < 4 ? h_sb_vdtor : h_sb_vcall));
    }
    g_sb_vt = vt;
    return (uint64_t)vt;
}

static void string_call(CPU *c, Stub *s) {
    const StrSpec *sp = s->str;
    uint64_t gx[9];
    memcpy(gx, c->x, sizeof gx);
    uint8_t tmp[8][24], rtmp[24] = {0};
    int own[8] = {0};
    uint8_t wb = sp->wb;
    for (int k = 0; k < 8; k++) {
        if (!(sp->mask >> k & 1)) continue;
        own[k] = k;
        for (int j = 0; j < k; j++)  // one guest object, one copy (s.append(s))
            if ((sp->mask >> j & 1) && own[j] == j && gx[j] == gx[k]) { own[k] = j; if (sp->wb >> k & 1) wb |= 1 << j; break; }
        if (own[k] == k) {
            if (sp->uninit >> k & 1) memset(tmp[k], 0, 24);
            else if (sp->wide >> k & 1) wstr_to_host(tmp[k], (const uint8_t *)gx[k]);
            else str_to_host(tmp[k], (const uint8_t *)gx[k]);
        }
        c->x[k] = (uint64_t)tmp[own[k]];
    }
    bool sbc = sp->sb && sb_enter((uint8_t *)gx[0]);
    if (sp->sret) c->x[8] = (uint64_t)rtmp;
    t_stk = s->stk;
    host_call(c, s->host, s->sret || sp->sret);
    t_stk = NULL;
    if (sbc) sb_leave((uint8_t *)gx[0]);
    if (t_landed) return;  // an exception went to a guest handler: the guest's strings stay as they were
    uint64_t r = c->x[0];
    for (int k = 0; k < 8; k++) {
        if (!(sp->mask >> k & 1) || own[k] != k) continue;
        bool wide = sp->wide >> k & 1;
        if (wb >> k & 1) { if (wide) wstr_to_guest((uint8_t *)gx[k], tmp[k]); else str_to_guest((uint8_t *)gx[k], tmp[k]); }
        uint64_t t = (uint64_t)tmp[k];
        if (r == t) r = gx[k];  // append/assign/... return *this
        else if (r > t && r < t + 24 && !(tmp[k][0] & 1)) r = gx[k] + (r - t - (wide ? 4 : 1));  // pointer into the inline characters
    }
    if (sp->sret) {
        if (sp->sretw) wstr_to_guest((uint8_t *)gx[8], rtmp); else str_to_guest((uint8_t *)gx[8], rtmp);
        r = gx[8];
    }
    for (int k = 2; k < 9; k++) c->x[k] = gx[k];  // (x1 may be the second half of a result, as in a string_view)
    c->x[0] = r;
}

static inline void apply_landing(CPU *c) {
    if (t_landed) { t_landed = false; c->x[0] = t_pend_x0; c->x[1] = t_pend_x1; }
}

static void stub_call_inner(CPU *c);
static bool trace_stub_wanted(const char *name);
// Handlers share the bridge's data and run one at a time; imports that only forward to a host function do not take the lock.
// REBAR_STUBPROF=<file>: calls and time (TSC ticks, GIL waits included) per import, written to <file>.<pid> every 2 s
static const char *g_stubprof;
static const char *g_sp_caller;   // REBAR_STUBPROF_CALLER=<import name>: who calls it (guest return address) and from which thread, written to <file>.<pid>.callers
static struct { uint64_t key, calls, ticks; } g_sp_c[4096];
static void sp_caller_note(uint64_t lr, uint64_t ticks, int idx) {
    uint64_t key = lr | ((uint64_t)idx << 48) | (pthread_main_np() ? 1ull << 63 : 0);
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 52);
    for (int k = 0; k < 64; k++) {
        unsigned i = (h + k) & 4095; uint64_t cur = __atomic_load_n(&g_sp_c[i].key, __ATOMIC_RELAXED);
        if (cur == 0) { uint64_t z = 0; if (!__atomic_compare_exchange_n(&g_sp_c[i].key, &z, key, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { k--; continue; } cur = key; }
        if (cur == key) { __atomic_fetch_add(&g_sp_c[i].calls, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_sp_c[i].ticks, ticks, __ATOMIC_RELAXED); return; }
    }
}
static struct { uint64_t calls, ticks, gilwait; } g_sp[MAX_STUBS];
static void *stubprof_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(2);
        char name[PATH_MAX], tmp[PATH_MAX]; snprintf(name, sizeof name, "%s.%d", g_stubprof, (int)getpid()); snprintf(tmp, sizeof tmp, "%s.tmp", name);
        FILE *f = fopen(tmp, "w"); if (!f) continue;
        for (int k = 0; k < g_nstubs; k++) if (g_sp[k].calls) fprintf(f, "%s %llu %llu %llu\n", g_stubs[k].name, (unsigned long long)g_sp[k].calls, (unsigned long long)g_sp[k].ticks, (unsigned long long)g_sp[k].gilwait);
        fclose(f); rename(tmp, name);
        if (g_sp_caller) {
            char cn[PATH_MAX], ct[PATH_MAX]; snprintf(cn, sizeof cn, "%s.callers", name); snprintf(ct, sizeof ct, "%s.tmp", cn);
            FILE *cf = fopen(ct, "w");
            if (cf) { for (int k = 0; k < 4096; k++) if (g_sp_c[k].key) fprintf(cf, "%llx %d %llu %llu %s\n", (unsigned long long)(g_sp_c[k].key & 0xffffffffffffull), (int)(g_sp_c[k].key >> 63), (unsigned long long)g_sp_c[k].calls, (unsigned long long)g_sp_c[k].ticks, g_stubs[(g_sp_c[k].key >> 48) & 0x7fff].name); fclose(cf); rename(ct, cn); }
        }
    }
    return NULL;
}
static void stubprof_start(const char *file) { g_stubprof = file; pthread_t th; pthread_create(&th, NULL, stubprof_thread, NULL); pthread_detach(th); }
static struct timespec g_slow_t0;
static int g_slow_ms;   // REBAR_SLOW_STUB=<ms>: log every import call that takes longer (name, arguments, caller, thread, when)
static void slow_stub_log(CPU *c, uint64_t off, uint64_t lr0, double ms) {
    if (off >= g_stub_size || off / 16 >= (uint64_t)g_nstubs) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[slow %.0f ms at %.1fs] %s(0x%llx, 0x%llx, 0x%llx) from guest 0x%llx thread %p\n", ms, (double)(ts.tv_sec - g_slow_t0.tv_sec) + (double)(ts.tv_nsec - g_slow_t0.tv_nsec) / 1e9, g_stubs[off / 16].name,
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)lr0, (void *)pthread_self());
}
static void stub_call(CPU *c) {
    uint64_t off = c->pc - g_stub_base;
    if (__builtin_expect(g_slow_ms > 0, 0) && !g_stubprof) {
        struct timespec a, b; uint64_t lr0 = c->x[30], x0 = c->x[0], x1 = c->x[1], x2 = c->x[2];
        bool lock0 = off < g_stub_size && off / 16 >= 2 && off / 16 < (uint64_t)g_nstubs && g_stubs[off / 16].handler && g_stubs[off / 16].handler != h_chkstk_darwin, had0 = t_have_gil;
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (lock0 && !had0) gil_acquire();
        stub_call_inner(c);
        if (!had0 && t_have_gil) gil_release();
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
        if (ms >= g_slow_ms) { uint64_t sv[3] = {c->x[0], c->x[1], c->x[2]}; c->x[0] = x0; c->x[1] = x1; c->x[2] = x2; slow_stub_log(c, off, lr0, ms); c->x[0] = sv[0]; c->x[1] = sv[1]; c->x[2] = sv[2]; }
        return;
    }
    bool lock = off < g_stub_size && off / 16 >= 2 && off / 16 < (uint64_t)g_nstubs && g_stubs[off / 16].handler && g_stubs[off / 16].handler != h_chkstk_darwin;   // (the stack probe does nothing: taking the bridge lock for it made 1.4 million calls queue up in Blender's renderer threads)
    bool had = t_have_gil;
    if (__builtin_expect(g_stubprof != NULL, 0)) {
        uint64_t t0 = __builtin_ia32_rdtsc(), lr0 = c->x[30];
        if (lock && !had) gil_acquire();
        uint64_t t1 = __builtin_ia32_rdtsc();
        stub_call_inner(c);
        uint64_t t2 = __builtin_ia32_rdtsc();
        if (!had && t_have_gil) gil_release();
        if (g_sp_caller && off < g_stub_size && off / 16 < (uint64_t)g_nstubs) { char nm[80]; snprintf(nm, sizeof nm, ",%s,", g_stubs[off / 16].name); char lst[400]; snprintf(lst, sizeof lst, ",%s,", g_sp_caller); if (strstr(lst, nm)) sp_caller_note(lr0, t2 - t0, (int)(off / 16)); }
        if (off < g_stub_size && off / 16 < (uint64_t)g_nstubs) { int k = (int)(off / 16); __atomic_fetch_add(&g_sp[k].calls, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_sp[k].ticks, t2 - t0, __ATOMIC_RELAXED); __atomic_fetch_add(&g_sp[k].gilwait, t1 - t0, __ATOMIC_RELAXED); }
        return;
    }
    if (lock && !had) gil_acquire();
    stub_call_inner(c);
    if (!had && t_have_gil) gil_release();  // (also when an exception that arrived from the host took the lock meanwhile)
}
// The host's seekoff / seekpos of a stream buffer (reached through the vtable of a buffer the host created, such as an ofstream's filebuf: tellp() ends there) return a 136-byte
// std::fpos by value and seekpos takes one; the type encodings let forward_enc do the sret and by-value conversions.
static const char *host_fn_enc(const char *sym) {
    static const struct { const char *name, *enc; } t[] = {
        {"_ZNSt3__113basic_filebufIcNS_11char_traitsIcEEE7seekoffExNS_8ios_base7seekdirEj", "{fpos=q[16q]}^vqiI"},
        {"_ZNSt3__113basic_filebufIcNS_11char_traitsIcEEE7seekposENS_4fposI11__mbstate_tEEj", "{fpos=q[16q]}^v{fpos=q[16q]}I"},
        {"_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekoffExNS_8ios_base7seekdirEj", "{fpos=q[16q]}^vqiI"},
        {"_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE7seekposENS_4fposI11__mbstate_tEEj", "{fpos=q[16q]}^v{fpos=q[16q]}I"},
        {"_ZNSt3__115basic_stringbufIcNS_11char_traitsIcEENS_9allocatorIcEEE7seekoffExNS_8ios_base7seekdirEj", "{fpos=q[16q]}^vqiI"},
        {"_ZNSt3__115basic_stringbufIcNS_11char_traitsIcEENS_9allocatorIcEEE7seekposENS_4fposI11__mbstate_tEEj", "{fpos=q[16q]}^v{fpos=q[16q]}I"},
    };
    if (!sym) return NULL;
    for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) if (!strcmp(sym, t[k].name)) return t[k].enc;
    return NULL;
}
static void str_to_guest(uint8_t *g, const uint8_t *d);
static bool host_fn_returns_string(const char *sym) {   // (char only: the wide strings have their own layout conversion)
    if (strncmp(sym, "_ZNKSt3__1", 10)) return false;
    if (strstr(sym, "numpunct") && strstr(sym, "IcE")) return strstr(sym, "11do_grouping") || strstr(sym, "11do_truename") || strstr(sym, "12do_falsename") || strstr(sym, "8groupingEv") || strstr(sym, "8truenameEv") || strstr(sym, "9falsenameEv");
    if (strstr(sym, "moneypunct") && strstr(sym, "IcL")) return strstr(sym, "11do_grouping") || strstr(sym, "14do_curr_symbol") || strstr(sym, "16do_positive_sign") || strstr(sym, "16do_negative_sign");
    return false;
}
static void stub_call_inner(CPU *c) {
    if (c->pc - g_stub_base >= g_stub_size) {  // an x86 host function reached through a pointer
        if (c->pc < 0x10000) die(c, "branch to invalid address", 0);
        void *fn = (void *)c->pc;
        // (dladdr is slow, a few microseconds: the answer for a host function is remembered, per thread)
        static __thread struct { uint64_t fn; bool returns_string; const char *enc; } t_hostfn_cache[256];
        unsigned hci = (unsigned)(c->pc >> 4) & 255;
        bool fn_returns_string; const char *fn_enc;
        if (t_hostfn_cache[hci].fn == c->pc) { fn_returns_string = t_hostfn_cache[hci].returns_string; fn_enc = t_hostfn_cache[hci].enc; }
        else {
            Dl_info di;
            if (!dladdr(fn, &di)) {  // not inside any host image either: a stray pointer, report where it came from
                fprintf(stderr, "rebar: branch to 0x%llx, which is neither guest nor host code; x0=%llx x1=%llx x2=%llx x8=%llx x16=%llx x17=%llx\n",
                        (unsigned long long)c->pc, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                        (unsigned long long)c->x[8], (unsigned long long)c->x[16], (unsigned long long)c->x[17]);
                c->pc = c->x[30];
                dump_backtrace(c);
                exit(127);
            }
            fn_returns_string = di.dli_sname && host_fn_returns_string(di.dli_sname);
            fn_enc = host_fn_enc(di.dli_sname);
            if (g_trace_stubs) fprintf(stderr, "[host fn by pointer] %p %s\n", fn, di.dli_sname ? di.dli_sname : "?");
            t_hostfn_cache[hci].fn = c->pc; t_hostfn_cache[hci].returns_string = fn_returns_string; t_hostfn_cache[hci].enc = fn_enc;
        }
        if (g_trace) fprintf(stderr, "  [host fn %p]\n", fn);
        c->pc = c->x[30];
        if (fn_enc) { forward_enc(c, fn, fn, fn_enc, "stream buffer seek"); apply_landing(c); return; }
        {   // a host virtual function that returns a std::string by value (a facet's grouping or sign strings, called through its vtable): the result is in memory at x8 here and in the
            // host's string layout there, and `this` shifts by one register
            if (fn_returns_string) {
                uint8_t rtmp[24] = {0}; uint64_t gx8 = c->x[8];
                c->x[8] = (uint64_t)rtmp;
                host_call(c, fn, true);
                str_to_guest((uint8_t *)gx8, rtmp);
                c->x[0] = gx8;
                apply_landing(c);
                return;
            }
        }
        {   // a function of the default zone, called through the clone
            ZoneClone *zc = clone_for((const uint64_t *)c->x[0]);
            if (zc && (uint64_t)fn == zc->orig[2]) fn = HOSTFN("malloc_size"), c->x[0] = c->x[1];  // (the zone's own size() does not know every pointer)
            else if (zc) c->x[0] = (uint64_t)zc->real;
        }
        host_call(c, fn, false);
        apply_landing(c);
        return;
    }
    int idx = (int)((c->pc - g_stub_base) / 16);
    if (idx == STUB_EXIT) run_exit(c, (int)c->x[0]);
    if (idx >= g_nstubs) die(c, "jump into unallocated stub", 0);
    Stub *s = &g_stubs[idx];
    t_last_stub = s;
    uint64_t this0 = c->x[0];
    RingEnt *ring_ent = &t_ring[t_ring_n++ & 127];
    *ring_ent = (RingEnt){s->name, c->x[0], c->x[1], c->x[2], c->x[30], 0};
    t_last_lr = c->x[30];
    if (g_trace) fprintf(stderr, "  [stub] %s(0x%llx, 0x%llx, 0x%llx)\n", s->name, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    if (g_trace && !strncmp(s->name, "_CFString", 9) && c->x[0] > 0x10000) {
        static bool (*get_cstr)(uint64_t, char *, long, uint32_t);
        if (!get_cstr) get_cstr = HOSTFN("CFStringGetCString");
        char buf[160] = "";
        if (get_cstr && get_cstr(c->x[0], buf, sizeof buf, 0x08000100)) fprintf(stderr, "  [cfstr] %s -> \"%s\"\n", s->name, buf);
    }
    { static const char *after; static int init; if (!init) { init = 1; after = getenv("REBAR_TRACE_AFTER"); } if (after && !g_trace && !strcmp(s->name, after)) g_trace = true; }
    { static const char *bt_stub = (const char *)1; if (bt_stub == (const char *)1) bt_stub = getenv("REBAR_BT_STUB");   // REBAR_BT_STUB=<name>: the guest's call stack at every call of that import
      if (bt_stub && !strcmp(s->name, bt_stub)) { fprintf(stderr, "[bt] %s(0x%llx, 0x%llx, 0x%llx)\n", s->name, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]); dump_backtrace(c);
        if (getenv("REBAR_DUMP_X0") && c->x[0] > 0x10000 && (in_guest(c->x[0]) || c->x[0] < 0x800000000000ull)) { int nw = atoi(getenv("REBAR_DUMP_X0")); fprintf(stderr, "  x0 words:"); for (int k = 0; k < nw; k++) fprintf(stderr, " %llx", (unsigned long long)((uint64_t *)c->x[0])[k]); fprintf(stderr, "\n");
          uint64_t isa = ((uint64_t *)c->x[0])[0]; if (in_guest(isa)) { fprintf(stderr, "  isa words:"); for (int k = 0; k < nw; k++) fprintf(stderr, " %llx", (unsigned long long)((uint64_t *)isa)[k]); fprintf(stderr, "\n"); } } } }
    static int trace_catch = -1; if (__builtin_expect(trace_catch < 0, 0)) trace_catch = getenv("REBAR_TRACE_CATCH") != NULL;   // (getenv takes the process-wide environment lock: not on every call)
    if (__builtin_expect(trace_catch, 0) && !strcmp(s->name, "_objc_begin_catch") && c->x[0] > 0x10000) {   // what the guest's @catch got: the exception's description
        void *(*send)(void *, void *) = HOSTFN("objc_msgSend"); void *(*selreg)(const char *) = HOSTFN("sel_registerName");
        void *d = send((void *)c->x[0], selreg("description")); const char *u = d ? (const char *)send(d, selreg("UTF8String")) : NULL;
        fprintf(stderr, "[catch] %s\n", u ? u : "?");
    }
    if (g_trace_stubs && trace_stub_wanted(s->name)) { char b[300]; int n = snprintf(b, sizeof b, "[stub] %s(%llx,%llx,%llx,%llx,%llx) sp=%llx lr=%llx\n", s->name, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4], (unsigned long long)c->sp, (unsigned long long)c->x[30]); write(2, b, n); }
    if (g_trace_stubs && trace_stub_wanted(s->name) && (!strcmp(s->name, "_dlsym") || !strcmp(s->name, "_dlopen") || !strcmp(s->name, "_sysctlbyname") || !strcmp(s->name, "_getenv"))) {   // the names these calls look for
        const char *arg = !strcmp(s->name, "_dlsym") ? (const char *)c->x[1] : (const char *)c->x[0];
        if (arg > (const char *)0x10000 && (uint64_t)arg < 0x7fffffffffffull) fprintf(stderr, "   %s(\"%.120s\")\n", s->name, arg);
    }
    if (g_trace_stubs && (!strcmp(s->name, "_vm_region_64") || !strcmp(s->name, "_mach_vm_region")) && c->x[1] > 0x10000) {
        char b[160]; int n = snprintf(b, sizeof b, "   [vm_region input address %llx]\n", (unsigned long long)*(uint64_t *)c->x[1]); write(2, b, n);
    }
    if (g_trace_paths) {
        static const char *path_fns[] = {"_access", "_stat", "_stat$INODE64", "_lstat", "_lstat$INODE64", "_open", "_openat", "_fopen",
            "_realpath", "_realpath$DARWIN_EXTSN", "_readlink", "_getenv", "_opendir", "_opendir$INODE64", "_dlopen", "_chdir", "_unlink", "_mkdir", "_rename", "_execve", "_posix_spawn", NULL};
        for (const char **f = path_fns; *f; f++)
            if (!strcmp(s->name, *f)) {
                const char *arg = !strcmp(s->name, "_posix_spawn") || !strcmp(s->name, "_openat") ? (const char *)c->x[1] : (const char *)c->x[0];
                if (arg > (const char *)0x10000 && (uint64_t)arg < 0x7fffffffffffull) fprintf(stderr, "  [path] %s(\"%.200s\")\n", s->name, arg);
                break;
            }
    }
    c->pc = c->x[30];  // return to the caller unless a handler redirects (longjmp)
    if (s->handler) s->handler(c);
    else if (s->host && s->enc) forward_enc(c, s->host, s->host, s->enc, s->name);
    else if (s->host && s->str) string_call(c, s);
    else if (s->host) {
        uint64_t addr_ptr = c->x[1], size = c->x[2];
        if (s->maps && ((uint32_t)c->x[4] & 1)) c->x[3] |= 0x3fff;  // VM_FLAGS_ANYWHERE: arm64 macOS places mappings on 16KB boundaries
        t_stk = s->stk; t_swift = s->swift; host_call(c, s->host, s->sret); t_stk = NULL;
        if (s->maps && (uint32_t)c->x[0] == 0 && !t_landed) own_add(*(uint64_t *)addr_ptr, size);  // vm_map & co.: the guest owns what came back
    }
    else {
        fprintf(stderr, "rebar: unresolved import %s%s%s%s\n", s->name,
                s->lib ? " (from " : "", s->lib ? s->lib : "", s->lib ? ")" : "");
        exit(127);
    }
    if (s->thisret && !t_landed) c->x[0] = this0;  // a constructor or destructor: arm64 returns this
    switch (s->narrow) {  // x86_64 leaves the upper bits of rax undefined for a bool/char/short result
    case 'B': case 'C': c->x[0] = (uint8_t)c->x[0]; break;
    case 'c': c->x[0] = (uint64_t)(int64_t)(int8_t)c->x[0]; break;
    case 'S': c->x[0] = (uint16_t)c->x[0]; break;
    case 's': c->x[0] = (uint64_t)(int64_t)(int16_t)c->x[0]; break;
    }
    apply_landing(c);
    ring_ent->ret = c->x[0];
    if (g_trace_stubs && getenv("REBAR_TRACE_STUBS") && strcmp(getenv("REBAR_TRACE_STUBS"), "1") && trace_stub_wanted(s->name)) { char b[64]; int n = snprintf(b, sizeof b, "   -> %llx\n", (unsigned long long)c->x[0]); write(2, b, n); }
    if (g_trace_stubs && (!strcmp(s->name, "_mmap") || !strcmp(s->name, "_munmap") || !strcmp(s->name, "_mprotect"))) { char b[64]; int n = snprintf(b, sizeof b, "   -> %llx\n", (unsigned long long)c->x[0]); write(2, b, n); }
}

// REBAR_TRACE_STUBS=<sub1,sub2,...> logs only the imports whose name contains one of the words (with their results);
// REBAR_TRACE_STUBS=1 (or any value without a match list use) logs all of them.
static bool trace_stub_wanted(const char *name) {
    static const char *list; static int init;
    if (!init) { init = 1; list = getenv("REBAR_TRACE_STUBS"); if (list && (!*list || !strcmp(list, "1"))) list = NULL; }
    if (!list) return true;
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        char tmp[128]; snprintf(tmp, sizeof tmp, "%.*s", (int)l, p);
        if (l && strstr(name, tmp)) return true;
        p += l + (e ? 1 : 0);
    }
    return false;
}

static int stub_new(const char *name, const char *lib, void *host, StubHandler h) {
    if (g_nstubs >= MAX_STUBS) { fprintf(stderr, "rebar: too many imports\n"); exit(1); }
    g_stubs[g_nstubs] = (Stub){strdup(name), lib ? strdup(lib) : NULL, host, h,
                               strstr(name, "9to_stringE") || strstr(name, "6substrE") || strstr(name, "6getlocE") || strstr(name, "17current_exceptionv"), NULL, 0, NULL, NULL, false, false};
    return g_nstubs++;
}

static void stubs_init(void) {
    g_stub_size = MAX_STUBS * 16;
    void *p = mmap(NULL, g_stub_size, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap stubs"); exit(1); }
    g_stub_base = (uint64_t)p;
    stub_new("<return>", NULL, NULL, NULL);
    stub_new("<exit>", NULL, NULL, NULL);
}

// The host has an x86_64 build of every system library at the same path; a library is
// usable exactly when it can be loaded. Calls then go through the generic forwarder, so
// functions with by-value structs or C++ classes in their signatures are only as good as
// the special cases in this file.
static struct { char *path; bool ok; } g_hostlibs[512];
static int g_nhostlibs;
static bool host_lib_ok(const char *path) {
    for (int k = 0; k < g_nhostlibs; k++) if (!strcmp(g_hostlibs[k].path, path)) return g_hostlibs[k].ok;
    if (g_nhostlibs >= 512) return false;
    g_hostlibs[g_nhostlibs].path = strdup(path);
    return g_hostlibs[g_nhostlibs++].ok = dlopen(path, RTLD_NOW | RTLD_GLOBAL) != NULL;
}
static bool lib_supported(const char *lib) { return !lib || host_lib_ok(lib); }
static void load_host_libraries(void) {  // dlopen every system library the guest images name
    for (int i = 0; i < g_nimages; i++)
        for (int k = 0; k < g_images[i]->ndeps; k++)
            if (g_images[i]->dep_host[k]) host_lib_ok(g_images[i]->dep_path[k]);
}

static bool stl_layout_dependent(const char *name);
// Apple's arm64 C++ ABI has constructors and (non-deleting) destructors return `this`, and optimized guest code uses
// the result; the host's x86_64 ones return nothing in particular, so such an import gets x0 back afterwards.
// Host functions that return a class by value which the Itanium ABI passes in memory (a class with a non-trivial copy constructor or destructor, or one over 16 bytes): the result's
// address travels in x8 on arm64 and in rdi on x86_64, and everything else shifts by one. The name does not tell the return type, so these are listed: std::locale (locale::global,
// ios_base::getloc / imbue, basic_ios::imbue, basic_streambuf::getloc / pubimbue), promise::get_future, std::current_exception and the std::filesystem functions that return file_status or space_info.
static bool returns_class_by_value(const char *n) {
    if (!strcmp(n, "__ZSt17current_exceptionv")) return true;   // std::exception_ptr: one pointer, but a destructor, so it comes back in memory
    if (strncmp(n, "__ZNSt3__1", 10) && strncmp(n, "__ZNKSt3__1", 11)) return false;
    return (strstr(n, "7promiseI") && strstr(n, "10get_futureEv")) || (strstr(n, "13packaged_taskI") && strstr(n, "10get_futureEv"))   // promise<T>::get_future returns a future<T>
        || strstr(n, "6locale6global") || strstr(n, "8ios_base6getloc") || strstr(n, "8ios_base5imbue") || (strstr(n, "9basic_ios") && strstr(n, "5imbueE"))
        || (strstr(n, "15basic_streambuf") && (strstr(n, "6getlocEv") || strstr(n, "8pubimbue")))
        || strstr(n, "10filesystem8__statusE") || strstr(n, "10filesystem15__symlink_statusE") || strstr(n, "10filesystem7__spaceE");
}
static bool returns_this(const char *name) {
    if (strncmp(name, "__ZN", 4) || !(strstr(name, "C1E") || strstr(name, "C2E") || strstr(name, "C3E") || strstr(name, "D1E") || strstr(name, "D2E"))) return false;
    if (!p_demangle) {
        p_demangle = HOSTFN("__cxa_demangle");
        if (!p_demangle) { void *h = dlopen("/usr/lib/libc++abi.dylib", RTLD_NOW); if (h) p_demangle = dlsym(h, "__cxa_demangle"); }
        if (!p_demangle) return false;
    }
    int st = 0;
    char *d = p_demangle(name + 1, NULL, NULL, &st);
    if (!d || st) { free(d); return false; }
    char *paren = strchr(d, '(');
    bool r = false;
    if (paren) {
        *paren = 0;
        // "ns::Class<targs>::Class" or "ns::Class::~Class": the last two components agree
        char *last = strrchr(d, ':');
        int depth = 0;
        for (char *q = paren - 1; q > d; q--) {  // skip template arguments of the last component when scanning back
            if (*q == '>') depth++; else if (*q == '<') depth--; else if (depth == 0 && q[0] == ':' && q > d) { last = q; break; }
        }
        if (last && last > d) {
            const char *fn = last + 1;
            if (*fn == '~') fn++;
            size_t fl = strlen(fn);
            // the component before the last "::"
            char *prev_end = last - 1;  // points at the first ':' of "::"
            if (prev_end >= d && *prev_end == ':') prev_end--;
            const char *pe = prev_end + 1;
            const char *ps = pe;
            int dp = 0;
            while (ps > d) {
                char ch = ps[-1];
                if (ch == '>') dp++; else if (ch == '<') dp--;
                else if (dp == 0 && ch == ':') break;
                ps--;
            }
            // strip template arguments of the class name
            size_t cl = (size_t)(pe - ps);
            const char *lt = memchr(ps, '<', cl);
            if (lt) cl = (size_t)(lt - ps);
            r = cl == fl && !strncmp(ps, fn, fl);
        }
    }
    free(d);
    return r;
}
static uint64_t stub_for(const char *name, const char *lib, bool weak) {
    for (int k = 2; k < g_nstubs; k++)
        if (!strcmp(g_stubs[k].name, name)) return stub_addr(k);
    {   // debugging aid: REBAR_NOP=_vm_protect,_madvise makes those imports do nothing and return 0
        const char *nop = getenv("REBAR_NOP");
        if (nop) {
            size_t nl = strlen(name);
            for (const char *p = nop; *p;) {
                const char *e = strchr(p, ',');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                if (l == nl && !strncmp(p, name, l)) return stub_addr(stub_new(name, lib, NULL, h_nop));
                p += l + (e ? 1 : 0);
            }
        }
    }
    StubHandler h = NULL;
    void *host = NULL;
    if (lib_supported(lib)) {
        for (const Special *s = k_special; s->name; s++) if (!strcmp(s->name, name)) h = s->h;
        if (!h) {
            const char *sym = name + (name[0] == '_');
            char buf[512];
            snprintf(buf, sizeof buf, "%s$INODE64", sym);  // x86_64 stat() & co. need the 64-bit inode variants
            host = dlsym(RTLD_DEFAULT, buf);
            if (!host) host = dlsym(RTLD_DEFAULT, sym);
        }
    }
    if (!h && !host && weak) return 0;
    int idx = stub_new(name, lib, host, h);
    if (host && !h) {  // returns bool/char/short? (generated table): the result needs extending for the arm64 caller
        int lo = 0, hi = (int)(sizeof k_narrow / sizeof k_narrow[0]) - 2;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, cmp = strcmp(name, k_narrow[mid].name);
            if (!cmp) { g_stubs[idx].narrow = k_narrow[mid].kind; break; }
            if (cmp < 0) hi = mid - 1; else lo = mid + 1;
        }
    }
    if (host && !h) {  // stack-passed arguments narrower than 8 bytes? (generated table)
        int lo = 0, hi = (int)(sizeof k_stackargs / sizeof k_stackargs[0]) - 2;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, cmp = strcmp(name, k_stackargs[mid].name);
            if (!cmp) { g_stubs[idx].stk = k_stackargs[mid].sizes; break; }
            if (cmp < 0) hi = mid - 1; else lo = mid + 1;
        }
    }
    if (host && !h && stl_layout_dependent(name))
        fprintf(stderr, "rebar: warning: %s takes libc++ objects whose arm64 layout rebar does not convert; expect it to misbehave\n", name);
    if (host && !h) g_stubs[idx].thisret = returns_this(name);
    if (host && !h && returns_class_by_value(name)) g_stubs[idx].sret = true;
    if (host && !h) g_stubs[idx].swift = name[0] == '_' && name[1] == '$' && (name[2] == 's' || name[2] == 'S');   // a Swift symbol: Swift's calling convention
    if (host && !h) g_stubs[idx].maps = !strcmp(name, "_vm_map") || !strcmp(name, "_mach_vm_map") || !strcmp(name, "_vm_remap") || !strcmp(name, "_mach_vm_remap");
    if (host && !h) {
        StrSpec *sp = g_stubs[idx].str = str_spec_for(name);
        if (g_trace_stubs && sp) fprintf(stderr, "[string] %s: args %02x write-back %02x uninit %02x%s%s\n", name, sp->mask, sp->wb, sp->uninit, sp->sret ? " returns string" : "", sp->dtor ? " destructor" : "");
    }
    if (host && !h) {  // by-value struct signature? (generated table)
        int lo = 0, hi = (int)(sizeof k_csigs / sizeof k_csigs[0]) - 2;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, cmp = strcmp(name, k_csigs[mid].name);
            if (!cmp) { g_stubs[idx].enc = k_csigs[mid].enc; break; }
            if (cmp < 0) hi = mid - 1; else lo = mid + 1;
        }
    }
    if (host && !h && !g_stubs[idx].enc && !strncmp(name, "__ZNSt3__1", 10)) {  // libc++ stream positions: a std::fpos<mbstate_t> is 136 bytes (OpenColorIO's istream::seekg branched to junk)
        static const char F[] = "{fpos=q[16q]}";
        static const struct { const char *name, *enc; } cxx[] = {
            {"__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5seekgENS_4fposI11__mbstate_tEE", "^v^v{fpos=q[16q]}"},
            {"__ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE5tellgEv", "{fpos=q[16q]}^v"},
            {"__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5seekpENS_4fposI11__mbstate_tEE", "^v^v{fpos=q[16q]}"},
            {"__ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE5tellpEv", "{fpos=q[16q]}^v"},
            {"__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE10pubseekposENS_4fposI11__mbstate_tEEj", "{fpos=q[16q]}^v{fpos=q[16q]}I"},
            {"__ZNSt3__115basic_streambufIcNS_11char_traitsIcEEE10pubseekoffExNS_8ios_base7seekdirEj", "{fpos=q[16q]}^vqiI"},
        };
        (void)F;
        for (size_t k = 0; k < sizeof cxx / sizeof cxx[0]; k++) if (!strcmp(name, cxx[k].name)) { g_stubs[idx].enc = cxx[k].enc; break; }
    }
    return stub_addr(idx);
}

static const char *k_data_syms[] = {
    "___stderrp", "___stdoutp", "___stdinp", "___stack_chk_guard", "_environ", "_optarg", "_optind", "_opterr",
    "_optopt", "_optreset", "___progname", "_NXArgc", "_NXArgv", "__DefaultRuneLocale", "_tzname", "_timezone",
    "_daylight", "_signgam", "_sys_errlist", "_sys_nerr", "_sys_siglist", "_h_errno", "_in6addr_any",
    "_in6addr_loopback", "_mach_task_self_", "_stdin", "_stdout", "_stderr",
    "_vm_page_size", "_vm_page_mask", "_vm_page_shift", "_vm_kernel_page_size", "_vm_kernel_page_mask", "_vm_kernel_page_shift", NULL,
};

// arm64 macOS has 16KB pages: the page-size variables libSystem exports say so (Chromium's allocator reads them)
static uint64_t g_vm_page_size = GUEST_PAGE, g_vm_page_mask = GUEST_PAGE - 1;
static int g_vm_page_shift = 14;
static void *data_addr(const char *name) {
    if (!strcmp(name, "_vm_page_size") || !strcmp(name, "_vm_kernel_page_size")) return &g_vm_page_size;
    if (!strcmp(name, "_vm_page_mask") || !strcmp(name, "_vm_kernel_page_mask")) return &g_vm_page_mask;
    if (!strcmp(name, "_vm_page_shift") || !strcmp(name, "_vm_kernel_page_shift")) return &g_vm_page_shift;
    if (!strcmp(name, "_NXArgc")) return &g_argc;
    if (!strcmp(name, "_NXArgv")) return &g_argv;
    if (!strcmp(name, "___progname")) return &g_progname;
    if (!strcmp(name, "_environ")) return &g_environ;
    return dlsym(RTLD_DEFAULT, name + 1);
}

// Is this host address code rather than data? Read-only constants live in __TEXT too (__TEXT,__const: CATransform3DIdentity,
// CGRectNull, CGAffineTransformIdentity, ... are structs of doubles with nothing to relocate), so the segment is not enough:
// only a section that holds instructions is code. Calling those constants "code" bound them to a stub that reads as zeros.
static bool host_addr_is_code(void *p) {
    Dl_info di;
    if (!dladdr(p, &di) || !di.dli_fbase) return true;
    const struct mach_header_64 *mh = di.dli_fbase;
    const struct load_command *lc = (const void *)(mh + 1);
    uint64_t slide = 0;
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        const struct segment_command_64 *sg = (const void *)lc;
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(sg->segname, "__TEXT")) slide = (uint64_t)mh - sg->vmaddr;
    }
    lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        const struct segment_command_64 *sg = (const void *)lc;
        if (lc->cmd != LC_SEGMENT_64) continue;
        uint64_t lo = sg->vmaddr + slide;
        if ((uint64_t)p < lo || (uint64_t)p >= lo + sg->vmsize) continue;
        if (strncmp(sg->segname, "__TEXT", 6)) return false;
        const struct section_64 *sc = (const void *)(sg + 1);
        for (uint32_t m = 0; m < sg->nsects; m++, sc++) {
            uint64_t a = sc->addr + slide;
            if ((uint64_t)p >= a && (uint64_t)p < a + sc->size) return (sc->flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0;
        }
        return true;
    }
    return true;
}


static uint64_t find_guest_symbol(const char *name);
static uint64_t image_export_lookup(const GImage *img, const char *name);

// Returns the value to store in the guest's pointer slot for this import.
// The address of a variable the host defines (kCFRunLoopCommonModes, stdout, ...), or 0 if `name` is not one.
static uint64_t host_data_addr(const char *name) {
    for (const char **d = k_data_syms; *d; d++)
        if (!strcmp(*d, name)) { void *p = data_addr(name); if (p) return (uint64_t)p; }
    for (const Special *s = k_special; s->name; s++) if (!strcmp(s->name, name)) return 0;
    void *p = dlsym(RTLD_DEFAULT, name + (name[0] == '_'));  // anything the host defines in a data segment is a variable, not a function
    return p && !host_addr_is_code(p) ? (uint64_t)p : 0;
}

static uint64_t resolve_import(GImage *img, const char *name, int ordinal, bool weak) {
    const char *lib = NULL;
    GImage *dep = NULL;
    if (ordinal >= 1 && ordinal <= img->ndeps) {
        dep = img->dep_img[ordinal - 1];
        if (!dep) lib = img->dep_path[ordinal - 1];
    }
    // weak lookups, "self" and flat lookups see the guest's own definitions first (ObjC EH types,
    // C++ typeinfo, inline variables); a dependency that is itself a guest image is searched directly.
    if (!strcmp(name, "__tlv_bootstrap")) return tlv_thunk();   // a thread-local variable's descriptor starts with this: see tlv_thunk
    if (dep) { uint64_t a = image_export_lookup(dep, name); if (a) return a; }
    if (!lib || ordinal <= 0) { uint64_t a = find_guest_symbol(name); if (a) return a; }
    if (!strcmp(name, "__ZTVNSt3__115basic_stringbufIcNS_11char_traitsIcEENS_9allocatorIcEEEE")) {  // see sb_proxy_vtable
        uint64_t a = sb_proxy_vtable();
        if (a) return a;
    }
    if (lib_supported(lib)) {
        uint64_t a = host_data_addr(name);
        if (a) return a;
    }
    return stub_for(name, lib, weak);
}

// ----------------------------------------------------------- chained fixups

// std::string is converted at the call boundary (string_call), but only the char flavour, and only for objects
// the guest owns: the wide strings (wchar_t, char16_t, char32_t, char8_t) are not converted, nor are the strings
// inside objects the host library creates and the guest then reads (std::filesystem directory iterators and entries,
// path component iterators) or moves (stringbuf, stringstream). Arm64 code that calls into the host's x86_64 libc++
// for those would hand it objects it misreads. Rather than crash later, a dlopen of a library that does this is
// refused with an explanation, and a program that does it gets a warning when the import is bound.
static bool stl_layout_dependent(const char *name) {
    for (const char *p = name; (p = strstr(p, "12basic_stringI")); p += 15)
        if (p[15] == 'D') return true;   // (char16_t / char32_t strings; wchar_t ones are converted)
    if (strstr(name, "stringstreamIw") || strstr(name, "stringstreamID") || strstr(name, "stringbufIw") || strstr(name, "stringbufID")) return true;
    if (strstr(name, "directory_entry")) return true;   // (directory_iterator and recursive_directory_iterator work on the host's own state; only the entry they hand out is converted: h_dir_deref)
    if (strstr(name, "10filesystem4path8iterator") || strstr(name, "10filesystem4path5beginEv") || strstr(name, "10filesystem4path3endEv")) return true;
    return (strstr(name, "stringbufI") || strstr(name, "stringstreamI")) && (strstr(name, "C1EOS") || strstr(name, "C2EOS") || strstr(name, "aSEOS"));
}
static bool g_bind_scan;            // run_bind_opcodes only looks at the names (see image_stl_import)
static const char *g_bind_hit;
static void apply_classic_binds(GImage *img);
static const char *image_stl_import(GImage *img) {
    if (!img->chained) {
        if (!img->dyldinfo) return NULL;
        g_bind_scan = true; g_bind_hit = NULL;
        apply_classic_binds(img);
        g_bind_scan = false;
        return g_bind_hit;
    }
    const uint8_t *fx = img->slice + img->chained->dataoff;
    const struct dyld_chained_fixups_header *h = (const void *)fx;
    const uint8_t *imports = fx + h->imports_offset, *syms = fx + h->symbols_offset;
    for (uint32_t k = 0; k < h->imports_count; k++) {
        uint32_t nameoff = h->imports_format == DYLD_CHAINED_IMPORT ? ((const uint32_t *)imports)[k] >> 9
                         : h->imports_format == DYLD_CHAINED_IMPORT_ADDEND ? ((const uint32_t *)imports)[2 * k] >> 9
                         : (uint32_t)(((const uint64_t *)imports)[2 * k] >> 32);
        if (stl_layout_dependent((const char *)syms + nameoff)) return (const char *)syms + nameoff;
    }
    return NULL;
}

static void apply_chained_fixups(GImage *img) {
    const uint64_t base = img->base;
    const uint8_t *fx = img->slice + img->chained->dataoff;
    const struct dyld_chained_fixups_header *h = (const void *)fx;
    const struct dyld_chained_starts_in_image *si = (const void *)(fx + h->starts_offset);
    const uint8_t *imports = fx + h->imports_offset, *syms = fx + h->symbols_offset;

    uint64_t *bound = calloc(h->imports_count ? h->imports_count : 1, 8);
    int64_t *addend = calloc(h->imports_count ? h->imports_count : 1, 8);
    for (uint32_t k = 0; k < h->imports_count; k++) {
        uint32_t nameoff; int ord; bool weak;
        switch (h->imports_format) {
        case DYLD_CHAINED_IMPORT: {
            uint32_t v = ((const uint32_t *)imports)[k];
            ord = v & 0xff; weak = (v >> 8) & 1; nameoff = v >> 9;
            if (ord > 0xf0) ord = (int8_t)ord;
            break;
        }
        case DYLD_CHAINED_IMPORT_ADDEND: {
            uint32_t v = ((const uint32_t *)imports)[2 * k];
            ord = v & 0xff; weak = (v >> 8) & 1; nameoff = v >> 9;
            if (ord > 0xf0) ord = (int8_t)ord;
            addend[k] = ((const int32_t *)imports)[2 * k + 1];
            break;
        }
        default: {  // DYLD_CHAINED_IMPORT_ADDEND64
            uint64_t v = ((const uint64_t *)imports)[2 * k];
            ord = v & 0xffff; weak = (v >> 16) & 1; nameoff = (uint32_t)(v >> 32);
            if (ord > 0xfff0) ord = (int16_t)ord;
            addend[k] = ((const int64_t *)imports)[2 * k + 1];
            break;
        }
        }
        bound[k] = resolve_import(img, (const char *)syms + nameoff, ord, weak);
    }

    for (uint32_t s = 0; s < si->seg_count; s++) {
        if (!si->seg_info_offset[s]) continue;
        const struct dyld_chained_starts_in_segment *sg =
            (const void *)((const uint8_t *)si + si->seg_info_offset[s]);
        if (sg->pointer_format != DYLD_CHAINED_PTR_64 && sg->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
            fprintf(stderr, "rebar: unsupported chained pointer format %u (arm64e?)\n", sg->pointer_format);
            exit(1);
        }
        for (uint32_t pg = 0; pg < sg->page_count; pg++) {
            uint16_t ps = sg->page_start[pg];
            if (ps == DYLD_CHAINED_PTR_START_NONE) continue;
            if (ps & DYLD_CHAINED_PTR_START_MULTI) { fprintf(stderr, "rebar: multi-start chains unsupported\n"); exit(1); }
            uint64_t *p = (uint64_t *)(base + sg->segment_offset + (uint64_t)pg * sg->page_size + ps);
            for (;;) {
                uint64_t raw = *p;
                uint32_t next = (raw >> 51) & 0xfff;
                if (raw >> 63) {
                    uint32_t ordn = raw & 0xffffff;
                    *p = bound[ordn] + addend[ordn] + ((raw >> 24) & 0xff);
                } else {
                    uint64_t target = raw & 0xfffffffffull, high8 = (raw >> 36) & 0xff;
                    *p = (sg->pointer_format == DYLD_CHAINED_PTR_64_OFFSET ? base + target : target + img->slide) | (high8 << 56);
                }
                if (!next) break;
                p = (uint64_t *)((uint8_t *)p + next * 4);
            }
        }
    }
    free(bound); free(addend);
}

// ----------------------------------------------- classic dyld info (bind opcodes)
// Loaded at the preferred address, so rebases are no-ops; only binds are needed.
// Lazy binds are resolved eagerly, so dyld_stub_binder is never reached.

static uint64_t uleb(const uint8_t **p) {
    uint64_t r = 0; int sh = 0;
    for (;;) { uint8_t b = *(*p)++; r |= (uint64_t)(b & 0x7f) << sh; sh += 7; if (!(b & 0x80)) break; }
    return r;
}
static int64_t sleb(const uint8_t **p) {
    int64_t r = 0; int sh = 0; uint8_t b;
    do { b = *(*p)++; r |= (int64_t)(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
    if (sh < 64 && (b & 0x40)) r |= -((int64_t)1 << sh);
    return r;
}

// The weak-binding stream lists the places that must end up pointing at the first definition of a symbol among all the loaded images (operator new and delete, inline
// functions, typeinfo). The ordinary streams have bound most of them already; a place that only this stream mentions (OpenImageIO's operator new in Blender) is empty.
static bool g_bind_weak_stream;
static void run_bind_opcodes(GImage *img, const uint8_t *p, const uint8_t *end) {
    int ord = 0; const char *name = ""; bool weak = false; int64_t addend = 0; uint64_t addr = 0;
    while (p < end) {
        uint8_t op = *p++, imm = op & 0x0f;
        switch (op & 0xf0) {
        case 0x00: break;  // DONE (lazy streams continue)
        case 0x10: ord = imm; break;
        case 0x20: ord = (int)uleb(&p); break;
        case 0x30: ord = imm ? (int8_t)(0xf0 | imm) : 0; break;
        case 0x40: name = (const char *)p; p += strlen(name) + 1; weak = imm & 1;
                   if (g_bind_scan && !g_bind_hit && stl_layout_dependent(name)) g_bind_hit = name;
                   break;
        case 0x50: break;  // type
        case 0x60: addend = sleb(&p); break;
        case 0x70: if (imm >= (unsigned)img->nseg) { fprintf(stderr, "rebar: bad bind segment\n"); exit(1); } addr = img->segaddr[imm] + uleb(&p); break;
        case 0x80: addr += uleb(&p); break;
        case 0x90: case 0xa0: case 0xb0: case 0xc0: {
            uint64_t times = 1, skip = 0, after = 0;
            if ((op & 0xf0) == 0xc0) { times = uleb(&p); skip = uleb(&p); }
            if ((op & 0xf0) == 0xa0) after = uleb(&p);
            if ((op & 0xf0) == 0xb0) after = (uint64_t)imm * 8;
            for (uint64_t t = 0; t < times; t++) {
                if (g_bind_scan) {}
                else if (g_bind_weak_stream) {
                    uint64_t *slot = (uint64_t *)addr, a = find_guest_symbol(name);
                    if (a) *slot = a + addend;
                    else if (!*slot) { uint64_t d = host_data_addr(name); *slot = (d ? d : resolve_import(img, name, 0, true)) + addend; }
                } else *(uint64_t *)addr = resolve_import(img, name, ord, weak) + addend;
                addr += 8 + skip;
            }
            addr += after;
            break;
        }
        default: fprintf(stderr, "rebar: unsupported bind opcode 0x%02x\n", op); exit(1);
        }
    }
}

static void apply_classic_binds(GImage *img) {
    const struct dyld_info_command *di = img->dyldinfo;
    if (di->bind_size) run_bind_opcodes(img, img->slice + di->bind_off, img->slice + di->bind_off + di->bind_size);
    if (di->lazy_bind_size) run_bind_opcodes(img, img->slice + di->lazy_bind_off, img->slice + di->lazy_bind_off + di->lazy_bind_size);
    if (di->weak_bind_size && !g_bind_scan && !getenv("REBAR_NOWEAKBIND")) {
        g_bind_weak_stream = true;
        run_bind_opcodes(img, img->slice + di->weak_bind_off, img->slice + di->weak_bind_off + di->weak_bind_size);
        g_bind_weak_stream = false;
    }
}

#include "unwind.h"
#include "loader.h"
