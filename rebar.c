// reverse-rebar: run arm64 macOS Mach-O binaries on an x86_64 Mac.
// Stage 1: Mach-O loader + ARM64 interpreter + BSD syscall passthrough.
//
// Guest memory is mapped at the guest's own virtual addresses, so a guest
// pointer is a host pointer. The emulator is linked at a low base address
// (see build.sh) so it does not collide with the guest's 0x100000000 image.

#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/mach_time.h>
#include <mach/machine.h>
#include <pthread.h>
#include <mach-o/dyld.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xlocale.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <crt_externs.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

typedef union {
    uint8_t b[16]; uint16_t h[8]; uint32_t s[4]; uint64_t d[2];
    float f[4]; double df[2];
} VReg;

typedef struct {
    uint64_t x[31];
    uint64_t sp, pc;
    bool n, z, c, v;
    VReg vr[32];
    uint32_t fpcr, fpsr;
    uint64_t tick;
    uint64_t code_lo, code_len;  // the executable range the last instruction was in: the check is skipped while pc stays inside
    uint32_t code_epoch;
    uint64_t tpidr;
    uint64_t tpidrro;    // TPIDRRO_EL0: the address of this thread's table of thread-local-variable blocks (t_tlv_tab, see h_tlv_bootstrap); read by the guest's TLV thunk
    int32_t jit_budget;  // instructions the translated code may still run before it returns to the dispatcher
    uint64_t jit_hsp, jit_hlim;  // the host stack pointer when translated code was entered (see the return-address stack in jit.h) and the lowest value its call frames may reach
} CPU;

// Guest threads run in parallel, each on its own host thread with its own CPU state. The interpreter lock that
// remains (the "GIL") protects the bridge, not the guest: the stub table, the Objective-C and C++ runtime
// bookkeeping, the loader and the other state that import handlers share. A thread holds it while it runs a
// handler, drops it around every call into the host (which is where threads block) and never holds it while it
// interprets guest instructions, except in nested runs started by a handler (callbacks, exception cleanups).
// Imports that have no handler, just a host function behind them, never take it.
static pthread_mutex_t g_gil = PTHREAD_MUTEX_INITIALIZER;
static __thread bool t_have_gil;
static int g_gil_waiters;  // threads blocked in gil_acquire: a thread only gives way when one is waiting
static inline void gil_acquire(void) {
    if (t_have_gil) return;
    __atomic_add_fetch(&g_gil_waiters, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_gil);
    __atomic_sub_fetch(&g_gil_waiters, 1, __ATOMIC_RELAXED);
    t_have_gil = true;
}
static inline bool gil_release(void) { if (t_have_gil) { t_have_gil = false; pthread_mutex_unlock(&g_gil); return true; } return false; }
static void gil_yield(void) { sched_yield(); }  // a guest spin-wait (YIELD/WFE) lets the host schedule the thread it waits for
static bool g_trace;
static uint32_t g_code_epoch;  // bumped when guest memory is unmapped: the CPUs' cached code ranges are dropped
static bool g_dbg;  // g_trace, g_watch_pc or g_wmem is set
static bool g_trace_stubs;  // REBAR_TRACE_STUBS=1: log every call into the host
static bool g_trace_paths;  // REBAR_TRACE_PATHS=1: log the file paths the guest touches
static __thread CPU *t_cpu;  // the CPU of the current host thread (see cur_cpu in bridge.h)
static CPU *g_stats_cpu;

#define B(i, hi, lo) (((i) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

static inline uint64_t rd(CPU *c, int r) { return r == 31 ? 0 : c->x[r]; }
static inline uint64_t rd_sp(CPU *c, int r) { return r == 31 ? c->sp : c->x[r]; }
static inline void wr(CPU *c, int r, uint64_t v) { if (r != 31) c->x[r] = v; }
static inline void wr_sp(CPU *c, int r, uint64_t v) { if (r == 31) c->sp = v; else c->x[r] = v; }
static inline uint64_t mask(int sf) { return sf ? ~0ull : 0xffffffffull; }
static inline int64_t sext(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }

static bool g_no_simd_fast;  // REBAR_NO_SIMD_FAST: use only the general AdvSIMD code (to compare, or to bisect a bug)
static void dump_backtrace(CPU *c);
static jmp_buf *g_die_jmp;  // set by --simd-selftest, which runs encodings the interpreter may reject
static __thread jmp_buf *t_fault_jb;   // where a guest memory fault, once its signal handler (the guest's own) has run, continues: the wrapper of run_until below
static uint64_t g_guest_fault_handler[3];   // the guest's SIGSEGV, SIGBUS and SIGILL handlers (0: none), see h_sigaction
static void deliver_guest_signal(CPU *c, int sig, int code, uint64_t addr);
static const char *g_die_why;   // (what --probe-insns reports)
[[noreturn]] static void die(CPU *c, const char *why, uint32_t insn) {
    if (g_die_jmp) { g_die_why = why; longjmp(*g_die_jmp, 1); }
    if (t_fault_jb && g_guest_fault_handler[2] && !strncmp(why, "unimplemented", 13)) deliver_guest_signal(c, SIGILL, 1, c->pc);   // an instruction the guest wants to trap on (HotSpot's deoptimization instruction)
    { const char *lf = getenv("REBAR_FAULT_LOG"); if (lf) { int fd = open(lf, O_WRONLY | O_APPEND | O_CREAT, 0644); if (fd >= 0) { char tag[64]; int n = snprintf(tag, sizeof tag, "=== pid %d\n", getpid()); write(fd, tag, n); dup2(fd, 2); } } }
    fprintf(stderr, "rebar: %s (insn 0x%08x at pc 0x%llx)\n", why, insn, (unsigned long long)c->pc);
    dump_backtrace(c);
    exit(127);
}

// ---------------------------------------------------------------- ALU helpers

static inline uint64_t add_flags(CPU *c, uint64_t a, uint64_t b, int cin, int sf) {
    uint64_t r;
    if (sf) {
        unsigned long long carry;
        r = __builtin_addcll(a, b, (unsigned long long)cin, &carry);
        c->c = carry;
        c->v = ((a ^ r) & (b ^ r)) >> 63;
        c->n = r >> 63;
    } else {
        a &= 0xffffffff; b &= 0xffffffff;
        uint64_t sum = a + b + (uint64_t)cin;
        r = sum & 0xffffffff;
        c->c = sum >> 32;
        c->v = ((a ^ r) & (b ^ r)) >> 31 & 1;
        c->n = r >> 31;
    }
    c->z = r == 0;
    return r;
}

static bool cond_holds(CPU *c, int cond) {
    bool r;
    switch (cond >> 1) {
    case 0: r = c->z; break;
    case 1: r = c->c; break;
    case 2: r = c->n; break;
    case 3: r = c->v; break;
    case 4: r = c->c && !c->z; break;
    case 5: r = c->n == c->v; break;
    case 6: r = (c->n == c->v) && !c->z; break;
    default: r = true; break;
    }
    return (cond & 1) && cond != 15 ? !r : r;
}

static uint64_t shift_val(uint64_t v, int type, int amt, int sf) {
    int bits = sf ? 64 : 32;
    v &= mask(sf);
    amt &= bits - 1;
    switch (type) {
    case 0: return (v << amt) & mask(sf);
    case 1: return v >> amt;
    case 2: return (uint64_t)((sf ? (int64_t)v : (int64_t)(int32_t)v) >> amt) & mask(sf);
    default: return amt ? (((v >> amt) | (v << (bits - amt))) & mask(sf)) : v;
    }
}

static uint64_t extend_val(uint64_t v, int opt, int shift) {
    switch (opt) {
    case 0: v = (uint8_t)v; break;
    case 1: v = (uint16_t)v; break;
    case 2: v = (uint32_t)v; break;
    case 3: break;
    case 4: v = (uint64_t)(int8_t)v; break;
    case 5: v = (uint64_t)(int16_t)v; break;
    case 6: v = (uint64_t)(int32_t)v; break;
    default: break;
    }
    return v << shift;
}

// Logical-immediate bitmask decode (ARM ARM DecodeBitMasks, immediate form).
static void jit_invalidate_cache_line(uint64_t a);   // (after jit.h)
static bool decode_bitmask(int N, int imms, int immr, int sf, uint64_t *out) {
    int x = (N << 6) | (~imms & 0x3f);
    if (!x) return false;
    int len = 31 - __builtin_clz(x);
    if (len < 1) return false;
    if (!sf && N) return false;
    int levels = (1 << len) - 1;
    int S = imms & levels, R = immr & levels;
    if (S == levels) return false;
    int esize = 1 << len;
    uint64_t welem = (S + 1 >= 64) ? ~0ull : ((1ull << (S + 1)) - 1);
    if (R && esize < 64) welem = ((welem >> R) | (welem << (esize - R))) & ((1ull << esize) - 1);
    else if (R) welem = (welem >> R) | (welem << (64 - R));
    uint64_t r = 0;
    for (int i = 0; i < 64; i += esize) r |= welem << i;
    *out = r & mask(sf);
    return true;
}

// ------------------------------------------------------------------- syscalls

static void do_syscall(CPU *c) {
    uint64_t n = c->x[16];
    uint64_t a0 = c->x[0], a1 = c->x[1], a2 = c->x[2];
    int64_t ret = 0;
    errno = 0;
    switch (n) {
    case 1:  fflush(NULL); _exit((int)a0);
    case 3:  ret = read((int)a0, (void *)a1, a2); break;
    case 4:  ret = write((int)a0, (const void *)a1, a2); break;
    case 5:  ret = open((const char *)a0, (int)a1, (int)a2); break;
    case 6:  ret = close((int)a0); break;
    case 20: ret = getpid(); break;
    case 24: ret = getuid(); break;
    default:
        fprintf(stderr, "rebar: unimplemented syscall %llu at pc 0x%llx\n",
                (unsigned long long)n, (unsigned long long)c->pc);
        exit(127);
    }
    if (ret == -1 && errno) { c->x[0] = errno; c->c = true; }
    else { c->x[0] = (uint64_t)ret; c->c = false; }
}

// ---------------------------------------------------------------- interpreter

static uint64_t mem_ld(uint64_t a, int size) {
    switch (size) {
    case 0: return *(volatile uint8_t *)a;   // (volatile: a load into the zero register is still made and faults on a bad address, which is how HotSpot's null checks and safepoint polls trap)
    case 1: return *(volatile uint16_t *)a;
    case 2: return *(volatile uint32_t *)a;
    default: return *(volatile uint64_t *)a;
    }
}
static void mem_st(uint64_t a, int size, uint64_t v) {
    switch (size) {
    case 0: *(uint8_t *)a = (uint8_t)v; break;
    case 1: *(uint16_t *)a = (uint16_t)v; break;
    case 2: *(uint32_t *)a = (uint32_t)v; break;
    default: *(uint64_t *)a = v; break;
    }
}

#include "fpsimd.h"
#include "simdfast.h"
#include "atomics.h"

// Addresses in [g_stub_base, g_stub_base + g_stub_size) are never executed:
// they stand for imported host functions (see bridge.h).
static uint64_t g_stub_base, g_stub_size, g_guest_lo, g_guest_size;
static void stub_call(CPU *c);
static bool guest_owned_code(uint64_t pc);
static void guest_code_range(uint64_t *lo, uint64_t *len);  // the range guest_owned_code last matched  // memory the guest mapped itself: generated code (a JIT) is interpreted too
static uint64_t g_watch_pc;   // REBAR_WATCH=<hex pc>[,<hex pc>...]: print the registers every time execution reaches it
static uint64_t g_watch_more[7];
static uint64_t g_wmem, g_wmem_last;  // REBAR_WATCH_MEM=<hex addr>: report the instruction that changes the 8 bytes there
static bool g_wmem_ok;
static void watch_mem_check(CPU *c) {
    if (!g_wmem_ok) {
        if (c->tick & 0xfff) return;  // the address may not be mapped yet: look every few thousand steps
        mach_vm_address_t a = g_wmem; mach_vm_size_t sz; vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS ||
            a > g_wmem || !(info.protection & VM_PROT_READ)) return;
        g_wmem_ok = true;
        g_wmem_last = *(uint64_t *)g_wmem;
        return;
    }
    uint64_t v = *(uint64_t *)g_wmem;
    if (v == g_wmem_last) return;
    fprintf(stderr, "[wmem %llx] %llx -> %llx by pc %llx:", (unsigned long long)g_wmem, (unsigned long long)g_wmem_last, (unsigned long long)v, (unsigned long long)c->pc);
    for (int k = 0; k < 31; k++) fprintf(stderr, " x%d=%llx", k, (unsigned long long)c->x[k]);
    fprintf(stderr, "\n");
    g_wmem_last = v;
}

#ifdef OPSTATS  // -DOPSTATS: exact dynamic instruction mix, keyed by encoding bits 31:21 and 15:10; REBAR_OPSTATS=<file> dumps <file>.<pid> at exit
static uint64_t g_opn[1 << 17]; static uint32_t g_ope[1 << 17];
static void opstats_dump(void) {
    const char *f = getenv("REBAR_OPSTATS"); if (!f) return;
    char name[PATH_MAX]; snprintf(name, sizeof name, "%s.%d", f, (int)getpid());
    FILE *o = fopen(name, "w"); if (!o) return;
    for (int k = 0; k < (1 << 17); k++) if (g_opn[k]) fprintf(o, "%08x %llu\n", g_ope[k], (unsigned long long)g_opn[k]);
    fclose(o);
}
static void *opstats_thread(void *a) { (void)a; for (;;) { sleep(3); opstats_dump(); } return NULL; }
#endif
// Everything the interpreter does that is neither one of its fast handlers nor a branch: hints, barriers, SVC, system
// registers, data processing, loads and stores, FP and SIMD. One instruction; true when it was understood. The interpreter's
// slow path and the JIT's fallback call this, so there is one definition of these semantics.
static bool exec_slow(CPU *c, uint32_t i) {
    int rt = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16), sf = B(i, 31, 31);
    switch (B(i, 28, 25)) {
    case 0x8: case 0x9: goto x_dpimm;
    case 0x4: case 0x6: case 0xc: case 0xe: goto x_ldst;
    case 0x5: case 0xd: goto x_dpreg;
    case 0x7: case 0xf: goto x_simd;
    default: break;
    }
    // ---- hints, barriers
    if ((i & 0xfffff000) == 0xd5032000) {           // hints (NOP, YIELD, WFE, PACIASP, ...)
        if (i == 0xd503203f || i == 0xd503205f) gil_yield();  // YIELD / WFE: a spin-wait must let the other threads run
        return true;
    }
    if ((i & 0xfffff000) == 0xd5033000) {           // DMB / DSB / ISB: ARM's memory model is weaker than x86's, so
        __atomic_thread_fence(__ATOMIC_SEQ_CST);    // guest lock-free code needs a real fence here
        return true;
    }
    // ---- SVC
    if ((i & 0xffe0001f) == 0xd4000001) { do_syscall(c); return true; }

    // ---- SYS: cache maintenance. JIT engines (wasmtime, V8) write code and then clean the data cache and invalidate the instruction cache line by line;
    // an instruction-cache invalidate has to drop the translations of that code
    if ((i & 0xfff80000) == 0xd5080000) {
        uint32_t op1 = B(i, 18, 16), crn = B(i, 15, 12), crm = B(i, 11, 8), op2 = B(i, 7, 5);
        uint64_t a = rd(c, rt);
        if (crn == 7 && op1 == 3 && crm == 5 && op2 == 1) { jit_invalidate_cache_line(a); return true; }                 // IC IVAU
        if (crn == 7 && op1 == 0 && op2 == 0 && (crm == 5 || crm == 1)) { jit_invalidate_cache_line(~0ull); return true; }   // IC IALLU / IALLUIS: everything
        if (crn == 7 && op1 == 3 && op2 == 1 && (crm == 10 || crm == 11 || crm == 14 || crm == 12)) return true;     // DC CVAC / CVAU / CIVAC / CVAP: x86 caches are coherent
        if (crn == 7 && op1 == 3 && op2 == 1 && crm == 4) { memset((void *)(a & ~63ull), 0, 64); return true; }          // DC ZVA
        die(c, "unsupported SYS instruction", i);
    }
    // ---- system registers: NZCV, FPCR, FPSR, TPIDR_EL0
    if ((i & 0xffd00000) == 0xd5100000) {
        bool read = B(i, 21, 21);
        uint32_t reg = B(i, 20, 5);
        uint64_t *slot = NULL, tmp = 0;
        uint32_t fp32;
        if (reg == 0xda10) {  // NZCV
            tmp = ((uint64_t)c->n << 31) | ((uint64_t)c->z << 30) | ((uint64_t)c->c << 29) | ((uint64_t)c->v << 28);
            if (read) wr(c, rt, tmp);
            else { uint64_t v = rd(c, rt); c->n = v >> 31 & 1; c->z = v >> 30 & 1; c->c = v >> 29 & 1; c->v = v >> 28 & 1; }
            return true;
        }
        if (reg == 0xda20 || reg == 0xda21) {  // FPCR / FPSR
            uint32_t *p = reg == 0xda20 ? &c->fpcr : &c->fpsr;
            if (read) wr(c, rt, *p); else { fp32 = (uint32_t)rd(c, rt); *p = fp32; }
            return true;
        }
        if (read && (reg == 0xdf02 || reg == 0xdf01 || reg == 0xdf00)) {  // CNTVCT_EL0 / CNTPCT_EL0 (OpenSSL in Krita's Python reads this one) / CNTFRQ_EL0, in nanoseconds
            mach_timebase_info_data_t tb; mach_timebase_info(&tb);
            wr(c, rt, reg == 0xdf00 ? 1000000000ull : mach_absolute_time() * tb.numer / tb.denom);
            return true;
        }
        if (read && reg == 0xd807) { wr(c, rt, getenv("REBAR_DCZID_PROHIBIT") ? 0x10 : 0x4); return true; }        // DCZID_EL0: 64-byte DC ZVA (HotSpot refuses a prohibited one); REBAR_DCZID_PROHIBIT=1: DZP = 1, DC ZVA is prohibited, so memset / bzero code (opencode's, Go's, libc++'s) uses ordinary stores
        if (read && reg == 0xd801) { wr(c, rt, 0x8444c004); return true; }  // CTR_EL0: 64-byte instruction and data cache lines (what the cache-maintenance loops of JIT engines read)
        if (reg == 0xde82) { slot = &c->tpidr; }  // TPIDR_EL0
        if (read && reg == 0xde83) { wr(c, rt, c->tpidrro); return true; }  // TPIDRRO_EL0
        if (slot) { if (read) wr(c, rt, *slot); else *slot = rd(c, rt); return true; }
        die(c, "unsupported system register", i);
    }
    return false;

x_dpimm:
    // ---- move wide
    if (B(i, 28, 23) == 0x25) {
        int opc = B(i, 30, 29), hw = B(i, 22, 21);
        uint64_t imm = (uint64_t)B(i, 20, 5) << (hw * 16);
        if (!sf && hw > 1) die(c, "bad movw", i);
        if (opc == 0) wr(c, rt, ~imm & mask(sf));
        else if (opc == 2) wr(c, rt, imm);
        else if (opc == 3) wr(c, rt, (rd(c, rt) & ~(0xffffull << (hw * 16))) | imm);
        else die(c, "bad movw", i);
        return true;
    }
    // ---- adr / adrp
    if (B(i, 28, 24) == 0x10) {
        int64_t imm = sext((B(i, 23, 5) << 2) | B(i, 30, 29), 21);
        if (B(i, 31, 31)) wr(c, rt, (c->pc & ~0xfffull) + (uint64_t)(imm << 12));
        else wr(c, rt, c->pc + imm);
        return true;
    }
    // ---- add/sub immediate
    if (B(i, 28, 23) == 0x22) {
        int op = B(i, 30, 30), S = B(i, 29, 29);
        uint64_t imm = B(i, 21, 10) << (B(i, 22, 22) ? 12 : 0);
        uint64_t a = rd_sp(c, rn);
        uint64_t r;
        if (S) { r = op ? add_flags(c, a, ~imm, 1, sf) : add_flags(c, a, imm, 0, sf); wr(c, rt, r); }
        else { r = (op ? a - imm : a + imm) & mask(sf); wr_sp(c, rt, r); }
        return true;
    }
    // ---- logical immediate
    if (B(i, 28, 23) == 0x24) {
        int opc = B(i, 30, 29);
        uint64_t m;
        if (!decode_bitmask(B(i, 22, 22), B(i, 15, 10), B(i, 21, 16), sf, &m)) die(c, "bad logical imm", i);
        uint64_t a = rd(c, rn), r = opc == 0 || opc == 3 ? a & m : opc == 1 ? a | m : a ^ m;
        r &= mask(sf);
        if (opc == 3) { c->n = (r >> (sf ? 63 : 31)) & 1; c->z = !r; c->c = c->v = false; wr(c, rt, r); }
        else wr_sp(c, rt, r);
        return true;
    }
    // ---- bitfield
    if (B(i, 28, 23) == 0x26) {
        int opc = B(i, 30, 29), immr = B(i, 21, 16), imms = B(i, 15, 10), ds = sf ? 64 : 32;
        uint64_t src = rd(c, rn) & mask(sf), dst = opc == 1 ? rd(c, rt) : 0, r;
        int width, pos;
        if (imms >= immr) { width = imms - immr + 1; pos = 0; src >>= immr; }
        else { width = imms + 1; pos = ds - immr; }
        uint64_t fm = width >= 64 ? ~0ull : (1ull << width) - 1;
        uint64_t field = (src & fm) << pos;
        if (opc == 1) r = (dst & ~(fm << pos)) | field;
        else if (opc == 0) r = (uint64_t)sext(field, pos + width);
        else r = field;
        wr(c, rt, r & mask(sf));
        return true;
    }
    // ---- extract (EXTR / ROR imm)
    if (B(i, 28, 23) == 0x27) {
        int lsb = B(i, 15, 10), ds = sf ? 64 : 32;
        uint64_t hi = rd(c, rn) & mask(sf), lo = rd(c, rm) & mask(sf);
        wr(c, rt, lsb ? (((lo >> lsb) | (hi << (ds - lsb))) & mask(sf)) : lo);
        return true;
    }
    return false;

x_ldst:
    if ((B(i, 29, 24) == 0x08 || B(i, 29, 24) == 0x19 || (B(i, 29, 27) == 7 && B(i, 25, 24) == 0 && B(i, 21, 21) && B(i, 11, 10) == 0)) && exec_atomic(c, i)) return true;

    // ---- loads / stores (GPR and SIMD&FP registers)
    if (B(i, 29, 27) == 0x7 && (B(i, 25, 24) == 1 || B(i, 25, 24) == 0)) {
        int size = B(i, 31, 30), opc = B(i, 23, 22), V = B(i, 26, 26);
        int sc = V && (opc & 2) ? 4 : size;  // scale: 128-bit accesses are 16 bytes
        uint64_t base = rd_sp(c, rn), addr;
        int wb = 0;
        if (B(i, 25, 24) == 1) addr = base + ((uint64_t)B(i, 21, 10) << sc);
        else if (B(i, 21, 21) && B(i, 11, 10) == 2) {
            int opt = B(i, 15, 13);
            if ((opt & 2) == 0) die(c, "bad ldst ext", i);
            addr = base + extend_val(rd(c, rm), opt, B(i, 12, 12) ? sc : 0);
        } else if (!B(i, 21, 21)) {
            int idx = B(i, 11, 10);
            int64_t imm = sext(B(i, 20, 12), 9);
            if (idx == 2 && V) die(c, "unprivileged ldst", i);   // LDTR / STTR family (unprivileged: ordinary at EL0)
            addr = idx == 1 ? base : base + imm;
            if (idx == 1 || idx == 3) wb = 1, base += imm;
        } else if (!V && size == 3 && B(i, 21, 21) && B(i, 10, 10)) {   // LDRAA / LDRAB (arm64e authenticated load: the pointer is not signed here): offset S:imm9 scaled by 8, optional pre-index writeback
            int64_t off = sext((B(i, 22, 22) << 9) | B(i, 20, 12), 10) * 8;
            addr = base + (uint64_t)off;
            wr(c, rt, mem_ld(addr, 3));
            if (B(i, 11, 11)) wr_sp(c, rn, addr);
            return true;
        } else die(c, "bad ldst", i);
        if (V) vmem(c, rt, size, opc, addr);
        else if (opc == 0) mem_st(addr, size, rd(c, rt));
        else if (opc == 1) wr(c, rt, mem_ld(addr, size));
        else {
            if (size == 3) return true;  // PRFM
            uint64_t v = (uint64_t)sext(mem_ld(addr, size), 8 << size);
            wr(c, rt, opc == 3 ? v & 0xffffffff : v);
        }
        if (wb) wr_sp(c, rn, base);
        return true;
    }
    if (B(i, 29, 27) == 0x5 && !B(i, 25, 25)) {  // LDP / STP
        int opc = B(i, 31, 30), L = B(i, 22, 22), idx = B(i, 24, 23), V = B(i, 26, 26);
        int rt2 = B(i, 14, 10);
        int scale = V ? 2 + opc : (opc == 2 ? 3 : 2);
        int64_t imm = sext(B(i, 21, 15), 7) * (1 << scale);
        uint64_t base = rd_sp(c, rn);
        uint64_t addr = idx == 1 ? base : base + imm;
        int esz = opc == 2 ? 3 : 2;
        if (V) {
            int bytes = 1 << scale;
            if (L) {
                VReg a, b; memset(&a, 0, 16); memset(&b, 0, 16);
                memcpy(&a, (void *)addr, bytes); memcpy(&b, (void *)(addr + bytes), bytes);
                c->vr[rt] = a; c->vr[rt2] = b;
            } else {
                memcpy((void *)addr, &c->vr[rt], bytes); memcpy((void *)(addr + bytes), &c->vr[rt2], bytes);
            }
        } else if (L) {
            uint64_t a = mem_ld(addr, esz), b = mem_ld(addr + (1 << esz), esz);
            if (opc == 1) { a = (uint64_t)sext(a, 32); b = (uint64_t)sext(b, 32); }
            wr(c, rt, a); wr(c, rt2, b);
        } else {
            mem_st(addr, esz, rd(c, rt)); mem_st(addr + (1 << esz), esz, rd(c, rt2));
        }
        if (idx == 1 || idx == 3) wr_sp(c, rn, base + imm);
        return true;
    }
    if (B(i, 29, 27) == 0x3 && B(i, 25, 24) == 0) {  // LDR literal
        int opc = B(i, 31, 30);
        uint64_t addr = c->pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
        if (B(i, 26, 26)) vmem(c, rt, 2 + opc, 1, addr);  // S, D or Q (opc 2 -> 16 bytes)
        else if (opc == 0) wr(c, rt, mem_ld(addr, 2));
        else if (opc == 1) wr(c, rt, mem_ld(addr, 3));
        else if (opc == 2) wr(c, rt, (uint64_t)sext(mem_ld(addr, 2), 32));
        return true;
    }
    goto x_simd;

x_dpreg:
    // ---- add/sub, logical: shifted register (+ extended register)
    if (B(i, 28, 24) == 0x0b) {
        int op = B(i, 30, 30), S = B(i, 29, 29);
        uint64_t a, b, r;
        if (B(i, 21, 21)) {  // extended register
            a = rd_sp(c, rn);
            b = extend_val(rd(c, rm), B(i, 15, 13), B(i, 12, 10));
        } else {
            a = rd(c, rn);
            b = shift_val(rd(c, rm), B(i, 23, 22), B(i, 15, 10), sf);
        }
        if (S) { r = op ? add_flags(c, a, ~b, 1, sf) : add_flags(c, a, b, 0, sf); wr(c, rt, r); }
        else { r = (op ? a - b : a + b) & mask(sf); if (B(i, 21, 21)) wr_sp(c, rt, r); else wr(c, rt, r); }
        return true;
    }
    if (B(i, 28, 24) == 0x0a) {
        int opc = B(i, 30, 29), N = B(i, 21, 21);
        uint64_t a = rd(c, rn), b = shift_val(rd(c, rm), B(i, 23, 22), B(i, 15, 10), sf);
        if (N) b = ~b & mask(sf);
        uint64_t r = (opc == 0 || opc == 3 ? a & b : opc == 1 ? a | b : a ^ b) & mask(sf);
        if (opc == 3) { c->n = (r >> (sf ? 63 : 31)) & 1; c->z = !r; c->c = c->v = false; }
        wr(c, rt, r);
        return true;
    }
    // ---- add/sub with carry
    if (B(i, 28, 21) == 0xd0 && B(i, 15, 10) == 0) {
        int op = B(i, 30, 30), S = B(i, 29, 29);
        uint64_t a = rd(c, rn), b = rd(c, rm);
        if (op) b = ~b;
        bool sc = c->c, sn = c->n, sz = c->z, sv = c->v;
        uint64_t r = add_flags(c, a, b, sc, sf);
        if (!S) { c->c = sc; c->n = sn; c->z = sz; c->v = sv; }
        wr(c, rt, r);
        return true;
    }
    // ---- FlagM: SETF8 / SETF16 and RMIF
    if ((i & 0xffffbc1f) == 0x3a00080d) {
        int hi = B(i, 14, 14) ? 15 : 7; uint32_t v = (uint32_t)rd(c, rn);
        c->n = (v >> hi) & 1; c->z = (v & ((2u << hi) - 1)) == 0; c->v = ((v >> (hi + 1)) ^ (v >> hi)) & 1;
        return true;
    }
    if ((i & 0xffe07c10) == 0xba000400) {
        int sh = B(i, 20, 15), mk = B(i, 3, 0); uint64_t x = rd(c, rn), t = sh ? (x >> sh) | (x << (64 - sh)) : x;
        if (mk & 8) c->n = (t >> 3) & 1;
        if (mk & 4) c->z = (t >> 2) & 1;
        if (mk & 2) c->c = (t >> 1) & 1;
        if (mk & 1) c->v = t & 1;
        return true;
    }
    // ---- conditional compare
    if (B(i, 29, 21) == 0x1d2 && !B(i, 10, 10) && !B(i, 4, 4)) {
        int cond = B(i, 15, 12), nzcv = B(i, 3, 0);
        if (cond_holds(c, cond)) {
            uint64_t b = B(i, 11, 11) ? (uint64_t)rm : rd(c, rm);
            if (B(i, 30, 30)) add_flags(c, rd(c, rn), ~b, 1, sf);
            else add_flags(c, rd(c, rn), b, 0, sf);
        } else { c->n = nzcv >> 3 & 1; c->z = nzcv >> 2 & 1; c->c = nzcv >> 1 & 1; c->v = nzcv & 1; }
        return true;
    }
    // ---- conditional select
    if (B(i, 29, 21) == 0xd4 && !B(i, 11, 11)) {
        int op = B(i, 30, 30), op2 = B(i, 10, 10);
        uint64_t r;
        if (cond_holds(c, B(i, 15, 12))) r = rd(c, rn);
        else {
            r = rd(c, rm);
            if (op) r = ~r;
            if (op2) r += 1;
        }
        wr(c, rt, r & mask(sf));
        return true;
    }
    // ---- data processing 2 source
    if (B(i, 30, 21) == 0x0d6) {
        int op = B(i, 15, 10);
        uint64_t a = rd(c, rn) & mask(sf), b = rd(c, rm) & mask(sf), r;
        switch (op) {
        case 2: r = b ? a / b : 0; break;
        case 3: r = !b ? 0 : (sf ? (b == ~0ull ? 0 - a : (uint64_t)((int64_t)a / (int64_t)b)) : ((uint32_t)b == ~0u ? (uint32_t)(0 - (uint32_t)a) : (uint32_t)((int32_t)a / (int32_t)b))); break;   // (INT_MIN / -1 is INT_MIN: the C division would trap)
        case 8: r = shift_val(a, 0, b, sf); break;
        case 9: r = shift_val(a, 1, b, sf); break;
        case 10: r = shift_val(a, 2, b, sf); break;
        case 11: r = shift_val(a, 3, b, sf); break;
        case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23:  // CRC32{B,H,W,X}, CRC32C{B,H,W,X}
            r = crc32_insn((uint32_t)a, b, 1 << (op & 3), op & 4); break;
        case 12: {   // PACGA Xd, Xn, Xm|SP: a 32-bit code in the top half (any fixed function of the inputs: nothing here checks it against a key)
            uint64_t mod = rm == 31 ? c->sp : b, h = (a ^ (mod * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull; h ^= h >> 29;
            r = (h & 0xffffffff00000000ull); break; }
        default: die(c, "unsupported dp2src", i);
        }
        wr(c, rt, r & mask(sf));
        return true;
    }
    // ---- pointer authentication (PACIA/AUTIA/XPAC...): pointers are never signed here, so signing,
    //      authenticating and stripping are all the identity
    if (B(i, 30, 21) == 0x2d6 && B(i, 20, 16) == 1 && sf) return true;
    // ---- data processing 1 source
    if (B(i, 30, 21) == 0x2d6 && !B(i, 20, 16)) {
        int op = B(i, 15, 10);
        uint64_t a = rd(c, rn) & mask(sf), r;
        if (op == 0) r = sf ? __builtin_bitreverse64(a) : __builtin_bitreverse32((uint32_t)a);
        else if (op == 1) {  // REV16
            uint64_t m = sf ? 0x00ff00ff00ff00ffull : 0x00ff00ffull;
            r = ((a & m) << 8) | ((a >> 8) & m);
        } else if (op == 2) {  // REV32 (64-bit) / REV (32-bit)
            r = sf ? ((uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32) | __builtin_bswap32((uint32_t)a)
                   : __builtin_bswap32((uint32_t)a);
        } else if (op == 3 && sf) r = __builtin_bswap64(a);
        else if (op == 4) r = a ? __builtin_clzll(a) - (sf ? 0 : 32) : (sf ? 64 : 32);
        else if (op == 5) {  // CLS
            uint64_t t = sf ? (a ^ (uint64_t)((int64_t)a >> 1)) : (uint32_t)((int32_t)a ^ ((int32_t)a >> 1));
            r = (t ? __builtin_clzll(t) - (sf ? 0 : 32) : (sf ? 64 : 32)) - 1;
        } else die(c, "unsupported dp1src", i);
        wr(c, rt, r);
        return true;
    }
    // ---- data processing 3 source
    if (B(i, 28, 24) == 0x1b) {
        int op31 = B(i, 23, 21), o0 = B(i, 15, 15), ra = B(i, 14, 10);
        uint64_t r;
        if (op31 == 0) {
            uint64_t p = rd(c, rn) * rd(c, rm);
            r = o0 ? rd(c, ra) - p : rd(c, ra) + p;
        } else if (op31 == 1) {
            uint64_t p = (uint64_t)((int64_t)(int32_t)rd(c, rn) * (int64_t)(int32_t)rd(c, rm));
            r = o0 ? rd(c, ra) - p : rd(c, ra) + p;
        } else if (op31 == 5) {
            uint64_t p = (uint64_t)(uint32_t)rd(c, rn) * (uint32_t)rd(c, rm);
            r = o0 ? rd(c, ra) - p : rd(c, ra) + p;
        } else if (op31 == 2) r = (uint64_t)(((__int128)(int64_t)rd(c, rn) * (int64_t)rd(c, rm)) >> 64);
        else if (op31 == 6) r = (uint64_t)(((unsigned __int128)rd(c, rn) * rd(c, rm)) >> 64);
        else die(c, "unsupported dp3src", i);
        wr(c, rt, r & mask(sf));
        return true;
    }
    return false;

x_simd:
    if (exec_fpsimd(c, i)) return true;

    return false;
}

#include "jit.h"
static void jit_invalidate_cache_line(uint64_t a) { if (!g_jit_on) return; if (a == ~0ull) jit_invalidate(0, ~0ull); else jit_invalidate(a & ~63ull, (a & ~63ull) + 64); }   // IC IVAU: one cache line (64 bytes), IC IALLU: all

// The interpreter loop. Instruction dispatch is threaded: the commonest instructions are found through a table
// indexed by the top byte of the encoding and every such handler ends by fetching and dispatching the next
// instruction itself, so the processor's indirect-branch predictor sees one branch per handler instead of one for
// the whole interpreter. Everything else goes through the group switch (L_group) and the chain of tests below it.
static void run_until_inner(CPU *c, uint64_t stop_pc);
static void run_until(CPU *c, uint64_t stop_pc) {
    jmp_buf jb; jmp_buf *prev = t_fault_jb;
    t_fault_jb = &jb;
    _setjmp(jb);   // (returns again after a delivered fault: the pc and registers are the handler's, the loop starts over from them)
    run_until_inner(c, stop_pc);
    t_fault_jb = prev;
}
static void run_until_inner(CPU *c, uint64_t stop_pc) {
    static const void *tbl[256], *tbl_dbg[256];
    static bool tbl_ok;
    uint32_t i;
    uint64_t npc;
    int rt, rn, rm, sf;
    if (__builtin_expect(!tbl_ok, 0)) {
        for (int b = 0; b < 256; b++)  // by the encoding group (bits 28:25), which the top byte contains
            tbl[b] = ((b >> 1) & 0xe) == 0xa ? &&L_br : &&L_slow;
        tbl[0x0a] = tbl[0x2a] = tbl[0x4a] = tbl[0x6a] = tbl[0x8a] = tbl[0xaa] = tbl[0xca] = tbl[0xea] = &&H_logic_sr;
        tbl[0x0b] = tbl[0x2b] = tbl[0x4b] = tbl[0x6b] = tbl[0x8b] = tbl[0xab] = tbl[0xcb] = tbl[0xeb] = &&H_addsub_sr;
        tbl[0x13] = tbl[0x33] = tbl[0x53] = tbl[0x93] = tbl[0xb3] = tbl[0xd3] = &&H_bitfield;
        tbl[0x1a] = tbl[0x5a] = tbl[0x9a] = tbl[0xda] = &&H_csel;
        tbl[0x91] = tbl[0xd1] = tbl[0x11] = tbl[0x51] = &&H_addsub_imm;
        tbl[0x31] = tbl[0x71] = tbl[0xb1] = tbl[0xf1] = &&H_addsubs_imm;
        tbl[0xf9] = &&H_ldst_x_uimm; tbl[0xb9] = &&H_ldst_w_uimm; tbl[0x79] = &&H_ldst_h_uimm; tbl[0x39] = &&H_ldst_b_uimm;
        tbl[0xf8] = tbl[0xb8] = tbl[0x38] = tbl[0x78] = &&H_ldst_reg;
        tbl[0xa8] = tbl[0xa9] = tbl[0x28] = tbl[0x29] = &&H_ldp_stp;
        tbl[0x90] = tbl[0xb0] = tbl[0xd0] = tbl[0xf0] = &&H_adrp;
        tbl[0x10] = tbl[0x30] = tbl[0x50] = tbl[0x70] = &&H_adr;
        tbl[0xd2] = tbl[0x52] = tbl[0xf2] = tbl[0x72] = tbl[0x92] = tbl[0x12] = &&H_movw;
        tbl[0x54] = &&H_bcond;
        tbl[0x34] = tbl[0x35] = tbl[0xb4] = tbl[0xb5] = &&H_cbz;
        tbl[0x36] = tbl[0x37] = tbl[0xb6] = tbl[0xb7] = &&H_tbz;
        tbl[0x14] = tbl[0x15] = tbl[0x16] = tbl[0x17] = tbl[0x94] = tbl[0x95] = tbl[0x96] = tbl[0x97] = &&H_b;
        tbl[0xd6] = &&H_br;
        tbl[0x0e] = tbl[0x2e] = tbl[0x4e] = tbl[0x6e] = tbl[0x0f] = tbl[0x2f] = tbl[0x4f] = tbl[0x6f] = &&H_simd;
        tbl[0x3c] = tbl[0x3d] = tbl[0x7c] = tbl[0x7d] = tbl[0xbc] = tbl[0xbd] = tbl[0xfc] = tbl[0xfd] = &&H_vmem;
        tbl[0x2c] = tbl[0x2d] = tbl[0x6c] = tbl[0x6d] = tbl[0xac] = tbl[0xad] = &&H_vpair;
        tbl[0x0c] = tbl[0x4c] = &&H_vmulti;
        tbl[0x0d] = tbl[0x4d] = &&H_vsingle;
        for (int b = 0; b < 256; b++) tbl_dbg[b] = &&L_dbg;
        tbl_ok = true;
    }

    // Fetch the instruction at pc and jump to its handler (the code is repeated at the end of every fast handler).
    // DISPATCH is the full form, for anywhere the pc may have moved somewhere new: it stops at stop_pc, finds stub
    // addresses and code outside the cached range, and every 8192 instructions publishes the count and notices that
    // the executable ranges have changed. Instructions that cannot branch end with NEXT_SEQ, which is just the fetch.
    const void *const *T = g_dbg ? tbl_dbg : tbl;
    uint64_t ticks = 0;
#ifdef OPSTATS
#define OPCOUNT() do { uint32_t k_ = ((i >> 21) << 6) | ((i >> 10) & 63); g_opn[k_ & 0x1ffff]++; g_ope[k_ & 0x1ffff] = i; } while (0)
#else
#define OPCOUNT() do { } while (0)
#endif
#define FETCH()                                                                                                 \
    do {                                                                                                        \
        ticks++;                                                                                                \
        i = *(uint32_t *)c->pc;                                                                                 \
        OPCOUNT();                                                                                              \
        npc = c->pc + 4;                                                                                        \
        rt = B(i, 4, 0); rn = B(i, 9, 5); rm = B(i, 20, 16); sf = B(i, 31, 31);                                 \
        goto *T[i >> 24];                                                                                       \
    } while (0)
#define DISPATCH()                                                                                              \
    do {                                                                                                        \
        if (__builtin_expect(c->pc == stop_pc, 0)) { c->tick += ticks; return; }                                \
        if (__builtin_expect(c->pc - c->code_lo >= c->code_len, 0)) {                                           \
            /* stub addresses, and code outside the guest image (host function pointers reached through */     \
            /* vtables or callbacks), are forwarded instead of interpreted */                                   \
            if (c->pc - g_stub_base < g_stub_size || (c->pc - g_guest_lo >= g_guest_size && !guest_owned_code(c->pc))) { \
                c->tick += ticks; ticks = 0;                                                                    \
                if (__builtin_expect(g_die_jmp != NULL, 0)) { fprintf(stderr, "selftest: the pc left the code area: %llx (x27 %llx x30 %llx)\n", (unsigned long long)c->pc, (unsigned long long)c->x[27], (unsigned long long)c->x[30]); longjmp(*g_die_jmp, 1); } \
                stub_call(c);                                                                                   \
                goto fetch;                                                                                     \
            }                                                                                                   \
            c->code_epoch = g_code_epoch;                                                                       \
            if (c->pc - g_guest_lo < g_guest_size) { c->code_lo = g_guest_lo; c->code_len = g_guest_size; }     \
            else guest_code_range(&c->code_lo, &c->code_len);                                                   \
        }                                                                                                       \
        if (__builtin_expect(ticks >= 8192, 0)) {                                                               \
            c->tick += ticks; ticks = 0;                                                                        \
            if (c->code_epoch != g_code_epoch) c->code_len = 0;                                                 \
        }                                                                                                       \
        if (g_jit_on && jit_try(c, &ticks)) goto fetch;                                                         \
        FETCH();                                                                                                \
    } while (0)
#define NEXT() do { c->pc = npc; DISPATCH(); } while (0)
#define NEXT_SEQ() do { c->pc = npc; FETCH(); } while (0)

fetch:
    DISPATCH();

L_dbg:  // REBAR_WATCH, REBAR_WATCH_MEM and -t: the debugging table sends every instruction through here
    c->tick++;
    if (g_wmem != 0) watch_mem_check(c);
    if (g_watch_pc != 0 && (c->pc == g_watch_pc || c->pc == g_watch_more[0] || c->pc == g_watch_more[1] || c->pc == g_watch_more[2] || c->pc == g_watch_more[3])) {
        fprintf(stderr, "[watch %llx]", (unsigned long long)c->pc);
        for (int k = 0; k < 31; k++) fprintf(stderr, " x%d=%llx", k, (unsigned long long)c->x[k]);
        fprintf(stderr, " sp=%llx\n", (unsigned long long)c->sp);
        if (getenv("REBAR_WATCH_DUMP")) {   // REBAR_WATCH_DUMP=<n>: also the n words at the address in x<REBAR_WATCH_REG> (default x1)
            const char *rg = getenv("REBAR_WATCH_REG"); int r = rg ? atoi(rg) : 1; int n = atoi(getenv("REBAR_WATCH_DUMP"));
            uint64_t a = c->x[r];
            fprintf(stderr, "  x%d words:", r);
            for (int k = 0; k < n; k++) fprintf(stderr, " %llx", (unsigned long long)((uint64_t *)a)[k]);
            fprintf(stderr, "\n");
        }
    }
    if (g_trace) fprintf(stderr, "%12llx: %08x\n", (unsigned long long)c->pc, i);
    goto *tbl[i >> 24];

H_addsub_imm: {  // ADD / SUB (immediate), no flags
    if (B(i, 23, 23)) goto L_group;
    uint64_t imm = (uint64_t)B(i, 21, 10) << (B(i, 22, 22) ? 12 : 0), a = rd_sp(c, rn);
    uint64_t r = B(i, 30, 30) ? a - imm : a + imm;
    wr_sp(c, rt, sf ? r : r & 0xffffffff);
    NEXT_SEQ();
}
H_addsubs_imm: {  // ADDS / SUBS (immediate): CMN, CMP
    if (B(i, 23, 23)) goto L_group;
    uint64_t imm = (uint64_t)B(i, 21, 10) << (B(i, 22, 22) ? 12 : 0), a = rd_sp(c, rn);
    wr(c, rt, B(i, 30, 30) ? add_flags(c, a, ~imm, 1, sf) : add_flags(c, a, imm, 0, sf));
    NEXT_SEQ();
}
#define LDST_UIMM(LABEL, SIZE)                                                                   \
    LABEL: {  /* LDR / STR (immediate, unsigned offset), general registers; the size is known here */ \
        int opc = B(i, 23, 22);                                                                  \
        uint64_t addr = rd_sp(c, rn) + ((uint64_t)B(i, 21, 10) << SIZE);                         \
        if (opc == 0) mem_st(addr, SIZE, rd(c, rt));                                             \
        else if (opc == 1) wr(c, rt, mem_ld(addr, SIZE));                                        \
        else if (SIZE != 3) {                                                                    \
            uint64_t v = (uint64_t)sext(mem_ld(addr, SIZE), 8 << SIZE);                          \
            wr(c, rt, opc == 3 ? v & 0xffffffff : v);                                            \
        }                                                                                        \
        NEXT_SEQ();                                                                                  \
    }
LDST_UIMM(H_ldst_x_uimm, 3)
LDST_UIMM(H_ldst_w_uimm, 2)
LDST_UIMM(H_ldst_h_uimm, 1)
LDST_UIMM(H_ldst_b_uimm, 0)
#undef LDST_UIMM
H_ldst_reg: {  // LDR / STR (register offset, unscaled, pre- and post-index), general registers
    int size = B(i, 31, 30), opc = B(i, 23, 22);
    uint64_t base = rd_sp(c, rn), addr;
    int wb = 0;
    if (B(i, 21, 21)) {
        if (B(i, 11, 10) != 2 || (B(i, 15, 13) & 2) == 0) goto L_group;
        addr = base + extend_val(rd(c, rm), B(i, 15, 13), B(i, 12, 12) ? size : 0);
    } else {
        int idx = B(i, 11, 10);
        if (idx == 2) goto L_group;
        int64_t imm = sext(B(i, 20, 12), 9);
        addr = idx == 1 ? base : base + imm;
        if (idx == 1 || idx == 3) wb = 1, base += imm;
    }
    if (opc == 0) mem_st(addr, size, rd(c, rt));
    else if (opc == 1) wr(c, rt, mem_ld(addr, size));
    else if (size != 3) {  // (size 3 is PRFM)
        uint64_t v = (uint64_t)sext(mem_ld(addr, size), 8 << size);
        wr(c, rt, opc == 3 ? v & 0xffffffff : v);
    }
    if (wb) wr_sp(c, rn, base);
    NEXT_SEQ();
}
H_ldp_stp: {  // LDP / STP, general registers
    int opc = B(i, 31, 30), L = B(i, 22, 22), idx = B(i, 24, 23), rt2 = B(i, 14, 10);
    if (opc == 1) goto L_group;  // LDPSW
    int esz = opc == 2 ? 3 : 2;
    int64_t imm = sext(B(i, 21, 15), 7) * (1 << esz);
    uint64_t base = rd_sp(c, rn);
    uint64_t addr = idx == 1 ? base : base + imm;
    if (L) { uint64_t a = mem_ld(addr, esz), b = mem_ld(addr + (1 << esz), esz); wr(c, rt, a); wr(c, rt2, b); }
    else { mem_st(addr, esz, rd(c, rt)); mem_st(addr + (1 << esz), esz, rd(c, rt2)); }
    if (idx == 1 || idx == 3) wr_sp(c, rn, base + imm);
    NEXT_SEQ();
}
H_adrp: {
    int64_t imm = sext((B(i, 23, 5) << 2) | B(i, 30, 29), 21);
    wr(c, rt, (c->pc & ~0xfffull) + (uint64_t)(imm << 12));
    NEXT_SEQ();
}
H_adr: {
    int64_t imm = sext((B(i, 23, 5) << 2) | B(i, 30, 29), 21);
    wr(c, rt, c->pc + imm);
    NEXT_SEQ();
}
H_movw: {  // MOVZ / MOVK / MOVN
    int opc = B(i, 30, 29), hw = B(i, 22, 21);
    uint64_t imm = (uint64_t)B(i, 20, 5) << (hw * 16);
    if (!B(i, 23, 23) || (!sf && hw > 1)) goto L_group;  // (bit 23 clear is a logical immediate)
    if (opc == 0) wr(c, rt, ~imm & mask(sf));
    else if (opc == 2) wr(c, rt, imm);
    else if (opc == 3) wr(c, rt, ((rd(c, rt) & ~(0xffffull << (hw * 16))) | imm) & mask(sf));
    else goto L_group;
    NEXT_SEQ();
}
H_bcond:
    if (B(i, 4, 4)) goto L_group;
    if (cond_holds(c, B(i, 3, 0))) npc = c->pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
    NEXT();
H_cbz: {  // CBZ / CBNZ
    bool z = (rd(c, rt) & mask(sf)) == 0;
    if (z != (bool)B(i, 24, 24)) npc = c->pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
    NEXT();
}
H_tbz: {  // TBZ / TBNZ
    int bit = (B(i, 31, 31) << 5) | B(i, 23, 19);
    bool set = (rd(c, rt) >> bit) & 1;
    if (set == (bool)B(i, 24, 24)) npc = c->pc + (uint64_t)(sext(B(i, 18, 5), 14) * 4);
    NEXT();
}
H_b:  // B / BL
    if (B(i, 31, 31)) c->x[30] = c->pc + 4;
    npc = c->pc + (uint64_t)(sext(B(i, 25, 0), 26) * 4);
    NEXT();
H_simd:  // AdvSIMD data processing: the commonest forms here, the rest by exec_fpsimd
    if (__builtin_expect(!g_no_simd_fast, 1) && simd_fast(c, i)) NEXT_SEQ();
    goto L_slow;
H_vmem:  // LDR / STR of a SIMD&FP register
    if (B(i, 21, 21) && B(i, 11, 10) == 0) goto L_group;  // (not a load or store of a vector register)
    if (__builtin_expect(!g_no_simd_fast, 1) && vmem_fast(c, i)) NEXT_SEQ();
    goto L_group;
H_vpair:  // LDP / STP of SIMD&FP registers
    if (__builtin_expect(!g_no_simd_fast, 1) && vpair_fast(c, i)) NEXT_SEQ();
    goto L_group;
H_vsingle:  // LD1 / ST1 (one lane), LD1R
    if (__builtin_expect(!g_no_simd_fast, 1) && (vld1r_fast(c, i) || vsingle_fast(c, i))) NEXT_SEQ();
    goto L_group;
H_vmulti:  // LD1..LD4 / ST1..ST4
    if (__builtin_expect(!g_no_simd_fast, 1) && vmulti_fast(c, i)) NEXT_SEQ();
    goto L_group;
H_br:  // BR / BLR / RET
    if ((i & 0xfe1ffc1f) != 0xd61f0000 || B(i, 24, 21) > 2) goto L_group;
    if (B(i, 24, 21) == 1) c->x[30] = c->pc + 4;
    npc = c->x[rn];
    NEXT();

H_logic_sr: {  // AND / ORR / EOR / ANDS / BIC / ORN / EON / BICS (shifted register)
    int opc = B(i, 30, 29);
    uint64_t a = rd(c, rn), b = shift_val(rd(c, rm), B(i, 23, 22), B(i, 15, 10), sf);
    if (B(i, 21, 21)) b = ~b & mask(sf);
    uint64_t r = (opc == 0 || opc == 3 ? a & b : opc == 1 ? a | b : a ^ b) & mask(sf);   // (a W operation clears the top half whatever the source registers held)
    if (opc == 3) { c->n = (r >> (sf ? 63 : 31)) & 1; c->z = !r; c->c = c->v = false; }
    wr(c, rt, r);
    NEXT_SEQ();
}
H_addsub_sr: {  // ADD / SUB / ADDS / SUBS (shifted or extended register)
    int op = B(i, 30, 30), S = B(i, 29, 29);
    uint64_t a, b, r;
    if (B(i, 21, 21)) {
        a = rd_sp(c, rn);
        b = extend_val(rd(c, rm), B(i, 15, 13), B(i, 12, 10));
    } else {
        a = rd(c, rn);
        b = shift_val(rd(c, rm), B(i, 23, 22), B(i, 15, 10), sf);
    }
    if (S) { r = op ? add_flags(c, a, ~b, 1, sf) : add_flags(c, a, b, 0, sf); wr(c, rt, r); }
    else { r = (op ? a - b : a + b) & mask(sf); if (B(i, 21, 21)) wr_sp(c, rt, r); else wr(c, rt, r); }
    NEXT_SEQ();
}
H_bitfield: {  // SBFM / BFM / UBFM (and so LSL, LSR, ASR, UBFX, SXTW, ...)
    if (B(i, 23, 23)) goto L_group;  // (EXTR)
    int opc = B(i, 30, 29), immr = B(i, 21, 16), imms = B(i, 15, 10), ds = sf ? 64 : 32;
    uint64_t src = rd(c, rn) & mask(sf), dst = opc == 1 ? rd(c, rt) : 0, r;
    int width, pos;
    if (imms >= immr) { width = imms - immr + 1; pos = 0; src >>= immr; }
    else { width = imms + 1; pos = ds - immr; }
    uint64_t fm = width >= 64 ? ~0ull : (1ull << width) - 1;
    uint64_t field = (src & fm) << pos;
    if (opc == 1) r = (dst & ~(fm << pos)) | field;
    else if (opc == 0) r = (uint64_t)sext(field, pos + width);
    else r = field;
    wr(c, rt, r & mask(sf));
    NEXT_SEQ();
}
H_csel: {  // CSEL / CSINC / CSINV / CSNEG
    if (B(i, 29, 21) != 0xd4 || B(i, 11, 11)) goto L_group;
    int op = B(i, 30, 30), op2 = B(i, 10, 10);
    uint64_t r;
    if (cond_holds(c, B(i, 15, 12))) r = rd(c, rn);
    else {
        r = rd(c, rm);
        if (op) r = ~r;
        if (op2) r += 1;
    }
    wr(c, rt, r & mask(sf));
    NEXT_SEQ();
}

L_group:
    // The top-level encoding group (bits 28:25) picks the section: 101x is the branches (and the system instructions,
    // which exec_slow handles when none of the branch tests match); everything else is exec_slow's.
    if (B(i, 28, 26) == 0x5) goto L_br;
    goto L_slow;

L_br:
    // ---- branches
    if (B(i, 30, 26) == 0x5) {  // B / BL
        if (B(i, 31, 31)) c->x[30] = c->pc + 4;
        npc = c->pc + (uint64_t)(sext(B(i, 25, 0), 26) * 4);
        goto done;
    }
    if ((i & 0xff000010) == 0x54000000) {  // B.cond
        if (cond_holds(c, B(i, 3, 0))) npc = c->pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
        goto done;
    }
    if (B(i, 30, 25) == 0x1a) {  // CBZ / CBNZ
        bool z = (rd(c, rt) & mask(sf)) == 0;
        if (z != (bool)B(i, 24, 24)) npc = c->pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
        goto done;
    }
    if (B(i, 30, 25) == 0x1b) {  // TBZ / TBNZ
        int bit = (B(i, 31, 31) << 5) | B(i, 23, 19);
        bool set = (rd(c, rt) >> bit) & 1;
        if (set == (bool)B(i, 24, 24)) npc = c->pc + (uint64_t)(sext(B(i, 18, 5), 14) * 4);
        goto done;
    }
    if ((i & 0xfe1ffc1f) == 0xd61f0000) {  // BR / BLR / RET
        int opc = B(i, 24, 21);
        uint64_t t = c->x[rn];
        if (opc == 1) c->x[30] = c->pc + 4;
        else if (opc != 0 && opc != 2) die(c, "unsupported branch-reg", i);
        npc = t;
        goto done;
    }

    // BRAA/BLRAA/BRAAZ/BLRAAZ/RETAA/RETAB: branches whose target authentication always succeeds
    if ((i & 0xfe1ff800) == 0xd61f0800) {
        int kind = B(i, 24, 21) & 7;
        if (kind == 4) die(c, "unsupported ERETAA/ERETAB", i);
        uint64_t t = kind == 2 ? c->x[30] : c->x[rn];
        if (kind == 1) c->x[30] = c->pc + 4;
        npc = t;
        goto done;
    }
    goto L_slow;

L_slow:
    if (exec_slow(c, i)) goto done;
    die(c, "unimplemented instruction", i);

done:
    c->pc = npc;
    DISPATCH();
#undef NEXT
#undef DISPATCH
}

// --------------------------------------------------------------------- loader

// rebar --probe-insns: A64 words on stdin (hex, one per line), each run once by the interpreter with every register pointing into a scratch window: prints OK (ran), FAULT (an access went
// outside the window: the decoder accepted it) or DIED <reason> (the interpreter does not implement it). Branches are skipped. A coverage tool: the words come from a disassembler.
static sigjmp_buf g_probe_jb;
static void probe_fault(int sig) { (void)sig; siglongjmp(g_probe_jb, 1); }
static int probe_insns(void) {
    size_t sz = 1 << 20;
    uint8_t *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = probe_fault; sa.sa_flags = SA_NODEFER; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    CPU *a = calloc(1, sizeof(CPU));
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        uint32_t w = (uint32_t)strtoul(line, NULL, 16);
        if (B(w, 30, 26) == 0x5 || (w & 0xfe000000) == 0xd6000000 || (w & 0x7e000000) == 0x34000000 || (w & 0xff000010) == 0x54000000 || (w & 0xff000000) == 0xd4000000 || (w & 0x7f000000) == 0x36000000) { printf("%08x SKIP\n", w); continue; }
        memset(a, 0, sizeof *a);
        for (int k = 0; k < 31; k++) a->x[k] = (uint64_t)mem + (sz / 2) + (uint64_t)k * 64;
        a->sp = (uint64_t)mem + sz / 2 + 4096;
        for (int k = 0; k < 32; k++) { a->vr[k].d[0] = 0x3ff0000000000000ull + (uint64_t)k; a->vr[k].d[1] = 0x4000000000000000ull; }
        code[0] = w; code[1] = 0x14000000 | ((2048 - 4) / 4);   // (b to the stop address: run_until looks for it at branches)
        a->pc = (uint64_t)code; a->code_lo = (uint64_t)code; a->code_len = 4096;
        jmp_buf jb; volatile int died = 0, faulted = 0;
        g_die_jmp = &jb; g_die_why = NULL; g_jit_on = false; t_cpu = a;
        if (sigsetjmp(g_probe_jb, 1)) { faulted = 1; }
        else if (setjmp(jb) == 0) run_until(a, (uint64_t)code + 2048); else died = 1;
        g_die_jmp = NULL;
        if (died) printf("%08x DIED %s\n", w, g_die_why ? g_die_why : "?"); else printf("%08x %s\n", w, faulted ? "FAULT" : "OK");
    }
    return 0;
}
// rebar --simd-selftest [count]: random AdvSIMD encodings, run once with the fast paths (simdfast.h) and once without,
// from the same random state; any difference in registers, flags or memory is printed.
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd64(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static void st_fill(CPU *c, int mode) {
    for (int r = 0; r < 32; r++) {
        for (int k = 0; k < 16; k++) {
            uint64_t v = rnd64();
            switch (mode) {
            case 0: c->vr[r].b[k] = (uint8_t)v; break;
            case 1: c->vr[r].b[k] = ((const uint8_t[]){0, 1, 0x7f, 0x80, 0xfe, 0xff, 2, 0x40})[v & 7]; break;
            default: c->vr[r].b[k] = (uint8_t)v; break;
            }
        }
        if (mode == 2) for (int k = 0; k < 4; k++) c->vr[r].f[k] = (float)((int)(rnd64() % 20001) - 10000) / (float)(1 + rnd64() % 100);
        if (mode == 3) for (int k = 0; k < 2; k++) c->vr[r].df[k] = (double)((int)(rnd64() % 2000001) - 1000000) / (double)(1 + rnd64() % 1000);
    }
}
static int simd_selftest(long count) {
    size_t sz = 1 << 18;
    uint8_t *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint32_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    long bad = 0, died_generic = 0, accepted_by_fast = 0, tested = 0;
    CPU *a = calloc(1, sizeof(CPU)), *b = calloc(1, sizeof(CPU));
    static const struct { const char *name; uint32_t base, varmask; } cls[] = {
        {"3same", 0x0e200400, 0x7fdff7ff & ~0x00000400u}, {"3diff", 0x0e200000, 0x7fdff3ff}, {"shift", 0x0f000400, 0x7f7ff7ff & ~0x00000400u},
        {"copy", 0x0e000400, 0x7fff7bff}, {"ldst-reg", 0x3c000000, 0xffffffff}, {"ldst-pair", 0x2c000000, 0xffffffff},
        {"ld1-st1", 0x0c000000, 0x40c0ffff}, {"misc2", 0x0e200800, 0x60dff3ff}, {"ld1-lane", 0x0d000000, 0x40c0ffff}, {"cvt-fixed", 0x0f20fc00, 0x6000ffff},
    };
    int ncls = sizeof cls / sizeof cls[0];
    long handled[16] = {0}, badc[16] = {0}, genericdie[16] = {0}, fastc[16] = {0};
    for (long it = 0; it < count; it++) {
        int k = (int)(rnd64() % ncls);
        uint32_t r = (uint32_t)rnd64(), i;
        switch (k) {
        case 0: i = 0x0e200400 | (r & 0x60dffbff); break;
        case 1: i = 0x0e200000 | (r & 0x60dff3ff); break;
        case 2: i = 0x0f000400 | (r & 0x607ffbff); if (!(i & 0x00780000)) i |= 0x00080000; break;
        case 3: i = 0x0e000400 | (r & 0x601f7bff); break;
        case 4: {  // LDR / STR vector register: base x0, offset register x1..x30
            i = 0x3c000000 | (r & 0xc1ffffff) | ((r & 0x1000000) ? 0 : 0);
            if (rnd64() & 1) i |= 0x01000000;
            i &= ~0x3e0u;
            if (B(i, 25, 24) == 0 && B(i, 20, 16) == 0) i |= 1u << 16;
            if (B(i, 25, 24) == 0 && B(i, 21, 21) && B(i, 20, 16) == 0) i |= 1u << 16;
            break;
        }
        case 5: i = 0x2c000000 | (r & 0xc1ffffff & ~0x3e0u & ~0x02000000u); if (rnd64() & 1) i |= 0x01000000; if ((i >> 30) == 3) i &= ~0x40000000u; break;
        case 6: i = 0x0c000000 | (r & 0x40c0ffff & ~0x3e0u); break;
        case 7: i = 0x0e200800 | (r & 0x60c1f3ff); break;
        case 8: i = 0x0d000000 | (r & 0x40c0ffff & ~0x3e0u); break;
        default: i = 0x0f000400 | (r & 0x607ffbff); i = (i & ~0xf800u) | ((rnd64() & 1) ? 0xe000u : 0xf800u); i = (i & ~0x00780000u) | 0x00200000u | ((r & 0x3) << 19) ; break;
        }
        int mode = (int)(rnd64() % 4);
        memset(a, 0, sizeof *a); st_fill(a, mode);
        for (int r = 0; r < 31; r++) a->x[r] = rnd64() % 256;
        a->x[0] = (uint64_t)mem + 0x20000; a->sp = (uint64_t)mem + 0x30000;
        a->fpcr = 0; a->fpsr = 0;
        *b = *a;
        for (size_t o = 0; o < sz; o += 8) *(uint64_t *)(mem + o) = rnd64();
        // fast
        code[0] = i; code[1] = 0x14000001;  // (the instruction, then a branch to stop_pc)
        jmp_buf jb; volatile int died = 0;
        g_die_jmp = &jb;
        uint8_t *snap = malloc(sz); memcpy(snap, mem, sz);
        {   // does the fast path take this encoding at all?
            CPU *t = malloc(sizeof(CPU)); *t = *a;
            bool took = k < 4 || k == 7 || k == 9 ? simd_fast(t, i) : k == 4 ? vmem_fast(t, i) : k == 5 ? vpair_fast(t, i) : k == 8 ? (vld1r_fast(t, i) || vsingle_fast(t, i)) : vmulti_fast(t, i);
            if (took) fastc[k]++;
            free(t); memcpy(mem, snap, sz);
        }
        g_no_simd_fast = false;
        a->pc = (uint64_t)code; a->code_lo = (uint64_t)code; a->code_len = 4096;
        if (setjmp(jb) == 0) run_until(a, (uint64_t)code + 8); else died = 1;
        uint8_t *after_fast = malloc(sz); memcpy(after_fast, mem, sz);
        memcpy(mem, snap, sz);
        volatile int died2 = 0;
        g_no_simd_fast = true;
        b->pc = (uint64_t)code; b->code_lo = (uint64_t)code; b->code_len = 4096;
        if (setjmp(jb) == 0) run_until(b, (uint64_t)code + 8); else died2 = 1;
        g_die_jmp = NULL; g_no_simd_fast = false;
        bool same = died == died2 && (died2 || (!memcmp(a->vr, b->vr, sizeof a->vr) && !memcmp(a->x, b->x, sizeof a->x) && a->sp == b->sp &&
                    a->n == b->n && a->z == b->z && a->c == b->c && a->v == b->v && a->fpsr == b->fpsr && a->fpcr == b->fpcr && !memcmp(after_fast, mem, sz)));
        tested++;
        if (died2) { died_generic++; genericdie[k]++; }
        if (!same) {
            bad++; badc[k]++;
            if (badc[k] <= 3) {
                fprintf(stderr, "MISMATCH %s insn %08x (fast %s, general %s)\n", cls[k].name, i, died ? "rejected" : "ok", died2 ? "rejected" : "ok");
                if (!died && !died2) for (int r = 0; r < 32; r++) if (memcmp(&a->vr[r], &b->vr[r], 16)) {
                    fprintf(stderr, "  v%d fast ", r); for (int q = 15; q >= 0; q--) fprintf(stderr, "%02x", a->vr[r].b[q]);
                    fprintf(stderr, "\n  v%d gen  ", r); for (int q = 15; q >= 0; q--) fprintf(stderr, "%02x", b->vr[r].b[q]);
                    fprintf(stderr, "\n  v%d src n/m/d:", r); fprintf(stderr, "\n"); break;
                }
            }
        }
        if (!died && !died2) handled[k]++;
        free(snap); free(after_fast);
    }
    (void)accepted_by_fast;
    for (int k = 0; k < ncls; k++) fprintf(stderr, "%-10s ran-clean %8ld  generic-rejected %8ld  taken-by-fast-path %8ld  mismatches %ld\n", cls[k].name, handled[k], genericdie[k], fastc[k], badc[k]);
    fprintf(stderr, "simd-selftest: %ld encodings, %ld mismatches\n", tested, bad);
    (void)died_generic;
    return bad ? 1 : 0;
}

// rebar --jit-selftest [count] [seed]: random non-branch instructions, run once by the interpreter and once as translated
// code from the same random registers, flags and memory; anything that differs is printed.
static const uint32_t *g_scan_words; static size_t g_scan_n;
// the __text section of an arm64 Mach-O file as an array of instruction words (rebar --jit-selftest ... scan <file>)
static bool scan_load(const char *path) {
    int fd = open(path, O_RDONLY); if (fd < 0) { perror(path); return false; }
    off_t sz = lseek(fd, 0, SEEK_END);
    uint8_t *m = mmap(NULL, (size_t)sz, PROT_READ, MAP_PRIVATE, fd, 0); if (m == MAP_FAILED) { perror("mmap"); return false; }
    struct mach_header_64 *mh = (void *)m; if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64) { fprintf(stderr, "%s: not an arm64 Mach-O\n", path); return false; }
    struct load_command *lc = (void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (void *)((uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64) continue;
        struct segment_command_64 *sg = (void *)lc; struct section_64 *sc = (void *)(sg + 1);
        for (uint32_t q = 0; q < sg->nsects; q++) if (!strcmp(sc[q].sectname, "__text") && !strcmp(sc[q].segname, "__TEXT")) { g_scan_words = (const uint32_t *)(m + sc[q].offset); g_scan_n = sc[q].size / 4; return true; }
    }
    fprintf(stderr, "%s: no __TEXT,__text\n", path); return false;
}
#include <execinfo.h>
// The host's terminate handler prints the name of the uncaught exception's type, and a guest typeinfo keeps the arm64 "non-unique" tag in bit 63 of its name pointer (see
// unwind_setup_image): clear the tag of that one type before the handler reads it.
static void (*g_orig_terminate)(void);
static void rebar_terminate(void) {
    uint64_t *(*cur)(void) = (uint64_t *(*)(void))dlsym(RTLD_DEFAULT, "__cxa_current_exception_type");
    uint64_t *ti = cur ? cur() : NULL;
    if (ti && (uint64_t)ti > 0x10000) ti[1] &= ~(1ull << 63);
    if (g_orig_terminate) g_orig_terminate();
    abort();
}
static void selftest_abrt(int sig) { void *bt[40]; int n = backtrace(bt, 40); fprintf(stderr, "selftest: SIGABRT\n"); backtrace_symbols_fd(bt, n, 2); _exit(134); }
static void selftest_segv(int sig) { (void)sig; if (g_die_jmp) longjmp(*g_die_jmp, 1); _exit(139); }
static int jit_selftest(long count, uint64_t seed, const char *only, bool tails) {
    uint64_t gt0 = 0, gt1 = 0, first_exit = 0; int gtail = 0, grr = 0;
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = selftest_segv; sa.sa_flags = SA_NODEFER; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); if (getenv("REBAR_SELFTEST_TRACE")) signal(SIGABRT, selftest_abrt); }   // a sequence that clobbers its own base register just faults, in both runs (a wild address through rbp is a stack fault, which arrives as SIGILL)
    setenv("REBAR_JIT", "1", 1);
    jit_init();
    if (!g_jit_on) { fprintf(stderr, "jit-selftest: the JIT did not start\n"); return 2; }
    g_rng = seed ? seed : 0x243f6a8885a308d3ull;
    const size_t MSZ = 1u << 20, WIN = MSZ, WBASE = 0, MID = 0x80000;   // all of the guest memory is restored and compared after each run
    uint8_t *mem = mmap(NULL, MSZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint8_t *cbuf = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint8_t *orig = malloc(WIN), *ma = malloc(WIN), *mb = malloc(WIN);
    CPU *a = calloc(1, sizeof(CPU)), *b = calloc(1, sizeof(CPU)), *init = calloc(1, sizeof(CPU));
    t_cpu = a;
    static uint8_t lowsave[0x2000]; memcpy(lowsave, (void *)0x1000, sizeof lowsave);   // (the host maps a page or two at 0x1000: a pointer near zero lands in it, so it is restored like the rest)
    typedef struct { const char *name; uint32_t base, mask; int kind; } T;
    static const T tmpl[] = {
        {"addsub-imm", 0x11000000, 0xE07FFFFF, 0}, {"logic-imm", 0x12000000, 0xE07FFFFF, 0}, {"movw", 0x12800000, 0xE07FFFFF, 0},
        {"adr", 0x10000000, 0xE0FFFFFF, 0}, {"bitfield", 0x13000000, 0xE07FFFFF, 0}, {"extr", 0x13800000, 0xE0FFFFFF & ~0x00200000u, 0},
        {"addsub-reg", 0x0b000000, 0xE0FFFFFF, 0}, {"logic-reg", 0x0a000000, 0xE0FFFFFF, 0}, {"adc", 0x1a000000, 0xE01F03FF, 0},
        {"ccmp", 0x3A400000, 0xC01FFBEF, 0}, {"csel", 0x1a800000, 0xC01FF3FF, 0}, {"shiftv", 0x1ac02000, 0x801F0FFF, 0},
        {"dp3", 0x1b000000, 0x80FFFFFF, 0}, {"dp1", 0x5ac00000, 0x8000FFFF, 0},
        {"ldst-uimm", 0x39000000, 0xFFFFFFFF & ~0x04000000u, 1}, {"ldst-idx", 0x38000000, 0xFFDFFFFF & ~0x04000000u, 1}, {"ldst-reg", 0x38200800, 0xFFFFF3FF & ~0x04000000u, 2},
        {"ldp-stp", 0x28000000, 0xC1FFFFFF, 1}, {"ldr-lit", 0x18000000, 0xC01FFFFF, 3}, {"vec-uimm", 0x3d000000, 0xFFFFFFFF, 1}, {"vec-idx", 0x3c000000, 0xFFDFFFFF, 1},
        {"ldp-stp-vec", 0x2c000000, 0xC1FFFFFF, 1},
        {"ldar-stlr", 0x089f7c00, 0xC04083FF, 1}, {"ldapr", 0x38bfc000, 0xC00003FF, 1}, {"ldapur-stlur", 0x19000000, 0xC0DFF3FF, 1},
        {"lse", 0x38200000, 0xC0DFF3FF, 1}, {"cas", 0x08a07c00, 0xC05F83FF, 1},
        {"v-three", 0x0e200400, 0x60DFFBFF, 0}, {"v-copy", 0x0e000400, 0x601F7BFF, 0}, {"v-modimm", 0x0f000400, 0x6007F3FF, 0}, {"v-ext", 0x2e000000, 0x401F7BFF, 0}, {"v-shift", 0x0f000400, 0x607FFBFF, 0},
        {"fp-2src", 0x1e200800, 0x005FF3FF, 0}, {"fp-1src", 0x1e204000, 0x005F83FF, 0}, {"fcmp", 0x1e202000, 0x005F03F8, 0}, {"fcsel", 0x1e200c00, 0x005FF3FF, 0},
        {"fmov-imm", 0x1e201000, 0x005FE01F, 0}, {"fp-int", 0x1e200000, 0x805F03FF, 0}, {"fmadd", 0x1f000000, 0x005FFFFF | 0x00A00000, 0},
        {"fccmp", 0x1e200400, 0x00DFF3FF, 0}, {"fp-fixed", 0x1e008000, 0x805F7FFF, 0}, {"div", 0x1ac00800, 0x801F07FF, 0}, {"prfm-reg", 0xf8a04800, 0x001FB3FF, 0},
        {"v-misc2", 0x0e200800, 0x60C1F3FF, 0}, {"v-perm", 0x0e000800, 0x40DF73FF, 0}, {"v-scopy", 0x5e000400, 0x001F03FF, 0}, {"v-indexed", 0x0f000000, 0x40FFFBFF, 0}, {"v-indexed-s", 0x5f000000, 0x00FFFBFF, 0},
        {"vi-fmla", 0x0f801000, 0x407F0BFF, 0}, {"vi-fmls", 0x0f805000, 0x407F0BFF, 0}, {"vi-fmul", 0x0f809000, 0x407F0BFF, 0}, {"vi-fmla-s", 0x5f801000, 0x007F0BFF, 0}, {"vi-fmul-s", 0x5f809000, 0x007F0BFF, 0},
        {"v3-sqadd", 0x0e200c00, 0x60DF03FF, 0}, {"v3-sqsub", 0x0e202c00, 0x60DF03FF, 0}, {"s-scvtf", 0x5e21d800, 0x006003FF | 0x20000000, 0}, {"m2-frint", 0x0e218800, 0x60E003FF | 0x00001000, 0}, {"m2-xtn", 0x0e212800, 0x40C003FF, 0}, {"m2-not", 0x2e205800, 0x400003FF, 0}, {"m2-cmeq0", 0x0e209800, 0x40C003FF, 0}, {"m2-cmgt0", 0x0e208800, 0x40C003FF, 0}, {"m2-cmge0", 0x2e208800, 0x40C003FF, 0},
        {"m2-cmle0", 0x2e209800, 0x40C003FF, 0}, {"m2-cmlt0", 0x0e20a800, 0x40C003FF, 0}, {"m2-fabs", 0x0ea0f800, 0x404003FF, 0}, {"m2-fneg", 0x2ea0f800, 0x404003FF, 0}, {"m2-ucvtf", 0x2e21d800, 0x404003FF, 0},
        {"m2-scvtf", 0x0e21d800, 0x404003FF, 0}, {"m2-fcvtzs", 0x0ea1b800, 0x404003FF, 0}, {"m2-fcvtzu", 0x2ea1b800, 0x404003FF, 0}, {"m2-shll", 0x2e213800, 0x40C003FF, 0},
        {"v3-fmax", 0x0e20f400, 0x40DF03FF, 0}, {"v3-fmaxnm", 0x0e20c400, 0x40DF03FF, 0}, {"v3-ushl4s", 0x2ea04400, 0x401F03FF, 0}, {"trn", 0x0e002800, 0x40DF43FF, 0}, {"m2-fredv", 0x6eb0f800, 0x008033FF, 0}, {"m2-addv4s", 0x4eb1b800, 0x000003FF, 0}, {"m2-fsqrt", 0x2ea1f800, 0x404003FF, 0}, {"tbl", 0x0e000000, 0x401F63FF, 0}, {"m2-fcm0", 0x0ea0c800, 0x6040F3FF, 0}, {"ld1-lane", 0x0d400000, 0x40DFFFFF, 1}, {"v3-faddp", 0x2e20d400, 0x405F03FF, 0}, {"m2-fcvtns", 0x0e21a800, 0x404003FF, 0}, {"m2-scvtf-fixed", 0x0f00e400, 0x407F83FF, 0}, {"ld1-multi", 0x0c402000, 0x4000FFFF | 0x00800000 | 0x001F0000, 1}, {"fcvtl", 0x0e217800, 0x404003FF, 0}, {"fcvtn", 0x0e216800, 0x404003FF, 0}, {"dot-vec", 0x0e809400, 0x601F03FF, 0}, {"dot-idx", 0x0f80e000, 0x603F0BFF, 0}, {"s-faddp", 0x7e30d800, 0x004003FF, 0}, {"v3-ushl2d", 0x6ee04400, 0x001F03FF, 0}, {"fcvtzs-gpr", 0x1e380000, 0x804003FF, 0}, {"v3-fadd", 0x0e20d400, 0x40DF03FF, 0}, {"v3-fmul", 0x2e20dc00, 0x405F03FF, 0}, {"v3-fdiv", 0x2e20fc00, 0x405F03FF, 0}, {"v3-fmla", 0x0e20cc00, 0x40DF03FF, 0},
    };
    int nt = sizeof tmpl / sizeof tmpl[0];
    long tested = 0, bad = 0, skipped = 0;
    long per_t[128] = {0}, bad_t[128] = {0};
    for (long it = 0; it < count; it++) {
        int K = 1 + (int)(rnd64() % 5);
        uint32_t code[8]; int tids[8];
        if (g_scan_words) {   // real code: a run of consecutive non-branch instructions from a random place in the file
            size_t pos = (size_t)(rnd64() % (g_scan_n - 16));
            K = 0;
            while (K < 6) {
                uint32_t w = g_scan_words[pos + K];
                if (B(w, 28, 26) == 5 || B(w, 28, 25) == 0 || (w & 0xffe0001f) == 0xd4000001) break;   // a branch, system instruction or data: the run ends
                code[K] = w; tids[K] = 0; K++;
            }
            if (!K) { skipped++; continue; }
        }
        for (int k = 0; !g_scan_words && k < K; k++) {
            int t = (int)(rnd64() % nt);
            if (only) { for (int u = 0; u < nt; u++) if (!strcmp(tmpl[u].name, only)) t = u; }
            tids[k] = t;
            uint32_t r = (uint32_t)rnd64(), i = tmpl[t].base | (r & tmpl[t].mask);
            if (tmpl[t].kind == 1 || tmpl[t].kind == 2) i = (i & ~0x3E0u) | ((uint32_t)(20 + rnd64() % 5) << 5);          // base register: one that holds a pointer
            if (tmpl[t].kind == 2) i = (i & ~0x1F0000u) | ((uint32_t)(25 + rnd64() % 4) << 16);                           // index register: a small number
            if (tmpl[t].kind == 3) i = (i & ~0xFFFFE0u) | ((uint32_t)(rnd64() & 0x1ff) << 5) | 0;                           // literal: stay inside the code page
            if (!strcmp(tmpl[t].name, "ldp-stp")) {   // LDPSW can load a 32-bit "address" into a base register: the low memory is the emulator's own
                if ((i & 31) >= 20 && (i & 31) <= 24) i -= 20;
                uint32_t r2 = (i >> 10) & 31; if (r2 >= 20 && r2 <= 24) i -= 20u << 10;
            }
            if (!strcmp(tmpl[t].name, "ldar-stlr") || !strcmp(tmpl[t].name, "ldapr") || !strcmp(tmpl[t].name, "ldapur-stlur") || !strcmp(tmpl[t].name, "lse") || !strcmp(tmpl[t].name, "cas")) {   // keep the base registers intact: a junk address only tests the memory below 64K
                if ((i & 31) >= 20 && (i & 31) <= 24) i -= 20;
                if (!strcmp(tmpl[t].name, "cas") || !strcmp(tmpl[t].name, "lse")) { uint32_t rs = (i >> 16) & 31; if (rs >= 20 && rs <= 24) i -= 20u << 16; }
            }
            if (tmpl[t].base == 0x19000000 && ((i >> 30) & 3) == 2 && ((i >> 22) & 3) == 3) i &= ~(1u << 22);                    // (LDAPURSW with a 32-bit result does not exist)
            if (tmpl[t].base == 0x2c000000 && (i >> 30) == 3) i &= ~(1u << 30);                               // (SIMD&FP pair: opc 3 is undefined)
            if (!strncmp(tmpl[t].name, "ldst-", 5)) {   // single register load/store: leave out the undefined corners
                if (((i >> 30) & 3) == 3 && ((i >> 22) & 3) >= 2) i = (i & ~(3u << 22)) | (1u << 22);          // PRFM with writeback does not exist
                if (!((i >> 21) & 1) && ((i >> 10) & 3) == 2) i &= ~(1u << 11);                                  // unprivileged forms
            }
            code[k] = i;
        }
        if (getenv("REBAR_SELFTEST_SEQ")) {   // a fixed sequence of instruction words (hex, separated by commas) instead of the random ones
            K = 0; for (const char *q = getenv("REBAR_SELFTEST_SEQ"); *q && K < 8; K++) { char *e; code[K] = (uint32_t)strtoul(q, &e, 16); tids[K] = 0; q = *e == ',' ? e + 1 : e; }
        }
        if (!g_scan_words) {   // the atomic stores and read-modify-writes write to wherever the base register points: nothing before them may turn it into a junk address (the memory below 64K is the emulator's own)
            int last = -1;
            for (int k = 0; k < K; k++) { const char *nm = tmpl[tids[k]].name; if (!strcmp(nm, "ldar-stlr") || !strcmp(nm, "ldapur-stlur") || !strcmp(nm, "lse") || !strcmp(nm, "cas")) last = k; }
            for (int k = 0; k < last; k++) if ((code[k] & 31) >= 20 && (code[k] & 31) <= 24 && strncmp(tmpl[tids[k]].name, "vec-", 4)) code[k] -= 20;
        }
        // the block ends with a branch to the stop address
        uint64_t cpc = (uint64_t)cbuf + (uint64_t)(it & 63) * 256, stop = (uint64_t)cbuf + (1 << 19);
        for (int k = 0; k < K; k++) memcpy((void *)(cpc + 4 * k), &code[k], 4);
        if (getenv("REBAR_SELFTEST_TRACE")) { fprintf(stderr, "seq %ld:", it); for (int k = 0; k < K; k++) fprintf(stderr, " %08x", code[k]); fputc(10, stderr); }
        // the tail of the sequence: a plain jump to the stop address, or (when `tails`) a conditional / register branch whose two
        // destinations each set a different marker in x28 before jumping to the stop address
        uint64_t tailpc = cpc + 4 * K, t0 = tailpc + 8, t1 = t0 + 8;   // fall-through and taken destinations (each: movz x28, #marker; b stop)
        int tail = tails ? (int)(rnd64() % 7) : 0;
        uint32_t tl;
        uint32_t cond = (uint32_t)(rnd64() % 14), rr = (uint32_t)(rnd64() % 27), sfb = (uint32_t)(rnd64() & 1), bit = (uint32_t)(rnd64() % 64);
        int64_t d1 = (int64_t)(t1 - tailpc) / 4;
        uint32_t nop = 0xd503201f;
        memcpy((void *)(tailpc + 4), &nop, 4);
        switch (tail) {
        case 0: tl = 0x14000000 | (uint32_t)(((int64_t)(stop - tailpc) / 4) & 0x3ffffff); break;
        case 1: tl = 0x54000000 | ((uint32_t)(d1 & 0x7ffff) << 5) | cond; break;                                   // b.cond
        case 2: tl = 0x34000000 | (sfb << 31) | ((uint32_t)(d1 & 0x7ffff) << 5) | rr; break;                        // cbz
        case 3: tl = 0x35000000 | (sfb << 31) | ((uint32_t)(d1 & 0x7ffff) << 5) | rr; break;                        // cbnz
        case 4: tl = 0x36000000 | ((bit >> 5) << 31) | ((bit & 31) << 19) | ((uint32_t)(d1 & 0x3fff) << 5) | rr; break;   // tbz
        case 5: tl = 0x37000000 | ((bit >> 5) << 31) | ((bit & 31) << 19) | ((uint32_t)(d1 & 0x3fff) << 5) | rr; break;   // tbnz
        default: {   // adr x27, one of the destinations; then br / blr / ret x27
            static const uint32_t regops[3] = {0xd61f0000, 0xd63f0000, 0xd65f0000};
            uint64_t tgt = (rnd64() & 1) ? t0 : t1; int64_t off = (int64_t)(tgt - tailpc);
            uint32_t adr = 0x10000000 | (((uint32_t)off & 3) << 29) | ((((uint32_t)off >> 2) & 0x7ffff) << 5) | 27;
            memcpy((void *)tailpc, &adr, 4);
            tl = regops[rnd64() % 3] | (27 << 5);
            memcpy((void *)(tailpc + 4), &tl, 4);
            tl = adr;
            break;
        }
        }
        if (tail != 6) memcpy((void *)tailpc, &tl, 4);
        if (tail) {
            uint32_t m0 = 0xd2800000 | (0x111u << 5) | 28, m1 = 0xd2800000 | (0x222u << 5) | 28;      // movz x28, #0x111 / #0x222
            uint32_t b0 = 0x14000000 | (uint32_t)(((int64_t)(stop - (t0 + 4)) / 4) & 0x3ffffff), b1 = 0x14000000 | (uint32_t)(((int64_t)(stop - (t1 + 4)) / 4) & 0x3ffffff);
            memcpy((void *)t0, &m0, 4); memcpy((void *)(t0 + 4), &b0, 4); memcpy((void *)t1, &m1, 4); memcpy((void *)(t1 + 4), &b1, 4);
        }
        gt0 = t0; gt1 = t1; gtail = tail; grr = (int)rr;
        // initial state
        memset(init, 0, sizeof *init);
        for (int r = 0; r < 31; r++) {
            uint64_t v = rnd64(); int m = (int)(rnd64() % 8);
            init->x[r] = m == 0 ? 0 : m == 1 ? ~0ull : m == 2 ? 0x7fffffffull : m == 3 ? 0x80000000ull : m == 4 ? 0x8000000000000000ull : m == 5 ? (v & 0xff) : v;
        }
        if (g_scan_words) {   // real code uses any register as a pointer: most of them point into the window
            for (int r = 0; r < 29; r++) { int m = (int)(rnd64() % 8); if (m < 4) init->x[r] = (uint64_t)mem + MID + ((rnd64() % 0x6000) & ~15ull); else if (m == 4) init->x[r] = (rnd64() % 64) * 8; }
        } else {
        for (int r = 20; r < 25; r++) init->x[r] = (uint64_t)mem + MID + ((rnd64() % 0x2000) & ~15ull);
        for (int r = 25; r < 29; r++) init->x[r] = (rnd64() % 64) * 8;
        }
        init->sp = (uint64_t)mem + MID + 0x1000;
        init->n = rnd64() & 1; init->z = rnd64() & 1; init->c = rnd64() & 1; init->v = rnd64() & 1;
        for (int r = 0; r < 32; r++) for (int q = 0; q < 2; q++) init->vr[r].d[q] = rnd64();
        {   // the interesting floating-point values: zeros, ones, infinities, quiet and signalling NaNs, denormals, powers of two near the integer limits, integers, in both widths
            static const uint64_t sp[] = {0, 0x8000000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000000ull,
                0x7ff4000000000000ull, 0x0000000000000001ull, 0x000fffffffffffffull, 0x41e0000000000000ull, 0xc1e0000000000000ull, 0x43e0000000000000ull, 0xc3e0000000000000ull, 0x43f0000000000000ull,
                0x4024000000000000ull, 0x4008000000000000ull, 0x3fe0000000000000ull, 0xbfe0000000000000ull, 0x3ff8000000000000ull, 0x400c000000000000ull, 0x4340000000000000ull,
                0x3f8000003f800000ull, 0xbf800000bf800000ull, 0x7f8000007f800000ull, 0x7fc000007fc00000ull, 0x7fa000007fa00000ull, 0x0000000100000001ull, 0x4f0000004f000000ull, 0xcf000000cf000000ull,
                0x4eff00004eff0000ull, 0x4b0000004b000000ull, 0x4b8000004b800000ull, 0x4100000040800000ull, 0xc0400000c0000000ull};
            for (int r = 0; r < 32; r++) for (int q = 0; q < 2; q++) if (rnd64() % 3 == 0) init->vr[r].d[q] = sp[rnd64() % (sizeof sp / sizeof sp[0])];
        }
        if (it == 0) for (size_t o = 0; o < WIN; o += 8) *(uint64_t *)(orig + o) = rnd64();
        // interpreter
        *a = *init; a->pc = cpc; a->code_lo = (uint64_t)cbuf; a->code_len = 1 << 20;
        memcpy(mem + WBASE, orig, WIN); memcpy((void *)0x1000, lowsave, sizeof lowsave);
        jmp_buf jb; volatile int died_a = 0, died_b = 0;
        g_die_jmp = &jb; g_jit_on = false;
        if (setjmp(jb) == 0) run_until(a, stop); else died_a = 1;
        memcpy(ma, mem + WBASE, WIN);
        if (getenv("REBAR_SELFTEST_TRACE")) fprintf(stderr, "  interpreter done (pc %llx, died %d)\n", (unsigned long long)a->pc, (int)died_a);
        // translated
        g_jit_on = true;
        *b = *init; b->pc = cpc; b->code_lo = (uint64_t)cbuf; b->code_len = 1 << 20;
        memcpy(mem + WBASE, orig, WIN); memcpy((void *)0x1000, lowsave, sizeof lowsave);
        jit_invalidate(cpc, cpc + 256);
        t_cpu = b;
        if (setjmp(jb) == 0) {
            uint8_t *blk = jit_compile(cpc);
            if (!blk) { skipped++; t_cpu = a; continue; }
            jit_run(b, blk); first_exit = b->pc;
            if (getenv("REBAR_SELFTEST_TRACE")) fprintf(stderr, "  translated block left at pc %llx (stop %llx)\n", (unsigned long long)b->pc, (unsigned long long)stop);
            if (b->pc != stop) { g_jit_on = false; t_cpu = b; run_until(b, stop); g_jit_on = true; }   // what the dispatcher does after a block that stopped early
        } else died_b = 1;
        t_cpu = a;
        g_die_jmp = NULL;
        memcpy(mb, mem + WBASE, WIN);
        if (died_a || died_b) { if (died_a != died_b) { bad++; fprintf(stderr, "MISMATCH (died: interpreter %d, jit %d):", died_a, died_b); for (int k = 0; k < K; k++) fprintf(stderr, " %08x(%s)", code[k], tmpl[tids[k]].name); fprintf(stderr, "\n"); } else skipped++; continue; }
        tested++;
        for (int k = 0; k < K; k++) per_t[tids[k]]++;
        bool flags_same = a->n == b->n && a->z == b->z && a->c == b->c && a->v == b->v;
        bool same = !memcmp(a->x, b->x, sizeof a->x) && a->sp == b->sp && flags_same && !memcmp(a->vr, b->vr, sizeof a->vr) && !memcmp(ma, mb, WIN) && b->pc == stop;
        if (!same) {
            bad++; for (int k = 0; k < K; k++) bad_t[tids[k]]++;
            if (bad <= 12) {
                fprintf(stderr, "MISMATCH:");
                for (int k = 0; k < K; k++) fprintf(stderr, " %08x(%s)", code[k], tmpl[tids[k]].name);
                fprintf(stderr, "\n");
                for (int r = 0; r < 31; r++) if (a->x[r] != b->x[r]) fprintf(stderr, "   x%d: init %llx interp %llx jit %llx\n", r, (unsigned long long)init->x[r], (unsigned long long)a->x[r], (unsigned long long)b->x[r]);
                if (a->sp != b->sp) fprintf(stderr, "   sp: interp %llx jit %llx\n", (unsigned long long)a->sp, (unsigned long long)b->sp);
                if (!flags_same) fprintf(stderr, "   nzcv: init %d%d%d%d interp %d%d%d%d jit %d%d%d%d\n", init->n, init->z, init->c, init->v, a->n, a->z, a->c, a->v, b->n, b->z, b->c, b->v);
                if (memcmp(ma, mb, WIN)) fprintf(stderr, "   memory differs\n");
                if (b->pc != stop) fprintf(stderr, "   jit pc %llx, expected %llx\n", (unsigned long long)b->pc, (unsigned long long)stop);
                { fprintf(stderr, "   init regs:"); for (int r = 0; r < 31; r++) fprintf(stderr, " x%d=%llx", r, (unsigned long long)init->x[r]); fprintf(stderr, " mem=%llx\n", (unsigned long long)mem); }
                fprintf(stderr, "   translated block left at pc offset %lld words into the sequence (sequence length %d)%s\n", (long long)((first_exit - cpc) / 4), K, first_exit == stop ? " = ran to the end" : "");
                if (memcmp(a->vr, b->vr, sizeof a->vr)) {
                    fprintf(stderr, "   vector registers differ\n");
                    for (int r = 0; r < 32; r++) if (memcmp(&a->vr[r], &b->vr[r], 16))
                        fprintf(stderr, "   v%d: init %016llx:%016llx interp %016llx:%016llx jit %016llx:%016llx\n", r, (unsigned long long)init->vr[r].d[1], (unsigned long long)init->vr[r].d[0], (unsigned long long)a->vr[r].d[1], (unsigned long long)a->vr[r].d[0], (unsigned long long)b->vr[r].d[1], (unsigned long long)b->vr[r].d[0]);
                }
            }
        }
    }
    for (int t = 0; t < nt; t++) fprintf(stderr, "%-12s used %7ld times, in %ld failing sequences\n", tmpl[t].name, per_t[t], bad_t[t]);
    fprintf(stderr, "jit-selftest: %ld sequences compared, %ld skipped, %ld mismatches (%llu blocks, %llu instructions, %llu through helpers)\n", tested, skipped, bad, (unsigned long long)g_jit_nblocks, (unsigned long long)g_jit_ninsns, (unsigned long long)g_jit_nfallback);
    return bad ? 1 : 0;
}

// REBAR_PROFILE=<file>: a sampling profiler for guest code. A thread looks at every guest CPU once a millisecond and counts
// the pc, and the kind of instruction at it, of each one that has executed something since the last look. The tables are
// written to <file>.<pid> once a second; touching <file>.reset clears them (to measure just one activity).
#define PROF_SLOTS (1 << 16)
static const char *prof_stub_name(int idx);
static bool guest_owned_code(uint64_t pc);
static CPU *g_prof_cpus[1024];
static int g_prof_ncpu;
static const char *g_prof_file;
static struct { uint64_t key, n; } g_prof_pc[PROF_SLOTS], g_prof_op[PROF_SLOTS], g_prof_stub[4096];
static void prof_register(CPU *c) {
    if (!g_prof_file) return;
    int k = __atomic_fetch_add(&g_prof_ncpu, 1, __ATOMIC_RELAXED);
    if (k < 1024) g_prof_cpus[k] = c;
}
static void prof_count(typeof(g_prof_pc) *tab, uint64_t key) {
    unsigned h = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 48);
    for (int k = 0; k < 64; k++) {
        unsigned j = (h + k) & (PROF_SLOTS - 1);
        if ((*tab)[j].key == 0) (*tab)[j].key = key;
        if ((*tab)[j].key == key) { (*tab)[j].n++; return; }
    }
}
static int prof_cmp(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void prof_dump(uint64_t total, uint64_t idle, uint64_t outside) {
    char name[PATH_MAX], tmp[PATH_MAX];
    snprintf(name, sizeof name, "%s.%d", g_prof_file, (int)getpid());
    snprintf(tmp, sizeof tmp, "%s.tmp", name);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "total %llu idle %llu outside %llu\n", (unsigned long long)total, (unsigned long long)idle, (unsigned long long)outside);
    static uint64_t rows[PROF_SLOTS][2];
    for (int t = 0; t < 2; t++) {
        typeof(g_prof_pc) *tab = t ? &g_prof_op : &g_prof_pc;
        int n = 0;
        for (int k = 0; k < PROF_SLOTS; k++) if ((*tab)[k].n) rows[n][0] = (*tab)[k].key, rows[n++][1] = (*tab)[k].n;
        qsort(rows, n, sizeof rows[0], prof_cmp);
        for (int k = 0; k < n && k < (t ? 80 : 400); k++) fprintf(f, "%s %llx %llu\n", t ? "op" : "pc", (unsigned long long)rows[k][0], (unsigned long long)rows[k][1]);
    }
    {
        int n = 0;
        for (int k = 0; k < 4096; k++) if (g_prof_stub[k].n) rows[n][0] = g_prof_stub[k].key, rows[n++][1] = g_prof_stub[k].n;
        qsort(rows, n, sizeof rows[0], prof_cmp);
        for (int k = 0; k < n && k < 60; k++) fprintf(f, "stub %s %llu\n", prof_stub_name((int)rows[k][0]), (unsigned long long)rows[k][1]);
    }
    fclose(f);
    rename(tmp, name);
}
static void *prof_thread(void *arg) {
    (void)arg;
    uint64_t last[1024] = {0}, total = 0, idle = 0, outside = 0;
    time_t reset_seen = 0;
    char ctl[PATH_MAX];
    snprintf(ctl, sizeof ctl, "%s.reset", g_prof_file);
    struct stat st0;
    if (stat(ctl, &st0) == 0) reset_seen = st0.st_mtime;
    for (unsigned round = 1;; round++) {
        usleep(1000);
        int n = __atomic_load_n(&g_prof_ncpu, __ATOMIC_ACQUIRE);
        if (n > 1024) n = 1024;
        for (int k = 0; k < n; k++) {
            CPU *c = g_prof_cpus[k];
            if (!c) continue;
            uint64_t tick = c->tick, pc = c->pc;
            uint64_t moved = tick - last[k];
            last[k] = tick;
            if (moved < 2000) { idle++; continue; }  // (a thread that wakes up for a few instructions every millisecond is not busy)
            total++;
            if (pc - g_guest_lo >= g_guest_size) {
                outside++;
                if (pc - g_stub_base < g_stub_size) {  // inside a call to an imported function
                    uint64_t key = (pc - g_stub_base) / 16 + 1;
                    unsigned h = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 52);
                    for (int q = 0; q < 64; q++) { unsigned j = (h + q) & 4095; if (!g_prof_stub[j].key) g_prof_stub[j].key = key; if (g_prof_stub[j].key == key) { g_prof_stub[j].n++; break; } }
                }
                else if (guest_owned_code(pc)) {  // generated code (a JIT's): sampled like the image's
                    prof_count(&g_prof_pc, pc);
                    prof_count(&g_prof_op, *(uint32_t *)pc & 0xffe0fc00u);
                    outside--;
                }
                continue;
            }
            prof_count(&g_prof_pc, pc);
            prof_count(&g_prof_op, *(uint32_t *)pc & 0xffe0fc00u);
        }
        if (round % 100 == 0) {
            struct stat st;
            if (stat(ctl, &st) == 0 && st.st_mtime != reset_seen) {
                reset_seen = st.st_mtime;
                memset(g_prof_pc, 0, sizeof g_prof_pc); memset(g_prof_op, 0, sizeof g_prof_op); memset(g_prof_stub, 0, sizeof g_prof_stub);
                total = idle = outside = 0;
            }
        }
        if (round % 1000 == 0) prof_dump(total, idle, outside);
    }
    return NULL;
}
static void prof_start(const char *file) {
    g_prof_file = file;
    pthread_t th;
    pthread_create(&th, NULL, prof_thread, NULL);
    pthread_detach(th);
}

// REBAR_JITPROF=<file>: an unbiased sampler for the translated code. Every millisecond the threads of the process that are
// running are stopped for an instant and their host instruction pointer is read: time in translated code is attributed to the
// guest block it belongs to, the rest to the host function. <file>.<pid> is rewritten every two seconds.
#include <mach/thread_act.h>
#include <mach/task.h>
#include <dlfcn.h>
#define JP_SLOTS (1 << 16)
static struct { uint64_t key, n; } g_jp_blk[JP_SLOTS], g_jp_host[JP_SLOTS];
static uint64_t g_jp_total, g_jp_jit, g_jp_threads;
static useconds_t g_jp_period = 4000;
static const char *g_jp_file;
static void jp_count(typeof(g_jp_blk) tab, uint64_t key) {
    unsigned h = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 48);
    for (int k = 0; k < 64; k++) { unsigned j = (h + k) & (JP_SLOTS - 1); if (!tab[j].key) tab[j].key = key; if (tab[j].key == key) { tab[j].n++; return; } }
}
static int jp_cmp(const void *a, const void *b) { uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1]; return x < y ? 1 : x > y ? -1 : 0; }
static void jp_dump(void) {
    char name[PATH_MAX], tmp[PATH_MAX]; snprintf(name, sizeof name, "%s.%d", g_jp_file, (int)getpid()); snprintf(tmp, sizeof tmp, "%s.tmp", name);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    static uint64_t rows[JP_SLOTS][2];
    fprintf(f, "samples %llu in translated code %llu\n", (unsigned long long)g_jp_total, (unsigned long long)g_jp_jit);
    for (int t = 0; t < 2; t++) {
        int n = 0;
        for (int k = 0; k < JP_SLOTS; k++) { typeof(&g_jp_blk[0]) tab = t ? g_jp_host : g_jp_blk; if (tab[k].n) { rows[n][0] = tab[k].key; rows[n++][1] = tab[k].n; } }
        qsort(rows, n, sizeof rows[0], jp_cmp);
        for (int k = 0; k < n && k < 400; k++) {
            if (t == 0) { const JReg *b = NULL; for (size_t q = 0; q < g_jit_reg_n; q++) if (g_jit_reg[q].pc == rows[k][0]) { b = &g_jit_reg[q]; break; } fprintf(f, "block %llx %llu insns=%u\n", (unsigned long long)rows[k][0], (unsigned long long)rows[k][1], b ? b->n : 0); }
            else { Dl_info di; const char *fn = dladdr((void *)rows[k][0], &di) && di.dli_sname ? di.dli_sname : "?"; fprintf(f, "host %llx %llu %s\n", (unsigned long long)rows[k][0], (unsigned long long)rows[k][1], fn); }
        }
    }
    fclose(f); rename(tmp, name);
}
static void *jp_thread(void *arg) {
    (void)arg;
    mach_port_t self = mach_thread_self();
    for (unsigned round = 1;; round++) {
        usleep(g_jp_period);
        thread_act_array_t th; mach_msg_type_number_t nth = 0;
        if (task_threads(mach_task_self(), &th, &nth) == KERN_SUCCESS) {
            for (mach_msg_type_number_t k = 0; k < nth; k++) {
                if (th[k] != self) {
                    thread_basic_info_data_t bi; mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
                    if (thread_info(th[k], THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS && bi.run_state == TH_STATE_RUNNING && thread_suspend(th[k]) == KERN_SUCCESS) {
                        x86_thread_state64_t st; mach_msg_type_number_t sc = x86_THREAD_STATE64_COUNT;
                        if (thread_get_state(th[k], x86_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
                            uint64_t rip = st.__rip; g_jp_total++;
                            if (g_jit_arena && rip - (uint64_t)g_jit_arena < JIT_ARENA) {
                                g_jp_jit++;
                                const JReg *b = jit_block_of(rip);
                                jp_count(g_jp_blk, b ? b->pc : 1);
                            } else jp_count(g_jp_host, rip);
                        }
                        thread_resume(th[k]);
                    }
                }
                mach_port_deallocate(mach_task_self(), th[k]);
            }
            vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof(thread_t));
        }
        if (round % 500 == 0) jp_dump();
    }
    return NULL;
}
static void *jitstats_thread(void *arg) {
    for (;;) {
        sleep(2);
        char name[PATH_MAX]; snprintf(name, sizeof name, "%s.%d", (const char *)arg, (int)getpid());
        FILE *f = fopen(name, "w"); if (!f) continue;
        fprintf(f, "blocks %llu insns %llu helper-calls-emitted %llu code-bytes %llu\n", (unsigned long long)g_jit_nblocks, (unsigned long long)g_jit_ninsns, (unsigned long long)g_jit_nfallback, (unsigned long long)(g_jit_top - g_jit_arena));
        if (g_jit_count) {
            uint64_t dyn = 0, hc = 0;
            for (size_t b = 0; b < g_jit_reg_n && b < (1u << 20); b++) dyn += g_jit_cnt[b] * g_jit_reg[b].n;
            for (int k = 0; k < 4096; k++) hc += g_jit_fbdyn[k];
            fprintf(f, "invalidate calls %llu killed %llu blocks %llu\n", (unsigned long long)g_ji_calls, (unsigned long long)g_ji_killed, (unsigned long long)g_jit_nblocks);
            fprintf(f, "indirect %llu cellmiss %llu\n", (unsigned long long)(g_jr_indk[0] + g_jr_indk[1] + g_jr_indk[2]), (unsigned long long)(g_jr_missk[0] + g_jr_missk[1] + g_jr_missk[2]));
            fprintf(f, "kinds br %llu/%llu blr %llu/%llu ret %llu/%llu\n", (unsigned long long)g_jr_indk[0], (unsigned long long)g_jr_missk[0], (unsigned long long)g_jr_indk[1], (unsigned long long)g_jr_missk[1], (unsigned long long)g_jr_indk[2], (unsigned long long)g_jr_missk[2]);
            fprintf(f, "returns runs %llu stub %llu budget %llu other %llu probe-found %llu cold %llu\n", (unsigned long long)g_jr_runs, (unsigned long long)g_jr_stub, (unsigned long long)g_jr_budget, (unsigned long long)g_jr_other, (unsigned long long)g_jr_found, (unsigned long long)g_jr_cold);
            fprintf(f, "dynamic translated-instructions %llu helper-calls %llu\n", (unsigned long long)dyn, (unsigned long long)hc);
            for (int k = 0; k < 4096; k++) if (g_jit_fbdyn[k]) fprintf(f, "helper %08x %llu\n", g_jit_fb_key[k], (unsigned long long)g_jit_fbdyn[k]);
            for (size_t b = 0; b < g_jit_reg_n && b < (1u << 20); b++) if (g_jit_cnt[b] > 2000) fprintf(f, "block %llx %llu %u\n", (unsigned long long)g_jit_reg[b].pc, (unsigned long long)g_jit_cnt[b], g_jit_reg[b].n);
        } else for (int k = 0; k < 4096; k++) if (g_jit_fb_n[k]) fprintf(f, "%08x %llu\n", g_jit_fb_key[k], (unsigned long long)g_jit_fb_n[k]);
        fclose(f);
    }
    return NULL;
}
static void jitprof_start(const char *file) { g_jp_file = file; atexit(jp_dump); pthread_t th; pthread_create(&th, NULL, jp_thread, NULL); pthread_detach(th); }

// REBAR_HOSTPROF=<file>: a profiler of the interpreter itself. Every millisecond of CPU time the host instruction pointer of
// whichever thread is running is counted; <file>.<pid> gets the hottest addresses (with the function they are in) every two
// seconds. Build with -g and symbolize with `atos -o rebar -l 0x10000 <addr>` to get source lines.
#include <ucontext.h>
#include <dlfcn.h>
#define HP_SLOTS (1 << 16)
static struct { uint64_t key, n; } g_hp[HP_SLOTS];
static const char *g_hp_file; static int g_hp_rows;   // REBAR_HOSTPROF_ROWS=<n>: how many addresses the report lists (default 600)
static int g_hp_main_only;   // REBAR_HOSTPROF_MAIN=1: only the samples that land on the process's main thread (the renderer's main thread is the one that sets the frame rate)
static void hostprof_sample(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    if (g_hp_main_only && !pthread_main_np()) return;
    uint64_t rip = (uint64_t)((ucontext_t *)uc)->uc_mcontext->__ss.__rip;
    unsigned h = (unsigned)((rip * 0x9e3779b97f4a7c15ull) >> 48);
    for (int k = 0; k < 64; k++) {
        unsigned j = (h + k) & (HP_SLOTS - 1);
        uint64_t cur = __atomic_load_n(&g_hp[j].key, __ATOMIC_RELAXED);
        if (cur == 0 && __atomic_compare_exchange_n(&g_hp[j].key, &cur, rip, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) cur = rip;
        if (cur == rip) { __atomic_fetch_add(&g_hp[j].n, 1, __ATOMIC_RELAXED); return; }
    }
}
static int hp_cmp(const void *a, const void *b) { uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1]; return x < y ? 1 : x > y ? -1 : 0; }
static void *hostprof_thread(void *arg) {
    (void)arg;
    static uint64_t rows[HP_SLOTS][2];
    for (;;) {
        sleep(2);
        char name[PATH_MAX], tmp[PATH_MAX];
        snprintf(name, sizeof name, "%s.%d", g_hp_file, (int)getpid()); snprintf(tmp, sizeof tmp, "%s.tmp", name);
        FILE *f = fopen(tmp, "w"); if (!f) continue;
        int n = 0; uint64_t total = 0;
        for (int k = 0; k < HP_SLOTS; k++) if (g_hp[k].n) { rows[n][0] = g_hp[k].key; rows[n++][1] = g_hp[k].n; total += g_hp[k].n; }
        qsort(rows, n, sizeof rows[0], hp_cmp);
        fprintf(f, "total %llu\n", (unsigned long long)total);
        for (int k = 0; k < n && k < (g_hp_rows ? g_hp_rows : 600); k++) { Dl_info di; const char *fn = (rows[k][0] - (uint64_t)g_jit_arena < JIT_ARENA) ? "<translated code>" : dladdr((void *)rows[k][0], &di) && di.dli_sname ? di.dli_sname : "?"; fprintf(f, "%llx %llu %s\n", (unsigned long long)rows[k][0], (unsigned long long)rows[k][1], fn); }
        fclose(f); rename(tmp, name);
    }
    return NULL;
}
static void hostprof_start(const char *file) {
    g_hp_file = file; if (getenv("REBAR_HOSTPROF_ROWS")) g_hp_rows = atoi(getenv("REBAR_HOSTPROF_ROWS")); g_hp_main_only = getenv("REBAR_HOSTPROF_MAIN") != NULL;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = hostprof_sample; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
    pthread_t th; pthread_create(&th, NULL, hostprof_thread, NULL); pthread_detach(th);
}

#include "bridge.h"
static const char *prof_stub_name(int key) { int idx = key - 1; return idx >= 0 && idx < g_nstubs && g_stubs[idx].name ? g_stubs[idx].name : "?"; }

static void usr2_backtrace(int sig) {
    CPU *c = t_cpu;
    fprintf(stderr, "rebar: SIGUSR2 on thread %p, guest pc 0x%llx\n", (void *)pthread_self(), c ? (unsigned long long)c->pc : 0ull);
    if (c) dump_backtrace(c);
}
static struct timespec g_t0;
static void print_stats(void) {
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - g_t0.tv_sec) + (double)(t1.tv_nsec - g_t0.tv_nsec) / 1e9;
    fprintf(stderr, "rebar: %llu instructions in %.2f s (%.1f M/s)\n", (unsigned long long)g_stats_cpu->tick, secs, g_stats_cpu->tick / secs / 1e6);
}
static void usage(void) {
    fprintf(stderr, "usage: rebar [-t] <arm64-macho> [args...]\n");
    exit(2);
}

// A guest fork() is a host fork() of a process with several threads: only the forking thread survives in the child, so any lock another
// thread held at that moment would stay locked for good (the child hangs in its first handler or in the translator) and the translator's
// lists could be caught half-updated. The forking thread takes the bridge's locks around the fork and the child starts them clean again.
static __thread bool t_fork_gil;
static __thread sigset_t t_fork_oldmask;
// While this thread holds the bridge's locks for the fork no guest signal handler may start on it (a SIGCHLD of another child that has just exited is the usual one: Go's handler
// would take the lock this thread already holds and the thread, and with it Go's fork lock, would hang for ever), so the signals are held back until the locks are given up.
static void fork_block_signals(void) {
    sigset_t all; sigfillset(&all);
    sigdelset(&all, SIGSEGV); sigdelset(&all, SIGBUS); sigdelset(&all, SIGILL); sigdelset(&all, SIGTRAP); sigdelset(&all, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &all, &t_fork_oldmask);
}
static void atfork_prepare(void) {
    fork_block_signals();
    if (!t_have_gil) { pthread_mutex_lock(&g_gil); t_fork_gil = true; }
    pthread_mutex_lock(&g_jit_mu); pthread_mutex_lock(&g_gstack_mu); pthread_mutex_lock(&g_owned_lock);
    pthread_mutex_lock(&g_thunk_mu); pthread_mutex_lock(&g_gpu_mu);
}
static void atfork_parent(void) {
    pthread_mutex_unlock(&g_gpu_mu); pthread_mutex_unlock(&g_thunk_mu); pthread_mutex_unlock(&g_owned_lock);
    pthread_mutex_unlock(&g_gstack_mu); pthread_mutex_unlock(&g_jit_mu);
    if (t_fork_gil) { t_fork_gil = false; pthread_mutex_unlock(&g_gil); }
    pthread_sigmask(SIG_SETMASK, &t_fork_oldmask, NULL);
}
static void atfork_child(void) {
    pthread_mutex_t fresh = PTHREAD_MUTEX_INITIALIZER;
    g_gpu_mu = fresh; g_thunk_mu = fresh; g_owned_lock = fresh; g_gstack_mu = fresh; g_jit_mu = fresh; g_gil = fresh;
    g_gil_waiters = 0;
    if (t_have_gil) pthread_mutex_lock(&g_gil);   // the forking thread was inside a handler: it still is
    t_fork_gil = false;
    pthread_sigmask(SIG_SETMASK, &t_fork_oldmask, NULL);
}

// UIKit for Mac Catalyst starts with [NSApplication _startWindowServerIfNecessary] (AppKit in macOS 26 has it; this macOS 15 does not): a process that is in a window-server session
// already (rebar always is) needs nothing there, so the class method is added as a no-op. The scene server (FuseBoard) only accepts a client that runs from an .app bundle.
static void ios_noop_ws(id self, SEL sel) { (void)self; (void)sel; }
// AppKit's NSWindowSectionContentController asks the background colour of the scroll view that UIKit registers for the titlebar separator with -colorUsingColorSpace:, an NSColor method;
// UIKit hands it a UIDualMacSystemColor (the grouped background of a SwiftUI List) which does not have it, and the layout pass throws. UIColor gets the method: its CGColor as an NSColor.
static id ios_color_using_cs(id self, SEL sel, id cs) {
    void *ms = dlsym(RTLD_DEFAULT, "objc_msgSend"); id (*send)(id, SEL) = (id (*)(id, SEL))ms; id (*send1)(id, SEL, id) = (id (*)(id, SEL, id))ms;
    id cg = send(self, sel_registerName("CGColor"));
    id ns = send1((id)objc_getClass("NSColor"), sel_registerName("colorWithCGColor:"), cg);
    return ns ? send1(ns, sel_registerName("colorUsingColorSpace:"), cs) : nil;
}
// UIKitCore (Mac Catalyst) calls objc_opt_respondsToSelector(obj, BOOL) in -[UINavigationController _applyViewControllers:...]: its second argument is a 0 or 1, not a selector. The answer is NO and
// harmless, but the runtime caches the "forward" answer for selector 1 in the class's method cache, and objc_msgSend takes a bucket whose selector is 1 for the end marker of the table: the next
// message that hashes to that bucket runs off the table (a second push on a NavigationStack while the first one animates crashed). The imports of the iOSSupport frameworks go through a
// wrapper that says NO for such a "selector" without asking the runtime.
static BOOL (*real_opt_responds)(id, SEL);
static BOOL ios_opt_responds(id o, SEL s) { if ((uintptr_t)s < 0x10000) return NO; return real_opt_responds(o, s); }
static void ios_patch_got(void) {
    real_opt_responds = (BOOL (*)(id, SEL))dlsym(RTLD_DEFAULT, "objc_opt_respondsToSelector");
    if (!real_opt_responds) return;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *nm = _dyld_get_image_name(i);
        if (!nm || strncmp(nm, "/System/iOSSupport/", 19)) continue;
        const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!mh || mh->magic != MH_MAGIC_64) continue;
        intptr_t slide = 0;   // (the slide dyld reports for cache images is not the one that fits the load commands: take it from where __TEXT really is)
        { const struct load_command *l0 = (const struct load_command *)(mh + 1);
          for (uint32_t k = 0; k < mh->ncmds; k++, l0 = (const struct load_command *)((const char *)l0 + l0->cmdsize))
              if (l0->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)l0)->segname, "__TEXT")) { slide = (intptr_t)((uintptr_t)mh - ((const struct segment_command_64 *)l0)->vmaddr); break; } }
        const struct load_command *lc = (const struct load_command *)(mh + 1);
        for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd != LC_SEGMENT_64) continue;
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            const struct section_64 *sc = (const struct section_64 *)(sg + 1);
            for (uint32_t j = 0; j < sg->nsects; j++, sc++) {
                if (strcmp(sc->sectname, "__stubs") || sc->reserved2 != 6) continue;   // (6-byte "jmp [rip+x]" stubs)
                const uint8_t *st = (const uint8_t *)(sc->addr + slide);
                for (uint64_t n = 0; n + 6 <= sc->size; n += 6) {
                    if (st[n] != 0xff || st[n + 1] != 0x25) continue;
                    void **slot = (void **)(st + n + 6 + *(const int32_t *)(st + n + 2));
                    if (*slot != (void *)real_opt_responds) continue;
                    if (getenv("REBAR_DEBUG_PATCH")) fprintf(stderr, "rebar: stub %p -> slot %p in %s\n", (void *)(st + n), (void *)slot, nm);
                    mach_vm_protect(mach_task_self(), (mach_vm_address_t)slot & ~0x3fffull, 0x4000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
                    *slot = (void *)ios_opt_responds;
                }
            }
        }
    }
}
static void ios_patch_uikit(void) {
    ios_patch_got();
    Class ui = objc_getClass("UIColor");
    if (ui && !class_getInstanceMethod(ui, sel_registerName("colorUsingColorSpace:"))) class_addMethod(ui, sel_registerName("colorUsingColorSpace:"), (IMP)ios_color_using_cs, "@@:@");
}
static void ios_prepare(void) {
    dlopen("/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", RTLD_NOW | RTLD_GLOBAL);
    Class app = objc_getClass("NSApplication");
    if (app && !class_getClassMethod(app, sel_registerName("_startWindowServerIfNecessary")))
        class_addMethod((Class)object_getClass((id)app), sel_registerName("_startWindowServerIfNecessary"), (IMP)ios_noop_ws, "v@:");
}

int main(int argc, char **argv) {
    int ai = 1;
    // libmalloc's nano zone (the default for small blocks) aborts a guest that runs Chromium or Node ("Non-aligned pointer being freed (2)" while Foundation's
    // path cache rehashes); the cause is not found yet. The zone is chosen when the process starts, so rebar starts itself once more with it off. (The shell
    // the tests run in has it off already, because Electron apps set it for everything they start; a launcher opened from the Dock has not.)
    if (!getenv("REBAR_KEEP_NANO") && (!getenv("MallocNanoZone") || strcmp(getenv("MallocNanoZone"), "0"))) {
        char self[PATH_MAX]; uint32_t sz = sizeof self;
        setenv("MallocNanoZone", "0", 1);
        if (_NSGetExecutablePath(self, &sz) == 0) execv(self, argv);
    }
    {   // CoreFoundation fixes the process's main bundle the first time anything asks for it, and it reads CFProcessPath for that. Something that loads before the guest (AppKit for VLC)
        // asked before main() could set it, so a program inside an .app is run by a rebar that starts with CFProcessPath already in its environment.
        const char *prog = NULL;
        for (int k = 1; k < argc; k++) if (argv[k][0] != '-') { prog = argv[k]; break; }
        char real0[PATH_MAX];
        if (prog && strstr(prog, ".app/Contents/MacOS/") && realpath(prog, real0) && (!getenv("CFProcessPath") || strcmp(getenv("CFProcessPath"), real0))) {
            char self[PATH_MAX]; uint32_t sz = sizeof self;
            setenv("CFProcessPath", real0, 1);
            if (_NSGetExecutablePath(self, &sz) == 0) execv(self, argv);
        }
    }
    if (!getenv("REBAR_NOATFORK")) pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    if (getenv("REBAR_TRACE_FILE")) {  // helper processes lose their stderr: each process writes its debug output to <file>.<pid>
        char tf[PATH_MAX]; snprintf(tf, sizeof tf, "%s.%d", getenv("REBAR_TRACE_FILE"), (int)getpid());
        int fd = open(tf, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) dup2(fd, 2);
    }
    g_trace_paths = getenv("REBAR_TRACE_PATHS") != NULL;
    if (getenv("REBAR_GPU_SYNC")) g_gpu_sync = atoi(getenv("REBAR_GPU_SYNC"));
    if (getenv("REBAR_DUMP_LAYERS")) g_dump_layers = atoi(getenv("REBAR_DUMP_LAYERS"));
    g_trace_stubs = getenv("REBAR_TRACE_STUBS") != NULL;
    g_trace_msg = getenv("REBAR_TRACE_MSG");
    if (getenv("REBAR_TRACE_MEM")) g_trace_mem = strtoull(getenv("REBAR_TRACE_MEM"), NULL, 16);
    if (getenv("REBAR_WATCH_MEM")) g_wmem = strtoull(getenv("REBAR_WATCH_MEM"), NULL, 16);
    if (getenv("REBAR_WATCH")) { const char *w = getenv("REBAR_WATCH"); g_watch_pc = strtoull(w, NULL, 16); int n = 0; for (const char *q = strchr(w, ','); q && n < 4; q = strchr(q + 1, ',')) g_watch_more[n++] = strtoull(q + 1, NULL, 16); }
    if (getenv("REBAR_BACKTRACE_ON_USR2")) {  // debugging aid: `kill -USR2 <pid>` prints the guest backtrace of the thread that takes the signal
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = usr2_backtrace; sigaction(SIGUSR2, &sa, NULL);
    }
    g_no_simd_fast = getenv("REBAR_NO_SIMD_FAST") != NULL;
    g_no_async_tail = getenv("REBAR_NO_ASYNC_TAIL") != NULL;
    pathmap_init();
    if (getenv("REBAR_STATS")) atexit(print_stats);  // instructions executed, and the speed
    if (ai < argc && !strcmp(argv[ai], "-t")) { g_trace = true; ai++; }
    if (ai < argc && !strcmp(argv[ai], "--jit-selftest")) {
        CPU cpu0 = {0}; t_cpu = &cpu0;
        if (ai + 5 < argc && !strcmp(argv[ai + 4], "scan") && !scan_load(argv[ai + 5])) return 2;
        return jit_selftest(ai + 1 < argc ? atol(argv[ai + 1]) : 200000, ai + 2 < argc ? strtoull(argv[ai + 2], NULL, 0) : 0, ai + 3 < argc && strcmp(argv[ai + 3], "-") ? argv[ai + 3] : NULL, ai + 4 < argc && (!strcmp(argv[ai + 4], "tails") || !strcmp(argv[ai + 4], "scan")));
    }
    if (ai < argc && !strcmp(argv[ai], "--probe-insns")) { CPU cpu0 = {0}; t_cpu = &cpu0; return probe_insns(); }
    if (ai < argc && !strcmp(argv[ai], "--simd-selftest")) {
        CPU cpu0 = {0}; t_cpu = &cpu0;
        return simd_selftest(ai + 1 < argc ? atol(argv[ai + 1]) : 1000000);
    }
    if (ai < argc && !strcmp(argv[ai], "--string-specs")) {  // developer aid: how each mangled import name on stdin would be marshalled
        char line[2048], note[400];
        g_spec_note = note;
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\n")] = 0;
            note[0] = 0;
            StrSpec *sp = str_spec_for(line);
            int st = 0;
            char *dm = p_demangle ? p_demangle(line + 1, NULL, NULL, &st) : NULL;
            if (!sp) printf("-\t%s\t%s\n", note, dm ? dm : line);
            else printf("args %02x wb %02x uninit %02x%s%s%s\t%s\t%s\n", sp->mask, sp->wb, sp->uninit, sp->sret ? " ret" : "", sp->sb ? " sb" : "", sp->dtor ? " dtor" : "", note, dm ? dm : line);
            free(dm);
        }
        return 0;
    }
    if (ai >= argc) usage();
    g_dbg = g_trace || g_wmem || g_watch_pc;
    jit_init();

    CPU cpu = {0};
    cpu.tpidrro = (uint64_t)t_tlv_tab;
    g_stats_cpu = &cpu;
    t_cpu = &cpu;
    if (getenv("REBAR_PROFILE")) prof_start(getenv("REBAR_PROFILE")), prof_register(&cpu);
    if (getenv("REBAR_HOSTPROF")) hostprof_start(getenv("REBAR_HOSTPROF"));
    if (getenv("REBAR_JITPROF")) jitprof_start(getenv("REBAR_JITPROF"));
    if (getenv("REBAR_ENTRYSTATS")) { g_entry_stats = true; pthread_t es; pthread_create(&es, NULL, entstats_thread, (void *)getenv("REBAR_ENTRYSTATS")); pthread_detach(es); }
    if (getenv("REBAR_HELPERSTATS")) { g_helperstats = true; pthread_t hs; pthread_create(&hs, NULL, helperstats_thread, (void *)getenv("REBAR_HELPERSTATS")); pthread_detach(hs); }
    if (getenv("REBAR_JITSTATS")) { pthread_t st; pthread_create(&st, NULL, jitstats_thread, (void *)getenv("REBAR_JITSTATS")); pthread_detach(st); }
#ifdef OPSTATS
    if (getenv("REBAR_OPSTATS")) { pthread_t ot; pthread_create(&ot, NULL, opstats_thread, NULL); pthread_detach(ot); }
#endif
    g_sp_caller = getenv("REBAR_STUBPROF_CALLER");
    if (getenv("REBAR_SLOW_STUB")) { g_slow_ms = atoi(getenv("REBAR_SLOW_STUB")); clock_gettime(CLOCK_MONOTONIC, &g_slow_t0); }
    if (getenv("REBAR_STUBPROF")) stubprof_start(getenv("REBAR_STUBPROF"));
    crypto_init();  // the AES/CRC tables, before there are threads
    stubs_init();
    reserve_guest_region();
    {   // a script whose interpreter is an arm64 program (#!/path/to/interpreter [argument]): run the interpreter with the script as its argument, as the kernel would
        int sfd = open(argv[ai], O_RDONLY); char sb[512]; ssize_t sn = sfd >= 0 ? read(sfd, sb, sizeof sb - 1) : -1; if (sfd >= 0) close(sfd);
        if (sn > 2 && sb[0] == '#' && sb[1] == '!') {
            sb[sn] = 0; char *nl = strchr(sb, '\n'); if (nl) *nl = 0;
            char *interp = sb + 2; while (*interp == ' ') interp++;
            char *iarg = interp; while (*iarg && *iarg != ' ') iarg++;
            if (*iarg) { *iarg++ = 0; while (*iarg == ' ') iarg++; }
            if (*interp && !needs_rebar(interp) && !getenv("REBAR_NO_EXECSHIM")) {   // a script for a host interpreter (/bin/sh, /usr/bin/env): the host runs it, with the exec shim, so that the arm64 programs it starts run under rebar
                proc_pidpath(getpid(), g_self_path, sizeof g_self_path);
                char shim[PATH_MAX]; rebar_lib_path("librebar_exec.dylib", shim, sizeof shim);
                if (access(shim, R_OK) == 0) { snprintf(g_shim_path, sizeof g_shim_path, "%s", shim); execve(argv[ai], argv + ai, env_with_shim(environ)); }
            }
            if (*interp && needs_rebar(interp)) {
                char **nv = calloc((size_t)argc + 4, sizeof *nv); int k = 0;
                for (int j = 0; j < ai; j++) nv[k++] = argv[j];
                nv[k++] = strdup(interp); if (*iarg) nv[k++] = strdup(iarg);
                for (int j = ai; j < argc; j++) nv[k++] = argv[j];
                argv = nv; argc = k;
            }
        }
    }
    char real[PATH_MAX];
    if (!realpath(argv[ai], real)) { perror(argv[ai]); return 1; }
    snprintf(g_exe_path, sizeof g_exe_path, "%s", real);
    setenv("CFProcessPath", getenv("REBAR_CFPROCESSPATH") ? getenv("REBAR_CFPROCESSPATH") : g_exe_path, 1);   // (REBAR_CFPROCESSPATH: an iOS app run from its host bundle, see tools/ipa.py) before anything loads AppKit: once something asks for the main bundle, CoreFoundation keeps the answer (it was the folder of rebar for VLC)
    proc_pidpath(getpid(), g_self_path, sizeof g_self_path);  // helper processes are started as `rebar <program>`
    if (!getenv("REBAR_NO_EXECSHIM")) {   // librebar_exec.dylib lives next to rebar
        char shim[PATH_MAX]; rebar_lib_path("librebar_exec.dylib", shim, sizeof shim);
        if (access(shim, R_OK) == 0) snprintf(g_shim_path, sizeof g_shim_path, "%s", shim);
    }
    if (getenv("REBAR_IOS_UI")) ios_prepare();   // an iOS app: UIKit (Mac Catalyst) asks AppKit for something this macOS does not have
    GImage *mainimg = load_image(real, NULL, true);  // maps the executable and, recursively, its guest dylibs
    if (!mainimg->have_entry) { fprintf(stderr, "rebar: no LC_MAIN (LC_UNIXTHREAD not supported yet)\n"); return 1; }

    if (getenv("REBAR_IMAGES"))
        for (int k = 0; k < g_nimages; k++)
            fprintf(stderr, "rebar: image %d %s base=0x%llx slide=0x%llx end=0x%llx\n", k, g_images[k]->path,
                    (unsigned long long)g_images[k]->base, (unsigned long long)g_images[k]->slide, (unsigned long long)g_images[k]->end);
    // Guest-visible process state.
    g_argc = argc - ai;
    g_argv = argv + ai;
    if (!getenv("REBAR_KEEP_ARGV") && argc >= 2) {
        // `ps` (and anything that checks what a process is running, as OpenCode's service manager does) must see the guest program's own command line, not "rebar <program> ...":
        // the guest gets copies of its arguments, and the host's argument area is overwritten with them
        char **copy = calloc((size_t)g_argc + 1, sizeof *copy);
        for (int k = 0; k < g_argc; k++) copy[k] = strdup(argv[ai + k]);
        char *area = argv[0], *end = argv[argc - 1] + strlen(argv[argc - 1]);
        size_t room = (size_t)(end - area);
        char *title = calloc(room + 1, 1); size_t used = 0;
        for (int k = 0; k < g_argc && used < room; k++) used += (size_t)snprintf(title + used, room + 1 - used, "%s%s", k ? " " : "", copy[k]);
        memset(area, 0, room + 1); memcpy(area, title, used < room ? used : room); free(title);
        g_argv = copy;
    }
    g_progname = strrchr(g_argv[0], '/') ? strrchr(g_argv[0], '/') + 1 : g_argv[0];
    g_environ = environ;
    setprogname(g_progname);
    setenv("CFProcessPath", getenv("REBAR_CFPROCESSPATH") ? getenv("REBAR_CFPROCESSPATH") : g_exe_path, 1);  // CoreFoundation derives the main bundle from this
    // argv, the environment and the apple strings in one array, the way the kernel lays them out: Go's runtime finds the environment as argv[argc + 1 ...]
    static char apple0[1100];
    snprintf(apple0, sizeof apple0, "executable_path=%s", g_exe_path);
    size_t nenv = 0;
    while (environ && environ[nenv]) nenv++;
    char **blk = calloc((size_t)g_argc + 1 + nenv + 1 + 2, sizeof *blk);
    size_t q = 0;
    for (int k = 0; k < g_argc; k++) blk[q++] = g_argv[k];
    blk[q++] = NULL;
    char **envp_blk = blk + q;
    for (size_t e = 0; e < nenv; e++) blk[q++] = environ[e];
    blk[q++] = NULL;
    char **apple = blk + q;
    blk[q++] = apple0; blk[q++] = NULL;
    g_argv = blk;
    g_apple = apple;
    *_NSGetArgv() = g_argv;  // NSProcessInfo and NSLog read these from the host process
    *_NSGetArgc() = g_argc;
    load_host_libraries();  // after the identity patch: Foundation snapshots argv when it loads
    if (getenv("REBAR_IOS_UI")) ios_patch_uikit();
    set_host_process_name(g_progname);

    size_t stack_size = 256u << 20;
    uint8_t *stack = alloc_guarded(stack_size);
    own_add((uint64_t)stack, stack_size);
    gstack_register((uint64_t)stack, stack_size);
    cpu.sp = ((uint64_t)stack + stack_size - 256) & ~0xfull;
    if (getenv("REBAR_IMAGES")) fprintf(stderr, "rebar: guest stack %p - %p\n", (void *)stack, (void *)(stack + stack_size));

    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    find_sigtramp();
    fpos_cb_init();
    { void (*(*set_term)(void (*)(void)))(void) = (void (*(*)(void (*)(void)))(void))dlsym(RTLD_DEFAULT, "_ZSt13set_terminatePFvvE"); if (set_term) g_orig_terminate = set_term(rebar_terminate); }
    ensure_altstack();
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    if (getenv("REBAR_ABORT_REPORT")) sigaction(SIGABRT, &sa, NULL);   // an abort() of the guest (or of a host library it called): report the last imports like a fault

    start_images(&cpu, 0);                   // fixups, unwind info, ObjC classes, +load and initializers of every dylib
    objc_load_image(&cpu, mainimg);          // the executable's own +load and static initializers come last
    run_image_initializers(&cpu, mainimg, apple);

    cpu.x[0] = (uint64_t)g_argc; cpu.x[1] = (uint64_t)g_argv; cpu.x[2] = (uint64_t)envp_blk; cpu.x[3] = (uint64_t)apple;
    cpu.x[30] = stub_addr(STUB_EXIT);
    cpu.pc = mainimg->base + mainimg->entryoff;
    for (;;) run_until(&cpu, ~0ull);
}
