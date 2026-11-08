#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <setjmp.h>
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
    uint32_t symoff, nsyms, stroff, strsize;   // LC_SYMTAB: the lookup of exported names for an image without an export trie (Dart's AOT snapshot dylibs)
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
    static int lim = -1; if (lim < 0) lim = getenv("REBAR_RECENT") ? atoi(getenv("REBAR_RECENT")) : 16;   // REBAR_RECENT=n: show n entries, without the retain / release noise
    int shown = 0;
    for (unsigned k = 1; k <= 128 && shown < lim; k++) {
        const RingEnt *e = &t_ring[(t_ring_n - k) & 127];
        if (t_ring_n < k || !e->name) break;
        bool skip = false;
        for (const char **nz = noise; *nz; nz++) if (!strcmp(e->name, *nz)) skip = true;
        if (lim > 16 && (!strncmp(e->name, "_objc_retain", 12) || !strncmp(e->name, "_objc_release", 13) || !strncmp(e->name, "_objc_autorelease", 17) || !strcmp(e->name, "_swift_retain") || !strcmp(e->name, "_swift_release") || !strcmp(e->name, "_swift_bridgeObjectRetain") || !strcmp(e->name, "_swift_bridgeObjectRelease"))) skip = true;
        if (skip) continue;
        if (!shown++) fprintf(stderr, "  recent imports on this thread, newest first:\n");
        char selb[96] = "";
        if (!strcmp(e->name, "_objc_msgSend") && e->x1 > 0x10000) { char t[2]; mach_vm_size_t got = 0; if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)e->x1, 1, (mach_vm_address_t)t, &got) == KERN_SUCCESS) { static const char *(*sg)(const void *); if (!sg) sg = (const char *(*)(const void *))dlsym(RTLD_DEFAULT, "sel_getName"); snprintf(selb, sizeof selb, "  [%s]", sg ? sg((const void *)e->x1) : "?"); } }
        fprintf(stderr, "    %s(0x%llx, 0x%llx, 0x%llx) = 0x%llx from 0x%llx%s\n", e->name, (unsigned long long)e->x0, (unsigned long long)e->x1, (unsigned long long)e->x2, (unsigned long long)e->ret, (unsigned long long)e->lr, selb);
    }
}
typedef struct { uint64_t pc, sp, fp, lr; uint64_t x[10]; uint64_t d[8]; } UFrame;  // callee-saved view of a guest frame
typedef struct HostCallRec { struct HostCallRec *prev; CPU *cpu; UFrame f; } HostCallRec;  // one per guest->host call
typedef struct Act { struct Act *prev; CPU *cpu, *saved; HostCallRec *hc_at_entry; bool own_gil; } Act;   // one per host->guest entry
static void dump_frames(uint64_t pc, uint64_t lr, uint64_t fp) {
    uint64_t pcs[24], n = 0;
    pcs[n++] = pc;
    pcs[n++] = lr;
    while (n < 24 && fp && !(fp & 7) && own_check(fp, 16)) {
        uint64_t l = ((uint64_t *)fp)[1], next = ((uint64_t *)fp)[0];
        if (!l) break;
        pcs[n++] = l;
        if (next <= fp) break;
        fp = next;
    }
    for (uint64_t k = 0; k < n; k++) {
        GImage *im = image_by_addr(pcs[k]);
        const char *name = im ? strrchr(im->path, '/') : NULL;
        fprintf(stderr, "  #%llu 0x%llx  %s+0x%llx\n", (unsigned long long)k, (unsigned long long)pcs[k],
                im ? (name ? name + 1 : im->path) : "?", im ? (unsigned long long)(pcs[k] - im->base) : 0ull);
    }
}
static __thread Act *t_act;
static void dump_backtrace(CPU *c) {
    dump_frames(c->pc, c->x[30], c->x[29]);
    for (Act *a = t_act; a && a->saved; a = a->prev) {   // (the guest code this one was entered from, when the host called back into the guest)
        fprintf(stderr, "  -- entered from the host; the guest code below made that host call --\n");
        dump_frames(a->saved->pc, a->saved->x[30], a->saved->x[29]);
    }
    dump_recent_imports();
}

// ---- exception-handling state (see unwind.h)
static __thread HostCallRec *t_hc;
static __thread bool t_landed;                  // an exception was delivered to a guest handler during this host call
static __thread uint64_t t_pend_x0, t_pend_x1;  // exception pointer / selector for the handler, applied by stub_call

static void uframe_from_cpu(const CPU *c, UFrame *f) {
    f->pc = c->x[30]; f->lr = c->x[30]; f->sp = c->sp; f->fp = c->x[29];
    for (int k = 0; k < 10; k++) f->x[k] = c->x[19 + k];
    for (int k = 0; k < 8; k++) f->d[k] = c->vr[8 + k].d[0];
}

typedef void (*StubHandler)(CPU *);
typedef struct { uint8_t mask, wb, uninit, wide, iter; bool dtor, sret, sb, sretw; } StrSpec;   // iter: 1 + the argument that is a __wrap_iter<char const*> into the string (insert, erase)  // which argument registers hold std::string objects (see string_call)
typedef struct { char *name, *lib; void *host; StubHandler handler; bool sret; const char *enc; char narrow; const char *stk; StrSpec *str; bool maps, thisret, swift, throwing, async_host; uint8_t heapret, pathmask, txn_mask; bool ffi_abi, ffi_closure, cb9; bool strret; } Stub;  // maps: (task, *address, size, ...) creates memory the guest owns
static __thread Stub *t_last_stub;   // the import this thread called last, for the crash report
static __thread uint64_t t_last_lr;  // and where it was called from

#define MAX_STUBS 65536
#define STUB_RET 0   // pc here means "return to the host" (see call_guest)
#define STUB_EXIT 1  // main() returned here
static Stub g_stubs[MAX_STUBS];
static int g_nstubs;

static inline uint64_t stub_addr(int idx) { return g_stub_base + 16 * (uint64_t)idx; }

// The imports that translated code calls itself (jit.h: emit_leaf_call): plain host functions of up to six integer arguments that never throw,
// never block (the thread may hold the interpreter lock, which the generic path drops around a host call) and never call back into the guest.
static bool g_no_async_tail;   // REBAR_NO_ASYNC_TAIL (set at start)
static bool g_zone_cloned;       // the guest asked for the default zone (to patch it, as Chromium's allocator shim does): its malloc and free must go through the handlers from now on
static bool g_leaf_malloc_used;  // translated code calls the host's malloc and free directly
static bool g_leaf_malloc_off;   // a code snapshot made after the guest took the default zone is in use: no leaf malloc (its blocks have none, and taking the zone again must not throw them away)
static bool jit_leaf_import(uint64_t target, void **fn, int *nargs, int *zext8) {
    uint64_t off = target - g_stub_base;
    if (off >= g_stub_size || (off & 15) || off / 16 < 2 || off / 16 >= (uint64_t)g_nstubs) return false;
    const Stub *s = &g_stubs[off / 16];
    // malloc, calloc, realloc, free: while the guest has not got hold of the default zone nothing can have patched it, so the call goes straight to the host (the handler path costs
    // ~140 ns a call and takes the interpreter lock); the moment it asks for the zone every translation is thrown away (h_malloc_default_zone) and these calls take the handlers
    if (s->name && !g_zone_cloned && !g_leaf_malloc_off && !getenv("REBAR_NO_LEAF_MALLOC") && (!strcmp(s->name, "_malloc") || !strcmp(s->name, "_free") || !strcmp(s->name, "_calloc") || !strcmp(s->name, "_realloc"))) {
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
    // libm: the arguments are in d0 / d1 (s0 / s1), the result in d0 (s0), which is where the x86 convention puts them too (xmm0 / xmm1): *zext8 = 2 (double) or 4 (float), *nargs = how many
    if (!s->narrow && !getenv("REBAR_NO_LEAF_LIBM")) {
        static const char *const f1[] = {"sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "exp", "exp2", "expm1", "log", "log2", "log10", "log1p", "cbrt", "erf", "erfc", "tgamma", "lgamma", NULL};
        static const char *const f2[] = {"atan2", "pow", "fmod", "hypot", "remainder", "copysign", "nextafter", "fdim", NULL};
        const char *nm = s->name + 1; size_t len = strlen(nm);
        bool isf = len > 1 && nm[len - 1] == 'f'; char base[16];
        if (s->name[0] == '_' && len < sizeof base) {
            snprintf(base, sizeof base, "%.*s", (int)(isf ? len - 1 : len), nm);
            for (int q = 0; f1[q]; q++) if (!strcmp(base, f1[q])) { *fn = s->host; *nargs = 1; *zext8 = isf ? 4 : 2; return *fn != NULL; }
            for (int q = 0; f2[q]; q++) if (!strcmp(base, f2[q])) { *fn = s->host; *nargs = 2; *zext8 = isf ? 4 : 2; return *fn != NULL; }
        }
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
extern void *rebar_hc_after_call_addr(void);   // the instruction after `call *%r11` in rebar_host_call: the return address a host function finds on its stack
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
    "  pushq %r14\n  .cfi_offset %r14, -48\n"
    "  movq %rcx, %rbx\n  movq %rdi, %r11\n  movq %rsi, %r10\n  movq %rdx, %r12\n"
    "  movq %r9, %rax\n"
    "1:\n  testq %rax, %rax\n  jz 2f\n  decq %rax\n  pushq (%r8,%rax,8)\n  jmp 1b\n"
    "2:\n"
    "  movq 0(%r10), %rdi\n  movq 8(%r10), %rsi\n  movq 16(%r10), %rdx\n"
    "  movq 24(%r10), %rcx\n  movq 32(%r10), %r8\n  movq 40(%r10), %r9\n"
    "  movsd 0(%r12), %xmm0\n  movsd 8(%r12), %xmm1\n  movsd 16(%r12), %xmm2\n  movsd 24(%r12), %xmm3\n"
    "  movsd 32(%r12), %xmm4\n  movsd 40(%r12), %xmm5\n  movsd 48(%r12), %xmm6\n  movsd 56(%r12), %xmm7\n"
    "  movq 56(%rbx), %r14\n"       // out[7]: the async context of a Swift async function (x22 of the arm64 caller; callee-saved for any other callee)
    "  movq 0(%rbx), %r13\n"        // out[0]: swiftself (x20 of a Swift caller; callee-saved, so a C function does not mind)
    "  movq 8(%rbx), %rax\n"        // out[1]: %al = the vector registers of a variadic C function (8), or the address of the indirect result of a Swift function
    "  xorl %r12d, %r12d\n"         // swifterror starts out clear
    "  call *%r11\n"
    "Lhc_after_call:\n"
    "  movq %rax, 0(%rbx)\n  movq %xmm0, 8(%rbx)\n  movq %rdx, 16(%rbx)\n  movq %xmm1, 24(%rbx)\n"
    "  movq %r12, 32(%rbx)\n  movq %rcx, 40(%rbx)\n  movq %r8, 48(%rbx)\n"   // the error, and the 3rd and 4th result registers of Swift
    "Lhc_epilogue:\n"
    "  leaq -32(%rbp), %rsp\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n"
    "Lhc_landing:\n"
    "  movq %rax, %r13\n  movq %rax, %rdi\n  movq %rdx, %rsi\n  call _rebar_arrive\n"
    "  testl %eax, %eax\n  jnz Lhc_epilogue\n"
    "  call _rebar_gil_drop\n"
    "  movq %r13, %rdi\n  call __Unwind_Resume\n"
    ".cfi_endproc\n"
    ".globl _rebar_host_landing_addr\n"
    "_rebar_host_landing_addr:\n  leaq Lhc_landing(%rip), %rax\n  ret\n"
    ".globl _rebar_hc_after_call_addr\n"
    "_rebar_hc_after_call_addr:\n  leaq Lhc_after_call(%rip), %rax\n  ret\n");

// A Swift async function of the host ends by a tail jump (jmp) to the continuation it was given, which is a guest function (arm64 code). The exec-fault redirect would run that
// continuation nested inside the host call, and every await of a recursive async function would add a few KB of host stack until the thread's stack (512 KB on a dispatch thread)
// is gone. For a jump made straight from the function the stub called (its return address is the one rebar_host_call pushed), the host call is abandoned instead and the guest goes on
// at the continuation with the async context (r14 -> x22): a real tail call. REBAR_NO_ASYNC_TAIL=1 turns it off.
static __thread bool t_async_call, t_async_tailed;
static __thread jmp_buf *t_async_jb;
static __thread uint64_t t_tail_regs[8];   // rdi, rsi, rdx, rcx, r8, r9, r12 (the error), r13 at the jump
static void trace_ctx_entry(uint64_t pc, uint64_t ctx, const char *how, const uint64_t *regs, uint64_t x20) {   // REBAR_TRACE_CTX=<guest pc>[,pc...]: the async context and its first 0xe0 bytes whenever the guest is entered at one of those pcs (async continuations)
    static uint64_t tc[4]; static int ntc = -1; if (ntc < 0) { ntc = 0; const char *e = getenv("REBAR_TRACE_CTX"); while (e && *e && ntc < 4) { tc[ntc++] = strtoull(e, (char **)&e, 16); if (*e == ',') e++; } }
    for (int q = 0; q < ntc; q++) if (pc == tc[q]) { const uint64_t *cx = (const uint64_t *)ctx; fprintf(stderr, "[ctx entry %s] pc %llx ctx %p thread %p regs %llx %llx %llx %llx %llx %llx:", how, (unsigned long long)pc, (void *)cx, (void *)pthread_self(), (unsigned long long)regs[0], (unsigned long long)regs[1], (unsigned long long)regs[2], (unsigned long long)regs[3], (unsigned long long)regs[4], (unsigned long long)regs[5]); if (ctx > 0x10000) for (int k = 0; k < 28; k++) fprintf(stderr, "%s%llx", k == 0 ? " " : k % 4 == 0 ? " | " : " ", (unsigned long long)cx[k]); fprintf(stderr, "\n");
        if (x20 > 0x10000 && x20 < 0x7fffffffffffull) { const uint64_t *o = (const uint64_t *)x20; fprintf(stderr, "    x20 object %p:", (void *)o); for (int k = 0; k < 8; k++) fprintf(stderr, " %llx", (unsigned long long)o[k]); fprintf(stderr, "\n"); } }
}
[[noreturn]] static void rebar_tail_to_guest(uint64_t target, uint64_t ctx) {
    CPU *c = t_cpu; trace_ctx_entry(target, ctx, "tail", t_tail_regs, t_tail_regs[7]); c->pc = target; c->x[22] = ctx; t_async_tailed = true;
    for (int k = 0; k < 6; k++) c->x[k] = t_tail_regs[k];   // the continuation's other arguments: results in x0.., a thrown error in x21 (r12), self / context in x20 (r13)
    if (t_tail_regs[6]) c->x[21] = t_tail_regs[6];
    c->x[20] = t_tail_regs[7];
    if (getenv("REBAR_TRACE_ASYNC")) fprintf(stderr, "[async tail] -> guest %llx ctx %llx rdi %llx rsi %llx rdx %llx r12 %llx r13(x20) %llx\n", (unsigned long long)target, (unsigned long long)ctx, (unsigned long long)t_tail_regs[0], (unsigned long long)t_tail_regs[1], (unsigned long long)t_tail_regs[2], (unsigned long long)t_tail_regs[6], (unsigned long long)t_tail_regs[7]);
    _longjmp(*t_async_jb, 1);
}
// Calls a host function with a call record so exceptions can find the calling guest frames.
static void host_invoke(CPU *c, void *fn, const uint64_t *ints, const uint64_t *dbls,
                        const uint64_t *stack, uint64_t nstack, uint64_t *out) {
    HostCallRec rec;
    rec.prev = t_hc; rec.cpu = c;
    uframe_from_cpu(c, &rec.f);
    t_hc = &rec;
    bool had_gil = gil_release();            // the host call may block; other guest threads run meanwhile
    bool async_call = t_async_call; t_async_call = false; jmp_buf *prev_jb = t_async_jb;
    if (async_call) {   // a Swift async host function (swift_task_switch, an `async` method) ends by jumping to the guest's continuation: see rebar_tail_to_guest
        jmp_buf jb; t_async_jb = &jb;
        if (_setjmp(jb) == 0) rebar_host_call(fn, ints, dbls, out, stack, nstack);
    } else { t_async_jb = NULL; rebar_host_call(fn, ints, dbls, out, stack, nstack); }
    t_async_jb = prev_jb;
    if (had_gil) gil_acquire();
    t_hc = rec.prev;
}

// Swift's own calling convention (imports named _$s...): like the C one for the arguments, but self travels in x20 (r13 on x86_64), a thrown error comes back in
// x21 (r12), the address of an indirect result is in x8 (rax) and up to four result registers are used (x0-x3, rax rdx rcx r8).
static __thread bool t_swift;
static void host_call_full(CPU *c, void *fn, const uint64_t *ints, const uint64_t *dbls,
                           const uint64_t *stack, uint64_t nstack) {
    uint64_t out[8] = {0, 8};
    out[7] = c->x[22];
    bool swift = t_swift; t_swift = false;
    if (swift) { out[0] = c->x[20]; out[1] = c->x[8]; }
    host_invoke(c, fn, ints, dbls, stack, nstack, out);
    if (t_async_tailed) { t_async_tailed = false; return; }   // the guest goes on at the continuation the host jumped to
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

// The guest CPU of every thread that runs guest code, by the thread's mach port: Boehm's collector (il2cpp / Unity, Mono) suspends the other threads with thread_suspend and reads their registers with
// thread_get_state(ARM_THREAD_STATE64) to scan their stacks (see h_thread_get_state).
static struct { uint32_t port; CPU *cpu; } g_thr_cpu[1024]; static int g_nthr_cpu;
static void thr_register(CPU *c) {
    int k = __atomic_fetch_add(&g_nthr_cpu, 1, __ATOMIC_RELAXED);
    if (k < 1024) { g_thr_cpu[k].cpu = c; __atomic_store_n(&g_thr_cpu[k].port, (uint32_t)mach_thread_self(), __ATOMIC_RELEASE); }
}
static CPU *cur_cpu(void) {
    if (!t_cpu) {
        t_cpu = calloc(1, sizeof(CPU));
        t_cpu->tpidrro = (uint64_t)t_tlv_tab;
        thr_register(t_cpu);
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

// call_guest for a short, non-blocking callback that the host calls many times from inside one handler (the comparison function of qsort / bsearch): the interpreter lock stays held (the callback
// is not going to wait for anything another guest thread holds) and only the state a callee can change and the caller still needs is saved: x19..x30 and sp are the callee's to restore (saved anyway, a handler must not trust them), but the pc, the
// counters and the translated code's return-address stack bounds are not; x0..x18 and the vector registers other than d8..d15 are caller-saved (the call is a call), the flags are not preserved
// by one either. Copying the whole structure twice (850 bytes) was 40% of a comparison's cost. REBAR_SLOW_CALLBACK=1 gives the full copy back.
static uint64_t call_guest_short(CPU *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    static int slow = -1; if (slow < 0) slow = getenv("REBAR_SLOW_CALLBACK") != NULL;
    if (slow) return call_guest(c, fn, a0, a1, a2, a3);
    uint64_t sx[12], ssp = c->sp, spc = c->pc, stick = c->tick, scl = c->code_lo, scn = c->code_len, shsp = c->jit_hsp, shlim = c->jit_hlim; uint32_t sepoch = c->code_epoch; int32_t sbud = c->jit_budget;
    bool sn = c->n, sz = c->z, sc = c->c, sv = c->v; uint64_t sd[8]; for (int k = 0; k < 8; k++) sd[k] = c->vr[8 + k].d[0];
    for (int k = 0; k < 12; k++) sx[k] = c->x[19 + k];
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    c->x[30] = stub_addr(STUB_RET);
    c->pc = fn;
    run_until(c, stub_addr(STUB_RET));
    uint64_t r = c->x[0];
    for (int k = 0; k < 12; k++) c->x[19 + k] = sx[k]; c->sp = ssp; c->pc = spc; c->tick = stick; c->code_lo = scl; c->code_len = scn; c->code_epoch = sepoch; c->jit_hsp = shsp; c->jit_hlim = shlim; c->jit_budget = sbud;
    c->n = sn; c->z = sz; c->c = sc; c->v = sv; for (int k = 0; k < 8; k++) c->vr[8 + k].d[0] = sd[k];
    return r;
}

typedef struct { uint64_t sret; bool mem; int hfa; uint64_t tmp[64]; } EntryState;
static const char *imp_encoding(uint64_t imp);
static void hot_entry(uint64_t imp);
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
    "  movq %rsp, %rbp\n  .cfi_def_cfa_register %rbp\n  subq $224, %rsp\n"
    "  movq %rax, 160(%rsp)\n  movq %r13, 168(%rsp)\n  movq %r12, 176(%rsp)\n  movq %r14, 200(%rsp)\n"
    "  movabsq $0x5ca1ab1e5ca1ab1e, %rax\n  cmpq %rax, %r10\n  sete %al\n  movzbl %al, %eax\n  movq %rax, 208(%rsp)\n"   // (r10 = the magic value: the call came from Swift Concurrency's own code and its call site follows it with `sub $8,%rsp` - the async convention; ordinary Swift code has that instruction after calls too, see the redirect in on_segv)
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
    "  movq 208(%rsp), %r10\n"
    "  leave\n  .cfi_def_cfa %rsp, 8\n  testq %r10, %r10\n  jnz 1f\n  ret\n"
    "1:\n  ret $8\n"   // a Swift async function is entered with a `call` that the caller follows with `subq $8,%rsp`: the callee returns with the stack 8 bytes higher
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
    sigset_t one, prev; sigemptyset(&one); sigaddset(&one, SIGUSR2);
    pthread_sigmask(SIG_UNBLOCK, &one, &prev);   // (a process started with SIGUSR2 blocked, e.g. by a guest's fork+exec, would keep the signal pending and die of it when the guest unblocks the mask later)
    raise(SIGUSR2);
    pthread_sigmask(SIG_SETMASK, &prev, NULL);
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

static __thread bool t_forking; static __thread pid_t t_fork_ppid;   // inside fork() on this thread (set by rebar.c's atfork_prepare): in the child, guest code can run before rebar's own atfork handler (see fork_child_reinit)
static void fork_child_reinit(void);
static int g_tp; static uint64_t fp_now(void); static void tp_entry_note(uint64_t target, uint64_t ns);   // (frameprof.h)
void rebar_bridge_enter(uint64_t *f) {
    if (__builtin_expect(t_forking, 0) && getpid() != t_fork_ppid) fork_child_reinit();   // the child of a guest fork(): libSystem's atfork handlers (xpc) release objects whose dealloc is guest code, before our handler has reset the locks the forking thread held
    if (((uintptr_t)f & 15) != 0 && getenv("REBAR_CHECK_ALIGN")) fprintf(stderr, "[align] guest entry with a misaligned host stack: frame %p target %llx caller-return %llx\n", (void *)f, (unsigned long long)f[14], (unsigned long long)((uint64_t *)f[15])[-1]);
    uint64_t tp_t0 = g_tp ? fp_now() : 0;
    CPU *c = cur_cpu();
    CPU saved = *c;
    int sigent = t_sig_entry; t_sig_entry = 0;
    const uint64_t *stk = (const uint64_t *)f[15];
    const char *enc = imp_encoding(f[14]);
    hot_entry(f[14]);   // (a guest method the host calls through the fault, again and again, gets a thunk: see objc.h)
    static int trace_entry = -1; if (trace_entry < 0) trace_entry = getenv("REBAR_TRACE_ENTRY") != NULL;   // REBAR_TRACE_ENTRY=1: every host call into a guest function (methods: self, selector)
    if (trace_entry) fprintf(stderr, "[entry] %llx%s%s self=%llx sel=%s a2=%llx a3=%llx\n", (unsigned long long)f[14], enc ? " enc=" : "", enc ? enc : "", (unsigned long long)f[0], (f[1] >> 36) == 0x7ff && ((int (*)(void *))dlsym(RTLD_DEFAULT, "sel_isMapped"))((void *)f[1]) ? ((const char *(*)(void *))dlsym(RTLD_DEFAULT, "sel_getName"))((void *)f[1]) : "-", (unsigned long long)f[2], (unsigned long long)f[3]);
    if (trace_entry) { void *ra = ((void **)f[15])[-1]; Dl_info di; fprintf(stderr, "        called from host %p %s+0x%llx (%s)\n", ra, dladdr(ra, &di) && di.dli_sname ? di.dli_sname : "?", dladdr(ra, &di) ? (unsigned long long)((char *)ra - (char *)di.dli_saddr) : 0ull, di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?"); }
    trace_ctx_entry(f[14], f[25], "host", f, f[21]);
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
        c->x[8] = f[20]; c->x[20] = f[21]; c->x[21] = f[22]; c->x[22] = f[25];   // Swift: indirect result address, context, error (all but the error are discarded when the CPU is restored below)
    }
    if (sigent > 0 && sigent < 32 && ((g_sig_onstack >> sigent) & 1) && t_gss_on && !(c->sp > t_gss_sp && c->sp <= t_gss_sp + t_gss_size)) c->sp = (t_gss_sp + t_gss_size) & ~0xfull;
    uint64_t swifterror_in = f[22];
    c->x[30] = stub_addr(STUB_RET);
    c->pc = f[14];
    run_until(c, stub_addr(STUB_RET));
    if (enc) entry_finish(c, f, &st);
    else { f[16] = c->x[0]; f[17] = c->x[1]; f[18] = c->vr[0].d[0]; f[19] = c->vr[1].d[0]; if (swifterror_in == 0) f[22] = c->x[21]; f[23] = c->x[2]; f[24] = c->x[3]; }   // (r12 is the swifterror register of a Swift caller, which clears it before the call; for any other caller it is callee-saved and must come back unchanged: a Swift async function leaves its own value in x21, and the job runner of Swift Concurrency keeps its autorelease pool token in r12)
    if (!enc && fpos_kind) f[16] = f[0];   // (SysV returns the result address in rax)
    t_act = act.prev;
    *c = saved;
    if (held) gil_acquire(); else if (t_have_gil) gil_release();  // an exception that arrived from the host may have taken the lock
    if (tp_t0) tp_entry_note(f[14], fp_now() - tp_t0);
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
// Go's preemption signal (SIGURG). Its handler reads the arm64 ucontext, which a host signal frame does not have, and it must not run on top of host code that holds a lock (a nested run there
// deadlocked go build), so the guest's handler is not the host's: a host handler runs it, at once when the thread is executing guest code, otherwise (inside a call into the host, or holding
// the bridge lock) when that call is done. The handler is given an arm64 ucontext of its own whose pc is 0, a place that is not Go code: it does its bookkeeping (preemptGen,
// pendingPreemptSignals, which exec waits for on Darwin) and leaves the goroutine alone; the scheduler preempts cooperatively.
static uint64_t g_urg_handler;
static __thread siginfo_t t_urg_si;
static void run_urg(CPU *c) {
    CPU saved = *c;
    static __thread uint8_t uctx[64], mctx[816];
    memset(uctx, 0, sizeof uctx); memset(mctx, 0, sizeof mctx);
    *(uint64_t *)(uctx + 48) = (uint64_t)mctx; *(uint64_t *)(uctx + 40) = 816;
    uint64_t *ss = (uint64_t *)(mctx + 16);
    for (int k = 0; k < 31; k++) ss[k] = saved.x[k];
    ss[31] = saved.sp; ss[32] = 0;
    if (t_gss_on && !(c->sp > t_gss_sp && c->sp <= t_gss_sp + t_gss_size)) c->sp = (t_gss_sp + t_gss_size - 256) & ~0xfull; else c->sp = (c->sp - 512) & ~0xfull;
    c->x[0] = SIGURG; c->x[1] = (uint64_t)&t_urg_si; c->x[2] = (uint64_t)uctx;
    c->x[30] = stub_addr(STUB_RET); c->pc = g_urg_handler;
    bool held = gil_release();
    run_until(c, stub_addr(STUB_RET));
    if (held) gil_acquire(); else if (t_have_gil) gil_release();
    *c = saved;
}
static void urg_host_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    CPU *c = t_cpu;
    if (!c || !g_urg_handler) return;
    t_urg_si = *si; t_urg_pending = 1;   // (never run from here: the thread may hold a lock the guest handler's code needs; see urg_deliver_pending)
}
static void urg_deliver_pending(CPU *c) { if (__builtin_expect(t_urg_pending, 0) && !t_have_gil && !t_hc && !t_jit_busy) { t_urg_pending = 0; run_urg(c); } }
static bool guest_cannot_handle(int sig) { static int ign = -1; if (ign < 0) ign = getenv("REBAR_IGNORE_SIGURG") != NULL; return ign && sig == SIGURG; }   // (SIGURG used to be dropped here; see rebar_bridge_enter: its handler is given an arm64 ucontext of its own)
static uint32_t g_guest_fault_flags[3];
H(h_sigaction) {
    int sig = (int)c->x[0];
    if (sig == SIGURG && !getenv("REBAR_IGNORE_SIGURG")) {
        if (c->x[2]) { struct sigaction *o = P(2); memset(o, 0, sizeof *o); o->sa_sigaction = (void (*)(int, siginfo_t *, void *))g_urg_handler; }
        if (c->x[1]) {
            const struct sigaction *n = P(1);
            g_urg_handler = (uint64_t)n->sa_sigaction;
            struct sigaction h; memset(&h, 0, sizeof h);
            if (g_urg_handler <= 1) { h.sa_handler = g_urg_handler ? SIG_IGN : SIG_DFL; g_urg_handler = 0; }
            else { h.sa_sigaction = urg_host_handler; h.sa_flags = SA_SIGINFO | SA_RESTART; }
            sigemptyset(&h.sa_mask); sigaction(SIGURG, &h, NULL);
        }
        RET(0);
        return;
    }
    if (guest_cannot_handle(sig)) {
        if (c->x[2]) memset(P(2), 0, sizeof(struct sigaction));
        RET(0);
        return;
    }
    if (fatal_sig(sig)) {   // the host keeps these (the emulator needs them); a handler the guest registers for SIGSEGV / SIGBUS is called by on_segv, for a fault of the interpreter
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
            int k = sig == SIGSEGV ? 0 : sig == SIGBUS ? 1 : 2;
            if (c->x[2]) { struct sigaction *o = P(2); memset(o, 0, sizeof *o); o->sa_sigaction = (void (*)(int, siginfo_t *, void *))g_guest_fault_handler[k]; o->sa_flags = (int)g_guest_fault_flags[k]; }
            if (c->x[1]) { const struct sigaction *n = P(1); g_guest_fault_handler[k] = (uint64_t)n->sa_sigaction; g_guest_fault_flags[k] = (uint32_t)n->sa_flags; if (g_guest_fault_handler[k] <= 1) g_guest_fault_handler[k] = 0; }
        } else if (c->x[2]) memset(P(2), 0, sizeof(struct sigaction));
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
// A guest that handles hardware faults through a Mach exception port (CoreCLR: its SIGSEGV handler only covers stack overflow, the rest arrives as a Mach message on the handler
// thread's port) registers the port here. A guest memory fault is then delivered as the kernel would: a mach_exception_raise request to the port (the faulting thread's port and the
// task port in it), the guest's handler thread reads and changes the thread's registers with thread_get_state / thread_set_state (answered from the guest CPU, see below) and replies;
// the faulting thread then goes on at the pc the handler left. Ports that only report crashes (Crashpad) are accepted and ignored: they ask for other exceptions or cannot be messaged.
static uint32_t g_mach_exc_port, g_mach_exc_mask, g_mach_exc_behavior;
static void mach_exc_note(CPU *c) {   // (thread / task, exception mask, port, behavior, flavor)
    uint32_t mask = (uint32_t)c->x[1], port = (uint32_t)c->x[2], behavior = (uint32_t)c->x[3];
    if (port && (mask & 2) && (behavior & ~0x80000000u) != 0 && !getenv("REBAR_NO_MACH_EXC")) { g_mach_exc_port = port; g_mach_exc_mask = mask; g_mach_exc_behavior = behavior; }   // 2 = 1 << EXC_BAD_ACCESS
}
H(h_set_exception_ports) { mach_exc_note(c); c->x[0] = 0; }
H(h_swap_exception_ports) { mach_exc_note(c); if (c->x[6]) *(uint32_t *)c->x[6] = 0; c->x[0] = 0; }

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
// A block that came from an allocator of the guest (jemalloc's zone: tmux; the host's strdup / realpath results copied into it, see heapret_fix) and is freed with the system's free: the
// guest zone that knows the block frees it, as libmalloc would find the zone by asking each one for the size.
static bool guest_zone_free(CPU *c, uint64_t p, uint64_t newsize, bool is_realloc) {
    if (!g_nguest_zones || !p || !own_check(p, 1)) return false;
    for (int k = 0; k < g_nguest_zones; k++) {
        uint64_t z = g_guest_zones[k], szfn = *(uint64_t *)(z + 16);
        if (!szfn || !call_guest(c, szfn, z, p, 0, 0)) continue;
        if (is_realloc) { uint64_t fn = *(uint64_t *)(z + 56); if (!fn) return false; c->x[0] = call_guest(c, fn, z, p, newsize, 0); return true; }
        uint64_t fn = *(uint64_t *)(z + 48); if (!fn) return false; call_guest(c, fn, z, p, 0, 0); c->x[0] = 0; return true;
    }
    return false;
}
H(h_free)    { if (guest_zone_free(c, c->x[0], 0, false)) return; uint64_t a[3] = {c->x[0], 0, 0}; if (def_patched(6) && zone_slot_call(c, g_def_zone.clone, 6, 1, a)) return; host_call(c, HOSTFN("free"), false); }
H(h_realloc) { if (guest_zone_free(c, c->x[0], c->x[1], true)) return; uint64_t a[3] = {c->x[0], c->x[1], 0}; if (def_patched(7) && zone_slot_call(c, g_def_zone.clone, 7, 2, a)) return; host_call(c, HOSTFN("realloc"), false); }
H(h_malloc_size) {
    for (int k = 0; k < g_nguest_zones; k++) {   // a guest allocator that registered its own zone (Firefox's mozjemalloc) knows the sizes of its blocks: ask it, as libmalloc would
        uint64_t z = g_guest_zones[k], fn = *(uint64_t *)(z + 16);
        if (fn && c->x[0]) { uint64_t r = call_guest(c, fn, z, c->x[0], 0, 0); if (r) { c->x[0] = r; return; } }
    }
    uint64_t a[3] = {c->x[0], 0, 0}; if (def_patched(2) && zone_slot_call(c, g_def_zone.clone, 2, 1, a)) return; host_call(c, HOSTFN("malloc_size"), false); }

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
// Guest code is interpreted: memory the guest maps or protects needs no execute permission, and with it the host would run the arm64 bytes of a function pointer the guest handed to host
// code (a JNA / libffi closure trampoline, a JIT's stub) as x86: SIGILL (IntelliJ died in a CFRunLoop callback). Without it the jump is an instruction-fetch fault, which on_segv bridges into the
// interpreter (the memory is the guest's: own_add). REBAR_KEEP_EXEC=1 leaves the permissions as they are.
static int guest_prot(int prot) { static int keep = -1; if (keep < 0) keep = getenv("REBAR_KEEP_EXEC") != NULL; return keep ? prot : (prot & ~4); }
static void strip_exec_region(uint64_t addr, uint64_t size) {   // a region the kernel has just made (vm_remap, vm_map): its protection is whatever the source had
    static int keep = -1; if (keep < 0) keep = getenv("REBAR_KEEP_EXEC") != NULL; if (keep || !addr || !size) return;
    for (uint64_t a = addr; a < addr + size;) {
        mach_vm_address_t ra = a; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &on) != KERN_SUCCESS || ra >= addr + size) break;
        if (bi.protection & VM_PROT_EXECUTE) { uint64_t lo = ra < addr ? addr : ra, hi = ra + rs > addr + size ? addr + size : ra + rs; mach_vm_protect(mach_task_self(), lo, hi - lo, false, bi.protection & ~VM_PROT_EXECUTE); }
        a = ra + rs;
    }
}
H(h_mmap) {  // (addr, len, prot, flags, fd, off)
    int flags = (int)c->x[3], prot = (int)c->x[2], fd = (int)c->x[4];
    off_t off = (off_t)c->x[5];
    uint64_t len = (c->x[1] + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    prot = guest_prot(prot);
    void *r = mmap(P(0), len, prot, flags, fd, off);
    if (r == MAP_FAILED && errno == EINVAL && fd >= 0 && !(flags & MAP_ANON)) {   // a POSIX shared memory object is rounded to the host's 4 KB pages, not to the 16 KB the arm64 guest assumes (multiprocessing.shared_memory maps 64 bytes): map what the object holds
        uint64_t len4 = (c->x[1] + 4095) & ~4095ull;
        if (len4 < len) { r = mmap(P(0), len4, prot, flags, fd, off); if (r != MAP_FAILED) len = len4; }
    }
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
    if (r == MAP_FAILED && errno == EINVAL && c->x[1] >= (1ull << 47)) errno = ENOMEM;   // (a length no address space holds: the arm64 kernel says ENOMEM where this one says EINVAL)
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
// Where rebar's support files live (librebar_exec.dylib, librebar_sec.dylib): next to the executable, or in REBAR_LIB_DIR (an iOS app runs from a copy of rebar inside a host .app).
static void rebar_lib_path(const char *name, char *out, size_t n) {
    const char *d = getenv("REBAR_LIB_DIR");
    if (d) { snprintf(out, n, "%s/%s", d, name); return; }
    snprintf(out, n, "%s", g_self_path); char *slash = strrchr(out, '/');
    if (slash) snprintf(slash + 1, n - (size_t)(slash + 1 - out), "%s", name); else snprintf(out, n, "%s", name);
}
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
        bool arm = false, arme = false, x86 = false;
        uint32_t na = __builtin_bswap32(fh->nfat_arch);
        for (uint32_t k = 0; k < na && (const uint8_t *)(fa + k + 1) <= buf + n; k++) {
            cpu_type_t t = (cpu_type_t)__builtin_bswap32(fa[k].cputype);
            if (t == CPU_TYPE_ARM64) { if ((__builtin_bswap32(fa[k].cpusubtype) & 0xff) == 2) arme = true; else arm = true; }   // (arm64e: pointer authentication is a no-op here; a file with arm64e and x86_64 slices, as the system tools, runs natively)
            if (t == CPU_TYPE_X86_64) x86 = true;
        }
        arm = arm || arme;
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
    static __thread pid_t t_spawn_pid; if (!c->x[0]) c->x[0] = (uint64_t)&t_spawn_pid;   // (a NULL pid pointer is allowed on arm64 macOS, the x86_64 kernel answers EFAULT)
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
    const char *spath = (const char *)c->x[1];
    if (search) spawn = HOSTFN("posix_spawnp");   // (a program that is not arm64 is found by the host's own PATH search)
    if (getenv("REBAR_LOG_SPAWN")) { FILE *lf = fopen(getenv("REBAR_LOG_SPAWN"), "a"); if (lf) { fprintf(lf, "[spawn %d] %s%s argv:", getpid(), spath, search ? " (search)" : ""); for (char *const *a = (char *const *)c->x[4]; a && *a; a++) fprintf(lf, " '%s'", *a); fprintf(lf, "\n"); fclose(lf); } }
    int r = spawn((pid_t *)c->x[0], spath, (const void *)c->x[2], (const void *)c->x[3], (char *const *)c->x[4], env);
    env_free_shim(env, (char *const *)c->x[5]);
    RET(r);
}
H(h_posix_spawnattr_setbinpref_np) { if (c->x[3]) *(uint64_t *)c->x[3] = c->x[1]; c->x[0] = 0; }   // (the guest asks for arm64; what rebar starts is its own x86_64 binary, the arm64 file is run by it)
H(h_posix_spawn)  { do_spawn(c, false); }
H(h_posix_spawnp) { do_spawn(c, true); }
static void do_exec(CPU *c, const char *file, char *const *argv, char *const *envp, bool search) {
    char resolved[PATH_MAX];
    char **v = NULL;
    if (getenv("REBAR_LOG_SPAWN")) { FILE *lf = fopen(getenv("REBAR_LOG_SPAWN"), "a"); if (lf) { fprintf(lf, "[exec %d] %s%s argv:", getpid(), file, search ? " (search)" : ""); for (char *const *a = argv; a && *a; a++) fprintf(lf, " '%s'", *a); fprintf(lf, "\n"); fclose(lf); } }
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
H(h_clear_cache) { jit_invalidate(c->x[0], c->x[1]); }   // __clear_cache(begin, end): HotSpot patches its generated code and calls this
// A guest that suspends another thread (Chromium's sampling profiler suspends the browser's main thread every few seconds) must not freeze it inside the
// translator: the suspender translates blocks too and would wait for the lock that the frozen thread holds, for ever. With the lock held across the
// suspension the target cannot be inside jit_compile / jit_invalidate when it stops.
static CPU *mach_faulting_cpu(uint32_t port, uint64_t *far);
H(h_thread_suspend) {
    if (mach_faulting_cpu((uint32_t)c->x[0], NULL)) { c->x[0] = 0; return; }   // (a thread that waits for its exception reply is stopped already)
    pthread_mutex_lock(&g_jit_mu);
    c->x[0] = (uint32_t)thread_suspend((thread_act_t)c->x[0]);
    pthread_mutex_unlock(&g_jit_mu);
}
H(h_munmap) { bool ok = own_check(c->x[0], c->x[1]); trace_mem(ok ? "munmap" : "munmap (ignored)", c->x[0], c->x[1], 0, 0, 0, c->x[30]); if (!ok) { RET(0); return; } own_remove(c->x[0], c->x[1]); RET(munmap(P(0), c->x[1])); }
H(h_mprotect) { bool ok = own_check(c->x[0], c->x[1]); trace_mem(ok ? "mprotect" : "mprotect (ignored)", c->x[0], c->x[1], c->x[2], 0, 0, c->x[30]); if (!ok) { RET(0); return; } RET(mprotect(P(0), c->x[1], guest_prot((int)c->x[2]))); }
H(h_madvise) { bool ok = own_check(c->x[0], c->x[1]); trace_mem(ok ? "madvise" : "madvise (ignored)", c->x[0], c->x[1], c->x[2], 0, 0, c->x[30]); if (!ok) { RET(0); return; } RET(madvise(P(0), c->x[1], (int)c->x[2])); }
H(h_vm_protect) {  // (task, addr, size, set_maximum, prot) - also used for mach_vm_protect
    if (!own_check(c->x[1], c->x[2])) { RET(0); return; }
    kern_return_t (*f)(uint64_t, uint64_t, uint64_t, int, int) = HOSTFN("mach_vm_protect");
    RET(f(c->x[0], c->x[1], c->x[2], (int)c->x[3], guest_prot((int)c->x[4])));
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
H(h_vsyslog) { VA_INIT(ap, c->x[2]); vsyslog((int)c->x[0], S(1), ap); c->x[0] = 0; }
// Swift's withVaList / CVarArg: the guest inlines the constructor and append of __VaListBuilder, which build the object in the arm64 layout (count at +0x18, capacity at +0x20, the
// words at +0x28 behind a pointer: every variadic argument is one 8-byte word, in order, as on the arm64 stack), but the class (its metadata, va_list(), deinit) is the host's own, laid
// out for x86_64 (a register save area header at +0x10, the storage reference at +0x28): the host's va_list() read the wrong fields and its deinit released the guest's malloc'd buffer as a Swift
// object (SwiftPM's sqlite3_vmprintf calls: Xcode died when a new project was created). This replaces va_list(): it makes an x86_64 va_list that has every word in the overflow area (gp and fp
// registers marked used), in the order of the arm64 stack, and empties the builder's fields so that the host's deinit finds nothing to release. The blocks are kept in a ring of 32 per thread.
H(h_VaListBuilder_va_list) {
    uint64_t obj = c->x[20];
    if (!obj || obj < 0x10000 || in_guest(obj)) { c->x[0] = 0; return; }
    uint64_t *o = (uint64_t *)obj, n = o[3], base = o[5];
    if (n > (1u << 20)) n = 0;
    uint8_t *blk = calloc(1, 24 + 176 + (n + 1) * 8);
    struct { uint32_t gp, fp; void *over, *regs; } *t = (void *)blk;
    uint8_t *words = blk + 24 + 176;
    if (n && base) memcpy(words, (void *)base, n * 8);
    t->gp = 48; t->fp = 176; t->over = words; t->regs = blk + 24;
    o[3] = o[4] = o[5] = 0;
    static __thread struct { uint8_t *blk; uint64_t base; } ring[32]; static __thread unsigned rn;
    unsigned k = rn++ & 31; free(ring[k].blk); if (ring[k].base) free((void *)ring[k].base);
    ring[k].blk = blk; ring[k].base = base;
    c->x[0] = (uint64_t)t;
}
// ... and the guest allocates the builder with the size its own arm64 class has (0x30), the host's x86_64 class is 0x40 bytes: its deinit reads the storage field behind the block. swift_allocObject
// gives that class the host's size (checked against the metadata, so every other Swift allocation goes through unchanged).
H(h_swift_allocObject) {
    static uint64_t vmeta; static bool tried;
    if (__builtin_expect(!tried, 0)) { vmeta = (uint64_t)dlsym(RTLD_DEFAULT, "$ss15__VaListBuilderCN"); tried = true; }
    if (__builtin_expect(c->x[0] == vmeta && vmeta && c->x[1] < 0x40, 0)) c->x[1] = 0x40;
    host_call(c, HOSTFN("swift_allocObject"), false);
}
// asl_log(client, msg, level, format, ...): the variadic arguments are on the guest stack (GPUToolsCore passed a pointer through the registers it left behind)
// swift_dynamicCast whose source is an Any container with no type metadata: the type word holds a small number, one of the "no payload" encodings of an Optional (the other words are leftovers of the stack).
// Code that tests only for nil (word == 0) took it for a value (Xcode: Combine's CompactMap handed IDEDelta's `workspaceHolder` closure such an Any?, the runtime read a vtable at address type-8 and died when
// a source file was opened). Casting nothing to a class can only fail, which is what the closure's `as?` is for; the source is left alone (a destroy of it would crash as well).
// REBAR_TRACE_CAST=1 reports every cast of an Any, REBAR_NO_CAST_GUARD=1 turns the guard off.
H(h_swift_dynamicCast) {
    static uint64_t any_md; if (!any_md) { void *p = dlsym(RTLD_DEFAULT, "$sypN"); any_md = p ? (uint64_t)p + 8 : 1; }
    static int tr = -1, off = 0; if (tr < 0) { tr = getenv("REBAR_TRACE_CAST") != NULL; off = getenv("REBAR_NO_CAST_GUARD") != NULL; }
    if (c->x[2] == any_md && c->x[1]) {
        uint64_t *w = (uint64_t *)c->x[1];
        if (tr) fprintf(stderr, "[dynamicCast of Any: src=%llx type word=%llx target=%llx flags=%llx lr=%llx]\n", (unsigned long long)c->x[1], (unsigned long long)w[3], (unsigned long long)c->x[3], (unsigned long long)c->x[4], (unsigned long long)c->x[30]);
        if (w[3] < 0x1000 && !off) { c->x[0] = 0; return; }
    }
    bool (*f)(void *, void *, const void *, const void *, uint64_t) = (bool (*)(void *, void *, const void *, const void *, uint64_t))dlsym(RTLD_DEFAULT, "swift_dynamicCast");
    c->x[0] = f((void *)c->x[0], (void *)c->x[1], (const void *)c->x[2], (const void *)c->x[3], c->x[4]) ? 1 : 0;
}
H(h_asl_log)  { VA_INIT(ap, c->sp); int (*f)(void *, void *, int, const char *, va_list) = dlsym(RTLD_DEFAULT, "asl_vlog"); c->x[0] = f ? (uint64_t)(int64_t)f((void *)c->x[0], (void *)c->x[1], (int)c->x[2], S(3), ap) : 0; }
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
// libxml2's printf-style writer functions (and xmlStrPrintf): the variadic arguments are on the guest's stack, so they go to the host's v-variant with a va_list that points there. `nf` is the
// number of fixed pointer arguments including the format.
#define XML_VFWD(handler, vname, nf) \
    H(handler) { VA_INIT(ap, c->sp); void *f = HOSTFN(vname); \
        switch (nf) { \
        case 2: RET(((int (*)(void *, const void *, va_list))f)(P(0), P(1), ap)); break; \
        case 3: RET(((int (*)(void *, const void *, const void *, va_list))f)(P(0), P(1), P(2), ap)); break; \
        case 4: RET(((int (*)(void *, const void *, const void *, const void *, va_list))f)(P(0), P(1), P(2), P(3), ap)); break; \
        default: RET(((int (*)(void *, const void *, const void *, const void *, const void *, va_list))f)(P(0), P(1), P(2), P(3), P(4), ap)); break; } }
XML_VFWD(h_xmlw_fmt_string, "xmlTextWriterWriteVFormatString", 2)
XML_VFWD(h_xmlw_fmt_raw, "xmlTextWriterWriteVFormatRaw", 2)
XML_VFWD(h_xmlw_fmt_comment, "xmlTextWriterWriteVFormatComment", 2)
XML_VFWD(h_xmlw_fmt_cdata, "xmlTextWriterWriteVFormatCDATA", 2)
XML_VFWD(h_xmlw_fmt_element, "xmlTextWriterWriteVFormatElement", 3)
XML_VFWD(h_xmlw_fmt_attribute, "xmlTextWriterWriteVFormatAttribute", 3)
XML_VFWD(h_xmlw_fmt_pi, "xmlTextWriterWriteVFormatPI", 3)
XML_VFWD(h_xmlw_fmt_element_ns, "xmlTextWriterWriteVFormatElementNS", 5)
XML_VFWD(h_xmlw_fmt_attribute_ns, "xmlTextWriterWriteVFormatAttributeNS", 5)
XML_VFWD(h_xmlw_fmt_dtd, "xmlTextWriterWriteVFormatDTD", 5)
XML_VFWD(h_xmlw_fmt_dtd_element, "xmlTextWriterWriteVFormatDTDElement", 3)
XML_VFWD(h_xmlw_fmt_dtd_attlist, "xmlTextWriterWriteVFormatDTDAttlist", 3)
XML_VFWD(h_xmlw_fmt_dtd_entity, "xmlTextWriterWriteVFormatDTDInternalEntity", 4)
H(h_xmlStrPrintf) { VA_INIT(ap, c->sp); int (*f)(void *, int, const void *, va_list) = HOSTFN("xmlStrVPrintf"); RET(f(P(0), (int)c->x[1], P(2), ap)); }
// sqlite3_mprintf / sqlite3_snprintf / sqlite3_str_appendf: the variadic arguments are on the arm64 stack (Logic Pro's Alchemy preset database faulted in the host's formatter)
H(h_sqlite3_mprintf)    { VA_INIT(ap, c->sp); char *(*f)(const char *, va_list) = HOSTFN("sqlite3_vmprintf"); c->x[0] = (uint64_t)f(S(0), ap); }
H(h_sqlite3_snprintf)   { VA_INIT(ap, c->sp); char *(*f)(int, char *, const char *, va_list) = HOSTFN("sqlite3_vsnprintf"); c->x[0] = (uint64_t)f((int)c->x[0], P(1), S(2), ap); }
H(h_sqlite3_str_appendf){ VA_INIT(ap, c->sp); void (*f)(void *, const char *, va_list) = HOSTFN("sqlite3_str_vappendf"); f(P(0), S(1), ap); }
// CFCalendarDecomposeAbsoluteTime(cal, at, desc, ...) / CFCalendarComposeAbsoluteTime(cal, &at, desc, ...) / CFCalendarAddComponents(cal, &at, options, desc, ...): one variadic argument
// per letter of the descriptor, in the arm64 stack slots (Logic Pro's audio unit support formats dates with them; the host read its out-pointers from registers that held other things).
typedef bool (*CalVarFn)(void *, ...);
static void calendar_var(CPU *c, int mode) {
    const char *desc = S(mode == 2 ? 3 : 2 - (mode == 0 ? 1 : 0));   // Decompose: desc in x1; Compose: x2; Add: x3
    const uint64_t *st = (const uint64_t *)c->sp; uint64_t v[8] = {0};
    size_t n = desc ? strlen(desc) : 0; if (n > 8) n = 8;
    for (size_t k = 0; k < n; k++) v[k] = st[k];
    const char *names[3] = {"CFCalendarDecomposeAbsoluteTime", "CFCalendarComposeAbsoluteTime", "CFCalendarAddComponents"};
    void *f = HOSTFN(names[mode]); if (!f) { c->x[0] = 0; return; }
    bool r;
    if (mode == 0) r = ((bool (*)(void *, double, const char *, ...))f)(P(0), c->vr[0].d[0], desc, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    else if (mode == 1) r = ((bool (*)(void *, void *, const char *, ...))f)(P(0), P(1), desc, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    else r = ((bool (*)(void *, void *, uint64_t, const char *, ...))f)(P(0), P(1), c->x[2], desc, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    c->x[0] = r;
}
H(h_CFCalendarDecomposeAbsoluteTime) { calendar_var(c, 0); }
H(h_CFCalendarComposeAbsoluteTime)   { calendar_var(c, 1); }
H(h_CFCalendarAddComponents)         { calendar_var(c, 2); }
// swift_bridgeObjectRetain / Release / _n of a value that cannot be a pointer (a non-zero value below 0x10000: Logic Pro's AppAnalytics retained a String whose object word was 2,
// read from the context of an async function that had been given another value there): the host's version dereferences it (SIGSEGV at 8). Such a value is no object, so nothing is retained
// (REBAR_NO_BRIDGEOBJ_GUARD=1: call the host function as before).
static bool bridge_obj_bogus(uint64_t p) { static int off = -1; if (off < 0) off = getenv("REBAR_NO_BRIDGEOBJ_GUARD") != NULL; return !off && p != 0 && p < 0x10000; }
H(h_swift_bridgeObjectRetain)    { if (bridge_obj_bogus(c->x[0])) return; host_call(c, HOSTFN("swift_bridgeObjectRetain"), false); }
H(h_swift_bridgeObjectRelease)   { if (bridge_obj_bogus(c->x[0])) return; host_call(c, HOSTFN("swift_bridgeObjectRelease"), false); }
H(h_swift_bridgeObjectRetain_n)  { if (bridge_obj_bogus(c->x[0])) return; host_call(c, HOSTFN("swift_bridgeObjectRetain_n"), false); }
H(h_swift_bridgeObjectRelease_n) { if (bridge_obj_bogus(c->x[0])) return; host_call(c, HOSTFN("swift_bridgeObjectRelease_n"), false); }
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
    return (int)call_guest_short(x->c, x->fn, x->has_thunk ? x->thunk : (uint64_t)a, x->has_thunk ? (uint64_t)a : (uint64_t)b,
                           x->has_thunk ? (uint64_t)b : 0, 0);
}
H(h_qsort)   { CbCtx x = {c, c->x[3], 0, false}; qsort_r(P(0), c->x[1], c->x[2], &x, qsort_cb); }
H(h_qsort_r) { CbCtx x = {c, c->x[4], c->x[3], true}; qsort_r(P(0), c->x[1], c->x[2], &x, qsort_cb); }
static void h_system_error_ctor(CPU *c);
H(h_bsearch) {
    uint64_t key = c->x[0], base = c->x[1], n = c->x[2], w = c->x[3], fn = c->x[4];
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2, el = base + mid * w;
        int r = (int)call_guest_short(c, fn, key, el, 0, 0);
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
    if (name && (!strcmp(name, "hw.pagesize") || !strcmp(name, "vm.pagesize") || !strcmp(name, "hw.pagesize32")) && !getenv("REBAR_NO_ARCH_SPOOF")) {   // Apple Silicon has 16 KiB pages (fastfetch and htop shift page counts by it)
        size_t *lenp = (size_t *)c->x[2]; size_t want = lenp ? *lenp : 0;
        if (c->x[1] && lenp) { if (want >= 8) *(int64_t *)c->x[1] = GUEST_PAGE; else if (want >= 4) *(int32_t *)c->x[1] = GUEST_PAGE; }
        if (lenp && *lenp != 4 && *lenp < 8) *lenp = 4; else if (lenp && *lenp > 8) *lenp = 8;
        RET(0); return;
    }
    if (name && !strcmp(name, "hw.machine") && !getenv("REBAR_NO_ARCH_SPOOF")) {   // an arm64 program is told it runs on arm64 (pip picks wheels by it; Python's platform.machine() asks uname)
        size_t *lenp = (size_t *)c->x[2]; if (c->x[1] && lenp && *lenp >= 6) memcpy((void *)c->x[1], "arm64", 6); if (lenp) *lenp = 6; RET(0); return;
    }
    host_call(c, HOSTFN("sysctlbyname"), false);
}
// objc_retain_x0 .. objc_retain_x28 / objc_release_x0 .. objc_release_x28: libobjc's register variants (code built for iOS 16 / macOS 13 and later calls them to save the `mov x0, xN`);
// the object is in xN, otherwise they act like objc_retain / objc_release (retain returns the object in x0)
#define XSTUB(N) H(h_objc_retain_x##N) { c->x[0] = c->x[N]; host_call(c, HOSTFN("objc_retain"), false); } H(h_objc_release_x##N) { c->x[0] = c->x[N]; host_call(c, HOSTFN("objc_release"), false); }
XSTUB(0) XSTUB(1) XSTUB(2) XSTUB(3) XSTUB(4) XSTUB(5) XSTUB(6) XSTUB(7) XSTUB(8) XSTUB(9) XSTUB(10) XSTUB(11) XSTUB(12) XSTUB(13) XSTUB(14) XSTUB(15) XSTUB(16) XSTUB(17) XSTUB(18) XSTUB(19)
XSTUB(20) XSTUB(21) XSTUB(22) XSTUB(23) XSTUB(24) XSTUB(25) XSTUB(26) XSTUB(27) XSTUB(28)
#define XREF(N) h_objc_retain_x##N, h_objc_release_x##N,
static StubHandler const k_objc_x[] = { XREF(0) XREF(1) XREF(2) XREF(3) XREF(4) XREF(5) XREF(6) XREF(7) XREF(8) XREF(9) XREF(10) XREF(11) XREF(12) XREF(13) XREF(14) XREF(15) XREF(16) XREF(17) XREF(18) XREF(19)
    XREF(20) XREF(21) XREF(22) XREF(23) XREF(24) XREF(25) XREF(26) XREF(27) XREF(28) };
static StubHandler objc_x_handler(const char *name) {
    int rel = !strncmp(name, "_objc_release_x", 15); if (!rel && strncmp(name, "_objc_retain_x", 14)) return NULL;
    const char *d = name + (rel ? 15 : 14); char *end; long n = strtol(d, &end, 10);
    return (*d && !*end && n >= 0 && n <= 28) ? k_objc_x[n * 2 + rel] : NULL;
}
// The host counts pages in 4 KiB units; an arm64 program expects the 16 KiB pages of Apple Silicon (hw.pagesize above), so the page counts of the VM statistics are divided by 4
// (otherwise "free + cached pages << 14" is four times the memory and a program computing used = total - that underflows).
H(h_host_page_size) { if (c->x[1]) *(uint64_t *)c->x[1] = GUEST_PAGE; RET(0); }
static void scale_vm_stats(CPU *c, int flavor, void *info) {
    if ((int)c->x[0] != 0 || !info) return;
    if (flavor == 4) { vm_statistics64_data_t *v = info; v->free_count /= 4; v->active_count /= 4; v->inactive_count /= 4; v->wire_count /= 4; v->purgeable_count /= 4; v->speculative_count /= 4;
        v->compressor_page_count /= 4; v->throttled_count /= 4; v->external_page_count /= 4; v->internal_page_count /= 4; }
    else if (flavor == 2) { vm_statistics_data_t *v = info; v->free_count /= 4; v->active_count /= 4; v->inactive_count /= 4; v->wire_count /= 4; v->purgeable_count /= 4; v->speculative_count /= 4; }
}
H(h_host_statistics64) { int fl = (int)c->x[1]; void *info = (void *)c->x[2]; host_call(c, HOSTFN("host_statistics64"), false); if (!getenv("REBAR_NO_ARCH_SPOOF")) scale_vm_stats(c, fl, info); }
H(h_host_statistics)   { int fl = (int)c->x[1]; void *info = (void *)c->x[2]; host_call(c, HOSTFN("host_statistics"), false);   if (!getenv("REBAR_NO_ARCH_SPOOF")) scale_vm_stats(c, fl, info); }
H(h_uname) {   // struct utsname: five fields of 256 bytes, machine is the fifth
    uint64_t buf = c->x[0];
    host_call(c, HOSTFN("uname"), false);
    if ((int)c->x[0] == 0 && buf && !getenv("REBAR_NO_ARCH_SPOOF")) memcpy((char *)buf + 4 * 256, "arm64", 6);
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
H(h_Unwind_GetCFA); H(h_Unwind_SetGR); H(h_Unwind_SetIP); H(h_Unwind_GetIP); H(h_Unwind_GetGR); H(h_Unwind_GetIPInfo); H(h_Unwind_GetLanguageSpecificData); H(h_Unwind_GetRegionStart); H(h_Unwind_GetDataRelBase); H(h_Unwind_GetTextRelBase);
H(h_objc_getProtocol); H(h_NSProtocolFromString); H(h_objc_alloc_init); H(h_objc_alloc); H(h_objc_opt_new); H(h_objc_allocWithZone);
H(h_vfork); H(h_objc_claimAutoreleasedReturnValue); H(h_objc_autoreleaseReturnValue); H(h_objc_retainAutoreleasedReturnValue); H(h_objc_unsafeClaimAutoreleasedReturnValue); H(h_uname); H(h_thread_get_state); H(h_thread_set_state); H(h_thread_resume); H(h_posix_spawnattr_setbinpref_np); H(h_SSLSetIOFuncs); H(h_SecItemAdd); H(h_SecItemCopyMatching); H(h_SecItemUpdate); H(h_SecItemDelete); H(h_CGGetActiveDisplayList); H(h_CVDisplayLinkCreateWithActiveCGDisplays); H(h_CGDisplayIsActive); H(h_unw_getcontext); H(h_unw_init_local); H(h_unw_step); H(h_unw_get_reg); H(h_unw_set_reg); H(h_unw_get_fpreg); H(h_unw_set_fpreg); H(h_unw_is_signal_frame);
H(h_abort_report); H(h_JRSFontGetBoundingBoxesForGlyphsAndStyle);
H(h_CFBundleGetFunctionPointerForName); H(h_CFBundleGetDataPointerForName);
H(h_dlopen); H(h_dlsym); H(h_dlclose); H(h_dladdr); H(h_dlerror);
H(h_dyld_image_count); H(h_dyld_get_image_name); H(h_dyld_get_image_header);
H(h_dyld_get_image_vmaddr_slide); H(h_dyld_register_func_for_add_image);
typedef struct { const char *name; StubHandler h; } Special;
H(h_path_begin_end); H(h_path_iter_step);
H(h_ffi_call); H(h_ffi_prep_cif_var); H(h_ffi_prep_cif);
// JavaRuntimeSupport (JBR's AWT font code): JRSFontGetBoundingBoxesForGlyphsAndStyle(font, matrix, style, glyphs, count, rects) of this macOS dereferences its style argument (the rendering style
// the arm64 JBR passes is the value 3: SIGSEGV at address 3 inside the host function). The boxes are asked of CoreText instead and moved by the matrix. REBAR_NO_JRS_BBOX=1 calls the host function.
H(h_JRSFontGetBoundingBoxesForGlyphsAndStyle) {
    static int off = -1; if (off < 0) off = getenv("REBAR_NO_JRS_BBOX") != NULL;
    typedef struct { double a, b, c, d, tx, ty; } Aff; typedef struct { double x, y, w, h; } Rc;
    uint64_t font = c->x[0]; const Aff *m = (const Aff *)c->x[1]; const uint16_t *glyphs = (const uint16_t *)c->x[3]; size_t n = c->x[4]; Rc *rects = (Rc *)c->x[5];
    Rc (*bb)(uint64_t, int, const uint16_t *, Rc *, long) = dlsym(RTLD_DEFAULT, "CTFontGetBoundingRectsForGlyphs");   // (it returns the union rectangle: a CGRect is returned in memory, through a hidden first argument: declared as void it wrote that rectangle over the font object)
    if (off || !bb || !rects || !glyphs) { host_call(c, HOSTFN("JRSFontGetBoundingBoxesForGlyphsAndStyle"), false); return; }
    (void)bb(font, 0, glyphs, rects, (long)n);
    if (m) for (size_t i = 0; i < n; i++) {   // (the four corners of the box, moved by the matrix: their bounds)
        double xs[4] = {rects[i].x, rects[i].x + rects[i].w, rects[i].x, rects[i].x + rects[i].w}, ys[4] = {rects[i].y, rects[i].y, rects[i].y + rects[i].h, rects[i].y + rects[i].h};
        double lo_x = 1e300, hi_x = -1e300, lo_y = 1e300, hi_y = -1e300;
        for (int k = 0; k < 4; k++) { double X = m->a * xs[k] + m->c * ys[k] + m->tx, Y = m->b * xs[k] + m->d * ys[k] + m->ty; if (X < lo_x) lo_x = X; if (X > hi_x) hi_x = X; if (Y < lo_y) lo_y = Y; if (Y > hi_y) hi_y = Y; }
        rects[i] = (Rc){lo_x, lo_y, hi_x - lo_x, hi_y - lo_y};
    }
    c->x[0] = 1;
}
H(h_abort_report) {   // (the guest calls abort(): say where from, then let the host do it)
    fprintf(stderr, "rebar: guest called abort() (pid %d) from lr 0x%llx\n", (int)getpid(), (unsigned long long)c->x[30]);
    dump_recent_imports();
    host_call(c, HOSTFN("abort"), false);
}
static const Special k_special[] = {
    {"_ffi_call", h_ffi_call}, {"_ffi_prep_cif_var", h_ffi_prep_cif_var}, {"_ffi_prep_cif", h_ffi_prep_cif}, {"__ZNKSt3__14__fs10filesystem4path5beginEv", h_path_begin_end}, {"__ZNKSt3__14__fs10filesystem4path3endEv", h_path_begin_end}, {"__ZNSt3__14__fs10filesystem4path8iterator11__incrementEv", h_path_iter_step}, {"__ZNSt3__14__fs10filesystem4path8iterator11__decrementEv", h_path_iter_step},
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
    {"_mmap", h_mmap}, {"_sys_icache_invalidate", h_sys_icache_invalidate}, {"___clear_cache", h_clear_cache}, {"_thread_suspend", h_thread_suspend}, {"___chkstk_darwin", h_chkstk_darwin}, {"_sysctlbyname", h_sysctlbyname}, {"_host_page_size", h_host_page_size}, {"_host_statistics64", h_host_statistics64}, {"_host_statistics", h_host_statistics},
    {"_SSLGetEnabledCiphers", h_SSLGetEnabledCiphers}, {"_SSLGetSupportedCiphers", h_SSLGetSupportedCiphers}, {"_SSLSetEnabledCiphers", h_SSLSetEnabledCiphers}, {"_SSLGetNegotiatedCipher", h_SSLGetNegotiatedCipher}, {"_munmap", h_munmap}, {"_mprotect", h_mprotect}, {"_madvise", h_madvise},
    {"_vm_protect", h_vm_protect}, {"_mach_vm_protect", h_vm_protect}, {"_vm_allocate", h_vm_allocate},
    {"_mach_vm_allocate", h_vm_allocate}, {"_vm_deallocate", h_vm_deallocate}, {"_mach_vm_deallocate", h_vm_deallocate},
    {"_vfork", h_vfork}, {"_objc_claimAutoreleasedReturnValue", h_objc_claimAutoreleasedReturnValue}, {"_objc_autoreleaseReturnValue", h_objc_autoreleaseReturnValue}, {"_objc_retainAutoreleasedReturnValue", h_objc_retainAutoreleasedReturnValue}, {"_objc_unsafeClaimAutoreleasedReturnValue", h_objc_unsafeClaimAutoreleasedReturnValue}, {"_uname", h_uname}, {"_thread_get_state", h_thread_get_state}, {"_thread_set_state", h_thread_set_state}, {"_thread_resume", h_thread_resume}, {"_posix_spawnattr_setbinpref_np", h_posix_spawnattr_setbinpref_np}, {"_SSLSetIOFuncs", h_SSLSetIOFuncs}, {"_SecItemAdd", h_SecItemAdd}, {"_SecItemCopyMatching", h_SecItemCopyMatching}, {"_SecItemUpdate", h_SecItemUpdate}, {"_SecItemDelete", h_SecItemDelete}, {"_CGGetActiveDisplayList", h_CGGetActiveDisplayList}, {"_CVDisplayLinkCreateWithActiveCGDisplays", h_CVDisplayLinkCreateWithActiveCGDisplays}, {"_CGDisplayIsActive", h_CGDisplayIsActive}, {"_unw_getcontext", h_unw_getcontext}, {"_unw_init_local", h_unw_init_local}, {"_unw_step", h_unw_step}, {"_unw_get_reg", h_unw_get_reg}, {"_unw_set_reg", h_unw_set_reg}, {"_unw_get_fpreg", h_unw_get_fpreg}, {"_unw_set_fpreg", h_unw_set_fpreg}, {"_unw_is_signal_frame", h_unw_is_signal_frame}, {"_objc_getProtocol", h_objc_getProtocol}, {"_objc_alloc_init", h_objc_alloc_init}, {"_UIApplicationMain", h_UIApplicationMain}, {"_objc_alloc", h_objc_alloc}, {"_objc_opt_new", h_objc_opt_new}, {"_objc_allocWithZone", h_objc_allocWithZone}, {"_NSProtocolFromString", h_NSProtocolFromString}, {"_open", h_open}, {"_openat", h_openat}, {"_fcntl", h_fcntl}, {"_ioctl", h_ioctl}, {"_semctl", h_semctl}, {"_shm_open", h_shm_open}, {"_sem_open", h_sem_open}, {"_syslog", h_syslog}, {"_vsyslog", h_vsyslog}, {"_asl_log", h_asl_log}, {"_swift_allocObject", h_swift_allocObject}, {"_$ss15__VaListBuilderC7va_lists03CVaB7PointerVyF", h_VaListBuilder_va_list}, {"_syslog$DARWIN_EXTSN", h_syslog}, {"_execl", h_execl}, {"_xmlTextWriterWriteFormatString", h_xmlw_fmt_string}, {"_xmlTextWriterWriteFormatRaw", h_xmlw_fmt_raw}, {"_xmlTextWriterWriteFormatComment", h_xmlw_fmt_comment}, {"_xmlTextWriterWriteFormatCDATA", h_xmlw_fmt_cdata}, {"_xmlTextWriterWriteFormatElement", h_xmlw_fmt_element}, {"_xmlTextWriterWriteFormatAttribute", h_xmlw_fmt_attribute}, {"_xmlTextWriterWriteFormatPI", h_xmlw_fmt_pi}, {"_xmlTextWriterWriteFormatElementNS", h_xmlw_fmt_element_ns}, {"_xmlTextWriterWriteFormatAttributeNS", h_xmlw_fmt_attribute_ns}, {"_xmlTextWriterWriteFormatDTD", h_xmlw_fmt_dtd}, {"_xmlTextWriterWriteFormatDTDElement", h_xmlw_fmt_dtd_element}, {"_xmlTextWriterWriteFormatDTDAttlist", h_xmlw_fmt_dtd_attlist}, {"_xmlTextWriterWriteFormatDTDInternalEntity", h_xmlw_fmt_dtd_entity}, {"_xmlStrPrintf", h_xmlStrPrintf}, {"_curl_easy_setopt", h_curl_easy_setopt}, {"_curl_easy_getinfo", h_curl_easy_getinfo}, {"_curl_multi_setopt", h_curl_multi_setopt}, {"_curl_share_setopt", h_curl_share_setopt}, {"_sqlite3_db_config", h_sqlite3_db_config}, {"_sqlite3_config", h_sqlite3_config}, {"_execlp", h_execlp}, {"_execle", h_execle},
    {"_open$NOCANCEL", h_open}, {"_openat$NOCANCEL", h_openat}, {"_fcntl$NOCANCEL", h_fcntl},   // (the mode / argument of these variadic functions is on the stack on arm64, not in x2 / x3: Bun, Zig and Rust call the $NOCANCEL forms directly)
    {"_exit", h_exit}, {"_atexit", h_atexit}, {"___cxa_atexit", h_cxa_atexit}, {"___cxa_finalize", h_nop},
    {"__ZNSt3__112system_errorC2ENS_10error_codeERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", h_system_error_ctor}, {"__ZNSt3__112system_errorC1ENS_10error_codeERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE", h_system_error_ctor},
    {"_qsort", h_qsort}, {"_qsort_r", h_qsort_r}, {"_bsearch", h_bsearch}, {"__ZNKSt3__14__fs10filesystem18directory_iterator13__dereferenceEv", h_dir_deref}, {"__ZNKSt3__14__fs10filesystem28recursive_directory_iterator13__dereferenceEv", h_rdir_deref}, {"_tsearch", h_tsearch}, {"_tfind", h_tfind}, {"_tdelete", h_tdelete}, {"_twalk", h_twalk}, {"_lfind", h_lfind}, {"_lsearch", h_lsearch},
    {"_setjmp", h_setjmp}, {"__setjmp", h_setjmp}, {"_sigsetjmp", h_setjmp},
    {"_longjmp", h_longjmp}, {"__longjmp", h_longjmp}, {"_siglongjmp", h_longjmp},
    {"__NSGetArgv", h_nsgetargv}, {"__NSGetArgc", h_nsgetargc}, {"__NSGetProgname", h_nsgetprogname},
    {"_getprogname", h_getprogname}, {"__NSGetExecutablePath", h_nsgetexepath},
    {"__tlv_bootstrap", h_tlv_bootstrap}, {"_pthread_setspecific", h_pthread_setspecific}, {"_swift_updateClassMetadata2", h_swift_updateClassMetadata2}, {"_swift_initClassMetadata2", h_swift_initClassMetadata2}, {"_swift_getInitializedObjCClass", h_swift_getInitializedObjCClass}, {"_DNSServiceProcessResult", h_DNSServiceProcessResult},
    {"_abort", h_abort_report}, {"_swift_dynamicCast", h_swift_dynamicCast}, {"_objc_opt_self", h_objc_opt_self}, {"_JRSFontGetBoundingBoxesForGlyphsAndStyle", h_JRSFontGetBoundingBoxesForGlyphsAndStyle},
    {"_dlopen", h_dlopen}, {"_dlerror", h_dlerror}, {"_dlsym", h_dlsym}, {"_dlclose", h_dlclose}, {"_dladdr", h_dladdr},
    {"__dyld_image_count", h_dyld_image_count}, {"__dyld_get_image_name", h_dyld_get_image_name},
    {"__dyld_get_image_header", h_dyld_get_image_header}, {"__dyld_get_image_vmaddr_slide", h_dyld_get_image_vmaddr_slide},
    {"__dyld_register_func_for_add_image", h_dyld_register_func_for_add_image}, {"__Unwind_Resume", h_Unwind_Resume}, {"__Unwind_GetIPInfo", h_Unwind_GetIPInfo}, {"__Unwind_GetLanguageSpecificData", h_Unwind_GetLanguageSpecificData}, {"__Unwind_GetRegionStart", h_Unwind_GetRegionStart}, {"__Unwind_GetDataRelBase", h_Unwind_GetDataRelBase}, {"__Unwind_GetTextRelBase", h_Unwind_GetTextRelBase}, {"__Unwind_GetCFA", h_Unwind_GetCFA}, {"__Unwind_SetGR", h_Unwind_SetGR}, {"__Unwind_SetIP", h_Unwind_SetIP}, {"__Unwind_GetIP", h_Unwind_GetIP}, {"__Unwind_GetGR", h_Unwind_GetGR},
    {"_objc_msgSend", h_objc_msgSend}, {"_objc_msgSendSuper2", h_objc_msgSendSuper2},
    {"_CFRunLoopObserverCreate", h_CFRunLoopObserverCreate}, {"_CFRunLoopTimerCreate", h_CFRunLoopTimerCreate}, {"_CFRunLoopSourceCreate", h_CFRunLoopSourceCreate}, {"_CVDisplayLinkSetOutputCallback", h_CVDisplayLinkSetOutputCallback},
    {"_method_setImplementation", h_method_setImplementation}, {"_method_getTypeEncoding", h_method_getTypeEncoding}, {"_method_copyReturnType", h_method_copyReturnType}, {"_method_getReturnType", h_method_getReturnType}, {"_method_copyArgumentType", h_method_copyArgumentType}, {"_method_getArgumentType", h_method_getArgumentType}, {"_class_replaceMethod", h_class_replaceMethod}, {"_class_addMethod", h_class_addMethod},
    {"_method_getImplementation", h_method_getImplementation}, {"_class_getMethodImplementation", h_class_getMethodImplementation},
    {"_NSLog", h_NSLog}, {"_NSLogv", h_NSLogv},
    {"_CFStringCreateWithFormat", h_CFStringCreateWithFormat},
    {"_CFStringCreateWithFormatAndArguments", h_CFStringCreateWithFormatAndArguments},
    {"_CFBundleGetFunctionPointerForName", h_CFBundleGetFunctionPointerForName}, {"_CFBundleGetDataPointerForName", h_CFBundleGetDataPointerForName},
    {"_sqlite3_mprintf", h_sqlite3_mprintf}, {"_sqlite3_snprintf", h_sqlite3_snprintf}, {"_sqlite3_str_appendf", h_sqlite3_str_appendf},
    {"_CFCalendarDecomposeAbsoluteTime", h_CFCalendarDecomposeAbsoluteTime}, {"_CFCalendarComposeAbsoluteTime", h_CFCalendarComposeAbsoluteTime}, {"_CFCalendarAddComponents", h_CFCalendarAddComponents},
    {"_swift_bridgeObjectRetain", h_swift_bridgeObjectRetain}, {"_swift_bridgeObjectRelease", h_swift_bridgeObjectRelease}, {"_swift_bridgeObjectRetain_n", h_swift_bridgeObjectRetain_n}, {"_swift_bridgeObjectRelease_n", h_swift_bridgeObjectRelease_n},
    {"_CFStringAppendFormat", h_CFStringAppendFormat}, {"_CFStringAppendFormatAndArguments", h_CFStringAppendFormatAndArguments},
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
// Runs the guest's handler for a synchronous fault of interpreted code (a memory access that faulted, an instruction that traps: HotSpot's implicit null checks, safepoint polls and
// deoptimization instruction, V8's and wasmtime's traps) with a siginfo and a ucontext in the arm64 layout, takes the registers and the pc back from the ucontext and continues
// the interpreter from there. Does not return. The caller has checked that there is a handler (g_guest_fault_handler) and that t_fault_jb is set.
// ---- Mach thread state of the guest threads that are waiting for an exception reply
static struct { uint32_t port; CPU *cpu; uint64_t far; } g_mach_faulting[32];
static pthread_mutex_t g_mach_fmu = PTHREAD_MUTEX_INITIALIZER;
static CPU *mach_faulting_cpu(uint32_t port, uint64_t *far) {
    CPU *r = NULL; pthread_mutex_lock(&g_mach_fmu);
    for (int k = 0; k < 32; k++) if (g_mach_faulting[k].port == port && g_mach_faulting[k].cpu) { r = g_mach_faulting[k].cpu; if (far) *far = g_mach_faulting[k].far; }
    pthread_mutex_unlock(&g_mach_fmu); return r;
}
enum { MFL_ARM_THREAD_STATE64 = 6, MFL_ARM_EXCEPTION_STATE64 = 7, MFL_ARM_NEON_STATE64 = 17 };
H(h_thread_get_state) {   // (thread, flavor, state*, count*)
    uint64_t far = 0; uint32_t port = (uint32_t)c->x[0]; CPU *t = mach_faulting_cpu(port, &far);
    if (!t && (c->x[1] == MFL_ARM_THREAD_STATE64 || c->x[1] == MFL_ARM_NEON_STATE64) && !getenv("REBAR_NO_THREAD_STATE")) {
        // Another guest thread that the guest has suspended: its registers are in its CPU structure, but translated code keeps some of them (and the stack pointer) in host registers until the
        // block ends, so a thread that was stopped inside translated code is let go on for a moment and stopped again until it is in the interpreter or in a call to the host.
        for (int k = 0; k < g_nthr_cpu && k < 1024 && !t; k++) if (__atomic_load_n(&g_thr_cpu[k].port, __ATOMIC_ACQUIRE) == port) t = g_thr_cpu[k].cpu;
        if (t) {
            for (int tries = 0; tries < 20000; tries++) {
                _STRUCT_X86_THREAD_STATE64 hs; mach_msg_type_number_t n = x86_THREAD_STATE64_COUNT;
                if (thread_get_state((thread_act_t)port, x86_THREAD_STATE64, (thread_state_t)&hs, &n) != KERN_SUCCESS) break;
                if (!g_jit_on || !jit_block_of(hs.__rip)) break;
                thread_resume((thread_act_t)port); usleep(20);
                pthread_mutex_lock(&g_jit_mu); thread_suspend((thread_act_t)port); pthread_mutex_unlock(&g_jit_mu);
            }
        }
    }
    if (!t) { host_call(c, HOSTFN("thread_get_state"), false); return; }
    int fl = (int)c->x[1]; uint64_t *st = (uint64_t *)c->x[2]; uint32_t *cnt = (uint32_t *)c->x[3];
    if (fl == MFL_ARM_THREAD_STATE64) {
        for (int k = 0; k < 29; k++) st[k] = t->x[k];
        st[29] = t->x[29]; st[30] = t->x[30]; st[31] = t->sp; st[32] = t->pc;
        ((uint32_t *)st)[66] = (t->n ? 1u << 31 : 0) | (t->z ? 1u << 30 : 0) | (t->c ? 1u << 29 : 0) | (t->v ? 1u << 28 : 0); ((uint32_t *)st)[67] = 0;
        *cnt = 68; c->x[0] = 0;
    } else if (fl == MFL_ARM_EXCEPTION_STATE64) { st[0] = far; ((uint32_t *)st)[2] = 0x92000045; ((uint32_t *)st)[3] = 0; *cnt = 4; c->x[0] = 0; }
    else if (fl == MFL_ARM_NEON_STATE64) { for (int k = 0; k < 32; k++) { st[2 * k] = t->vr[k].d[0]; st[2 * k + 1] = t->vr[k].d[1]; } ((uint32_t *)st)[64] = t->fpsr; ((uint32_t *)st)[65] = t->fpcr; *cnt = 68; c->x[0] = 0; }
    else if (fl == 15) { memset(st, 0, 520); *cnt = 130; c->x[0] = 0; }   // ARM_DEBUG_STATE64: no breakpoints, no watchpoints
    else c->x[0] = 4;   // KERN_INVALID_ARGUMENT
}
H(h_thread_set_state) {   // (thread, flavor, state*, count)
    CPU *t = mach_faulting_cpu((uint32_t)c->x[0], NULL);
    if (!t) { host_call(c, HOSTFN("thread_set_state"), false); return; }
    int fl = (int)c->x[1]; const uint64_t *st = (const uint64_t *)c->x[2];
    if (fl == MFL_ARM_THREAD_STATE64) {
        for (int k = 0; k < 31; k++) t->x[k] = st[k];
        t->sp = st[31]; t->pc = st[32]; uint32_t f = ((const uint32_t *)st)[66]; t->n = f >> 31 & 1; t->z = f >> 30 & 1; t->c = f >> 29 & 1; t->v = f >> 28 & 1;
    } else if (fl == MFL_ARM_NEON_STATE64) { for (int k = 0; k < 32; k++) { t->vr[k].d[0] = st[2 * k]; t->vr[k].d[1] = st[2 * k + 1]; } t->fpsr = ((const uint32_t *)st)[64]; t->fpcr = ((const uint32_t *)st)[65]; }
    c->x[0] = 0;
}
H(h_thread_resume)  { if (mach_faulting_cpu((uint32_t)c->x[0], NULL)) { c->x[0] = 0; return; } host_call(c, HOSTFN("thread_resume"), false); }

// Sends the exception request and waits for the reply; on success the guest CPU has been changed by the handler and the interpreter goes on from it (does not return).
static void deliver_mach_exception(CPU *c, int sig, uint64_t addr) {
    struct { mach_msg_header_t h; mach_msg_body_t body; mach_msg_port_descriptor_t thread, task; NDR_record_t ndr; int32_t exception; uint32_t codeCnt; int64_t code[2]; } req;
    struct { mach_msg_header_t h; NDR_record_t ndr; int32_t ret; char pad[64]; } rep;
    mach_port_t reply = MACH_PORT_NULL, me = mach_thread_self();
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS) return;
    memset(&req, 0, sizeof req);
    req.h.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    req.h.msgh_size = sizeof req; req.h.msgh_remote_port = g_mach_exc_port; req.h.msgh_local_port = reply; req.h.msgh_id = 2405;   // mach_exception_raise
    req.body.msgh_descriptor_count = 2;
    req.thread.name = me; req.thread.disposition = MACH_MSG_TYPE_COPY_SEND; req.thread.type = MACH_MSG_PORT_DESCRIPTOR;
    req.task.name = mach_task_self(); req.task.disposition = MACH_MSG_TYPE_COPY_SEND; req.task.type = MACH_MSG_PORT_DESCRIPTOR;
    req.ndr = NDR_record; req.exception = 1; req.codeCnt = 2; req.code[0] = 1; req.code[1] = (int64_t)addr;   // EXC_BAD_ACCESS, KERN_INVALID_ADDRESS, the address
    if (getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[mach exception: EXC_BAD_ACCESS at %llx, pc %llx, to port %u]\n", (unsigned long long)addr, (unsigned long long)c->pc, g_mach_exc_port);
    int slot = -1; pthread_mutex_lock(&g_mach_fmu);
    for (int k = 0; k < 32; k++) if (!g_mach_faulting[k].cpu) { slot = k; g_mach_faulting[k].port = me; g_mach_faulting[k].cpu = c; g_mach_faulting[k].far = addr; break; }
    pthread_mutex_unlock(&g_mach_fmu);
    bool had = gil_release();
    // (the timeouts only count with their option flags: without them a handler thread that never answers, Crashlytics' after it wrote its report, left the faulting thread and with it the whole game hanging for good instead of ending with rebar's fault report)
    kern_return_t kr = mach_msg(&req.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof req, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
    if (kr == KERN_SUCCESS) kr = mach_msg(&rep.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof rep, reply, 20000, MACH_PORT_NULL);
    if (had) gil_acquire();
    if (slot >= 0) { pthread_mutex_lock(&g_mach_fmu); g_mach_faulting[slot].cpu = NULL; pthread_mutex_unlock(&g_mach_fmu); }
    mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1); mach_port_deallocate(mach_task_self(), me);
    if (kr != KERN_SUCCESS || rep.ret != 0) { if (getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[mach exception: no usable reply (kr %d, ret %d)]\n", (int)kr, (int)rep.ret); return; }
    if (getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[mach exception: handled, continuing at pc %llx sp %llx]\n", (unsigned long long)c->pc, (unsigned long long)c->sp);
    _longjmp(*t_fault_jb, 1);
}
__thread int t_in_deliver;
static void deliver_guest_signal(CPU *c, int sig, int code, uint64_t addr) {
    t_in_deliver = 1;
    int hk = sig == SIGSEGV ? 0 : sig == SIGBUS ? 1 : 2;
    if (getenv("REBAR_TRACE_SIGNALS")) { fprintf(stderr, "[signal %d at guest pc %llx, address %llx, handler %llx]\n", sig, (unsigned long long)c->pc, (unsigned long long)addr, (unsigned long long)g_guest_fault_handler[hk]); dump_backtrace(c);
        if (c->pc > 0x10000) { fprintf(stderr, "  words at pc-16..pc+8:"); for (int k = -4; k <= 2; k++) fprintf(stderr, " %08x", *(uint32_t *)(c->pc + 4 * k)); fprintf(stderr, "\n  x0-x30:"); for (int k = 0; k < 31; k++) fprintf(stderr, " x%d=%llx", k, (unsigned long long)c->x[k]); fprintf(stderr, " sp=%llx\n", (unsigned long long)c->sp); } }
    static __thread uint8_t sinfo[128], uctx[64], mctx[1024];
    memset(sinfo, 0, sizeof sinfo); memset(uctx, 0, sizeof uctx); memset(mctx, 0, sizeof mctx);
    *(int *)(sinfo) = sig; *(int *)(sinfo + 8) = code; *(uint64_t *)(sinfo + 24) = addr;
    *(uint64_t *)(uctx + 48) = (uint64_t)mctx; *(uint64_t *)(uctx + 40) = 816;
    *(uint64_t *)(mctx) = addr; *(uint32_t *)(mctx + 8) = hk == 2 ? 0x02000000 : 0x92000045;
    uint64_t *ss = (uint64_t *)(mctx + 16);
    for (int k = 0; k < 29; k++) ss[k] = c->x[k];
    ss[29] = c->x[29]; ss[30] = c->x[30]; ss[31] = c->sp; ss[32] = c->pc;
    *(uint32_t *)(mctx + 16 + 33 * 8) = (c->n ? 1u << 31 : 0) | (c->z ? 1u << 30 : 0) | (c->c ? 1u << 29 : 0) | (c->v ? 1u << 28 : 0);
    CPU before = *c;
    // a handler registered with SA_ONSTACK runs on the thread's alternate signal stack, as the kernel would run it (Go's sigtrampgo checks that sp is on the signal stack and gives up otherwise)
    if ((g_guest_fault_flags[hk] & SA_ONSTACK) && t_gss_on && !(c->sp > t_gss_sp && c->sp <= t_gss_sp + t_gss_size)) c->sp = (t_gss_sp + t_gss_size - 512) & ~0xfull;
    else c->sp = (c->sp - 512) & ~0xfull;
    call_guest(c, g_guest_fault_handler[hk], (uint64_t)sig, (uint64_t)sinfo, (uint64_t)uctx, 0);
    *c = before;
    for (int k = 0; k < 29; k++) c->x[k] = ss[k];
    c->x[29] = ss[29]; c->x[30] = ss[30]; c->sp = ss[31]; c->pc = ss[32];
    if (getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[signal %d: guest handler returned, continuing at pc %llx sp %llx x0 %llx x30 %llx]\n", sig, (unsigned long long)c->pc, (unsigned long long)c->sp, (unsigned long long)c->x[0], (unsigned long long)c->x[30]);
    uint32_t nz = *(uint32_t *)(mctx + 16 + 33 * 8);
    c->n = nz >> 31 & 1; c->z = nz >> 30 & 1; c->c = nz >> 29 & 1; c->v = nz >> 28 & 1;
    _longjmp(*t_fault_jb, 1);
}
// Swift Concurrency's job runner closes its autorelease pool with a token that, under rebar, is sometimes not in the thread's chain of pool pages (cause not found: see the notes on
// Gemini); libobjc then walks off the end of the chain (AutoreleasePoolPage::releaseUntil) and faults at address 0. Instead of dying, the fault is answered by unwinding the host stack
// out of objc_autoreleasePoolPop and continuing in its caller as if the pop had returned: the pool's objects leak. Experimental and OFF by default: REBAR_POOL_GUARD=1 turns it on (the resumed job runner then returns to a bad address; the frame layout at the resume point is not understood).
#include <libunwind.h>
// An extra inhabitant of a pointer (the "none" of an Optional<Optional<AnyHashable>>, say) is written by arm64 code as index, by the host's x86_64 runtime as index << 1 (bit 0 is reserved for Objective-C
// tagged pointers). A host value witness that destroys such a value reads the guest's 1 as a live metadata pointer and faults at 1 - 8 = 0xfffffffffffffff9 (Xcode's SwiftUI canvas, IDECanvasUI,
// destroying a (AnyHashable?, AnyHashable) tuple; Logic Pro's Jet). Destroying an extra inhabitant is nothing to do, so the fault is answered by unwinding out of the destroy witness (a host function
// whose name ends in "wxx") and continuing in its caller as if it had returned. REBAR_NO_XI_GUARD=1 turns it off. Copies and assignments of such values still fault.
static bool unwind_out_of_xi_destroy(ucontext_t *uc) {
    unw_context_t ctx; unw_cursor_t cur; memset(&ctx, 0, sizeof ctx); memcpy(&ctx, &uc->uc_mcontext->__ss, sizeof(uc->uc_mcontext->__ss));
    if (unw_init_local(&cur, &ctx) != 0) return false;
    bool found = false;
    for (int guard = 0; guard < 8; guard++) {
        char nm[256] = ""; unw_word_t off;
        if (unw_get_proc_name(&cur, nm, sizeof nm, &off) == 0) { size_t n = strlen(nm); if (n > 3 && !strcmp(nm + n - 3, "wxx")) { found = true; break; } }
        if (unw_step(&cur) <= 0) return false;
    }
    if (!found || unw_step(&cur) <= 0) return false;   // (the frame of the destroy witness, then its caller)
    unw_word_t ip, sp, bp, bx, r12, r13, r14, r15;
    unw_get_reg(&cur, UNW_REG_IP, &ip); unw_get_reg(&cur, UNW_REG_SP, &sp); unw_get_reg(&cur, UNW_X86_64_RBP, &bp); unw_get_reg(&cur, UNW_X86_64_RBX, &bx);
    unw_get_reg(&cur, UNW_X86_64_R12, &r12); unw_get_reg(&cur, UNW_X86_64_R13, &r13); unw_get_reg(&cur, UNW_X86_64_R14, &r14); unw_get_reg(&cur, UNW_X86_64_R15, &r15);
    _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    ss->__rip = ip; ss->__rsp = sp; ss->__rbp = bp; ss->__rbx = bx; ss->__r12 = r12; ss->__r13 = r13; ss->__r14 = r14; ss->__r15 = r15; ss->__rax = 0;
    static int n; if (n++ < 3 && getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[destroy of a guest extra inhabitant skipped, continuing at %llx]\n", (unsigned long long)ip);
    return true;
}
static bool unwind_out_of_pool_pop(ucontext_t *uc) {
    Dl_info di; if (!dladdr((void *)uc->uc_mcontext->__ss.__rip, &di) || !di.dli_fname || !strstr(di.dli_fname, "libobjc")) return false;   // (a fault inside libobjc; the stack must show a pool pop)
    unw_context_t ctx; unw_cursor_t cur; memset(&ctx, 0, sizeof ctx); memcpy(&ctx, &uc->uc_mcontext->__ss, sizeof(uc->uc_mcontext->__ss));   // (x86_64 libunwind's context starts with the same register block as the signal's thread state: the unwind begins at the faulting frame, with its registers)
    if (unw_init_local(&cur, &ctx) != 0) return false;
    bool seen_pop = false;
    for (int guard = 0; guard < 40 && unw_step(&cur) > 0; guard++) {
        char nm[256] = ""; unw_word_t off;
        bool named = unw_get_proc_name(&cur, nm, sizeof nm, &off) == 0;
        if (named && strstr(nm, "autoreleasePoolPop")) { seen_pop = true; continue; }
        if (!seen_pop) continue;
        unw_word_t ip, sp, bp, bx, r12, r13, r14, r15;
        unw_get_reg(&cur, UNW_REG_IP, &ip); unw_get_reg(&cur, UNW_REG_SP, &sp); unw_get_reg(&cur, UNW_X86_64_RBP, &bp); unw_get_reg(&cur, UNW_X86_64_RBX, &bx);
        unw_get_reg(&cur, UNW_X86_64_R12, &r12); unw_get_reg(&cur, UNW_X86_64_R13, &r13); unw_get_reg(&cur, UNW_X86_64_R14, &r14); unw_get_reg(&cur, UNW_X86_64_R15, &r15);
        if (getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[unwind target %s: ip %llx sp %llx bp %llx bx %llx r12 %llx r13 %llx r14 %llx r15 %llx; fault sp %llx bp %llx]\n", nm, (unsigned long long)ip, (unsigned long long)sp, (unsigned long long)bp, (unsigned long long)bx, (unsigned long long)r12, (unsigned long long)r13, (unsigned long long)r14, (unsigned long long)r15, (unsigned long long)uc->uc_mcontext->__ss.__rsp, (unsigned long long)uc->uc_mcontext->__ss.__rbp);
        if (getenv("REBAR_TRACE_SIGNALS")) { fprintf(stderr, "[stack at target sp:"); for (int q = -2; q < 14; q++) fprintf(stderr, " %llx", (unsigned long long)((uint64_t *)sp)[q]); fprintf(stderr, "]\n"); }
        _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
        ss->__rip = ip; ss->__rsp = sp; ss->__rbp = bp; ss->__rbx = bx; ss->__r12 = r12; ss->__r13 = r13; ss->__r14 = r14; ss->__r15 = r15; ss->__rax = 0;
        static int n; if (n++ < 3 && getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[autorelease pool pop with a token outside the thread's pools: skipped, continuing in %s]\n", named ? nm : "?");
        return true;
    }
    return false;
}
static void dump_metadata_chain(uint64_t em, int depth) {   // debugging aid: metadata words and every value witness with its symbol, then what an Optional wraps
    for (int d = 0; d < 4 && em > 0x10000; d++) {
        uint64_t mw[6] = {0}, vt = 0, vw[20] = {0}; mach_vm_size_t g = 0;
        if (mach_vm_read_overwrite(mach_task_self(), em, 48, (mach_vm_address_t)mw, &g) != KERN_SUCCESS) return;
        mach_vm_read_overwrite(mach_task_self(), em - 8, 8, (mach_vm_address_t)&vt, &g); mach_vm_read_overwrite(mach_task_self(), vt, 160, (mach_vm_address_t)vw, &g);
        fprintf(stderr, "    [chain %d.%d] metadata %llx kind %llx size %llu flags %llx xi %llu\n", depth, d, (unsigned long long)em, (unsigned long long)mw[0], (unsigned long long)vw[8], (unsigned long long)(vw[10] & 0xffffffff), (unsigned long long)(vw[10] >> 32));
        for (int k = 0; k < 20; k++) { if (k >= 8 && k <= 10) continue; Dl_info dv; const char *sn = (vw[k] > 0x10000 && dladdr((void *)vw[k], &dv) && dv.dli_sname) ? dv.dli_sname : "?"; fprintf(stderr, "       vw[%d] = %llx %.90s\n", k, (unsigned long long)vw[k], sn); }
        if (mw[0] == 0x202 || mw[0] == 0x201 || mw[0] == 0x200) em = mw[2]; else break;
    }
}
// A guest memory fault inside translated code that keeps registers in host registers: the block was translated with a record of each of its memory accesses (JSite: guest pc and what
// the register cache held), so the CPU structure can be made exact from the interrupted host registers and the fault given to the guest's handler like one of the interpreter's.
// (The N Z C V flags are not rebuilt: a handler that resumes the very instruction may see old ones; one that unwinds, as Go's panic from a nil dereference does, does not care.)
static uint64_t host_reg_of(const _STRUCT_X86_THREAD_STATE64 *r, int n) {
    switch (n) { case 0: return r->__rax; case 1: return r->__rcx; case 2: return r->__rdx; case 3: return r->__rbx; case 5: return r->__rbp; case 6: return r->__rsi; case 7: return r->__rdi;
                 case 8: return r->__r8; case 9: return r->__r9; case 10: return r->__r10; case 11: return r->__r11; case 12: return r->__r12; case 13: return r->__r13; case 14: return r->__r14; default: return 0; }
}
static bool jit_recover_state(ucontext_t *uc, const JReg *jb, uint64_t *ea) {
    if (!t_cpu || !jb->sites || getenv("REBAR_NO_JIT_RECOVER")) return false;
    uint64_t rip = uc->uc_mcontext->__ss.__rip;
    const JSite *hit = NULL;
    for (uint32_t k = 0; k < jb->nsites; k++) { uint64_t d = (uint64_t)jb->sites[k].at - rip; if ((int64_t)d >= 0 && d <= 8 && (!hit || jb->sites[k].at < hit->at)) hit = &jb->sites[k]; }   // (the instruction starts a few bytes before its ModRM byte)
    if (!hit) return false;
    const _STRUCT_X86_THREAD_STATE64 *r = &uc->uc_mcontext->__ss;
    static const uint8_t host[8] = {RBX, RBP, R12, R13, R14, R10, R11, RDI};
    for (int k = 0; k < 8; k++) {
        if (!hit->reg_of[k]) continue;
        bool pinned = k < g_npin;
        if (!pinned && !(hit->dirty & (1u << k))) continue;
        int g = hit->reg_of[k] - 1; uint64_t v = host_reg_of(r, host[k]);
        if (g == 31) t_cpu->sp = v; else if (g < 31) t_cpu->x[g] = v;
    }
    t_cpu->pc = hit->pc;
    { uint64_t a = host_reg_of(r, hit->base) + (uint64_t)(int64_t)hit->disp; if (hit->idx >= 0) a += host_reg_of(r, hit->idx) << hit->scale; *ea = a; }   // (the address the access used: a general protection fault, as for a non-canonical address, reports none)
    return true;
}
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    { static int xg = -1; if (xg < 0) xg = getenv("REBAR_NO_XI_GUARD") == NULL; if (xg && sig == SIGSEGV && (uint64_t)si->si_addr >= 0xffffffffffffff00ull && unwind_out_of_xi_destroy(uc)) return; }
    { static int pg = -1; if (pg < 0) pg = getenv("REBAR_POOL_GUARD") != NULL; if (pg && sig == SIGSEGV && (uint64_t)si->si_addr == 0 && unwind_out_of_pool_pop(uc)) return; }   // (getenv is a linear scan of the environment under a lock: not on every fault)
    if (sig == SIGILL || sig == SIGTRAP || sig == SIGFPE) {  // never an instruction-fetch bridge: report without touching any state
        { const JReg *jb = g_jit_on ? jit_block_of(uc->uc_mcontext->__ss.__rip) : NULL; if (jb) { char bb[160]; int nn = snprintf(bb, sizeof bb, "rebar: trap in translated code of the block at guest pc 0x%llx (%u instructions)\n", (unsigned long long)jb->pc, jb->n); write(2, bb, nn); } }
        char b[200];
        int n = snprintf(b, sizeof b, "rebar: signal %d code=%d at host pc %llx (si_addr %p), rsp %llx\n", sig, si->si_code,
                         (unsigned long long)uc->uc_mcontext->__ss.__rip, si->si_addr, (unsigned long long)uc->uc_mcontext->__ss.__rsp);
        write(2, b, n);
        if (getenv("REBAR_FRAMES")) {   // (a trap in host code, e.g. libsystem aborting on a recursive lock: the frame pointer chain, as raw addresses)
            uint64_t fp = uc->uc_mcontext->__ss.__rbp; char fb[64];
            for (int k = 0; k < 40 && fp > 0x1000 && !(fp & 7); k++) { uint64_t *f = (uint64_t *)fp; int m = snprintf(fb, sizeof fb, "  frame %d %llx\n", k, (unsigned long long)f[1]); write(2, fb, m); if (f[0] <= fp) break; fp = f[0]; }
        }
        if ((uint64_t)si->si_addr != uc->uc_mcontext->__ss.__rip) _exit(140 + sig);
    }
    if (!t_altstack) ensure_altstack();
    uint64_t rip = uc->uc_mcontext->__ss.__rip;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("REBAR_DEBUG_SIG") != NULL;
    if (dbg) { sigset_t cur; pthread_sigmask(SIG_BLOCK, NULL, &cur); char b[200]; int n = snprintf(b, sizeof b, "[sig %d code=%d si_addr=%p rip=%llx rsp=%llx mask=%x thr=%p]\n", sig, si->si_code, si->si_addr, (unsigned long long)rip, (unsigned long long)uc->uc_mcontext->__ss.__rsp, (unsigned)cur, (void *)pthread_self()); write(2, b, n); }
    if ((uint64_t)si->si_addr == rip) {
        if (rip - g_guest_lo < g_guest_size || guest_owned_code(rip)) {  // host code jumped to a guest function
            if (t_async_jb && *(uint64_t *)uc->uc_mcontext->__ss.__rsp == (uint64_t)rebar_hc_after_call_addr()) {   // a tail jump out of the async host function the stub called
                { const _STRUCT_X86_THREAD_STATE64 *r = &uc->uc_mcontext->__ss; t_tail_regs[0] = r->__rdi; t_tail_regs[1] = r->__rsi; t_tail_regs[2] = r->__rdx; t_tail_regs[3] = r->__rcx; t_tail_regs[4] = r->__r8; t_tail_regs[5] = r->__r9; t_tail_regs[6] = r->__r12; t_tail_regs[7] = r->__r13; }
                uc->uc_mcontext->__ss.__rdi = rip; uc->uc_mcontext->__ss.__rsi = uc->uc_mcontext->__ss.__r14;
                uc->uc_mcontext->__ss.__rip = (uint64_t)rebar_tail_to_guest;
                return;
            }
            { uint64_t ra = *(uint64_t *)uc->uc_mcontext->__ss.__rsp; if (g_sigtramp_ra && ra - (g_sigtramp_ra - 0x80) < 0x100) t_sig_entry = (int)uc->uc_mcontext->__ss.__rdi; }   // called by the host's signal trampoline: a signal handler
            { static int chk = -1; if (chk < 0) chk = getenv("REBAR_CHECK_ALIGN") != NULL;
              if (chk && (uc->uc_mcontext->__ss.__rsp & 15) != 8) { static int n; if (n++ < 8) { uint64_t ra = *(uint64_t *)uc->uc_mcontext->__ss.__rsp; Dl_info di; const char *nm = dladdr((void *)ra, &di) && di.dli_sname ? di.dli_sname : "?";
                char b[300]; int k = snprintf(b, sizeof b, "[align] host jumped to guest %llx with rsp %llx (mod 16 = %d); return address %llx in %s\n", (unsigned long long)rip, (unsigned long long)uc->uc_mcontext->__ss.__rsp, (int)(uc->uc_mcontext->__ss.__rsp & 15), (unsigned long long)ra, nm); write(2, b, k); } } }
            { uint64_t ra0 = *(uint64_t *)uc->uc_mcontext->__ss.__rsp; uc->uc_mcontext->__ss.__r10 = 0;
              // only a host caller inside libswift_Concurrency matters here: its address range is found once (again when more images were loaded and it was not there yet), and the four bytes
              // at the return address are then read directly (a mach_vm_read_overwrite per fault cost more than the rest of the handler: 500000 faults opening a project)
              static int noasync = -1; if (noasync < 0) noasync = getenv("REBAR_NO_ASYNC_RET") != NULL;
              static uint64_t conc_lo, conc_hi; static uint32_t conc_n;
              if (!noasync) {
                  uint32_t nimg = _dyld_image_count();
                  if (!conc_hi && nimg != conc_n) { conc_n = nimg; for (uint32_t k = 0; k < nimg; k++) { const char *pn = _dyld_get_image_name(k); if (pn && strstr(pn, "libswift_Concurrency")) { const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(k); unsigned long sz = 0; if (mh && getsegmentdata(mh, "__TEXT", &sz)) { conc_lo = (uint64_t)mh; conc_hi = conc_lo + sz; } break; } } }
                  if (ra0 - conc_lo < conc_hi - conc_lo && *(uint32_t *)ra0 == 0x08ec8348u) { uc->uc_mcontext->__ss.__r10 = 0x5ca1ab1e5ca1ab1eull; if (getenv("REBAR_TRACE_ASYNC")) { Dl_info di; const char *nm = dladdr((void *)ra0, &di) && di.dli_sname ? di.dli_sname : "?"; char b[240]; int k = snprintf(b, sizeof b, "[async return] guest %llx called from %llx in %s\n", (unsigned long long)rip, (unsigned long long)ra0, nm); write(2, b, k); } }   // 48 83 ec 08 = subq $8,%rsp
              } }
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
    bool jit_exact = false; uint64_t jit_ea = 0;   // a fault in a block that is not precise: rebuild the guest state from the access record, if there is one
    if (g_jit_on && t_cpu && t_fault_jb && (sig == SIGSEGV || sig == SIGBUS) && (uint64_t)si->si_addr != rip) { const JReg *jb0 = jit_block_of(rip); if (jb0 && !jb0->precise) jit_exact = jit_recover_state(uc, jb0, &jit_ea); }
    if (jit_exact && t_cpu && t_fault_jb && (sig == SIGSEGV || sig == SIGBUS) && (jit_ea >> 56) && !(jit_ea >> 63 && (jit_ea >> 56) == 0xff) && !getenv("REBAR_NO_TBI")) {   // a tag in the top byte of the address (arm64 ignores it): the interpreter makes this access, masked
        static uint64_t n; if (getenv("REBAR_TRACE_TBI") && __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 50) { char bb[120]; int nn = snprintf(bb, sizeof bb, "[tbi] guest pc %llx address %llx\n", (unsigned long long)t_cpu->pc, (unsigned long long)jit_ea); write(2, bb, nn); }
        t_tbi_step = 1;
        _longjmp(*t_fault_jb, 1);
    }
    if (g_mach_exc_port && t_cpu && t_fault_jb && (uint64_t)si->si_addr != rip && (sig == SIGSEGV || sig == SIGBUS) && (jit_exact || !(g_jit_on && jit_block_of(rip) && !jit_block_of(rip)->precise)) && t_cpu->pc - g_stub_base >= g_stub_size)
        deliver_mach_exception(t_cpu, sig, (uint64_t)si->si_addr);   // (returns only when nothing could be delivered)
    if (t_cpu && t_fault_jb && (uint64_t)si->si_addr != rip && (sig == SIGSEGV || sig == SIGBUS) && g_guest_fault_handler[sig == SIGSEGV ? 0 : 1] && (jit_exact || !(g_jit_on && jit_block_of(rip) && !jit_block_of(rip)->precise)) && t_cpu->pc - g_stub_base >= g_stub_size)   // (not in a stub's host handler: a fault there is rebar's own and must not run guest code with its locks held)
        deliver_guest_signal(t_cpu, sig, jit_exact && !si->si_addr ? 1 : si->si_code, jit_exact && !si->si_addr ? jit_ea : (uint64_t)si->si_addr);   // (does not return: it continues at the pc the guest's handler chose)
    { const char *lf = getenv("REBAR_FAULT_LOG"); if (lf) { int fd = open(lf, O_WRONLY | O_APPEND | O_CREAT, 0644); if (fd >= 0) { char tag[64]; int n = snprintf(tag, sizeof tag, "=== pid %d\n", getpid()); write(fd, tag, n); dup2(fd, 2); } } }  // REBAR_FAULT_LOG=<file>: fault reports of helper processes whose stderr is lost
    { const JReg *jb = g_jit_on ? jit_block_of(rip) : NULL;
      if (jb) {
          fprintf(stderr, "rebar: the fault is in translated code of the block at guest pc 0x%llx (%u instructions), %u bytes into it\n", (unsigned long long)jb->pc, jb->n, (unsigned)(rip - (uint64_t)jb->code));
          { fprintf(stderr, "  guest instructions of the block:"); for (uint32_t k = 0; k < jb->n && k < 40; k++) fprintf(stderr, " %08x", ((uint32_t *)jb->pc)[k]); fprintf(stderr, "\n"); }
          if (t_cpu) { fprintf(stderr, "  guest registers:"); for (int k = 0; k < 31; k++) fprintf(stderr, " x%d=%llx", k, (unsigned long long)t_cpu->x[k]); fprintf(stderr, " sp=%llx\n", (unsigned long long)t_cpu->sp); }
          fprintf(stderr, "  host code at the fault:"); for (int k = -16; k < 16; k++) fprintf(stderr, " %02x", ((uint8_t *)rip)[k]); fprintf(stderr, "\n  block code:"); for (uint32_t k = 0; k < jb->len && k < 400; k++) fprintf(stderr, "%s%02x", k % 32 ? " " : "\n   ", jb->code[k]); fprintf(stderr, "\n");
          fprintf(stderr, "  host registers: rax=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r15=%llx rsp=%llx\n", (unsigned long long)uc->uc_mcontext->__ss.__rax, (unsigned long long)uc->uc_mcontext->__ss.__rcx, (unsigned long long)uc->uc_mcontext->__ss.__rdx, (unsigned long long)uc->uc_mcontext->__ss.__rsi, (unsigned long long)uc->uc_mcontext->__ss.__rdi, (unsigned long long)uc->uc_mcontext->__ss.__r8, (unsigned long long)uc->uc_mcontext->__ss.__r9, (unsigned long long)uc->uc_mcontext->__ss.__r15, (unsigned long long)uc->uc_mcontext->__ss.__rsp);
      } }
    if (getenv("REBAR_FAULT_FRAMES")) {   // the host frames above the fault by rbp chain (a fault inside a host library: who called it)
        uint64_t bp = uc->uc_mcontext->__ss.__rbp; fprintf(stderr, "rebar: host frames at the fault (rbp chain):\n");
        for (int k = 0; k < 24 && bp > 0x1000 && !(bp & 7); k++) {
            uint64_t fr[2]; mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), bp, 16, (mach_vm_address_t)fr, &got) != 0 || got != 16) break;
            Dl_info di; bool ok = dladdr((void *)fr[1], &di) && di.dli_sname;
            fprintf(stderr, "    ret 0x%llx %s+%ld (%s)\n", (unsigned long long)fr[1], ok ? di.dli_sname : "?", ok ? (long)(fr[1] - (uint64_t)di.dli_saddr) : 0L, di.dli_fname ? strrchr(di.dli_fname, '/') + 1 : "?");
            if (fr[0] <= bp) break; bp = fr[0];
        }
    }
    fprintf(stderr, "rebar: fault (pid %d, signal %d) addr %p, host pc %p, last guest pc 0x%llx, last import %s called from 0x%llx\n",
            (int)getpid(), sig, si->si_addr, (void *)rip, (unsigned long long)(t_cpu ? t_cpu->pc : 0), t_last_stub ? t_last_stub->name : "-", (unsigned long long)t_last_lr);
    if (getenv("REBAR_DUMP_CODE_AT")) { uint64_t a = strtoull(getenv("REBAR_DUMP_CODE_AT"), NULL, 16); const uint8_t *q = (const uint8_t *)(a - 0x100); fprintf(stderr, "  [code from %p:", (void *)q); for (int k = 0; k < 0x180; k++) fprintf(stderr, " %02x", q[k]); fprintf(stderr, "]\n"); }
    if (getenv("REBAR_DUMP_CODE")) { const uint8_t *pc = (const uint8_t *)uc->uc_mcontext->__ss.__rip; fprintf(stderr, "  [code at host pc %p, 48 bytes before and 16 after:", (void *)pc); for (int k = -48; k < 16; k++) fprintf(stderr, " %02x", pc[k]); fprintf(stderr, "]\n"); }
    if (getenv("REBAR_DUMP_ADDR")) { for (char *spec = strdup(getenv("REBAR_DUMP_ADDR")), *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) { uint64_t a = strtoull(tok, NULL, 16); uint64_t w[16] = {0}; mach_vm_size_t got = 0; bool ok = mach_vm_read_overwrite(mach_task_self(), a, 128, (mach_vm_address_t)w, &got) == KERN_SUCCESS; fprintf(stderr, "  [memory at %llx%s]", (unsigned long long)a, ok ? "" : " (not readable)"); if (ok) for (int k = 0; k < 16; k++) fprintf(stderr, " %llx", (unsigned long long)w[k]); fprintf(stderr, "\n"); } }
    if (getenv("REBAR_DUMP_HOTPAGE")) fprintf(stderr, "  [hotPage TLS slot 43 of this thread = %p]\n", pthread_getspecific(43));
    if (getenv("REBAR_DUMP_OBJ")) {   // REBAR_DUMP_OBJ=rbx|...: the 8 words at the address a register holds, and the class name its isa gives (debugging aid)
        const _STRUCT_X86_THREAD_STATE64 *r = &uc->uc_mcontext->__ss; const char *n = getenv("REBAR_DUMP_OBJ");
        uint64_t a = (n[0] == 'x' && t_cpu) ? t_cpu->x[atoi(n + 1)] : !strcmp(n, "rbx") ? r->__rbx : !strcmp(n, "r12") ? r->__r12 : !strcmp(n, "r13") ? r->__r13 : !strcmp(n, "r14") ? r->__r14 : !strcmp(n, "r15") ? r->__r15 : !strcmp(n, "rdi") ? r->__rdi : !strcmp(n, "rax") ? r->__rax : !strcmp(n, "rcx") ? r->__rcx : r->__rsi;
        uint64_t w[48] = {0}; mach_vm_size_t got = 0; bool ok = mach_vm_read_overwrite(mach_task_self(), a, 384, (mach_vm_address_t)w, &got) == KERN_SUCCESS;
        fprintf(stderr, "  [object at %s = %llx%s]", n, (unsigned long long)a, ok ? "" : " (not readable)"); if (ok) { for (int k = 0; k < (n[0] == 'x' ? 48 : 8); k++) fprintf(stderr, " %llx", (unsigned long long)w[k]);
            Class cl = (Class)(w[0] & 0x7ffffffffff8ull); char tmp[8]; if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)cl, 8, (mach_vm_address_t)tmp, &got) == KERN_SUCCESS) fprintf(stderr, "  class %s", class_getName(cl)); } fprintf(stderr, "\n");
        if (ok) { Class cl0 = (Class)(w[0] & 0x7ffffffffff8ull); uint64_t cw[26] = {0}; mach_vm_size_t g3 = 0; if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)cl0, 208, (mach_vm_address_t)cw, &g3) == KERN_SUCCESS) { fprintf(stderr, "    class metadata %p words:", (void *)cl0); for (int k = 0; k < 26; k++) fprintf(stderr, " %llx", (unsigned long long)cw[k]); fprintf(stderr, "\n");
            if (getenv("REBAR_DUMP_ELEMENT")) { uint64_t em = cw[18];   // the generic argument of _ContiguousArrayStorage<T>: T's metadata, its value witness table and (for an Optional or a tuple) what it wraps
                for (int depth = 0; depth < 3 && em > 0x10000; depth++) { uint64_t mw[14] = {0}, vt = 0, vw[16] = {0}; mach_vm_size_t g4 = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), em, 112, (mach_vm_address_t)mw, &g4) != KERN_SUCCESS) break;
                    mach_vm_read_overwrite(mach_task_self(), em - 8, 8, (mach_vm_address_t)&vt, &g4); mach_vm_read_overwrite(mach_task_self(), vt, 128, (mach_vm_address_t)vw, &g4);
                    fprintf(stderr, "    T%d metadata %llx words:", depth, (unsigned long long)em); for (int k = 0; k < 14; k++) fprintf(stderr, " %llx", (unsigned long long)mw[k]); fprintf(stderr, "\n      vwt %llx: size %llu stride %llu flags %llx xi %llu (", (unsigned long long)vt, (unsigned long long)vw[8], (unsigned long long)vw[9], (unsigned long long)(vw[10] & 0xffffffff), (unsigned long long)(vw[10] >> 32));
                    for (int k = 0; k < 8; k++) { Dl_info dv; const char *sn = (vw[k] > 0x10000 && dladdr((void *)vw[k], &dv) && dv.dli_sname) ? dv.dli_sname : "?"; fprintf(stderr, "%.40s%s", sn, k < 7 ? " | " : ""); } fprintf(stderr, ")\n");
                    if (mw[0] == 0x301) { for (int e = 0; e < (int)mw[1] && e < 4; e++) { uint64_t et = mw[3 + 2 * e], eo = mw[4 + 2 * e], ev = 0, evw[12] = {0}, ek = 0;
                            mach_vm_read_overwrite(mach_task_self(), et, 8, (mach_vm_address_t)&ek, &g4); mach_vm_read_overwrite(mach_task_self(), et - 8, 8, (mach_vm_address_t)&ev, &g4); mach_vm_read_overwrite(mach_task_self(), ev, 96, (mach_vm_address_t)evw, &g4);
                            fprintf(stderr, "      element %d: type %llx kind %llx offset %llu size %llu stride %llu flags %llx xi %llu\n", e, (unsigned long long)et, (unsigned long long)ek, (unsigned long long)eo, (unsigned long long)evw[8], (unsigned long long)evw[9], (unsigned long long)(evw[10] & 0xffffffff), (unsigned long long)(evw[10] >> 32)); } }
                    { uint64_t buf[3] = {0, 0, 1}; typedef int (*gets_t)(void *, unsigned, const void *); gets_t g = (gets_t)vw[6]; if (vw[6] > 0x10000) fprintf(stderr, "      getEnumTagSinglePayload((0,0,1), 1, T%d) -> %d\n", depth, g(buf, 1, (const void *)(uintptr_t)em)); }
                    if (mw[0] == 0x301) { for (int e = 0; e < (int)mw[1] && e < 4; e++) { uint64_t et = mw[3 + 2 * e], ev2 = 0, evw2[12] = {0}; mach_vm_read_overwrite(mach_task_self(), et - 8, 8, (mach_vm_address_t)&ev2, &g4); mach_vm_read_overwrite(mach_task_self(), ev2, 96, (mach_vm_address_t)evw2, &g4);
                            if (e == 1) dump_metadata_chain(et, e);
                            uint64_t b2[3] = {0, 0, 1}; typedef int (*gets_t)(void *, unsigned, const void *); if (evw2[6] > 0x10000) { Dl_info d2; const char *n2 = dladdr((void *)evw2[6], &d2) && d2.dli_sname ? d2.dli_sname : "?"; fprintf(stderr, "      element %d getEnumTagSinglePayload at offset %llu of (0,0,1) -> %d  (%.60s)\n", e, (unsigned long long)mw[4 + 2 * e], ((gets_t)evw2[6])((char *)b2 + mw[4 + 2 * e], 1, (const void *)(uintptr_t)et), n2); } } }
                    em = (mw[0] == 0x301) ? 0 : mw[2]; } }
        } }
        if (ok) { uint64_t vw = 0; mach_vm_size_t g2 = 0; if (mach_vm_read_overwrite(mach_task_self(), a - 8, 8, (mach_vm_address_t)&vw, &g2) == KERN_SUCCESS && vw > 0x10000) { uint64_t t[14] = {0}; if (mach_vm_read_overwrite(mach_task_self(), vw, 112, (mach_vm_address_t)t, &g2) == KERN_SUCCESS) { fprintf(stderr, "    value witness table %llx:", (unsigned long long)vw); for (int k = 0; k < 14; k++) { Dl_info dv; const char *sn = (t[k] > 0x10000 && dladdr((void *)t[k], &dv) && dv.dli_sname) ? dv.dli_sname : ""; fprintf(stderr, " %llx%s%s", (unsigned long long)t[k], *sn ? "=" : "", sn); } fprintf(stderr, "\n"); } } }
        if (ok) for (int k = 1; k < 4; k++) { uint64_t o2 = w[k]; uint64_t v2[2] = {0}; if (o2 < 0x10000) continue; if (mach_vm_read_overwrite(mach_task_self(), o2, 16, (mach_vm_address_t)v2, &got) != KERN_SUCCESS) { fprintf(stderr, "    w[%d]=%llx not readable\n", k, (unsigned long long)o2); continue; }
            Class c2 = (Class)(v2[0] & 0x7ffffffffff8ull); char t2[8]; const char *n2 = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)c2, 8, (mach_vm_address_t)t2, &got) == KERN_SUCCESS ? class_getName(c2) : "?";
            fprintf(stderr, "    w[%d]=%llx isa %llx -> class %s  words %llx %llx%s\n", k, (unsigned long long)o2, (unsigned long long)v2[0], n2 ? n2 : "?", (unsigned long long)v2[0], (unsigned long long)v2[1], malloc_zone_from_ptr((void *)o2) ? "" : "  [not a live malloc block]"); }
    }
    if (getenv("REBAR_DUMP_FAULT_REG")) {   // REBAR_DUMP_FAULT_REG=rbx|r12|...: 80 words around the address that register holds (debugging aid)
        const _STRUCT_X86_THREAD_STATE64 *r = &uc->uc_mcontext->__ss; const char *n = getenv("REBAR_DUMP_FAULT_REG"); uint64_t a = !strcmp(n, "rbx") ? r->__rbx : !strcmp(n, "r12") ? r->__r12 : !strcmp(n, "r13") ? r->__r13 : !strcmp(n, "r14") ? r->__r14 : !strcmp(n, "r15") ? r->__r15 : !strcmp(n, "rdi") ? r->__rdi : r->__rsi;
        uint64_t base = a & ~0xfffull; fprintf(stderr, "  [dump of the page of %s = %llx]\n", n, (unsigned long long)a);
        for (int k = 0; k < 14; k++) fprintf(stderr, "    +%03x: %016llx\n", k * 8, (unsigned long long)((uint64_t *)base)[k]);
        if (getenv("REBAR_DUMP_POOL")) {   // the slots of an autorelease pool page and of its child pages: object, its isa, its class's name when readable
          uint64_t pg = base; for (int pgn = 0; pgn < 12 && pg; pgn++) {
            fprintf(stderr, "  [page %llx: next %llx thread %llx parent %llx child %llx]\n", (unsigned long long)pg, (unsigned long long)((uint64_t *)pg)[2], (unsigned long long)((uint64_t *)pg)[3], (unsigned long long)((uint64_t *)pg)[4], (unsigned long long)((uint64_t *)pg)[5]);
            for (int k = 7; k < 512; k++) {
                uint64_t o = ((uint64_t *)pg)[k]; if (!o) continue; if (pgn == 0 && k < 480) continue;
                uint64_t isa = 0, nm = 0; mach_vm_size_t got = 0;
                if (o < 0x10000 || o == 0xa3a3a3a3a3a3a3a3ull || mach_vm_read_overwrite(mach_task_self(), o, 8, (mach_vm_address_t)&isa, &got) != KERN_SUCCESS) { fprintf(stderr, "    slot %3d: %016llx (not readable)\n", k, (unsigned long long)o); continue; }
                const char *cn = "?"; Class cl = (Class)(isa & 0x7ffffffffff8ull); char tmp[8];
                if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)cl, 8, (mach_vm_address_t)tmp, &got) == KERN_SUCCESS) cn = class_getName(cl);
                (void)nm; fprintf(stderr, "    slot %3d: obj %llx isa %llx class %s%s\n", k, (unsigned long long)o, (unsigned long long)isa, cn ? cn : "?", (malloc_zone_from_ptr((void *)(o & 0xffffffffffffull)) == NULL) ? "  [NOT A LIVE MALLOC BLOCK]" : "");
            }
            pg = ((uint64_t *)pg)[5]; }
        }
    }
    { const _STRUCT_X86_THREAD_STATE64 *r = &uc->uc_mcontext->__ss; fprintf(stderr, "  host registers: rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx rsp=%llx rbp=%llx\n",
        (unsigned long long)r->__rax, (unsigned long long)r->__rbx, (unsigned long long)r->__rcx, (unsigned long long)r->__rdx, (unsigned long long)r->__rsi, (unsigned long long)r->__rdi, (unsigned long long)r->__r8, (unsigned long long)r->__r9,
        (unsigned long long)r->__r10, (unsigned long long)r->__r11, (unsigned long long)r->__r12, (unsigned long long)r->__r13, (unsigned long long)r->__r14, (unsigned long long)r->__r15, (unsigned long long)r->__rsp, (unsigned long long)r->__rbp); }   // (the interpreter keeps the guest pc in one of them)
    if (r_r10(uc) > 0x10000) { char tmp[8]; mach_vm_size_t got = 0; Class cl = (Class)(r_r10(uc) & 0x7ffffffffff8ull);   // (in objc_msgSend r10 holds the receiver's class: say which one)
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)cl, 8, (mach_vm_address_t)tmp, &got) == KERN_SUCCESS) fprintf(stderr, "  r10 as class: %s   rsi as selector: %s\n", class_getName(cl), (r_rsi(uc) > 0x10000 && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)r_rsi(uc), 1, (mach_vm_address_t)tmp, &got) == KERN_SUCCESS) ? (const char *)r_rsi(uc) : "?"); }
    { const uint64_t hv[] = {r_rax(uc), r_rbx(uc), r_rcx(uc), r_rdx(uc), r_rsi(uc), r_rdi(uc), r_r8(uc), r_r9(uc), r_r10(uc), r_r11(uc), r_r12(uc), r_r13(uc), r_r14(uc), r_r15(uc)};
      static const char *hn[] = {"rax","rbx","rcx","rdx","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
      for (int k = 0; k < 14; k++) { GImage *gi = image_by_addr(hv[k]); if (gi && gi->path) fprintf(stderr, "  %s = %llx is in %s +0x%llx\n", hn[k], (unsigned long long)hv[k], gi->path, (unsigned long long)(hv[k] - gi->base)); } }
    if (t_cpu && getenv("REBAR_DUMP_REGS_MEM")) for (int k = 0; k < 3; k++) { uint64_t base = (k == 0 ? t_cpu->x[1] : k == 1 ? t_cpu->x[20] : t_cpu->x[0]); if (base > 0x10000 && base < 0x7fffffffffffull) { fprintf(stderr, "  mem at x%d=%llx:", k == 0 ? 1 : k == 1 ? 20 : 0, (unsigned long long)base); mach_vm_size_t got = 0; uint64_t w[24]; if (mach_vm_read_overwrite(mach_task_self(), base, sizeof w, (mach_vm_address_t)w, &got) == KERN_SUCCESS) for (int j = 0; j < 24; j++) fprintf(stderr, " %llx", (unsigned long long)w[j]); fprintf(stderr, "\n"); } }
    if (t_cpu) { fprintf(stderr, "  guest registers (pc %llx):", (unsigned long long)t_cpu->pc); for (int k = 0; k < 31; k++) fprintf(stderr, " x%d=%llx", k, (unsigned long long)t_cpu->x[k]); fprintf(stderr, " sp=%llx\n", (unsigned long long)t_cpu->sp); }
    if (t_ring_n) { const RingEnt *e = &t_ring[(t_ring_n - 1) & 127]; fprintf(stderr, "  last import: %s(0x%llx, 0x%llx, 0x%llx)\n", e->name, (unsigned long long)e->x0, (unsigned long long)e->x1, (unsigned long long)e->x2); }
    if (t_cpu) {   // where the guest was: image and offset of the pc and the return address, and the frame-pointer chain
        fprintf(stderr, "  guest frames at the fault:\n"); dump_frames(t_cpu->pc, t_cpu->x[30], t_cpu->x[29]);
        for (Act *a = t_act; a && a->saved; a = a->prev) { fprintf(stderr, "  -- entered from the host --\n"); dump_frames(a->saved->pc, a->saved->x[30], a->saved->x[29]); }
    }
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
        static int maxf; if (!maxf) maxf = getenv("REBAR_FRAMES") ? atoi(getenv("REBAR_FRAMES")) : 24;
        for (int k = 0; k < maxf && fp && !(fp & 7); k++) {
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

// std::system_error(error_code, const std::string &what): the host's constructor builds the message by calling ec.category().message(value), a virtual of a category the guest defined
// (AppleMediaServicesKit has its own); that virtual returns a std::string through a hidden pointer (x86: rdi, arm64: x8), so the host call arrived in the guest with x0 = 0 and faulted.
// The exception is built with the host's system_category (the same value, so what() has a sensible text) and the guest's category pointer is stored afterwards.
static void h_system_error_ctor(CPU *c) {
    uint8_t *self = P(0); uint8_t tmp[24];
    str_to_host(tmp, (const uint8_t *)c->x[3]);
    void (*ctor)(void *, long, const void *, const void *) = HOSTFN("_ZNSt3__112system_errorC2ENS_10error_codeERKNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE");
    const void *(*sysc)(void) = HOSTFN("_ZNSt3__115system_categoryEv");
    ctor(self, (long)(int)c->x[1], sysc(), tmp);
    *(uint64_t *)(self + 24) = c->x[2];
    c->x[0] = c->x[0];
}

// std::filesystem::path::begin() / end() / iterator::__increment() / __decrement(). A path::iterator is 56 bytes: the current element as a path (a std::string, in the host's layout
// when the host made it), the path it walks (pointer), a string_view of the element inside that path's characters, and a state byte. The host functions parse the path they are
// given, so they get a host-layout copy of it; the iterator that comes back is converted to the guest's layout and made to point at the guest's path and characters again.
static void path_it_to_guest(uint8_t *it, const uint8_t *host_path, uint8_t *guest_path) {
    uint8_t tmp[24]; str_to_guest(tmp, it); memcpy(it, tmp, 24);
    *(uint64_t *)(it + 24) = (uint64_t)guest_path;
    uint64_t e = *(uint64_t *)(it + 32), h = (uint64_t)host_path;
    if (!(host_path[0] & 1) && e >= h && e <= h + 24) *(uint64_t *)(it + 32) = (uint64_t)guest_path + (e - h) - 1;   // a short string's characters: at 1 in the host layout, at 0 in the guest's
}
static void path_it_to_host(uint8_t *hit, const uint8_t *git, uint8_t *host_path, const uint8_t *guest_path) {
    memcpy(hit, git, 56);
    uint8_t tmp[24]; str_to_host(tmp, git); memcpy(hit, tmp, 24);
    str_to_host(host_path, guest_path);
    *(uint64_t *)(hit + 24) = (uint64_t)host_path;
    uint64_t e = *(uint64_t *)(git + 32), g = (uint64_t)guest_path;
    if (!(guest_path[23] & 0x80) && e >= g && e <= g + 24) *(uint64_t *)(hit + 32) = (uint64_t)host_path + (e - g) + 1;
}
H(h_path_begin_end) {   // x0 = the path, x8 = where the iterator goes
    Stub *s = t_last_stub; void *hostfn = s->host ? s->host : dlsym(RTLD_DEFAULT, s->name + 1); uint8_t *gp = (uint8_t *)c->x[0], *git = (uint8_t *)c->x[8];
    uint8_t hp[24], hit[56]; memset(hit, 0, sizeof hit);
    str_to_host(hp, gp);
    c->x[0] = (uint64_t)hp; c->x[8] = (uint64_t)hit;
    t_stk = NULL; host_call(c, hostfn, true);
    path_it_to_guest(hit, hp, gp); memcpy(git, hit, 56);
    c->x[0] = (uint64_t)git;
}
H(h_path_iter_step) {   // x0 = the iterator; both return a reference to it
    Stub *s = t_last_stub; void *hostfn = s->host ? s->host : dlsym(RTLD_DEFAULT, s->name + 1); uint8_t *git = (uint8_t *)c->x[0], *gp = *(uint8_t **)(git + 24);
    uint8_t hp[24], hit[56];
    path_it_to_host(hit, git, hp, gp);
    c->x[0] = (uint64_t)hit;
    t_stk = NULL; host_call(c, hostfn, false);
    path_it_to_guest(hit, hp, gp); memcpy(git, hit, 56);
    c->x[0] = (uint64_t)git;
}

// ---- libffi: ffi_call into a guest function. The host's libffi is the x86_64 one, which would call with the System V convention; an arm64 (Apple) function wants other registers,
// other struct rules and a different stack layout, so for a guest target the call is made here from the cif's types. (Calls to host code still go through the host's ffi_call.)
typedef struct FfiType { uint64_t size; uint16_t align, type; struct FfiType **elements; } FfiType;
typedef struct { int abi; unsigned nargs; FfiType **arg_types; FfiType *rtype; } FfiCif;
static struct { const void *cif; unsigned nfixed; } g_ffi_var[128]; static unsigned g_ffi_var_n;   // cifs made by ffi_prep_cif_var: the arguments from nfixed on are variadic (8-byte stack slots on Apple's arm64)
static void ffi_var_forget(const void *cif) { for (unsigned k = 0; k < 128 && k < g_ffi_var_n; k++) if (g_ffi_var[k].cif == cif) g_ffi_var[k].cif = NULL; }   // (a cif is usually a stack variable: its address is reused)
H(h_ffi_prep_cif_var) {   // (cif, abi, nfixedargs, ntotalargs, rtype, atypes)
    ffi_var_forget((const void *)c->x[0]);
    g_ffi_var[g_ffi_var_n++ % 128].cif = (const void *)c->x[0]; g_ffi_var[(g_ffi_var_n - 1) % 128].nfixed = (unsigned)c->x[2];
    if ((uint32_t)c->x[1] == 1) c->x[1] = 2;
    host_call(c, HOSTFN("ffi_prep_cif_var"), false);
}
H(h_ffi_prep_cif) { ffi_var_forget((const void *)c->x[0]); if ((uint32_t)c->x[1] == 1) c->x[1] = 2; host_call(c, HOSTFN("ffi_prep_cif"), false); }
static int ffi_flat_fp(const FfiType *t, int *kind) {   // the number of float / double members of a type that is a homogeneous floating-point aggregate, else -1
    if (t->type == 2 || t->type == 3) { if (*kind && *kind != t->type) return -1; *kind = t->type; return 1; }
    if (t->type != 13) return -1;
    int n = 0;
    for (FfiType **e = t->elements; e && *e; e++) { int k = ffi_flat_fp(*e, kind); if (k < 0) return -1; n += k; }
    return n;
}
static uint64_t ffi_int_value(const FfiType *t, const void *p) {   // an integer-class argument as the callee expects it: extended to 64 bits
    switch (t->type) {
        case 5: return *(const uint8_t *)p; case 6: return (uint64_t)(int64_t)*(const int8_t *)p;
        case 7: return *(const uint16_t *)p; case 8: return (uint64_t)(int64_t)*(const int16_t *)p;
        case 9: return *(const uint32_t *)p; case 1: case 10: return (uint64_t)(int64_t)*(const int32_t *)p;
        default: return *(const uint64_t *)p;
    }
}
H(h_ffi_call) {   // (cif, fn, rvalue, avalue)
    const FfiCif *cif = (const FfiCif *)c->x[0]; uint64_t fn = c->x[1]; void *rvalue = (void *)c->x[2]; void **av = (void **)c->x[3];
    if (!(in_guest(fn) || guest_owned_code(fn) || fn - g_stub_base < g_stub_size)) { host_call(c, HOSTFN("ffi_call"), false); return; }
    unsigned nfixed = cif->nargs;
    for (int k = g_ffi_var_n > 128 ? 127 : (int)g_ffi_var_n - 1; k >= 0; k--) if (g_ffi_var[k].cif == cif) { nfixed = g_ffi_var[k].nfixed; break; }
    if (getenv("REBAR_DEBUG_FFI")) { fprintf(stderr, "[ffi_call] fn %llx nargs %u nfixed %u rtype %d size %llu:", (unsigned long long)fn, cif->nargs, nfixed, cif->rtype->type, (unsigned long long)cif->rtype->size); for (unsigned i = 0; i < cif->nargs; i++) fprintf(stderr, " (t%d s%llu v%llx)", cif->arg_types[i]->type, (unsigned long long)cif->arg_types[i]->size, (unsigned long long)*(uint64_t *)av[i]); fprintf(stderr, "\n"); }
    uint64_t xr[8] = {0}; int ngrn = 0, nsrn = 0; VReg vr[8]; memset(vr, 0, sizeof vr);
    uint8_t stk[1024]; size_t so = 0; static __thread uint8_t big[16][256]; int nbig = 0;
    #define STK_PUT(al, sz, src) do { size_t a_ = (al); so = (so + a_ - 1) & ~(a_ - 1); if (so + (sz) <= sizeof stk) memcpy(stk + so, (src), (sz)); so += (sz); } while (0)
    for (unsigned i = 0; i < cif->nargs; i++) {
        const FfiType *t = cif->arg_types[i]; const void *a = av[i]; bool var = i >= nfixed;
        if (t->type == 13) {
            int kind = 0, nfp = ffi_flat_fp(t, &kind);
            if (!var && nfp >= 1 && nfp <= 4) {
                size_t msz = kind == 2 ? 4 : 8;
                if (nsrn + nfp <= 8) { for (int m = 0; m < nfp; m++) memcpy(&vr[nsrn++], (const uint8_t *)a + m * msz, msz); }
                else { nsrn = 8; STK_PUT(8, ((t->size + 7) & ~7ull), a); }
            } else if (t->size > 16) {
                const void *ptr = a; if (t->size <= 256 && nbig < 16) { memcpy(big[nbig], a, t->size); ptr = big[nbig++]; }
                uint64_t v = (uint64_t)ptr; if (!var && ngrn < 8) xr[ngrn++] = v; else STK_PUT(8, 8, &v);
            } else {
                int words = (int)((t->size + 7) / 8); uint64_t w[2] = {0, 0}; memcpy(w, a, t->size);
                if (!var && ngrn + words <= 8) { xr[ngrn++] = w[0]; if (words == 2) xr[ngrn++] = w[1]; }
                else { if (!var) ngrn = 8; STK_PUT(8, (size_t)words * 8, w); }
            }
        } else if (t->type == 2 || t->type == 3) {
            if (!var && nsrn < 8) memcpy(&vr[nsrn++], a, t->type == 2 ? 4 : 8);
            else if (var) { uint64_t w = 0; memcpy(&w, a, t->type == 2 ? 4 : 8); STK_PUT(8, 8, &w); }
            else STK_PUT(t->type == 2 ? 4 : 8, t->type == 2 ? 4 : 8, a);
        } else {
            uint64_t v = ffi_int_value(t, a); size_t sz = t->size > 8 ? 8 : t->size;
            if (!var && ngrn < 8) xr[ngrn++] = v; else if (var) STK_PUT(8, 8, &v); else STK_PUT(sz ? sz : 8, sz ? sz : 8, &v);
        }
    }
    const FfiType *rt = cif->rtype; int rkind = 0, rfp = rt->type == 13 ? ffi_flat_fp(rt, &rkind) : -1; bool rmem = rt->type == 13 && rfp < 1 && rt->size > 16;
    if (rt->type == 13 && rfp > 4) rmem = true;
    CPU saved = *c;
    for (int k = 0; k < 8; k++) { c->x[k] = xr[k]; c->vr[k] = vr[k]; }
    if (rmem) c->x[8] = (uint64_t)rvalue;
    c->sp = (saved.sp - 512 - ((so + 15) & ~15ull)) & ~0xfull; memcpy((void *)c->sp, stk, so < sizeof stk ? so : sizeof stk);
    c->x[30] = stub_addr(STUB_RET); c->pc = fn;
    bool held = gil_release();
    run_until(c, stub_addr(STUB_RET));
    if (held) gil_acquire(); else if (t_have_gil) gil_release();
    uint64_t r0 = c->x[0], r1 = c->x[1]; VReg rv[4]; for (int k = 0; k < 4; k++) rv[k] = c->vr[k];
    *c = saved;
    switch (rt->type) {
        case 0: break;
        case 2: memcpy(rvalue, &rv[0], 4); break;
        case 3: memcpy(rvalue, &rv[0], 8); break;
        case 13:
            if (rfp >= 1 && rfp <= 4) { size_t msz = rkind == 2 ? 4 : 8; for (int m = 0; m < rfp; m++) memcpy((uint8_t *)rvalue + m * msz, &rv[m], msz); }
            else if (!rmem) { uint64_t w[2] = {r0, r1}; memcpy(rvalue, w, rt->size); }
            break;
        case 5: *(uint64_t *)rvalue = (uint8_t)r0; break;
        case 6: *(int64_t *)rvalue = (int8_t)r0; break;
        case 7: *(uint64_t *)rvalue = (uint16_t)r0; break;
        case 8: *(int64_t *)rvalue = (int16_t)r0; break;
        case 9: *(uint64_t *)rvalue = (uint32_t)r0; break;
        case 1: case 10: *(int64_t *)rvalue = (int32_t)r0; break;
        default: *(uint64_t *)rvalue = r0; break;
    }
    #undef STK_PUT
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
        } else if (strstr(tok, "__wrap_iter<char") && !strchr(tok, 0x26)) { if (!sp.iter && argi < 8) sp.iter = (uint8_t)(argi + 1); argi++;   // an iterator into the string: a pointer into its characters
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

// A guest whose own operator new / delete replace the system's (Firefox's mozjemalloc in libmozglue) cannot hand its string buffers to the host's libc++: the host would free them with the system
// free, and the buffers the host allocates would later be freed by the guest's free. In that case a long string is given to the host in a copy made with the host's malloc,
// and what comes back is copied into a buffer from the guest's malloc (the old guest buffer is kept when it is big enough, or freed with the guest's free).
static uint64_t find_guest_symbol(const char *name);
static uint64_t g_gmalloc, g_gfree; static int g_galloc = -1;
static bool guest_custom_alloc(void) {
    if (g_galloc < 0) { g_gmalloc = find_guest_symbol("__Znwm"); g_gfree = find_guest_symbol("__ZdlPv"); g_galloc = g_gmalloc && g_gfree; }   // (libc++ allocates its string buffers with operator new / delete)
    return g_galloc == 1;
}
typedef struct { void *hbuf; uint64_t gptr, gcap; } StrCopy;
static void str_to_host_copy(uint8_t *d, const uint8_t *g, bool wide, StrCopy *sc) {
    if (!(g[23] & 0x80)) { if (wide) wstr_to_host(d, g); else str_to_host(d, g); return; }
    uint64_t gp = *(const uint64_t *)g, sz = *(const uint64_t *)(g + 8), cap = *(const uint64_t *)(g + 16) & ~(1ull << 63);
    unsigned es = wide ? 4 : 1;
    if (cap < sz + 1) cap = sz + 1;
    void *hb = malloc(cap * es);
    memcpy(hb, (const void *)gp, (sz + 1) * es);
    sc->hbuf = hb; sc->gptr = gp; sc->gcap = cap;
    uint64_t o[3] = {cap | 1, sz, (uint64_t)hb};
    memcpy(d, o, 24);
}
// the host's string d (host layout) back into the guest string g; returns through *hdata / *gdata where the characters were and are now (for a pointer the call returned into them)
static void str_from_host_copy(uint8_t *g, const uint8_t *d, bool wide, StrCopy *sc, uint64_t *hdata, uint64_t *gdata) {
    *hdata = *gdata = 0;
    if (!(d[0] & 1)) {   // short now: the guest's long buffer, if there was one, is not needed any more
        if (sc->gptr) { call_guest(cur_cpu(), g_gfree, sc->gptr, 0, 0, 0); }
        if (sc->hbuf) free(sc->hbuf);
        if (wide) wstr_to_guest(g, d); else str_to_guest(g, d);
        return;
    }
    uint64_t cap = *(const uint64_t *)d & ~1ull, sz = *(const uint64_t *)(d + 8), hp = *(const uint64_t *)(d + 16);
    unsigned es = wide ? 4 : 1;
    uint64_t gp;
    if (sc->gptr && hp == (uint64_t)sc->hbuf && cap <= sc->gcap) { gp = sc->gptr; cap = sc->gcap; }   // untouched capacity: back into the guest's own buffer
    else { gp = call_guest(cur_cpu(), g_gmalloc, cap * es, 0, 0, 0); if (sc->gptr) call_guest(cur_cpu(), g_gfree, sc->gptr, 0, 0, 0); }
    memcpy((void *)gp, (const void *)hp, (sz + 1) * es);
    uint64_t o[3] = {gp, sz, cap | (1ull << 63)};
    memcpy(g, o, 24);
    *hdata = hp; *gdata = gp;
    free((void *)hp);   // (a host buffer the host call allocated, or the copy given to it; if the host reallocated, it already freed ours)
    sc->hbuf = NULL;
}

static void string_call(CPU *c, Stub *s) {
    const StrSpec *sp = s->str;
    uint64_t gx[9];
    memcpy(gx, c->x, sizeof gx);
    uint8_t tmp[8][24], rtmp[24] = {0};
    int own[8] = {0};
    bool cust = guest_custom_alloc() && !sp->sb;
    StrCopy sc[8]; memset(sc, 0, sizeof sc); uint64_t hd[8] = {0}, gd[8] = {0};
    uint8_t wb = sp->wb;
    for (int k = 0; k < 8; k++) {
        if (!(sp->mask >> k & 1)) continue;
        own[k] = k;
        for (int j = 0; j < k; j++)  // one guest object, one copy (s.append(s))
            if ((sp->mask >> j & 1) && own[j] == j && gx[j] == gx[k]) { own[k] = j; if (sp->wb >> k & 1) wb |= 1 << j; break; }
        if (own[k] == k) {
            if (sp->uninit >> k & 1) memset(tmp[k], 0, 24);
            else if (cust) str_to_host_copy(tmp[k], (const uint8_t *)gx[k], sp->wide >> k & 1, &sc[k]);
            else if (sp->wide >> k & 1) wstr_to_host(tmp[k], (const uint8_t *)gx[k]);
            else str_to_host(tmp[k], (const uint8_t *)gx[k]);
        }
        c->x[k] = (uint64_t)tmp[own[k]];
    }
    uint64_t iter_off = 0; int ik = sp->iter ? sp->iter - 1 : -1;
    if (ik >= 0 && (sp->mask & 1)) {   // the iterator points into the guest's characters: make it point into the host copy at the same offset
        const uint8_t *g = (const uint8_t *)gx[0]; uint64_t gdata = (g[23] & 0x80) ? *(const uint64_t *)g : (uint64_t)g, hdata = (tmp[0][0] & 1) ? *(const uint64_t *)(tmp[0] + 16) : (uint64_t)tmp[0] + 1;
        iter_off = gx[ik] - gdata; c->x[ik] = hdata + iter_off;
    }
    bool sbc = sp->sb && sb_enter((uint8_t *)gx[0]);
    if (sp->sret) c->x[8] = (uint64_t)rtmp;
    t_stk = s->stk;
    if (getenv("REBAR_DEBUG_STR")) fprintf(stderr, "[string_call %s] x0=%llx x1=%llx x2=%llx x8=%llx s->sret=%d sp->sret=%d stk=%p\n", s->name, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[8], s->sret, sp->sret, (void *)s->stk);
    host_call(c, s->host, s->sret || sp->sret);
    t_stk = NULL;
    if (sbc) sb_leave((uint8_t *)gx[0]);
    if (t_landed) { for (int k = 0; k < 8; k++) if (sc[k].hbuf && (*(uint64_t *)(tmp[k] + 16) == (uint64_t)sc[k].hbuf)) free(sc[k].hbuf); return; }  // an exception went to a guest handler: the guest's strings stay as they were
    uint64_t r = c->x[0];
    if (cust && sp->dtor && sc[0].hbuf) { if (sc[0].gptr) call_guest(c, g_gfree, sc[0].gptr, 0, 0, 0); sc[0].hbuf = NULL; }   // a destructor: the host freed its copy, the guest's own buffer is the guest allocator's to free
    for (int k = 0; k < 8; k++) {
        if (!(sp->mask >> k & 1) || own[k] != k) continue;
        bool wide = sp->wide >> k & 1;
        if (cust && (wb >> k & 1)) str_from_host_copy((uint8_t *)gx[k], tmp[k], wide, &sc[k], &hd[k], &gd[k]);
        else if (cust && sc[k].hbuf) { if ((tmp[k][0] & 1) && *(uint64_t *)(tmp[k] + 16) == (uint64_t)sc[k].hbuf) free(sc[k].hbuf); sc[k].hbuf = NULL; }   // (not written back: the copy is just dropped)
        else if (wb >> k & 1) { if (wide) wstr_to_guest((uint8_t *)gx[k], tmp[k]); else str_to_guest((uint8_t *)gx[k], tmp[k]); }
        if (cust && hd[k] && r >= hd[k] && r <= hd[k] + (*(uint64_t *)((uint8_t *)gx[k] + 8) + 1) * (wide ? 4 : 1)) { r = gd[k] + (r - hd[k]); continue; }   // a pointer into the characters
        uint64_t t = (uint64_t)tmp[k];
        if (ik >= 0 && k == 0) {   // insert / erase return an iterator into the string: the same offset in the guest's characters (after the write-back, which may have moved them)
            const uint8_t *g = (const uint8_t *)gx[0]; uint64_t gdata = (g[23] & 0x80) ? *(const uint64_t *)g : (uint64_t)g, hdata = (tmp[0][0] & 1) ? *(const uint64_t *)(tmp[0] + 16) : (uint64_t)tmp[0] + 1;
            if (r >= hdata && r <= hdata + 4096) { r = gdata + (r - hdata); continue; }
        }
        if (r == t) r = gx[k];  // append/assign/... return *this
        else if (r > t && r < t + 24 && !(tmp[k][0] & 1)) r = gx[k] + (r - t - (wide ? 4 : 1));  // pointer into the inline characters
    }
    if (sp->sret) {
        if (cust) { StrCopy none = {0}; uint64_t h1, g1; str_from_host_copy((uint8_t *)gx[8], rtmp, sp->sretw, &none, &h1, &g1); }
        else if (sp->sretw) wstr_to_guest((uint8_t *)gx[8], rtmp); else str_to_guest((uint8_t *)gx[8], rtmp);
        r = gx[8];
    }
    for (int k = 2; k < 9; k++) c->x[k] = gx[k];  // (x1 may be the second half of a result, as in a string_view)
    c->x[0] = r;
}

// A guest with its own malloc and free (Firefox) frees what it gets from these host functions with its own free: copy the result into memory from the guest's allocator (operator new /
// delete are what libc++ and the guest agree on, but `free` of strdup's result is the guest's: use the guest's malloc for these, which the guest's free matches).
static void heapret_fix(CPU *c, Stub *s, uint64_t out_ptr, uint64_t arg1) {
    static uint64_t gm, gf; if (!gm) { gm = find_guest_symbol("_malloc"); gf = find_guest_symbol("_free"); }
    if (!gm || !gf) return;
    uint64_t p = s->heapret == 2 ? (uint64_t)((int)c->x[0] >= 0 ? *(uint64_t *)out_ptr : 0) : c->x[0];
    if (s->heapret == 1 && !strcmp(s->name, "_getcwd") && out_ptr) return;
    if (s->heapret == 1 && !strncmp(s->name, "_realpath", 9) && arg1) return;   // (realpath into the caller's own buffer returns that buffer)   // (getcwd into the caller's own buffer returns that buffer)
    if (!p || p < 0x10000) return;
    size_t len = strlen((const char *)p);
    uint64_t g = call_guest(c, gm, len + 1, 0, 0, 0);
    if (!g) return;
    memcpy((void *)g, (const void *)p, len + 1);
    free((void *)p);
    if (s->heapret == 2) *(uint64_t *)out_ptr = g; else c->x[0] = g;
}
static inline void apply_landing(CPU *c) {
    if (t_landed) { t_landed = false; c->x[0] = t_pend_x0; c->x[1] = t_pend_x1; }
}

static void stub_call_inner(CPU *c);
static void ti_canonicalize_tree(uint64_t ti, int depth);
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
// An arm64 guest passes Optional<SwiftUI.Transaction>.none as 1: Transaction is a frozen struct in the SDK, so the compiler lays Optional out itself, and on arm64 the first extra inhabitant
// of a struct around a class reference is 1, while the x86_64 SwiftUI (compiled with x86_64's pointer layout) wants 2 (Query(sort:) stored the 1 as a pointer and crashed on the first copy).
// For a Swift import whose demangled signature takes a `Transaction?` the argument register with 1 in it is turned into 2.
static bool param_is_optional_transaction(const char *seg, const char *end) {   // "label: Swift.Optional<SwiftUI.Transaction>" or "label: SwiftUI.Transaction?"
    size_t n = (size_t)(end - seg);
    if (n >= 20 && !strncmp(end - 20, "SwiftUI.Transaction?", 20)) return true;
    for (const char *q = seg; q + 29 <= end; q++) if (!strncmp(q, "Optional<SwiftUI.Transaction>", 29)) return true;
    return false;
}
static uint8_t transaction_arg_mask(const char *mangled) {
    if (!strstr(mangled, "11TransactionVSg")) return 0;
    static char *(*demangle)(const char *, size_t, char *, size_t *, uint32_t);
    if (!demangle) demangle = (char *(*)(const char *, size_t, char *, size_t *, uint32_t))dlsym(RTLD_DEFAULT, "swift_demangle");
    if (!demangle) { void *h = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_NOW | RTLD_GLOBAL); if (h) demangle = (char *(*)(const char *, size_t, char *, size_t *, uint32_t))dlsym(h, "swift_demangle"); }
    if (!demangle) return 0;
    const char *m = mangled[0] == '_' ? mangled + 1 : mangled;
    char *d = demangle(m, strlen(m), NULL, NULL, 0);
    if (!d) return 0;
    uint8_t mask = 0; int depth = 0, idx = 0; const char *p = d, *start = NULL;
    for (; *p; p++) {   // the first '(' outside angle brackets opens the parameter list
        if (*p == '<') depth++; else if (*p == '>') depth--; else if (*p == '(' && depth == 0) { start = p + 1; break; }
    }
    if (start) {
        depth = 0; const char *seg = start;
        for (p = start; *p; p++) {
            char ch = *p;
            if (ch == '<' || ch == '(' || ch == '[') depth++;
            else if ((ch == '>' || ch == ']') && depth > 0) depth--;
            else if (ch == ')' && depth == 0) { if (idx < 8 && param_is_optional_transaction(seg, p)) mask |= 1u << idx; break; }
            else if (ch == ')') depth--;
            else if (ch == ',' && depth == 0) { if (idx < 8 && param_is_optional_transaction(seg, p)) mask |= 1u << idx; idx++; seg = p + 1; }
        }
    }
    free(d);
    return mask;
}
static int g_slow_ms;   // REBAR_SLOW_STUB=<ms>: log every import call that takes longer (name, arguments, caller, thread, when)
static void slow_stub_log(CPU *c, uint64_t off, uint64_t lr0, double ms) {
    if (off >= g_stub_size || off / 16 >= (uint64_t)g_nstubs) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!strcmp(g_stubs[off / 16].name, "_objc_msgSend") && c->x[0] > 0x10000 && c->x[1] > 0x10000) {   // which message to which class
        fprintf(stderr, "[slow %.0f ms at %.1fs] [%s %s] from guest 0x%llx thread %p\n", ms, (double)(ts.tv_sec - g_slow_t0.tv_sec) + (double)(ts.tv_nsec - g_slow_t0.tv_nsec) / 1e9,
                class_getName(object_getClass((id)c->x[0])), sel_getName((SEL)c->x[1]), (unsigned long long)lr0, (void *)pthread_self());
        return;
    }
    fprintf(stderr, "[slow %.0f ms at %.1fs] %s(0x%llx, 0x%llx, 0x%llx) from guest 0x%llx thread %p\n", ms, (double)(ts.tv_sec - g_slow_t0.tv_sec) + (double)(ts.tv_nsec - g_slow_t0.tv_nsec) / 1e9, g_stubs[off / 16].name,
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)lr0, (void *)pthread_self());
}
static void h_objc_msgSend(CPU *c);
static bool msgsend_fast(CPU *c);
static void stub_call_plain(CPU *c) {
    uint64_t off = c->pc - g_stub_base;
    if (__builtin_expect(off < g_stub_size && off / 16 >= 2 && off / 16 < (uint64_t)g_nstubs && g_stubs[off / 16].handler == h_chkstk_darwin && !g_stubprof && g_slow_ms <= 0, 1)) { c->pc = c->x[30]; return; }   // (the stack probe does nothing: embree's traversal calls it for a 9.7 KB frame millions of times a second)
    if (__builtin_expect(g_slow_ms > 0, 0) && !g_stubprof) {
        struct timespec a, b; uint64_t lr0 = c->x[30], x0 = c->x[0], x1 = c->x[1], x2 = c->x[2];
        bool lock0 = off < g_stub_size && off / 16 >= 2 && off / 16 < (uint64_t)g_nstubs && g_stubs[off / 16].handler && g_stubs[off / 16].handler != h_chkstk_darwin, had0 = t_have_gil;
        clock_gettime(CLOCK_MONOTONIC, &a);
        bool fast0 = off < g_stub_size && off / 16 < (uint64_t)g_nstubs && g_stubs[off / 16].handler == h_objc_msgSend && msgsend_fast(c);   // (timed like the rest: the slow ones are the interesting ones)
        if (fast0) apply_landing(c);
        else {
        if (lock0 && !had0) gil_acquire();
        stub_call_inner(c);
        }
        if (!had0 && t_have_gil) gil_release();
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
        if (ms >= g_slow_ms) { uint64_t sv[3] = {c->x[0], c->x[1], c->x[2]}; c->x[0] = x0; c->x[1] = x1; c->x[2] = x2; slow_stub_log(c, off, lr0, ms); c->x[0] = sv[0]; c->x[1] = sv[1]; c->x[2] = sv[2]; }
        return;
    }
    if (off < g_stub_size && off / 16 < (uint64_t)g_nstubs && g_stubs[off / 16].handler == h_objc_msgSend && !g_stubprof) {   // objc_msgSend: repeats of a plain send need no bridge lock (msgsend_fast in objc.h)
        bool had_f = t_have_gil;
        t_last_stub = &g_stubs[off / 16]; t_last_lr = c->x[30];
        if (msgsend_fast(c)) {
            apply_landing(c);
            if (!had_f && t_have_gil) gil_release();
            urg_deliver_pending(c);
            return;
        }
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
    urg_deliver_pending(c);
}
#include "frameprof.h"
static void stub_call(CPU *c) { if (__builtin_expect(!g_fp, 1)) { stub_call_plain(c); return; } fp_stub_call(c); }
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
// REBAR_PATHMAP=<from>=<to>[:<from>=<to>...]: file names the guest passes to the system calls and library functions below that start with <from> are looked up under <to> (a program built
// for /opt/homebrew running from another prefix, an iOS app's container). The first argument that is a path of a call is rewritten in place of its register, for the call only.
static struct { const char *from, *to; size_t fl, tl; } g_pathmap[8]; static int g_npathmap;
static void pathmap_init(void) {
    static bool done; if (done) return; done = true;
    const char *e = getenv("REBAR_PATHMAP"); if (!e) return;
    char *copy = strdup(e); for (char *tok = strtok(copy, ":"); tok && g_npathmap < 8; tok = strtok(NULL, ":")) {
        char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0; g_pathmap[g_npathmap].from = tok; g_pathmap[g_npathmap].to = eq + 1; g_pathmap[g_npathmap].fl = strlen(tok); g_pathmap[g_npathmap].tl = strlen(eq + 1); g_npathmap++; }
}
static const char *pathmap_apply(const char *p) {
    static __thread char ring[8][PATH_MAX]; static __thread int slot;
    for (int k = 0; k < g_npathmap; k++)
        if (!strncmp(p, g_pathmap[k].from, g_pathmap[k].fl) && (p[g_pathmap[k].fl] == '/' || !p[g_pathmap[k].fl])) {
            char *o = ring[slot++ & 7]; snprintf(o, PATH_MAX, "%s%s", g_pathmap[k].to, p + g_pathmap[k].fl); return o; }
    return p;
}
static uint8_t pathmask_for(const char *name) {   // bit k: argument k is a path
    static const struct { const char *n; uint8_t m; } t[] = {
        {"_open", 1}, {"_openat", 2}, {"_stat", 1}, {"_lstat", 1}, {"_fstatat", 2}, {"_access", 1}, {"_faccessat", 2}, {"_opendir", 1}, {"_fopen", 1}, {"_freopen", 1}, {"_readlink", 1}, {"_readlinkat", 2},
        {"_realpath", 1}, {"_execve", 1}, {"_execv", 1}, {"_execvp", 1}, {"_posix_spawn", 2}, {"_posix_spawnp", 2}, {"_chdir", 1}, {"_mkdir", 1}, {"_mkdirat", 2}, {"_rmdir", 1}, {"_unlink", 1},
        {"_unlinkat", 2}, {"_rename", 3}, {"_chmod", 1}, {"_chown", 1}, {"_lchown", 1}, {"_utimes", 1}, {"_symlink", 2}, {"_link", 3}, {"_truncate", 1}, {"_statfs", 1}, {"_pathconf", 1}, {"_scandir", 1},
        {"_getattrlist", 1}, {"_setxattr", 1}, {"_getxattr", 1}, {"_listxattr", 1}, {"_removexattr", 1}, {"_mkfifo", 1}, {"_mknod", 1}, {"_creat", 1}, {"_dlopen", 1}, {"_glob", 1}, {"_utime", 1}, {"_lutimes", 1},
        {"_chroot", 1}, {"_ftw", 1}, {"_nftw", 1}, {"_fts_open", 1}, {"_copyfile", 3}, {"_clonefile", 3}, {"_renamex_np", 3}, {"_exchangedata", 3}, {"_mount", 2}, {"_unmount", 1}, {"_utimensat", 2}, {"_fchmodat", 2},
    };
    char base[64]; size_t n = 0; while (name[n] && name[n] != '$' && n < 63) { base[n] = name[n]; n++; } base[n] = 0;
    for (size_t k = 0; k < sizeof t / sizeof t[0]; k++) if (!strcmp(base, t[k].n)) return t[k].m;
    return 0;
}
// A Swift String that a host Swift function returns (Foundation's URL.path ...). The host here is macOS 15, whose Foundation hands out a lazily bridged NSString (a "foreign" String: the count in
// its word is the UTF-16 length, the object word 0x5...); the code of an Xcode built for macOS 26 was compiled against a Foundation that returns native Strings, and counts its bytes inline
// (SymbolCacheIndexing compared a 122-byte path with a 119-unit one and trapped: Xcode died while indexing a project). Such a result is turned into a native String
// (String(cString:)); the NSString it held is released. REBAR_NO_NATIVE_STRINGS=1 turns it off.
static void swift_string_to_native(CPU *c) {
    static int off = -1; if (off < 0) off = getenv("REBAR_NO_NATIVE_STRINGS") != NULL;
    if (off || (c->x[1] >> 60) != 0x5 || (c->x[0] >> 48) > 0x7fff) return;
    static uint64_t (*cfstr_len)(const void *); static bool (*cfstr_get)(const void *, char *, long, uint32_t); typedef struct { uint64_t a, b; } SwStr2; static SwStr2 (*mk)(const char *); static void (*rel)(uint64_t);
    if (!mk) {
        cfstr_len = dlsym(RTLD_DEFAULT, "CFStringGetLength"); cfstr_get = dlsym(RTLD_DEFAULT, "CFStringGetCString");
        mk = dlsym(RTLD_DEFAULT, "$sSS7cStringSSSPys4Int8VG_tcfC"); rel = dlsym(RTLD_DEFAULT, "swift_bridgeObjectRelease");
        if (!mk) mk = (void *)1;
    }
    if (mk == (void *)1 || !cfstr_len || !cfstr_get || !rel) return;
    const void *ns = (const void *)(c->x[1] & 0x00ffffffffffffffull);
    long n = (long)cfstr_len(ns) * 3 + 4;
    char *buf = malloc((size_t)n);
    if (!buf) return;
    if (cfstr_get(ns, buf, n, 0x08000100)) {
        uint64_t old = c->x[1];
        SwStr2 r = mk(buf);
        c->x[0] = r.a; c->x[1] = r.b;
        rel(old);
    }
    free(buf);
}
static void stub_call_inner(CPU *c) {
    { static int chk = -1; if (chk < 0) chk = getenv("REBAR_CHECK_ALIGN") != NULL; if (chk && ((uintptr_t)__builtin_frame_address(0) & 15) != 0) { static int n; if (n++ < 6) fprintf(stderr, "[align] stub_call_inner frame %p misaligned (pc %llx, lr %llx)\n", __builtin_frame_address(0), (unsigned long long)c->pc, (unsigned long long)c->x[30]); } }
    if (c->pc - g_stub_base >= g_stub_size) {  // an x86 host function reached through a pointer
        if (c->pc < 0x1000) die(c, "branch to invalid address", 0);   // (libffi's closure trampolines can sit low in the address space: executable host memory is accepted below)
        void *fn = (void *)c->pc;
        // (dladdr is slow, a few microseconds: the answer for a host function is remembered, per thread)
        static __thread struct { uint64_t fn; bool returns_string; const char *enc; bool swift; } t_hostfn_cache[256];
        unsigned hci = (unsigned)(c->pc >> 4) & 255;
        bool fn_returns_string, fn_swift; const char *fn_enc;
        if (t_hostfn_cache[hci].fn == c->pc) { fn_returns_string = t_hostfn_cache[hci].returns_string; fn_enc = t_hostfn_cache[hci].enc; fn_swift = t_hostfn_cache[hci].swift; }
        else {
            Dl_info di;
            bool host_exec = false;   // runtime-generated host code (libffi's closure trampolines, a host JIT): executable memory the guest does not own, in no image
            if (!dladdr(fn, &di)) {
                mach_vm_address_t ra = (mach_vm_address_t)fn; mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t bi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t on = 0;
                if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &on) == KERN_SUCCESS && ra <= (mach_vm_address_t)fn && (bi.protection & VM_PROT_EXECUTE) && !guest_owned_code((uint64_t)fn)) { host_exec = true; memset(&di, 0, sizeof di); }
            }
            if (!host_exec && !dladdr(fn, &di)) {  // not inside any host image either: a stray pointer, report where it came from
                fprintf(stderr, "rebar: branch to 0x%llx, which is neither guest nor host code; x0=%llx x1=%llx x2=%llx x8=%llx x16=%llx x17=%llx\n",
                        (unsigned long long)c->pc, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                        (unsigned long long)c->x[8], (unsigned long long)c->x[16], (unsigned long long)c->x[17]);
                c->pc = c->x[30];
                dump_backtrace(c);
                exit(127);
            }
            fn_returns_string = di.dli_sname && host_fn_returns_string(di.dli_sname);
            // a Swift closure or witness called by the guest (Combine's promise handed to Future's closure, a SwiftUI body): it takes its context in r13, a thrown error comes back in r12.
            // Its symbol is a mangled Swift name, or none at all (the dyld cache has no local symbols for the thunks)
            fn_swift = !di.dli_sname || (di.dli_saddr != (void *)fn && getenv("REBAR_NO_LOCAL_SWIFT_CC") == NULL) || di.dli_sname[0] == '$' || (di.dli_sname[0] == '_' && di.dli_sname[1] == '$') || (di.dli_fname && strstr(di.dli_fname, "libswift")) || !strncmp(di.dli_sname, "swift_", 6) || !strncmp(di.dli_sname, "_ZN5swift", 9);   // (the runtime's completion and resume functions take the error in the swiftself register)
            fn_enc = host_fn_enc(di.dli_sname);
            if (g_trace_stubs) fprintf(stderr, "[host fn by pointer] %p %s\n", fn, di.dli_sname ? di.dli_sname : "?");
            t_hostfn_cache[hci].fn = c->pc; t_hostfn_cache[hci].returns_string = fn_returns_string; t_hostfn_cache[hci].enc = fn_enc; t_hostfn_cache[hci].swift = fn_swift;
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
        t_swift = fn_swift && !getenv("REBAR_NO_SWIFT_PTR_CC");
        static const char *trace_hf; static int trace_hf_init; if (!trace_hf_init) { trace_hf_init = 1; trace_hf = getenv("REBAR_TRACE_HOSTFN"); }   // REBAR_TRACE_HOSTFN=substring: arguments, the first 16 bytes at x0 before and after, and the result of host functions called by pointer
        Dl_info tdi; bool tr = trace_hf && dladdr(fn, &tdi) && tdi.dli_sname && strstr(tdi.dli_sname, trace_hf);
        uint64_t ta[4] = {c->x[0], c->x[1], c->x[2], c->x[3]}; uint64_t tb[2] = {0, 0}; if (tr && ta[0] > 0x10000) memcpy(tb, (void *)ta[0], 16);
        host_call(c, fn, false);
        if (tr) { uint64_t tc[2] = {0, 0}; if (ta[0] > 0x10000) memcpy(tc, (void *)ta[0], 16); fprintf(stderr, "[hostfn %s] x0=%llx x1=%llx x2=%llx x3=%llx swift=%d before {%llx %llx} after {%llx %llx} -> x0=%llx\n", tdi.dli_sname, (unsigned long long)ta[0], (unsigned long long)ta[1], (unsigned long long)ta[2], (unsigned long long)ta[3], (int)t_swift, (unsigned long long)tb[0], (unsigned long long)tb[1], (unsigned long long)tc[0], (unsigned long long)tc[1], (unsigned long long)c->x[0]); }
        apply_landing(c);
        return;
    }
    int idx = (int)((c->pc - g_stub_base) / 16);
    if (idx == STUB_EXIT) run_exit(c, (int)c->x[0]);
    if (idx >= g_nstubs) die(c, "jump into unallocated stub", 0);
    Stub *s = &g_stubs[idx];
    t_last_stub = s;
    if (__builtin_expect(g_npathmap != 0, 0) && s->pathmask) for (int k = 0; k < 4; k++) if (s->pathmask >> k & 1) { uint64_t a = c->x[k]; if (a > 0x10000) { const char *np = pathmap_apply((const char *)a); if (np != (const char *)a) c->x[k] = (uint64_t)np; } }
    uint64_t this0 = c->x[0], arg1_in = c->x[1];
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
    if (__builtin_expect(s->throwing, 0) && c->x[1] > 0x10000) ti_canonicalize_tree(c->x[1], 0);
    static int trace_throw = -1; if (__builtin_expect(trace_throw < 0, 0)) trace_throw = getenv("REBAR_TRACE_THROW") != NULL;   // REBAR_TRACE_THROW=1: the type of every C++ exception the guest throws
    if (__builtin_expect(trace_throw, 0) && !strcmp(s->name, "___cxa_throw") && c->x[1] > 0x10000) {
        const char *nm = (const char *)(*(uint64_t *)(c->x[1] + 8) & ~(1ull << 63));
        fprintf(stderr, "[throw] %s (typeinfo %llx) from guest %llx\n", nm && (uint64_t)nm > 0x10000 ? nm : "?", (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
    }
    static int trace_catch = -1; if (__builtin_expect(trace_catch < 0, 0)) trace_catch = getenv("REBAR_TRACE_CATCH") != NULL;   // (getenv takes the process-wide environment lock: not on every call)
    if (__builtin_expect(trace_catch, 0) && !strcmp(s->name, "_objc_begin_catch") && c->x[0] > 0x10000) {   // what the guest's @catch got: the exception's description
        void *(*send)(void *, void *) = HOSTFN("objc_msgSend"); void *(*selreg)(const char *) = HOSTFN("sel_registerName");
        void *d = send((void *)c->x[0], selreg("description")); const char *u = d ? (const char *)send(d, selreg("UTF8String")) : NULL;
        fprintf(stderr, "[catch] %s\n", u ? u : "?");
    }
    if (g_trace_stubs && trace_stub_wanted(s->name)) { char b[300]; int n = snprintf(b, sizeof b, "[stub] %s(%llx,%llx,%llx,%llx,%llx) sp=%llx lr=%llx x8=%llx\n", s->name, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4], (unsigned long long)c->sp, (unsigned long long)c->x[30], (unsigned long long)c->x[8]); write(2, b, n); }
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
        if (s->ffi_abi && (uint32_t)c->x[1] == 1) c->x[1] = 2;
        if (s->ffi_closure && c->x[0] < 128) c->x[0] = 128;
        if (s->txn_mask) for (int k = 0; k < 8; k++) if (((s->txn_mask >> k) & 1) && c->x[k] == 1) c->x[k] = 2;
        t_stk = s->stk; t_swift = s->swift; t_async_call = s->async_host && !g_no_async_tail; if (s->cb9) t_cb_stackargs++; host_call(c, s->host, s->sret); if (s->cb9) t_cb_stackargs--; t_stk = NULL;
        if (s->maps && (uint32_t)c->x[0] == 0 && !t_landed) { own_add(*(uint64_t *)addr_ptr, size); strip_exec_region(*(uint64_t *)addr_ptr, size); }  // vm_map & co.: the guest owns what came back
    }
    else {
        fprintf(stderr, "rebar: unresolved import %s%s%s%s\n", s->name,
                s->lib ? " (from " : "", s->lib ? s->lib : "", s->lib ? ")" : "");
        dump_backtrace(c);
        exit(127);
    }
    if (s->heapret && !t_landed && guest_custom_alloc()) heapret_fix(c, s, this0, arg1_in);
    if (s->thisret && !t_landed) c->x[0] = this0;  // a constructor or destructor: arm64 returns this
    switch (s->narrow) {  // x86_64 leaves the upper bits of rax undefined for a bool/char/short result
    case 'B': case 'C': c->x[0] = (uint8_t)c->x[0]; break;
    case 'c': c->x[0] = (uint64_t)(int64_t)(int8_t)c->x[0]; break;
    case 'S': c->x[0] = (uint16_t)c->x[0]; break;
    case 's': c->x[0] = (uint64_t)(int64_t)(int16_t)c->x[0]; break;
    }
    apply_landing(c);
    if (__builtin_expect(s->strret, 0) && !t_landed) swift_string_to_native(c);
    ring_ent->ret = c->x[0];
    { static const char *r0 = (const char *)1; if (r0 == (const char *)1) r0 = getenv("REBAR_TRACE_RET0");   // REBAR_TRACE_RET0=<name prefix>: imports of that kind that returned 0 (a metadata lookup that found nothing)
      if (r0 && c->x[0] == 0 && !strncmp(s->name, r0, strlen(r0))) { fprintf(stderr, "[ret0] %s(0x%llx, 0x%llx, 0x%llx) from 0x%llx\n", s->name, (unsigned long long)ring_ent->x0, (unsigned long long)ring_ent->x1, (unsigned long long)ring_ent->x2, (unsigned long long)ring_ent->lr);
        if (ring_ent->x0 > 0x10000 && ring_ent->x0 < 0x7fffffffffffull && !strncmp(s->name, "_swift_getTypeByMangledName", 27)) { char nm[80]; mach_vm_size_t got = 0; if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)ring_ent->x0, 79, (mach_vm_address_t)nm, &got) == 0) { nm[got < 79 ? got : 79] = 0; fprintf(stderr, "        name bytes: "); for (int k = 0; k < 40 && nm[k]; k++) fputc(nm[k] >= 32 && nm[k] < 127 ? nm[k] : '.', stderr); fputc('\n', stderr); } } } }
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

// Stubs by name: stub_for was a linear scan over every stub made so far (tens of thousands for Xcode: seconds of strcmp while its images are bound).
static int32_t g_stub_hash[MAX_STUBS * 2];   // stub index + 1, 0 = free
static inline uint32_t stub_name_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static int stub_find(const char *name) {
    uint32_t m = MAX_STUBS * 2 - 1, h = stub_name_hash(name) & m;
    while (g_stub_hash[h]) { int k = g_stub_hash[h] - 1; if (!strcmp(g_stubs[k].name, name)) return k; h = (h + 1) & m; }
    return -1;
}
static int stub_new(const char *name, const char *lib, void *host, StubHandler h) {
    if (g_nstubs >= MAX_STUBS) { fprintf(stderr, "rebar: too many imports\n"); exit(1); }
    g_stubs[g_nstubs] = (Stub){strdup(name), lib ? strdup(lib) : NULL, host, h,
                               strstr(name, "9to_stringE") || strstr(name, "6substrE") || strstr(name, "6getlocE") || strstr(name, "17current_exceptionv"), NULL, 0, NULL, NULL, false, false};
    { uint32_t m = MAX_STUBS * 2 - 1, h = stub_name_hash(g_stubs[g_nstubs].name) & m; while (g_stub_hash[h]) h = (h + 1) & m; g_stub_hash[h] = g_nstubs + 1; }
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
    bool ok = dlopen(path, RTLD_NOW | RTLD_GLOBAL) != NULL;
    if (!ok && !strncmp(path, "/System/Library/", 16)) { char sp[400]; snprintf(sp, sizeof sp, "/System/iOSSupport%s", path); ok = dlopen(sp, RTLD_NOW | RTLD_GLOBAL) != NULL; }   // (an iOS framework that lives under /System/iOSSupport on the Mac: MarketplaceKit, ...)
    if (!ok && !strncmp(path, "@rpath/libswift", 15)) { char sp[300]; snprintf(sp, sizeof sp, "/usr/lib/swift/%s", path + 7); ok = dlopen(sp, RTLD_NOW | RTLD_GLOBAL) != NULL; }   // (an iOS app's back-deployed Swift runtime: the host's own)
    return g_hostlibs[g_nhostlibs++].ok = ok;
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
        || strstr(n, "10filesystem8__statusE") || strstr(n, "10filesystem16__symlink_statusE") || strstr(n, "10filesystem7__spaceE");
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
// Swift's extra inhabitants of a class reference (the bit patterns an enum borrows from a pointer field to say "no value": Optional<Class> nil is 0, Optional<Optional<Class>> nil is
// the next one, a tuple or struct holding such a field borrows the same way) are numbered by the ABI of the machine: on arm64 the pattern for index i is the value i; on x86_64 the
// low bit belongs to Objective-C tagged pointers, so the pattern is i << 1 and an odd value is taken for a real pointer. Code compiled for arm64 writes the first, libswiftCore of
// this host reads the second: a "no value" read as a live reference (SwiftNIO's Optional<(Frame, Optional<Promise>)> array was destroyed as live and crashed). Both witnesses of the
// retainable-box value witness table are patched in memory to the arm64 numbering: the reader (index = value, no low-bit test) and the writer (no doubling).
// REBAR_NO_SWIFT_XI_PATCH=1 turns it off.
static void patch_swift_extra_inhabitants(void) {
    static bool done; if (done || getenv("REBAR_NO_SWIFT_XI_PATCH")) return; done = true;
    void *sym = dlsym(RTLD_DEFAULT, "swift_retain"); Dl_info di; if (!sym || !dladdr(sym, &di) || !di.dli_fbase) return;
    const struct mach_header_64 *mh = di.dli_fbase; const struct load_command *lc = (const void *)(mh + 1); uint64_t tb = 0, ts = 0;
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) { const struct segment_command_64 *sg = (const void *)lc; ts = sg->vmsize; tb = (uint64_t)mh; break; }
    if (!tb) return;
    static const uint8_t rd[] = {0x48,0x89,0xca,0x48,0xc1,0xea,0x20,0x89,0xce,0x83,0xe6,0x01,0x48,0xd1,0xe9,0xff,0xc1,0x31,0xc0,0x48,0x09,0xd6,0x0f,0x44,0xc1};   // movq %rcx,%rdx; shrq $32,%rdx; movl %ecx,%esi; andl $1,%esi; shrq %rcx; incl %ecx; xorl %eax,%eax; orq %rdx,%rsi; cmovel %ecx,%eax
    static const uint8_t wr[] = {0xff,0xce,0x48,0x01,0xf6,0x48,0x89,0x37};   // decl %esi; addq %rsi,%rsi; movq %rsi,(%rdi)
    int nr = 0, nw = 0;
    for (uint64_t o = 0; o + sizeof rd < ts; o++) {
        uint8_t *q = (uint8_t *)(tb + o);
        int which = (q[0] == rd[0] && !memcmp(q, rd, sizeof rd)) ? 1 : (q[0] == wr[0] && !memcmp(q, wr, sizeof wr)) ? 2 : 0;
        if (!which) continue;
        mach_vm_address_t pg = (mach_vm_address_t)(uintptr_t)q & ~0xfffull;
        if (mach_vm_protect(mach_task_self(), pg, 0x2000, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) continue;
        if (which == 1) { q[11] = 0x00; q[12] = 0x0f; q[13] = 0x1f; q[14] = 0x00; nr++; }   // andl $0,%esi ; nop3 (instead of shrq %rcx)
        else { q[2] = 0x0f; q[3] = 0x1f; q[4] = 0x00; nw++; }                              // nop3 (instead of addq %rsi,%rsi)
        mach_vm_protect(mach_task_self(), pg, 0x2000, false, VM_PROT_READ | VM_PROT_EXECUTE);
    }
    if (getenv("REBAR_TRACE_SIGNALS")) fprintf(stderr, "[swift extra inhabitants: %d reader(s), %d writer(s) patched to the arm64 numbering]\n", nr, nw);
}
// A Swift function that returns Bool (or a Bool getter): the host's x86_64 code leaves the rest of rax undefined, and arm64 callers test the whole w0 (`bl swift_isUniquelyReferenced_nonNull; mov x8, x0; cbz w8, ...`).
// With garbage above bit 0 a shared array buffer looked uniquely referenced and was mutated in place (Xcode: Array.replaceSubrange moved the tail of a shared buffer with a negative length when a
// source file was opened). The runtime's own functions are listed; a mangled name is demangled once and its result type looked at. REBAR_NO_SWIFT_BOOL=1 turns it off.
static bool swift_returns_bool(const char *name) {
    static int off = -1; if (off < 0) off = getenv("REBAR_NO_SWIFT_BOOL") != NULL;
    if (off) return false;
    static const char *const rt[] = {"_swift_isUniquelyReferenced_nonNull", "_swift_isUniquelyReferenced_nonNull_native", "_swift_isUniquelyReferencedNonObjC", "_swift_isUniquelyReferencedNonObjC_nonNull",
        "_swift_isUniquelyReferencedNonObjC_nonNull_bridgeObject", "_swift_isEscapingClosureAtFileLocation", "_swift_dynamicCast", "_swift_isClassType", "_swift_isOptionalType",
        "_swift_isDeallocating", "_swift_unknownObjectUnownedIsDeallocating", "_swift_bridgeObjectUnownedIsDeallocating", "_swift_unownedRetainStrong", "_swift_stdlib_isStackAllocationSafe", "_swift_isOptionalType", "_swift_conformsToProtocolCommon", NULL};
    for (const char *const *q = rt; *q; q++) if (!strcmp(*q, name)) return !!strcmp(name, "_swift_unownedRetainStrong") && !!strcmp(name, "_swift_conformsToProtocolCommon");
    if (name[0] != '_' || name[1] != '$' || (name[2] != 's' && name[2] != 'S') || !strstr(name, "Sb")) return false;
    static char *(*demangle)(const char *, size_t, char *, size_t *, uint32_t);
    if (!demangle) demangle = (char *(*)(const char *, size_t, char *, size_t *, uint32_t))dlsym(RTLD_DEFAULT, "swift_demangle");
    if (!demangle) return false;
    const char *m = name + 1; char *d = demangle(m, strlen(m), NULL, NULL, 0);
    if (!d) return false;
    size_t n = strlen(d); bool r = (n > 13 && !strcmp(d + n - 13, "-> Swift.Bool")) || (n > 20 && strstr(d, ".getter : Swift.Bool") && !strchr(strstr(d, ".getter : Swift.Bool") + 20, ' '));
    free(d); return r;
}
static uint64_t stub_for(const char *name, const char *lib, bool weak) {
    if (name && (!strncmp(name, "_swift_", 7) || !strncmp(name, "_$s", 3))) patch_swift_extra_inhabitants();
    { int k = stub_find(name); if (k >= 2) return stub_addr(k); }
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
        if (!h) h = objc_x_handler(name);
        if (!h) {
            const char *sym = name + (name[0] == '_');
            char buf[512];
            static const char *const inode64[] = {"alphasort", "fdopendir", "fstat", "fstatat", "fstatfs", "fstatx_np", "fts_children", "fts_close", "fts_open", "fts_open_b", "fts_read", "fts_set", "ftw",
                "getfsstat", "getmntinfo", "glob", "glob_b", "lstat", "lstatx_np", "nftw", "opendir", "readdir", "readdir_r", "rewinddir", "scandir", "scandir_b", "seekdir", "stat", "statfs", "statx_np", "telldir", NULL};
            bool i64 = false; for (const char *const *q = inode64; *q; q++) if (!strcmp(*q, sym)) { i64 = true; break; }   // (a lookup that finds nothing walks every loaded image: only the names that have the variant are asked for it)
            snprintf(buf, sizeof buf, "%s$INODE64", sym);  // x86_64 stat() & co. need the 64-bit inode variants
            if (i64) host = dlsym(RTLD_DEFAULT, buf);
            if (!host) host = dlsym(RTLD_DEFAULT, sym);
        }
    }
    if (!h && !host && weak) { if (getenv("REBAR_TRACE_WEAK")) fprintf(stderr, "[weak] %s (%s) not found: null\n", name, lib ? lib : "?"); return 0; }
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
    if (host && !h && !g_stubs[idx].narrow && swift_returns_bool(name)) g_stubs[idx].narrow = 'B';
    if (host && !h && stl_layout_dependent(name))
        fprintf(stderr, "rebar: warning: %s takes libc++ objects whose arm64 layout rebar does not convert; expect it to misbehave\n", name);
    if (host && !h) g_stubs[idx].thisret = returns_this(name);
    if (host && !h && name[0] == '_' && name[1] == '$') { size_t nl = strlen(name); g_stubs[idx].strret = (nl > 4 && (!strcmp(name + nl - 4, "SSvg") || !strcmp(name + nl - 4, "SSyF"))); }   // (a getter or a method that returns a String)
    if (host && !h) g_stubs[idx].throwing = !strcmp(name, "___cxa_throw");
    g_stubs[idx].pathmask = pathmask_for(name);
    g_stubs[idx].async_host = host && name && (!strcmp(name, "_swift_task_switch") || !strcmp(name, "_swift_continuation_await") || !strcmp(name, "_swift_task_future_wait") || !strcmp(name, "_swift_task_future_wait_throwing") || !strcmp(name, "_swift_task_group_wait_next_throwing") || !strcmp(name, "_swift_asyncLet_get") || !strcmp(name, "_swift_asyncLet_get_throwing") || !strcmp(name, "_swift_asyncLet_finish") || !strcmp(name, "_swift_task_run_inline") || (name[0] == '_' && name[1] == '$' && strstr(name, "Ya") && (strstr(name, "YaF") || strstr(name, "YaKF") || strstr(name, "Yacig") || strstr(name, "YaKcfC"))));
    g_stubs[idx].heapret = !strncmp(name, "_realpath", 9) || !strcmp(name, "_strdup") || !strcmp(name, "_strndup") || !strcmp(name, "_getcwd") ? 1 : (!strcmp(name, "_asprintf") || !strcmp(name, "_vasprintf") ? 2 : 0);   // host functions that return memory from the system's malloc (see heapret_fix)
    if (host && !h && returns_class_by_value(name)) g_stubs[idx].sret = true;
    if (host && !h) g_stubs[idx].swift = (name[0] == '_' && name[1] == '$' && (name[2] == 's' || name[2] == 'S')) || !strcmp(name, "_swift_getAtKeyPath") || !strcmp(name, "_swift_setAtWritableKeyPath") || !strcmp(name, "_swift_modifyAtWritableKeyPath") || !strcmp(name, "_swift_getAtAnyKeyPath") || !strcmp(name, "_swift_getAtPartialKeyPath");   // a Swift symbol: Swift's calling convention (also the key path runtime functions: swiftcc with an indirect result in x8)
    if (host && !h) g_stubs[idx].cb9 = lib && (strstr(lib, "libxml2") || strstr(lib, "libexpat") || strstr(lib, "libxslt"));   // these parsers call SAX / declaration handlers with nine arguments (startElementNs, expat's EntityDeclHandler): the ninth is on the arm64 callee's stack
    if (host && !h) g_stubs[idx].ffi_closure = !strcmp(name, "_ffi_closure_alloc");   // the caller sizes the closure by the arm64 ffi_closure (40 bytes); the x86_64 libffi keeps its trampoline table and entry after its own, larger fields
    if (host && !h) g_stubs[idx].ffi_abi = !strcmp(name, "_ffi_prep_cif") || !strcmp(name, "_ffi_prep_cif_var");   // an arm64 libffi caller passes FFI_DEFAULT_ABI (FFI_SYSV = 1); the host libffi is the x86_64 one, where 1 is no ABI (FFI_UNIX64 = 2 is the default)
    if (host && !h && g_stubs[idx].swift) { g_stubs[idx].txn_mask = transaction_arg_mask(name); if (g_stubs[idx].txn_mask && g_trace_stubs) fprintf(stderr, "[stub] %s: Transaction? arguments %02x\n", name, g_stubs[idx].txn_mask); }
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
    if (!strcmp(name, "_environ")) return _NSGetEnviron();   // the host's own variable: setenv / putenv reallocate the array, a copy of the pointer goes stale (tmux's client read freed memory)
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
static uint64_t host_data_addr_uncached(const char *name);
// The same names are asked for by every image (objc_msgSend, __stack_chk_guard, ... in each of Xcode's thousand images), and each answer costs a scan of the special tables, a dlsym over
// every loaded image and a dladdr: the answers (also "not a variable") are kept.
static uint64_t host_data_addr(const char *name) {
    static struct { const char *name; uint64_t addr; } *tab; static const unsigned N = 1u << 17; static unsigned used; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    uint64_t h = 1469598103934665603ull; for (const char *q = name; *q; q++) h = (h ^ (uint8_t)*q) * 1099511628211ull;
    pthread_mutex_lock(&mu);
    if (!tab) tab = calloc(N, sizeof *tab);
    if (tab) for (unsigned k = 0; k < N; k++) {
        unsigned j = (unsigned)((h >> 20) + k) & (N - 1);
        if (!tab[j].name) {
            pthread_mutex_unlock(&mu);
            uint64_t a = host_data_addr_uncached(name);
            pthread_mutex_lock(&mu);
            if (used < N / 2 && !tab[j].name) { tab[j].name = strdup(name); tab[j].addr = a; used++; }
            pthread_mutex_unlock(&mu);
            return a;
        }
        if (!strcmp(tab[j].name, name)) { uint64_t a = tab[j].addr; pthread_mutex_unlock(&mu); return a; }
    }
    pthread_mutex_unlock(&mu);
    return host_data_addr_uncached(name);
}
static uint64_t host_data_addr_uncached(const char *name) {
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
    if (!lib || ordinal <= 0 || lib[0] == '@') { uint64_t a = find_guest_symbol(name); if (a) return a; }   // (an @rpath / @loader_path library that did not resolve to a file may still be one that is loaded: libjava's libjvm)
    if (!strcmp(name, "__ZTVNSt3__115basic_stringbufIcNS_11char_traitsIcEENS_9allocatorIcEEEE")) {  // see sb_proxy_vtable
        uint64_t a = sb_proxy_vtable();
        if (a) return a;
    }
    if (lib_supported(lib)) {
        uint64_t a = host_data_addr(name);
        if (a) return a;
    }
    if (getenv("REBAR_TRACE_BINDS") && !strncmp(name, "_OBJC_CLASS_$_", 14)) fprintf(stderr, "[bind] class %s ordinal %d lib %s supported %d -> stub\n", name, ordinal, lib ? lib : "(none)", lib ? (int)lib_supported(lib) : -1);
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
        bool e64 = sg->pointer_format == 1 || sg->pointer_format == 9 || sg->pointer_format == 12;   // DYLD_CHAINED_PTR_ARM64E, _USERLAND, _USERLAND24 (arm64e: pointer authentication codes are never made here, the signed pointers are plain ones)
        if (sg->pointer_format != DYLD_CHAINED_PTR_64 && sg->pointer_format != DYLD_CHAINED_PTR_64_OFFSET && !e64) {
            fprintf(stderr, "rebar: unsupported chained pointer format %u\n", sg->pointer_format);
            exit(1);
        }
        for (uint32_t pg = 0; pg < sg->page_count; pg++) {
            uint16_t ps = sg->page_start[pg];
            if (ps == DYLD_CHAINED_PTR_START_NONE) continue;
            if (ps & DYLD_CHAINED_PTR_START_MULTI) { fprintf(stderr, "rebar: multi-start chains unsupported\n"); exit(1); }
            uint64_t *p = (uint64_t *)(base + sg->segment_offset + (uint64_t)pg * sg->page_size + ps);
            for (;;) {
                uint64_t raw = *p;
                if (e64) {
                    bool auth = raw >> 63, bind = (raw >> 62) & 1; uint32_t nx = (raw >> 51) & 0x7ff;
                    if (bind) {
                        uint32_t ordn = sg->pointer_format == 12 ? (uint32_t)(raw & 0xffffff) : (uint32_t)(raw & 0xffff);
                        int64_t inl = auth ? 0 : (int64_t)((raw >> 32) & 0x7ffff); if (!auth && (inl & 0x40000)) inl -= 0x80000;
                        *p = bound[ordn] + addend[ordn] + inl;
                    } else {
                        uint64_t target = auth ? (raw & 0xffffffffull) : (raw & 0x7ffffffffffull), high8 = auth ? 0 : (raw >> 43) & 0xff;
                        *p = (sg->pointer_format == 1 ? target + img->slide : base + target) | (high8 << 56);
                    }
                    if (!nx) break;
                    p = (uint64_t *)((uint8_t *)p + nx * 8);
                    continue;
                }
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
static void jitcache_kick(int upto);
#include "loader.h"
