// A template JIT from arm64 to x86-64, used by the interpreter as an accelerator (REBAR_JIT=1).
//
// What it translates is a basic block: the straight-line instructions from a branch target up to and including the next
// branch. All guest state stays in the CPU structure (registers are loaded from it and stored back by every instruction), so
// the interpreter, the bridge, exceptions and signals see the same state whichever of the two executed the instructions, and
// leaving a block is just setting c->pc and returning. Instructions with no template are executed by calling exec_slow()
// (the interpreter's own code for them), so every non-branch instruction can be part of a block.
//
// Blocks end in direct jumps to the next blocks (patched in when those exist), indirect branches go through a lookup stub
// written in machine code below, and a budget counter in every block entry returns to the interpreter's dispatcher every
// few thousand instructions. Code is never freed: invalidation (the guest flushed an instruction cache range, or unmapped
// memory) removes blocks from the lookup tables and turns their first bytes into a jump back to the dispatcher.
//
// x86 register use inside a template: rax rcx rdx rsi rdi r8-r11 are scratch, r15 holds the CPU pointer.

#include <stddef.h>

#define JIT_ARENA (768ull << 20)
#define JIT_TAB_BITS 20
#define JIT_IC_BITS 20
#define JIT_HOT_BITS 20
#define JIT_PEND_BITS 14
#define JIT_PAGE_BITS 14
#define JIT_MAX_INSNS 40
static int g_jit_maxn = JIT_MAX_INSNS;   // REBAR_JIT_MAXN=<n>: longest block, in guest instructions
#define JIT_BUDGET (1 << 20)

typedef struct { uint64_t pc; uint8_t *code; } JEnt;          // code == NULL: this pc is not translated (and not worth trying again)
typedef struct JBlk { uint64_t pc; uint32_t bytes, exit_off; uint8_t *code; struct JBlk *next; } JBlk;   // exit_off: where the sequence that leaves with c->pc = pc starts (a dead block's entry jumps there)
typedef struct JPend { uint64_t target; uint8_t *site; struct JPend *next; } JPend;

static bool g_jit_on;
static uint8_t *g_jit_arena, *g_jit_top, *g_jit_end;
static uint8_t *g_jit_cell_top, *g_jit_cell_end, *g_jit_sink;   // the indirect-branch cells: eight bytes each, in a pool at the top of the arena (a store next to code would be a self-modifying-code stall)
static JEnt *g_jit_tab;         // every translated pc (open addressing, entries are never removed, only killed)
static uint8_t **g_jit_ic;      // direct-mapped cache of block entries read by the generated indirect-branch stub: one word per slot, so it can never tear;
                                // the word before a block's code holds its guest pc, which the stub compares (a slot that was overwritten by another thread just misses)
static uint8_t *g_jit_hot;      // how often each (hashed) pc was reached by the interpreter
static JBlk *g_jit_pages[1 << JIT_PAGE_BITS];
static JPend *g_jit_pend[1 << JIT_PEND_BITS];
// The translator's bookkeeping nodes live in their own pools (free lists, carved from mmap chunks), not in the host malloc heap that the guest shares: a guest that
// frees a block twice or writes behind one must not be able to take the translator's lists down with it.
typedef struct JNode { struct JNode *next; } JNode;
static JNode *g_jpool_free[2]; static uint8_t *g_jpool_cur[2], *g_jpool_end[2];
static int g_jpool_malloc = -1;   // REBAR_JIT_POOL=0: the nodes come from malloc again (to compare)
static void *jpool_alloc(int kind, size_t sz) {   // under g_jit_mu
    if (g_jpool_malloc < 0) g_jpool_malloc = getenv("REBAR_JIT_POOL") && !strcmp(getenv("REBAR_JIT_POOL"), "0");
    if (g_jpool_malloc) return malloc(sz);
    if (g_jpool_free[kind]) { JNode *n = g_jpool_free[kind]; g_jpool_free[kind] = n->next; return n; }
    if (g_jpool_cur[kind] + sz > g_jpool_end[kind]) {
        size_t chunk = 1u << 20; uint8_t *m = mmap(NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) abort();
        g_jpool_cur[kind] = m; g_jpool_end[kind] = m + chunk;
    }
    void *r = g_jpool_cur[kind]; g_jpool_cur[kind] += sz; return r;
}
static void jpool_free(int kind, void *p) { if (g_jpool_malloc > 0) { free(p); return; } JNode *n = p; n->next = g_jpool_free[kind]; g_jpool_free[kind] = n; }
static pthread_mutex_t g_jit_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_jit_enter_thunk, *g_jit_exit_stub, *g_jit_ind_stub, *g_jit_dummy;   // g_jit_dummy: a "block" whose guest pc (the word before it) is 0, the initial content of every indirect-branch cell
static unsigned g_jit_threshold = 1;   // translate a pc at its second visit by the interpreter (24 left cold code interpreted: a UI scrolls through thousands of paths a few times each; 1 scrolls VS Code 25% faster)
static uint64_t g_jit_nblocks, g_jit_ninsns, g_jit_nfallback;
static bool g_jit_count;             // REBAR_JITCOUNT: translated code counts block entries and helper calls
static FILE *g_jit_dump, *g_jit_map;               // REBAR_JITDUMP: every translated block (guest words and host bytes), for reading the generated code
static uint32_t g_jit_offs[64];   // REBAR_JITDUMP: where in the block's host code each guest instruction ends
static uint64_t *g_jit_cnt; static uint64_t g_jit_fbdyn[1 << 12];
static bool jit_leaf_import(uint64_t target, void **fn, int *nargs, int *zext8);   // bridge.h: an import that translated code may call directly
static bool g_jit_noleaf;       // REBAR_JIT_NOLEAF=1: every import goes through the dispatcher
static uint64_t g_ji_calls, g_ji_killed, g_ji_pages;   // REBAR_JITCOUNT: jit_invalidate calls, blocks they dropped
static int g_ind_kind;   // 0 BR, 1 BLR, 2 RET: what emit_indirect is emitting (for the counters)
static uint64_t g_jr_indk[3], g_jr_missk[3];
static uint64_t g_jr_ind, g_jr_indmiss;   // REBAR_JITCOUNT: indirect branches executed in translated code / those that missed their cell
static uint64_t g_jr_runs, g_jr_stub, g_jr_budget, g_jr_other, g_jr_found, g_jr_cold;   // REBAR_JITCOUNT: why translated code returned to the dispatcher
static uint32_t g_jit_fb_key[1 << 12]; static uint64_t g_jit_fb_n[1 << 12];   // instructions that got a helper call instead of a template, by encoding class

typedef struct { uint8_t *at; uint64_t pc; uint8_t reg_of[8]; uint8_t dirty; } JSite;   // a guest memory access in translated code: where its ModRM byte is, the guest pc, and which guest registers the cache held (and had not stored back) just before it
typedef struct { uint8_t *code; uint32_t len; uint32_t n; uint64_t pc; bool precise; JSite *sites; uint32_t nsites; } JReg;   // precise: every memory access of the block leaves the CPU structure exact first (see g_jit_precise_lo)
static uint64_t g_jit_precise_lo, g_jit_precise_hi;   // guest code in [lo, hi) is translated in the precise form: the pc and the registers are stored before each load / store, so that a fault in it can be given to the guest's signal handler
static JReg *g_jit_reg;       // every block ever translated, in code-address order (for profiling and fault reports)
static size_t g_jit_reg_n, g_jit_reg_cap;
static const JReg *jit_block_of(uint64_t rip) {   // the block whose code contains the host address
    size_t lo = 0, hi = g_jit_reg_n;
    while (lo < hi) { size_t m = (lo + hi) / 2; if ((uint64_t)g_jit_reg[m].code <= rip) lo = m + 1; else hi = m; }
    if (!lo) return NULL;
    const JReg *r = &g_jit_reg[lo - 1];
    return rip - (uint64_t)r->code < r->len ? r : NULL;
}

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G, CC_ALWAYS = 100, CC_NEVER = 101 };

typedef struct {
    uint8_t *p, *limit;
    int flagkind;        // what the x86 flags hold right now, if anything: 1 = result of a subtraction, 2 = of an addition, 3 = of a logical op
    int sf_flags;        // (reserved)
    uint64_t pc;         // guest pc of the instruction being translated
    uint64_t blockpc;
    bool failed;
    bool kind_set;       // the instruction being translated set flagkind
    // the register cache: guest registers (0..30, 31 = sp) held in host registers for the rest of the block, written back at every exit and helper call
    uint8_t rc_slot_of[32];   // guest register -> cache slot + 1 (0: not cached)
    uint8_t rc_reg_of[8];     // cache slot -> guest register + 1 (0: free)
    uint8_t rc_dirty;         // bit per slot: the host register is newer than the CPU structure
    uint8_t rc_pin;           // slots whose host registers the instruction being translated is using: they are not evicted
    int insn_left;            // how many more instructions the block being translated can take after the current one
    struct { uint8_t *at; uint64_t target; uint8_t dirty; uint8_t reg_of[8]; uint8_t fkind; } sx[16];   // conditional branches taken out of the middle of the block (see side_exit)
    int nsx;
    uint32_t rc_age[8], rc_tick;
    uint8_t *bm_rel, *bm_cell, *bm_cont;   // a BLR through the return-address stack: where its cell check jumps on a miss (to a stub after the block), the cell, and the code after the call
    uint8_t *cs_rel; uint64_t cs_target;   // a BL that goes through the return-address stack: the rel32 of its call, which leads to an exit stub (placed after the block) until the callee is translated
    uint64_t fl_pend_pc; int fl_pend_kind; bool fl_pend_taken, fl_pend_fall;   // flags_out deferred the N Z C V stores of the instruction before the B.cond at fl_pend_pc: see flags_deferrable
    uint8_t *mw_start, *mw_end;   // the code of the MOVZ / MOVN / MOVK just translated (a MOVK that follows rewrites it as one move)
    uint64_t mw_val, mw_pc; int mw_rd, mw_sf;
    uint8_t fresh;            // cache slots handed out for a register that the current instruction is about to write: their host register has no meaning yet
    JSite *sites; int nsites, capsites;   // the guest memory accesses of the block (see JSite)
} JC;

#define GX(r) ((int32_t)(offsetof(CPU, x) + 8 * (r)))
#define GSP ((int32_t)offsetof(CPU, sp))
#define GPC ((int32_t)offsetof(CPU, pc))
#define GN ((int32_t)offsetof(CPU, n))
#define GZ ((int32_t)offsetof(CPU, z))
#define GC ((int32_t)offsetof(CPU, c))
#define GV ((int32_t)offsetof(CPU, v))
#define GBUDGET ((int32_t)offsetof(CPU, jit_budget))
#define GHSP ((int32_t)offsetof(CPU, jit_hsp))
#define GHLIM ((int32_t)offsetof(CPU, jit_hlim))

// ------------------------------------------------------------------------------------------------ assembler

static inline void e1(JC *j, uint8_t b) { *j->p++ = b; }
static inline void e4(JC *j, uint32_t v) { memcpy(j->p, &v, 4); j->p += 4; }
static inline void e8(JC *j, uint64_t v) { memcpy(j->p, &v, 8); j->p += 8; }

// REX prefix: w = 64-bit operand, reg = the ModRM.reg register, rm = the ModRM.rm (or base) register; byte = a byte register operand that needs a REX to name sil/dil
static inline void rex(JC *j, int w, int reg, int rm, int byte) {
    uint8_t r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0);
    if (r != 0x40 || (byte && (reg >= 4 || rm >= 4))) e1(j, r);
}
static inline void modrm_rr(JC *j, int reg, int rm) { e1(j, 0xC0 | ((reg & 7) << 3) | (rm & 7)); }
static void jsite_add(JC *j) {
    if (j->nsites == j->capsites) { j->capsites = j->capsites ? j->capsites * 2 : 64; j->sites = realloc(j->sites, (size_t)j->capsites * sizeof(JSite)); }
    JSite *s = &j->sites[j->nsites++];
    s->at = j->p; s->pc = j->pc; memcpy(s->reg_of, j->rc_reg_of, 8); s->dirty = j->rc_dirty;
    for (int k = 0; k < 8; k++) if (j->fresh & (1u << k)) { s->reg_of[k] = 0; s->dirty &= (uint8_t)~(1u << k); }
}
static void modrm_mem(JC *j, int reg, int base, int32_t disp) {
    if (base != R15) jsite_add(j);   // (a memory operand that is not the CPU structure: a guest access, or the code's own cells)
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    e1(j, (mod << 6) | ((reg & 7) << 3) | ((base & 7) == 4 ? 4 : (base & 7)));
    if ((base & 7) == 4) e1(j, 0x24);
    if (mod == 1) e1(j, (uint8_t)disp); else if (mod == 2) e4(j, (uint32_t)disp);
}
// op r, [base+disp] style: opcode bytes (1 or 2 after the optional 0F) with the register field
static void op_rm(JC *j, int w, int twobyte, uint8_t opc, int reg, int base, int32_t disp) {
    rex(j, w, reg, base, 0); if (twobyte) e1(j, 0x0F); e1(j, opc); modrm_mem(j, reg, base, disp);
}
static void op_rr(JC *j, int w, int twobyte, uint8_t opc, int reg, int rm) {
    rex(j, w, reg, rm, 0); if (twobyte) e1(j, 0x0F); e1(j, opc); modrm_rr(j, reg, rm);
}
static inline void mov_rm(JC *j, int dst, int base, int32_t disp, int w) { op_rm(j, w, 0, 0x8B, dst, base, disp); }   // mov dst, [base+disp]
static inline void mov_mr(JC *j, int base, int32_t disp, int src, int w) { op_rm(j, w, 0, 0x89, src, base, disp); }   // mov [base+disp], src
static inline void mov_rr(JC *j, int dst, int src, int w) { op_rr(j, w, 0, 0x89, src, dst); }
static void mov_ri(JC *j, int dst, uint64_t v) {  // the shortest way to load a constant
    if (v == 0) { rex(j, 0, dst, dst, 0); e1(j, 0x31); modrm_rr(j, dst, dst); }                       // xor r32, r32 (clobbers flags!)
    else if (v <= 0xffffffffull) { rex(j, 0, 0, dst, 0); e1(j, 0xB8 + (dst & 7)); e4(j, (uint32_t)v); }
    else if ((int64_t)v >= INT32_MIN && (int64_t)v < 0) { rex(j, 1, 0, dst, 0); e1(j, 0xC7); modrm_rr(j, 0, dst); e4(j, (uint32_t)v); }
    else { rex(j, 1, 0, dst, 0); e1(j, 0xB8 + (dst & 7)); e8(j, v); }
}
// a constant that must not touch the flags
static void mov_ri_nf(JC *j, int dst, uint64_t v) {
    if (v == 0) { rex(j, 0, 0, dst, 0); e1(j, 0xB8 + (dst & 7)); e4(j, 0); } else mov_ri(j, dst, v);
}
// ALU register, register: op = 0x01 add, 0x09 or, 0x11 adc, 0x19 sbb, 0x21 and, 0x29 sub, 0x31 xor, 0x39 cmp, 0x85 test (dst op= src)
static inline void alu_rr(JC *j, uint8_t op, int dst, int src, int w) { op_rr(j, w, 0, op, src, dst); }
// ALU register, [mem]: op as above + 2
static inline void alu_rm(JC *j, uint8_t op, int dst, int base, int32_t disp, int w) { op_rm(j, w, 0, op + 2, dst, base, disp); }
// ALU register, immediate: ext = 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp
static void alu_ri(JC *j, int ext, int dst, int32_t imm, int w) {
    rex(j, w, 0, dst, 0);
    if (imm >= -128 && imm <= 127) { e1(j, 0x83); modrm_rr(j, ext, dst); e1(j, (uint8_t)imm); }
    else { e1(j, 0x81); modrm_rr(j, ext, dst); e4(j, (uint32_t)imm); }
}
static void test_ri(JC *j, int dst, uint32_t imm, int w) { rex(j, w, 0, dst, 0); e1(j, 0xF7); modrm_rr(j, 0, dst); e4(j, imm); }
static void shift_ri(JC *j, int ext, int dst, int amt, int w) {  // ext: 0 rol, 1 ror, 4 shl, 5 shr, 7 sar
    if (!amt) return;
    rex(j, w, 0, dst, 0);
    if (amt == 1) { e1(j, 0xD1); modrm_rr(j, ext, dst); } else { e1(j, 0xC1); modrm_rr(j, ext, dst); e1(j, (uint8_t)amt); }
}
static void shift_cl(JC *j, int ext, int dst, int w) { rex(j, w, 0, dst, 0); e1(j, 0xD3); modrm_rr(j, ext, dst); }
static void unary(JC *j, int ext, int dst, int w) { rex(j, w, 0, dst, 0); e1(j, 0xF7); modrm_rr(j, ext, dst); }   // 2 not, 3 neg, 4 mul, 5 imul, 6 div, 7 idiv
static void imul_rr(JC *j, int dst, int src, int w) { op_rr(j, w, 1, 0xAF, dst, src); }
static void movzx8_rm(JC *j, int dst, int base, int32_t disp) { op_rm(j, 0, 1, 0xB6, dst, base, disp); }
static void movzx16_rm(JC *j, int dst, int base, int32_t disp) { op_rm(j, 0, 1, 0xB7, dst, base, disp); }
static void movsx_rm(JC *j, int dst, int base, int32_t disp, int size, int w) {  // sign-extending load of 1, 2 or 4 bytes
    if (size == 4) op_rm(j, 1, 0, 0x63, dst, base, disp); else op_rm(j, w, 1, size == 1 ? 0xBE : 0xBF, dst, base, disp);
}
static void movzx_rr(JC *j, int dst, int src, int size) {  // zero-extend the low 1, 2 or 4 bytes of src into dst (64-bit)
    if (size == 4) { op_rr(j, 0, 0, 0x89, src, dst); }
    else { rex(j, 0, dst, src, 1); e1(j, 0x0F); e1(j, size == 1 ? 0xB6 : 0xB7); modrm_rr(j, dst, src); }
}
static void movsx_rr(JC *j, int dst, int src, int size, int w) {
    if (size == 4) op_rr(j, 1, 0, 0x63, dst, src);
    else { rex(j, w, dst, src, 1); e1(j, 0x0F); e1(j, size == 1 ? 0xBE : 0xBF); modrm_rr(j, dst, src); }
}
static void st_sized(JC *j, int base, int32_t disp, int src, int size) {  // store the low 1, 2, 4 or 8 bytes
    if (size == 2) e1(j, 0x66);
    rex(j, size == 8, src, base, size == 1);
    e1(j, size == 1 ? 0x88 : 0x89); modrm_mem(j, src, base, disp);
}
static void setcc_m(JC *j, int cc, int base, int32_t disp) { rex(j, 0, 0, base, 0); e1(j, 0x0F); e1(j, 0x90 + cc); modrm_mem(j, 0, base, disp); }
static void mov_m8_imm(JC *j, int base, int32_t disp, uint8_t v) { rex(j, 0, 0, base, 0); e1(j, 0xC6); modrm_mem(j, 0, base, disp); e1(j, v); }
static void cmp_m8_imm(JC *j, int base, int32_t disp, uint8_t v) { rex(j, 0, 0, base, 0); e1(j, 0x80); modrm_mem(j, 7, base, disp); e1(j, v); }
static void movzx8_rm_(JC *j, int dst, int base, int32_t disp) { movzx8_rm(j, dst, base, disp); }
static void cmov(JC *j, int cc, int dst, int src, int w) { op_rr(j, w, 1, 0x40 + cc, dst, src); }
static void lea_rm(JC *j, int dst, int base, int32_t disp, int w) { op_rm(j, w, 0, 0x8D, dst, base, disp); }
static void bt_ri(JC *j, int reg, int bit) { rex(j, 1, 0, reg, 0); e1(j, 0x0F); e1(j, 0xBA); modrm_rr(j, 4, reg); e1(j, (uint8_t)bit); }
static void shrd_ri(JC *j, int dst, int src, int amt, int w) { rex(j, w, src, dst, 0); e1(j, 0x0F); e1(j, 0xAC); modrm_rr(j, src, dst); e1(j, (uint8_t)amt); }
static void bswap(JC *j, int reg, int w) { rex(j, w, 0, reg, 0); e1(j, 0x0F); e1(j, 0xC8 + (reg & 7)); }
static void cdq_cqo(JC *j, int w) { if (w) e1(j, 0x48); e1(j, 0x99); }
static void jmp_reg(JC *j, int reg) { rex(j, 0, 0, reg, 0); e1(j, 0xFF); modrm_rr(j, 4, reg); }
static void call_reg(JC *j, int reg) { rex(j, 0, 0, reg, 0); e1(j, 0xFF); modrm_rr(j, 2, reg); }
static void ret_(JC *j) { e1(j, 0xC3); }
static void mfence_(JC *j) { e1(j, 0x0F); e1(j, 0xAE); e1(j, 0xF0); }
// ---- the register cache.  Every access to a guest register goes through ld_x / st_x (and the sp variants): a register read or written
// stays in one of eight host registers until the block leaves (a branch, a helper call, a join of two code paths), when the
// written ones are stored back.  Only mov instructions are emitted for that, so the x86 flags are never disturbed.
#define GXR(r) ((int32_t)(offsetof(CPU, x) + 8 * (r)))
#define RC_N 8
static const uint8_t RC_HOST[RC_N] = {RBX, RBP, R12, R13, R14, R10, R11, RDI};   // (the last three are caller-saved: the cache is emptied at every helper call anyway)
static int32_t rc_mem(int g) { return g == 31 ? (int32_t)offsetof(CPU, sp) : GXR(g); }
// The first g_npin slots are pinned: the guest registers g_pin[] (x8, x9 and x0 by default: the temporaries and the argument/result register that a third of the instructions
// name) live in those host registers for the whole time translated code runs, across blocks, and are never stored back at a block's end (a chained jump leaves them where
// they are).  They reach the CPU structure only when translated code leaves for C: the thunk's end (every exit to the dispatcher), a helper call, a call of a host function.
// REBAR_JIT_PIN=8,9,0 chooses them, REBAR_JIT_PIN=none turns it off.
static int g_npin;
static uint8_t g_pin[5];
static void rc_pin_apply(JC *j) { for (int k = 0; k < g_npin; k++) { j->rc_reg_of[k] = (uint8_t)(g_pin[k] + 1); j->rc_slot_of[g_pin[k]] = (uint8_t)(k + 1); } }
static void rc_store_slot(JC *j, int k) { mov_mr(j, R15, rc_mem(j->rc_reg_of[k] - 1), RC_HOST[k], 1); j->rc_dirty &= ~(1u << k); }
static void rc_flush(JC *j) { for (int k = g_npin; k < RC_N; k++) if (j->rc_dirty & (1u << k)) rc_store_slot(j, k); }
static void rc_clear(JC *j) { memset(j->rc_slot_of, 0, sizeof j->rc_slot_of); memset(j->rc_reg_of, 0, sizeof j->rc_reg_of); j->rc_dirty = 0; rc_pin_apply(j); }
static void rc_sync_out(JC *j) { for (int k = 0; k < g_npin; k++) mov_mr(j, R15, rc_mem(g_pin[k]), RC_HOST[k], 1); }   // before translated code calls C: the pinned registers into the CPU structure
static void rc_sync_in(JC *j) { for (int k = 0; k < g_npin; k++) mov_rm(j, RC_HOST[k], R15, rc_mem(g_pin[k]), 1); }    // and back after it (C may have changed them)
static int rc_alloc(JC *j, int g) {   // a slot for guest register g, which is not cached
    int k = -1;
    for (int n = g_npin; n < RC_N; n++) if (!j->rc_reg_of[n]) { k = n; break; }
    if (k < 0) {
        for (int n = g_npin; n < RC_N; n++) if (!(j->rc_pin & (1u << n)) && (k < 0 || j->rc_age[n] < j->rc_age[k])) k = n;
        if (k < 0) k = g_npin;   // (an instruction names at most four registers)
        if (j->rc_dirty & (1u << k)) rc_store_slot(j, k);
        j->rc_slot_of[j->rc_reg_of[k] - 1] = 0;
    }
    j->rc_reg_of[k] = (uint8_t)(g + 1); j->rc_slot_of[g] = (uint8_t)(k + 1);
    return k;
}
typedef struct { uint8_t *at; } Fix;
static Fix jcc_fwd(JC *j, int cc) { rc_flush(j); e1(j, 0x0F); e1(j, 0x80 + cc); Fix f = {j->p}; e4(j, 0); return f; }   // (both ways out of the branch see the registers in memory)
static Fix jmp_fwd(JC *j) { rc_flush(j); rc_clear(j); e1(j, 0xE9); Fix f = {j->p}; e4(j, 0); return f; }
static void fix_here(JC *j, Fix f) { rc_flush(j); rc_clear(j); int32_t d = (int32_t)(j->p - (f.at + 4)); memcpy(f.at, &d, 4); }
static void jmp_to(JC *j, uint8_t *target) { e1(j, 0xE9); int32_t d = (int32_t)(target - (j->p + 4)); e4(j, (uint32_t)d); }
static void jcc_to(JC *j, int cc, uint8_t *target) { e1(j, 0x0F); e1(j, 0x80 + cc); int32_t d = (int32_t)(target - (j->p + 4)); e4(j, (uint32_t)d); }

// ------------------------------------------------------------------------------------------------ guest registers

static void ld_g(JC *j, int dst, int g, int w) {            // guest register g (31 = sp) through the cache
    int k = j->rc_slot_of[g] - 1;
    if (k < 0) { k = rc_alloc(j, g); mov_rm(j, RC_HOST[k], R15, rc_mem(g), 1); }
    j->rc_age[k] = ++j->rc_tick;
    mov_rr(j, dst, RC_HOST[k], w);
}
static void st_g(JC *j, int g, int src) {
    int k = j->rc_slot_of[g] - 1;
    if (k < 0) k = rc_alloc(j, g);
    j->rc_age[k] = ++j->rc_tick;
    mov_rr(j, RC_HOST[k], src, 1);
    if (k >= g_npin) j->rc_dirty |= (uint8_t)(1u << k);
}
// direct access to the host register of a guest register (31 = sp here; the caller deals with xzr): hr reads it, hw is for a new value that
// fills all 64 bits, hrw updates it in place.  The register stays valid, and is not evicted, until the instruction is done.
static int rc_slot(JC *j, int g, bool load) {
    int k = j->rc_slot_of[g] - 1;
    if (k < 0) { k = rc_alloc(j, g); if (load) mov_rm(j, RC_HOST[k], R15, rc_mem(g), 1); else if (k >= g_npin) j->fresh |= (uint8_t)(1u << k); }
    j->rc_age[k] = ++j->rc_tick; j->rc_pin |= (uint8_t)(1u << k);
    return k;
}
static int hr(JC *j, int g) { return RC_HOST[rc_slot(j, g, true)]; }
static int hw(JC *j, int g) { int k = rc_slot(j, g, false); if (k >= g_npin) j->rc_dirty |= (uint8_t)(1u << k); return RC_HOST[k]; }
static int hrw(JC *j, int g) { int k = rc_slot(j, g, true); if (k >= g_npin) j->rc_dirty |= (uint8_t)(1u << k); return RC_HOST[k]; }
static void ld_x(JC *j, int dst, int r, int w) {            // xr (31 = xzr)
    if (r == 31) { mov_ri_nf(j, dst, 0); return; }
    ld_g(j, dst, r, w);
}
static void ld_xsp(JC *j, int dst, int r, int w) { ld_g(j, dst, r, w); }
static void st_x(JC *j, int r, int src) { if (r != 31) st_g(j, r, src); }
static void st_xsp(JC *j, int r, int src) { st_g(j, r, src); }
static void ld_imm_pc(JC *j, int dst, uint64_t v) { mov_ri_nf(j, dst, v); }

// ------------------------------------------------------------------------------------------------ blocks, exits, chaining

static inline unsigned jit_hash(uint64_t pc) { return (unsigned)((pc >> 2) * 0x9E3779B97F4A7C15ull >> (64 - JIT_TAB_BITS)); }

// Authoritative lookup; safe against a concurrent insert (an entry is published by writing its code before its pc).
static inline JEnt *jit_probe(uint64_t pc) {
    unsigned h = jit_hash(pc);
    for (int k = 0; k < 64; k++) {
        JEnt *e = &g_jit_tab[(h + k) & ((1 << JIT_TAB_BITS) - 1)];
        uint64_t p = __atomic_load_n(&e->pc, __ATOMIC_ACQUIRE);
        if (p == pc) return e;
        if (p == 0) return NULL;
    }
    return NULL;
}

static uint8_t *jit_find_code(uint64_t pc) { JEnt *e = jit_probe(pc); return e ? e->code : NULL; }

// exit to the dispatcher with c->pc = target (the sequence the chain patching replaces by a jump to the block, once it exists)
static void emit_exit_to(JC *j, uint64_t target) {
    mov_ri_nf(j, RAX, target);
    mov_mr(j, R15, GPC, RAX, 1);
    jmp_to(j, g_jit_exit_stub);
}

// pending patch sites by target pc
static void jit_pend_add(uint64_t target, uint8_t *site) {
    JPend *n = jpool_alloc(0, sizeof *n);
    unsigned h = (unsigned)((target >> 2) * 0x9E3779B97F4A7C15ull >> (64 - JIT_PEND_BITS));
    n->target = target; n->site = site; n->next = g_jit_pend[h]; g_jit_pend[h] = n;
}

// a direct jump to the translation of `target`, or a patchable one that for now leaves through the dispatcher
static void emit_goto(JC *j, uint64_t target) {
    rc_flush(j);
    if (target - g_stub_base < g_stub_size) { emit_exit_to(j, target); return; }  // imports are called by the interpreter
    uint8_t *code = jit_find_code(target);
    if (code) { jmp_to(j, code); return; }
    while (((uintptr_t)(j->p + 1) & 3) != 0) e1(j, 0x90);       // the rel32 must not straddle a 4-byte boundary: patching it is then atomic
    uint8_t *site = j->p;
    e1(j, 0xE9); e4(j, 0);                                         // jmp .+0, patched to jmp <block>
    jit_pend_add(target, site);
    emit_exit_to(j, target);
}
// ---- the return-address stack.  A BL is a host `push <return pc>; call <callee>` and a RET that finds its guest return pc on top of that stack is a host `ret 8`: the
// processor's own return predictor then does what the per-site cells below get wrong for functions called from many places.  What the stack holds is only a prediction:
// after the call comes the code that goes on at the return pc, so the host `ret` lands at the right place whenever the guest pc matches; any mismatch (longjmp, an exception,
// a callee entered by BLR or from the dispatcher) takes the ordinary path through the cells.  Every way out of translated code resets rsp to the value it had at the entry.
static uintptr_t g_jit_align = 16;   // REBAR_JIT_ALIGN=<16|32|64>: where block code starts (the word before it is the guest pc)
static bool g_nodmbopt;   // REBAR_JIT_NODMBOPT=1: every DMB / DSB is an mfence again (to compare)
static bool g_jit_ras = true;   // REBAR_JIT_RAS=0 turns it off
static void emit_call_ras(JC *j, uint64_t target, uint64_t retpc) {
    rc_flush(j);
    op_rm(j, 1, 0, 0x3B, RSP, R15, GHLIM);                    // cmp rsp, [hlim]: too deep, start again from the base
    Fix ok = jcc_fwd(j, CC_A);
    mov_rm(j, RSP, R15, GHSP, 1);
    fix_here(j, ok);
    mov_ri_nf(j, RAX, retpc); e1(j, 0x50);                    // push rax
    uint8_t *code = jit_find_code(target);
    while (((uintptr_t)(j->p + 1) & 3) != 0) e1(j, 0x90);     // the rel32 must not straddle a 4-byte boundary: patching it is then atomic
    uint8_t *site = j->p;
    e1(j, 0xE8); j->cs_rel = j->p;
    if (code) { int32_t d = (int32_t)(code - (j->p + 4)); e4(j, (uint32_t)d); j->cs_rel = NULL; }
    else { e4(j, 0); j->cs_target = target; jit_pend_add(target, site); }
    emit_goto(j, retpc);                                      // (the callee returns here)
}
// BLR: the target is in rax and x30 holds the return pc.  Like emit_call_ras, but the callee comes from the site's cell (a miss pushes the continuation by hand and goes to the shared stub).
static void emit_blr_ras(JC *j, uint64_t retpc) {
    rc_flush(j);
    if (g_jit_count) { mov_ri_nf(j, R9, (uint64_t)&g_jr_indk[1]); e1(j, 0x49); e1(j, 0xFF); e1(j, 0x01); }
    op_rm(j, 1, 0, 0x3B, RSP, R15, GHLIM);
    Fix ok = jcc_fwd(j, CC_A);
    mov_rm(j, RSP, R15, GHSP, 1);
    fix_here(j, ok);
    mov_ri_nf(j, RDX, retpc); e1(j, 0x52);                      // push rdx
    if (g_jit_cell_top >= g_jit_cell_end) { emit_goto(j, retpc); return; }   // (cannot happen in practice; keep the block well formed)
    uint8_t *cell = g_jit_cell_top; g_jit_cell_top += 8; *(uint64_t *)cell = (uint64_t)g_jit_dummy;
    rex(j, 1, RSI, 0, 0); e1(j, 0x8B); e1(j, 0x05 | ((RSI & 7) << 3)); uint8_t *d1 = j->p; e4(j, 0);   // mov rsi, [rip+cell]
    op_rm(j, 1, 0, 0x3B, RAX, RSI, -8);                                                              // cmp rax, [rsi-8]
    e1(j, 0x0F); e1(j, 0x85); j->bm_rel = j->p; e4(j, 0);                                            // jne <stub after the block>
    e1(j, 0xFF); e1(j, 0xD6);                                                                        // call rsi
    j->bm_cell = cell; j->bm_cont = j->p;
    int32_t r1 = (int32_t)(cell - (d1 + 4)); memcpy(d1, &r1, 4);
    emit_goto(j, retpc);                                                                             // (the callee returns here)
}
static void emit_blr_stub(JC *j) {   // after the block: the cell missed
    if (!j->bm_rel) return;
    int32_t d = (int32_t)(j->p - (j->bm_rel + 4)); memcpy(j->bm_rel, &d, 4);
    if (g_jit_count) { mov_ri_nf(j, R9, (uint64_t)&g_jr_missk[1]); e1(j, 0x49); e1(j, 0xFF); e1(j, 0x01); }
    mov_mr(j, R15, GPC, RAX, 1);
    rex(j, 1, R8, 0, 0); e1(j, 0x8D); e1(j, 0x05 | ((R8 & 7) << 3)); uint8_t *d2 = j->p; e4(j, 0);   // lea r8, [rip+cell]
    int32_t r2 = (int32_t)(j->bm_cell - (d2 + 4)); memcpy(d2, &r2, 4);
    rex(j, 1, RDX, 0, 0); e1(j, 0x8D); e1(j, 0x05 | ((RDX & 7) << 3)); uint8_t *d3 = j->p; e4(j, 0);   // lea rdx, [rip+continuation]
    int32_t r3 = (int32_t)(j->bm_cont - (d3 + 4)); memcpy(d3, &r3, 4);
    e1(j, 0x52);                                                                                      // push rdx: what the call would have pushed
    jmp_to(j, g_jit_ind_stub);
}
// An indirect branch (BR, BLR, RET) to the guest address in host register src.  Every site has its own cell holding the block it last went to: a
// hit costs a load from the (hot) cell, a compare with the guest pc stored before the block, and a jump that the processor predicts per site.
// A miss stores the target in c->pc and goes to the shared stub with r8 = the cell, which looks the target up in the global cache and refills the cell.
static void emit_indirect(JC *j, int src) {
    rc_flush(j);
    if (src != RAX) mov_rr(j, RAX, src, 1);
    if (g_jit_cell_top >= g_jit_cell_end) { mov_mr(j, R15, GPC, RAX, 1); mov_ri_nf(j, R8, (uint64_t)g_jit_sink); jmp_to(j, g_jit_ind_stub); return; }   // (out of cells: the shared stub alone, refilling a cell nobody reads)
    uint8_t *cell = g_jit_cell_top; g_jit_cell_top += 8; *(uint64_t *)cell = (uint64_t)g_jit_dummy;
    if (g_jit_count) { mov_ri_nf(j, R9, (uint64_t)&g_jr_indk[g_ind_kind]); e1(j, 0x49); e1(j, 0xFF); e1(j, 0x01); }
    rex(j, 1, RSI, 0, 0); e1(j, 0x8B); e1(j, 0x05 | ((RSI & 7) << 3)); uint8_t *d1 = j->p; e4(j, 0);   // mov rsi, [rip+cell]
    op_rm(j, 1, 0, 0x3B, RAX, RSI, -8);                                                              // cmp rax, [rsi-8]
    Fix miss = jcc_fwd(j, CC_NE);
    jmp_reg(j, RSI);
    fix_here(j, miss);
    if (g_jit_count) { mov_ri_nf(j, R9, (uint64_t)&g_jr_missk[g_ind_kind]); e1(j, 0x49); e1(j, 0xFF); e1(j, 0x01); }
    mov_mr(j, R15, GPC, RAX, 1);
    rex(j, 1, R8, 0, 0); e1(j, 0x8D); e1(j, 0x05 | ((R8 & 7) << 3)); uint8_t *d2 = j->p; e4(j, 0);   // lea r8, [rip+cell]
    jmp_to(j, g_jit_ind_stub);
    int32_t r1 = (int32_t)(cell - (d1 + 4)), r2 = (int32_t)(cell - (d2 + 4));
    memcpy(d1, &r1, 4); memcpy(d2, &r2, 4);
}

// A conditional branch does not end the block: the not-taken path runs on, with the registers still in the host registers, and the taken
// path leaves through a stub that stores the registers that were written and goes to the target.  The stubs follow the block's own code.
static void flags_from_x86(JC *j, int kind);
static bool g_flagpoison;   // REBAR_JIT_FLAGPOISON=1: a skipped flag store writes 0xFE into the four bytes and a translated read of them traps if it finds one (to find a wrong 'dead' answer)
static void flags_poison(JC *j) { mov_m8_imm(j, R15, GN, 0xFE); mov_m8_imm(j, R15, GZ, 0xFE); mov_m8_imm(j, R15, GC, 0xFE); mov_m8_imm(j, R15, GV, 0xFE); }
static bool side_exit_room(JC *j) { return j->nsx < 15; }
static void side_exit(JC *j, int cc, uint64_t target) {
    e1(j, 0x0F); e1(j, 0x80 + cc);
    int k = j->nsx++;
    j->sx[k].at = j->p; j->sx[k].target = target; j->sx[k].dirty = j->rc_dirty; memcpy(j->sx[k].reg_of, j->rc_reg_of, RC_N);
    e4(j, 0);
}
static void emit_side_exits(JC *j) {
    rc_clear(j);
    for (int k = 0; k < j->nsx; k++) {
        int32_t d = (int32_t)(j->p - (j->sx[k].at + 4)); memcpy(j->sx[k].at, &d, 4);
        if (j->sx[k].fkind == 0x10) flags_poison(j);
        else if (j->sx[k].fkind) flags_from_x86(j, j->sx[k].fkind);   // (the jump came straight from the instruction that set the flags)
        for (int n = 0; n < RC_N; n++) if (j->sx[k].dirty & (1u << n)) mov_mr(j, R15, rc_mem(j->sx[k].reg_of[n] - 1), RC_HOST[n], 1);
        emit_goto(j, j->sx[k].target);
    }
    j->nsx = 0;
}   // c->pc already holds the target

// ------------------------------------------------------------------------------------------------ flags and conditions

// materialize N Z C V from the x86 flags just produced; kind 1: subtraction (ARM C = not borrow), 2: addition, 3: logical
static void flags_from_x86(JC *j, int kind) {
    setcc_m(j, CC_S, R15, GN); setcc_m(j, CC_E, R15, GZ);
    if (kind == 1) setcc_m(j, CC_AE, R15, GC); else if (kind == 2) setcc_m(j, CC_B, R15, GC); else mov_m8_imm(j, R15, GC, 0);
    if (kind == 3) mov_m8_imm(j, R15, GV, 0); else setcc_m(j, CC_O, R15, GV);
    j->flagkind = kind; j->kind_set = true;
}

// ---- flags that nobody reads.  An instruction that sets N Z C V need not store them when every path from there overwrites them before a
// reader (a conditional branch or select, ADC, CCMP, a system register read, a call, a return or an indirect branch: LLVM's outlined functions
// take flags in and hand them back, so the calling convention is no excuse, anything not recognised).  The scan reads the guest code that follows.
static int fl_class(uint32_t i) {   // 0 neither reads nor writes the flags, 1 reads them (or is not recognised), 2 writes all four
    switch (B(i, 28, 25)) {
    case 0x8: case 0x9:
        if (B(i, 28, 23) == 0x22) return B(i, 29, 29) ? 2 : 0;
        if (B(i, 28, 23) == 0x24) return B(i, 30, 29) == 3 ? 2 : 0;
        if (B(i, 28, 23) == 0x25 || B(i, 28, 23) == 0x26 || B(i, 28, 23) == 0x27 || B(i, 28, 24) == 0x10) return 0;
        return 1;
    case 0x4: case 0x6: case 0xc: case 0xe: return 0;   // loads and stores
    case 0x5: case 0xd:
        if (B(i, 28, 24) == 0x0b) return B(i, 29, 29) ? 2 : 0;
        if (B(i, 28, 24) == 0x0a) return B(i, 30, 29) == 3 ? 2 : 0;
        if (B(i, 30, 21) == 0x0d6 || B(i, 30, 21) == 0x2d6 || B(i, 28, 24) == 0x1b) return 0;
        return 1;
    case 0x7: case 0xf:
        if ((i & 0xff20fc07) == 0x1e202000) return 2;   // FCMP / FCMPE
        if ((i & 0xff200c00) == 0x1e200400 || (i & 0xff200c00) == 0x1e200c00) return 1;   // FCCMP, FCSEL
        return 0;
    default: return 1;
    }
}
static int g_fl_budget = 96;   // REBAR_JIT_FLBUDGET=<n>: how many guest instructions a flag-liveness scan may look at
static bool g_fl_callscan = true;   // REBAR_JIT_NOCALLSCAN=1: a call keeps the flags live (to bisect)
static uint64_t g_fl_live_hist[4096]; static uint32_t g_fl_live_enc[4096]; static bool g_fl_dbg;   // REBAR_JIT_FLAGDBG: which instructions made a scan answer 'live'
static void fl_dbg_dump(void) { for (int k = 0; k < 4096; k++) if (g_fl_live_hist[k] > 20) fprintf(stderr, "flaglive %08x %llu\n", g_fl_live_enc[k], (unsigned long long)g_fl_live_hist[k]); }
static bool fl_dead_from(uint64_t pc, int *budget) {   // true when the flags are overwritten (or irrelevant) on every path from the instruction at pc
    bool fell = false;   // arrived by running on from the previous instruction (the next page may not be mapped)
    while (*budget > 0) {
        if (fell && (pc & 4095) == 0) return false;
        uint32_t i = *(uint32_t *)pc;
        (*budget)--;
        if (B(i, 30, 26) == 0x5) {   // B / BL
            uint64_t t = pc + (uint64_t)(sext(B(i, 25, 0), 26) * 4);
            // BL: the flags are dead when the callee overwrites them before anything reads them; a return counts as a reader (outlined functions hand
            // flags back: `cmp x10, #0x3f; ret`), so a callee that returns without writing them keeps them live.  What the code after the call sees is what the callee left
            if (B(i, 31, 31)) return g_fl_callscan && fl_dead_from(t, budget);
            pc = t; fell = false; continue;
        }
        if ((i & 0xfe1ffc1f) == 0xd61f0000) return false;   // BR / BLR / RET: the same
        if ((i & 0xffe0001f) == 0xd4200000) return true;   // BRK: nothing runs after it
        if ((i & 0xfffff01f) == 0xd503201f || (i & 0xfffff09f) == 0xd5033f9f) { pc += 4; fell = true; continue; }   // hints (NOP...), DMB / DSB / ISB
        if (B(i, 30, 25) == 0x1a || B(i, 30, 25) == 0x1b) {   // CBZ CBNZ TBZ TBNZ: both ways
            uint64_t t = pc + (uint64_t)(sext(B(i, 30, 25) == 0x1a ? B(i, 23, 5) : B(i, 18, 5), B(i, 30, 25) == 0x1a ? 19 : 14) * 4);
            if (!fl_dead_from(t, budget)) return false;
            pc += 4; fell = true; continue;
        }
        int c = fl_class(i);
        if (c == 2) return true;
        if (c == 1) { if (g_fl_dbg) { unsigned k = (i >> 20) & 4095; g_fl_live_hist[k]++; g_fl_live_enc[k] = i; } return false; }
        pc += 4; fell = true;
    }
    return false;
}
// the x86 condition can stand for the ARM one for flags of this kind (see cond_cc)
static bool fl_fusable(int kind, int cond) { return !(kind == 2 && (cond >> 1) == 4); }
// may the instruction being translated leave the flags in the x86 flags only?  Only when the next instruction is a conditional branch or
// select in this same block (it reads the x86 flags) and the flags are dead after it, or when they are dead right away.
static bool g_noflagskip;   // REBAR_JIT_NOFLAGSKIP=1: store the flags after every instruction that sets them (to bisect)
static int g_skipmode;   // REBAR_JIT_FLAGSKIP_MODE=1/2/3: only the b.cond-consumer / csel-consumer / no-consumer kind of skip (to bisect)
static uint64_t g_noskip_lo, g_noskip_hi;   // REBAR_JIT_NOFLAGSKIP_RANGE=lo-hi (hex): never skip the flags of instructions in that range
static uint64_t g_flagskip_lo, g_flagskip_hi = ~0ull;   // REBAR_JIT_FLAGSKIP=lo-hi (hex): skip flags only for instructions in that range (to bisect)
static bool flags_skippable(JC *j, int kind) {
    if (g_noflagskip || j->pc < g_flagskip_lo || j->pc >= g_flagskip_hi || (j->pc >= g_noskip_lo && j->pc < g_noskip_hi)) return false;
    uint64_t p = j->pc + 4;
    if ((p & 4095) == 0) return false;
    uint32_t n = *(uint32_t *)p;
    int budget = g_fl_budget;
    if ((n & 0xff000010) == 0x54000000) {   // B.cond
        if (g_skipmode && g_skipmode != 1) return false;
        if (j->insn_left < 1 || !fl_fusable(kind, B(n, 3, 0) & 15)) return false;
        uint64_t t = p + (uint64_t)(sext(B(n, 23, 5), 19) * 4);
        return fl_dead_from(t, &budget) && fl_dead_from(p + 4, &budget);
    }
    if (B(n, 29, 21) == 0xd4 && !B(n, 11, 11)) {   // CSEL / CSINC / CSINV / CSNEG
        if (g_skipmode && g_skipmode != 2) return false;
        if (j->insn_left < 1 || !fl_fusable(kind, B(n, 15, 12))) return false;
        return fl_dead_from(p + 4, &budget);
    }
    if (g_skipmode && g_skipmode != 3) return false;
    return fl_dead_from(p, &budget);
}
static bool g_noflagdefer;   // REBAR_JIT_NOFLAGDEFER=1: store the flags before the branch as before (to bisect)
// The instruction that sets the flags is followed by a B.cond whose two ways need the flags differently (one reads them, the other overwrites them first): the four
// stores then happen only on the way that needs them, right after the conditional jump (the fall-through) or at the start of the exit stub (the taken way), where
// the x86 flags are still the ones the instruction produced.
static bool flags_deferrable(JC *j, int kind) {
    if (g_noflagdefer || g_noflagskip || j->pc < g_flagskip_lo || j->pc >= g_flagskip_hi || (j->pc >= g_noskip_lo && j->pc < g_noskip_hi)) return false;
    uint64_t p = j->pc + 4;
    if ((p & 4095) == 0) return false;
    uint32_t n = *(uint32_t *)p;
    if ((n & 0xff000010) != 0x54000000) return false;
    int cond = B(n, 3, 0);
    if (j->insn_left < 1 || !side_exit_room(j) || (cond & 14) == 14 || !fl_fusable(kind, cond)) return false;
    if (kind == 3 && (cond == 2 || cond == 3 || cond == 6 || cond == 7 || cond == 8 || cond == 9)) return false;   // (a logical result has C and V clear: those conditions are constant)
    int budget = g_fl_budget;
    uint64_t t = p + (uint64_t)(sext(B(n, 23, 5), 19) * 4);
    bool dead_taken = fl_dead_from(t, &budget), dead_fall = fl_dead_from(p + 4, &budget);
    j->fl_pend_pc = p; j->fl_pend_kind = kind; j->fl_pend_taken = !dead_taken; j->fl_pend_fall = !dead_fall;
    j->flagkind = kind; j->kind_set = true;
    return true;
}
static void flags_out(JC *j, int kind) {
    if (flags_skippable(j, kind)) {
        if (g_flagpoison) flags_poison(j);
        j->flagkind = kind; j->kind_set = true; return;
    }
    if (flags_deferrable(j, kind)) return;
    flags_from_x86(j, kind);
}

// x86 condition (valid for the instructions emitted so far in this call) equivalent to ARM condition `cond`
static int cond_cc(JC *j, int cond) {
    if ((cond & 14) == 14) return CC_ALWAYS;
    if (j->flagkind) {  // the x86 flags still hold what the last instruction produced
        static const int sub_cc[14] = {CC_E, CC_NE, CC_AE, CC_B, CC_S, CC_NS, CC_O, CC_NO, CC_A, CC_BE, CC_GE, CC_L, CC_G, CC_LE};
        if (j->flagkind == 1) return sub_cc[cond];
        if (j->flagkind == 2) {  // C is the carry itself: HI and LS have no single x86 condition
            static const int add_cc[14] = {CC_E, CC_NE, CC_B, CC_AE, CC_S, CC_NS, CC_O, CC_NO, -1, -1, CC_GE, CC_L, CC_G, CC_LE};
            if (add_cc[cond] >= 0) return add_cc[cond];
        } else {         // logical: C and V are clear
            static const int log_cc[14] = {CC_E, CC_NE, CC_NEVER, CC_ALWAYS, CC_S, CC_NS, CC_NEVER, CC_ALWAYS, CC_NEVER, CC_ALWAYS, CC_GE, CC_L, CC_G, CC_LE};
            return log_cc[cond];
        }
    }
    if (g_flagpoison) { static const int32_t offs[4] = {GN, GZ, GC, GV}; for (int q = 0; q < 4; q++) { cmp_m8_imm(j, R15, offs[q], 1); Fix ok = jcc_fwd(j, CC_BE); e1(j, 0x0F); e1(j, 0x0B); fix_here(j, ok); } }   // ud2 if a flag byte is poisoned
    switch (cond) {
    case 0: cmp_m8_imm(j, R15, GZ, 0); return CC_NE;
    case 1: cmp_m8_imm(j, R15, GZ, 0); return CC_E;
    case 2: cmp_m8_imm(j, R15, GC, 0); return CC_NE;
    case 3: cmp_m8_imm(j, R15, GC, 0); return CC_E;
    case 4: cmp_m8_imm(j, R15, GN, 0); return CC_NE;
    case 5: cmp_m8_imm(j, R15, GN, 0); return CC_E;
    case 6: cmp_m8_imm(j, R15, GV, 0); return CC_NE;
    case 7: cmp_m8_imm(j, R15, GV, 0); return CC_E;
    case 8: case 9:   // HI: C and not Z  <=>  C > Z as bytes
        movzx8_rm(j, R9, R15, GC); alu_rm(j, 0x38, R9, R15, GZ, 0);  // cmp r9b, [Z]  (8-bit cmp: 3A /r)
        return cond == 8 ? CC_A : CC_BE;
    case 10: case 11: // GE: N == V
        movzx8_rm(j, R9, R15, GN); alu_rm(j, 0x38, R9, R15, GV, 0);
        return cond == 10 ? CC_E : CC_NE;
    default:          // GT: not Z and N == V
        movzx8_rm(j, R9, R15, GN); alu_rm(j, 0x30, R9, R15, GV, 0); alu_rm(j, 0x08, R9, R15, GZ, 0);  // xor r9b,[V]; or r9b,[Z]
        return cond == 12 ? CC_E : CC_NE;
    }
}

// ------------------------------------------------------------------------------------------------ helpers called by generated code

// REBAR_HELPERSTATS=<file>: how often each kind of instruction runs through a helper (the dynamic counterpart of REBAR_JITSTATS, which counts code sites): every encoding
// is keyed by bits 31:21 and 15:10 and the first one seen is kept; <file>.<pid> gets the top rows every two seconds (decode with `.inst` and llvm-objdump).
#define HS_SLOTS 4096
static struct { uint32_t key, enc; uint64_t n; } g_hs[HS_SLOTS];
static bool g_helperstats;
static inline void helperstats_note(uint32_t i) {
    uint32_t key = ((i >> 21) << 6 | ((i >> 10) & 0x3f)) | 0x80000000u, h = (key * 2654435761u) >> 20;
    for (int p = 0; p < 64; p++, h = (h + 1) & (HS_SLOTS - 1)) {
        uint32_t k = __atomic_load_n(&g_hs[h].key, __ATOMIC_RELAXED);
        if (k == 0) { uint32_t z = 0; if (__atomic_compare_exchange_n(&g_hs[h].key, &z, key, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { g_hs[h].enc = i; k = key; } else k = __atomic_load_n(&g_hs[h].key, __ATOMIC_RELAXED); }
        if (k == key) { __atomic_fetch_add(&g_hs[h].n, 1, __ATOMIC_RELAXED); return; }
    }
}
static void *helperstats_thread(void *arg) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s.%d", (const char *)arg, (int)getpid());
    for (;;) {
        sleep(2);
        FILE *f = fopen(path, "w"); if (!f) continue;
        static uint32_t order[HS_SLOTS]; int n = 0;
        for (int k = 0; k < HS_SLOTS; k++) if (g_hs[k].key) order[n++] = (uint32_t)k;
        for (int a = 0; a < n && a < 80; a++) { int b = a; for (int k = a + 1; k < n; k++) if (g_hs[order[k]].n > g_hs[order[b]].n) b = k; uint32_t t = order[a]; order[a] = order[b]; order[b] = t; }
        for (int a = 0; a < n && a < 80; a++) fprintf(f, "%08x %llu\n", g_hs[order[a]].enc, (unsigned long long)g_hs[order[a]].n);
        fclose(f);
    }
    return NULL;
}
static void jit_h_slow(CPU *c, uint32_t i) { if (g_helperstats) helperstats_note(i); if (!exec_slow(c, i)) die(c, "unimplemented instruction", i); }
static void jit_h_simd(CPU *c, uint32_t i) { if (g_helperstats) helperstats_note(i); if (!g_no_simd_fast && simd_fast(c, i)) return; if (!exec_slow(c, i)) die(c, "unimplemented instruction", i); }
static void jit_h_vls(CPU *c, uint32_t i) {   // SIMD&FP loads and stores: the fast function for the class the interpreter would pick by the top byte
    if (g_helperstats) helperstats_note(i);
    if (!g_no_simd_fast) {
        switch (i >> 24) {
        case 0x3c: case 0x3d: case 0x7c: case 0x7d: case 0xbc: case 0xbd: case 0xfc: case 0xfd:
            if (!(B(i, 21, 21) && B(i, 11, 10) == 0) && vmem_fast(c, i)) return; break;
        case 0x2c: case 0x2d: case 0x6c: case 0x6d: case 0xac: case 0xad: if (vpair_fast(c, i)) return; break;
        case 0x0c: case 0x4c: if (vmulti_fast(c, i)) return; break;
        case 0x0d: case 0x4d: if (vld1r_fast(c, i) || vsingle_fast(c, i)) return; break;
        default: break;
        }
    }
    if (!exec_slow(c, i)) die(c, "unimplemented instruction", i);
}
static void emit_helper(JC *j, void *fn, uint32_t i) {  // fn(c, insn) with c->pc set, for one instruction
    rc_flush(j); rc_sync_out(j);
    mov_ri_nf(j, RAX, j->pc); mov_mr(j, R15, GPC, RAX, 1);
    mov_rr(j, RDI, R15, 1); mov_ri_nf(j, RSI, i);
    mov_ri_nf(j, RAX, (uint64_t)fn); call_reg(j, RAX);
    rc_clear(j); rc_sync_in(j);   // the helper may have changed any register
    g_jit_nfallback++;
    { uint32_t k = ((i >> 21) << 6 | ((i >> 10) & 63)) & 0xfff; g_jit_fb_key[k] = i; g_jit_fb_n[k]++;
      if (g_jit_count) { mov_ri_nf(j, R9, (uint64_t)&g_jit_fbdyn[k]); e1(j, 0x49); e1(j, 0xFF); e1(j, 0x01); /* inc qword [r9] */ } }
}

typedef enum { JI_NO = 0, JI_OK = 1, JI_END = 2 } JI;
#define W32 0

static void and_imm(JC *j, int reg, uint64_t m, int w) {  // reg &= m
    if (!w) { alu_ri(j, 4, reg, (int32_t)(uint32_t)m, 0); return; }
    if ((int64_t)m == (int64_t)(int32_t)m) alu_ri(j, 4, reg, (int32_t)m, 1);
    else { mov_ri_nf(j, R8, m); alu_rr(j, 0x21, reg, R8, 1); }
}
static void or_imm(JC *j, int reg, uint64_t m, int w) {
    if (!w) { alu_ri(j, 1, reg, (int32_t)(uint32_t)m, 0); return; }
    if ((int64_t)m == (int64_t)(int32_t)m) alu_ri(j, 1, reg, (int32_t)m, 1);
    else { mov_ri_nf(j, R8, m); alu_rr(j, 0x09, reg, R8, 1); }
}
static void xor_imm(JC *j, int reg, uint64_t m, int w) {
    if (!w) { alu_ri(j, 6, reg, (int32_t)(uint32_t)m, 0); return; }
    if ((int64_t)m == (int64_t)(int32_t)m) alu_ri(j, 6, reg, (int32_t)m, 1);
    else { mov_ri_nf(j, R8, m); alu_rr(j, 0x31, reg, R8, 1); }
}

// shift reg by an immediate amount (type 0 lsl, 1 lsr, 2 asr, 3 ror), as the ARM shifted-register operand
static void shift_operand(JC *j, int reg, int type, int amt, int w) {
    amt &= (w ? 64 : 32) - 1;
    static const int ext[4] = {4, 5, 7, 1};
    shift_ri(j, ext[type], reg, amt, w);
}
// extend a register as the ARM extended-register operand (option 0..7), then shift left
static void extend_operand(JC *j, int reg, int opt, int sh) {
    switch (opt) {
    case 0: movzx_rr(j, reg, reg, 1); break;
    case 1: movzx_rr(j, reg, reg, 2); break;
    case 2: movzx_rr(j, reg, reg, 4); break;
    case 4: movsx_rr(j, reg, reg, 1, 1); break;
    case 5: movsx_rr(j, reg, reg, 2, 1); break;
    case 6: movsx_rr(j, reg, reg, 4, 1); break;
    default: break;
    }
    shift_ri(j, 4, reg, sh, 1);
}

static JI emit_addsub_imm(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op = B(i, 30, 30), S = B(i, 29, 29), rn = B(i, 9, 5), rd = B(i, 4, 0);
    int32_t v = (int32_t)(B(i, 21, 10) << (B(i, 22, 22) ? 12 : 0));
    int a = hr(j, rn);                                          // (register 31 is the stack pointer here)
    if (S && rd == 31) {                                        // CMP / CMN with an immediate
        if (op) { alu_ri(j, 7, a, v, sf); flags_out(j, 1); }
        else { mov_rr(j, RAX, a, sf); alu_ri(j, 0, RAX, v, sf); flags_out(j, 2); }
        return JI_OK;
    }
    int d = hw(j, rd);                                          // (the stack pointer when not setting flags, xzr is handled above)
    if (!S && d != a) { lea_rm(j, d, a, op ? -v : v, sf); return JI_OK; }
    if (d != a) mov_rr(j, d, a, sf);
    alu_ri(j, op ? 5 : 0, d, v, sf);
    if (S) flags_out(j, op ? 1 : 2);
    return JI_OK;
}
static JI emit_addsub_reg(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op = B(i, 30, 30), S = B(i, 29, 29), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (B(i, 21, 21)) {  // extended register
        if (B(i, 12, 10) > 4) return JI_NO;
        ld_xsp(j, RAX, rn, sf);
        ld_x(j, RCX, rm, 1);
        extend_operand(j, RCX, B(i, 15, 13), B(i, 12, 10));
        alu_rr(j, op ? 0x29 : 0x01, RAX, RCX, sf);
        if (S) { flags_out(j, op ? 1 : 2); st_x(j, rd, RAX); } else st_xsp(j, rd, RAX);
        return JI_OK;
    }
    int type = B(i, 23, 22), amt = B(i, 15, 10);
    if (type == 3 || (!sf && amt >= 32)) return JI_NO;
    if (rn == 31 || rm == 31) {   // zero as an operand (NEG, CMP against zero, ...): the general sequence
        ld_x(j, RAX, rn, sf); ld_x(j, RCX, rm, sf);
        shift_operand(j, RCX, type, amt, sf);
        alu_rr(j, op ? 0x29 : 0x01, RAX, RCX, sf);
        if (S) flags_out(j, op ? 1 : 2);
        st_x(j, rd, RAX);
        return JI_OK;
    }
    if (!S && rd == 31) return JI_OK;   // (the result goes nowhere)
    int a = hr(j, rn), b = hr(j, rm);
    if (amt) { mov_rr(j, RCX, b, sf); shift_operand(j, RCX, type, amt, sf); b = RCX; }
    if (S && rd == 31) {                // CMP / CMN
        if (op) { alu_rr(j, 0x39, a, b, sf); flags_out(j, 1); }
        else { mov_rr(j, RAX, a, sf); alu_rr(j, 0x01, RAX, b, sf); flags_out(j, 2); }
        return JI_OK;
    }
    int d = hw(j, rd);
    if (d == a) { alu_rr(j, op ? 0x29 : 0x01, d, b, sf); if (S) flags_out(j, op ? 1 : 2); return JI_OK; }
    if (d == b) {
        if (!op) { alu_rr(j, 0x01, d, a, sf); if (S) flags_out(j, 2); return JI_OK; }
        mov_rr(j, RAX, a, sf); alu_rr(j, 0x29, RAX, b, sf); if (S) flags_out(j, 1); mov_rr(j, d, RAX, 1);   // d = a - d
        return JI_OK;
    }
    mov_rr(j, d, a, sf);
    alu_rr(j, op ? 0x29 : 0x01, d, b, sf);
    if (S) flags_out(j, op ? 1 : 2);
    return JI_OK;
}
static JI emit_logic_reg(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), opc = B(i, 30, 29), N = B(i, 21, 21), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    int type = B(i, 23, 22), amt = B(i, 15, 10);
    if (!sf && amt >= 32) return JI_NO;
    if (opc == 1 && rn == 31 && !N && !amt) {   // MOV
        if (rd == 31) return JI_OK;
        if (rm == 31) { mov_ri_nf(j, hw(j, rd), 0); return JI_OK; }
        int b = hr(j, rm), d = hw(j, rd);
        if (d != b || !sf) mov_rr(j, d, b, sf);   // (a 32-bit move also clears the upper half)
        return JI_OK;
    }
    if (rn == 31 || rm == 31) {   // zero as an operand
        ld_x(j, RAX, rn, sf); ld_x(j, RCX, rm, sf);
        shift_operand(j, RCX, type, amt, sf);
        if (N) unary(j, 2, RCX, sf);
        alu_rr(j, opc == 0 || opc == 3 ? 0x21 : opc == 1 ? 0x09 : 0x31, RAX, RCX, sf);
        if (opc == 3) flags_out(j, 3);
        st_x(j, rd, RAX);
        return JI_OK;
    }
    int a = hr(j, rn), b = hr(j, rm);
    if (amt || N) { mov_rr(j, RCX, b, sf); shift_operand(j, RCX, type, amt, sf); if (N) unary(j, 2, RCX, sf); b = RCX; }
    uint8_t aop = opc == 0 || opc == 3 ? 0x21 : opc == 1 ? 0x09 : 0x31;
    if (opc == 3 && rd == 31) { alu_rr(j, 0x85, a, b, sf); flags_out(j, 3); return JI_OK; }   // TST
    if (opc != 3 && rd == 31) return JI_OK;
    int d = hw(j, rd);
    if (d == a) alu_rr(j, aop, d, b, sf);
    else if (d == b) alu_rr(j, aop, d, a, sf);   // (and, or and xor commute)
    else { mov_rr(j, d, a, sf); alu_rr(j, aop, d, b, sf); }
    if (opc == 3) flags_out(j, 3);
    return JI_OK;
}
static JI emit_logic_imm(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), opc = B(i, 30, 29), rn = B(i, 9, 5), rd = B(i, 4, 0);
    uint64_t m;
    if (!decode_bitmask(B(i, 22, 22), B(i, 15, 10), B(i, 21, 16), sf, &m)) return JI_NO;
    if (rn == 31) {   // zero as the operand: the result is a constant
        uint64_t v = opc == 0 || opc == 3 ? 0 : m;
        if (opc == 3) { mov_ri_nf(j, RAX, 0); alu_rr(j, 0x85, RAX, RAX, 1); flags_out(j, 3); }
        if (rd == 31 && opc == 3) return JI_OK;
        mov_ri_nf(j, hw(j, rd), v);
        return JI_OK;
    }
    int a = hr(j, rn);
    if (opc == 3 && rd == 31) {   // TST
        if (!sf || (int64_t)m == (int64_t)(int32_t)m) test_ri(j, a, (uint32_t)m, sf);
        else { mov_rr(j, RAX, a, 1); and_imm(j, RAX, m, 1); alu_rr(j, 0x85, RAX, RAX, 1); }
        flags_out(j, 3);
        return JI_OK;
    }
    int d = hw(j, rd);   // (the stack pointer for AND / ORR / EOR with register 31)
    if (d != a) mov_rr(j, d, a, sf);
    if (opc == 0 || opc == 3) and_imm(j, d, m, sf); else if (opc == 1) or_imm(j, d, m, sf); else xor_imm(j, d, m, sf);
    if (opc == 3) flags_out(j, 3);
    return JI_OK;
}
static JI emit_movw(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), opc = B(i, 30, 29), hwd = B(i, 22, 21), rd = B(i, 4, 0);
    if (!sf && hwd > 1) return JI_NO;
    if (opc == 1) return JI_NO;
    uint64_t imm = (uint64_t)B(i, 20, 5) << (hwd * 16);
    if (opc == 3) {   // MOVK: in place
        if (rd == 31) return JI_OK;
        if (j->mw_pc == j->pc - 4 && j->mw_rd == rd && j->mw_sf == sf && j->mw_end == j->p && j->rc_slot_of[rd]) {   // after a MOVZ / MOVN / MOVK of the same register: one move of the combined constant
            uint64_t v = (j->mw_val & ~(0xffffull << (hwd * 16))) | imm;
            if (!sf) v &= 0xffffffffull;
            int d = hw(j, rd);   // (the register is cached, so this emits nothing)
            if (j->p == j->mw_end) { j->p = j->mw_start; mov_ri_nf(j, d, v); j->mw_end = j->p; j->mw_val = v; j->mw_pc = j->pc; return JI_OK; }
        }
        int d = hrw(j, rd);
        if (!sf) mov_rr(j, d, d, 0);
        and_imm(j, d, ~(0xffffull << (hwd * 16)), sf); or_imm(j, d, imm, sf);
        return JI_OK;
    }
    if (rd == 31) return JI_OK;
    uint64_t v = opc == 0 ? ~imm & (sf ? ~0ull : 0xffffffffull) : imm;
    int d = hw(j, rd);
    j->mw_start = j->p; mov_ri_nf(j, d, v); j->mw_end = j->p; j->mw_val = v; j->mw_pc = j->pc; j->mw_rd = rd; j->mw_sf = sf;
    return JI_OK;
}
static JI emit_adr(JC *j, uint32_t i) {
    int rd = B(i, 4, 0);
    int64_t imm = sext((B(i, 23, 5) << 2) | B(i, 30, 29), 21);
    uint64_t v = B(i, 31, 31) ? (j->pc & ~0xfffull) + (uint64_t)(imm << 12) : j->pc + (uint64_t)imm;
    if (rd == 31) return JI_OK;
    mov_ri_nf(j, hw(j, rd), v);
    return JI_OK;
}
static JI emit_bitfield_via_scratch(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), opc = B(i, 30, 29), N = B(i, 22, 22), immr = B(i, 21, 16), imms = B(i, 15, 10), rn = B(i, 9, 5), rd = B(i, 4, 0);
    int ds = sf ? 64 : 32;
    if (opc == 3 || N != sf || immr >= ds || imms >= ds) return JI_NO;
    int width, pos;
    if (imms >= immr) { width = imms - immr + 1; pos = 0; } else { width = imms + 1; pos = ds - immr; }
    uint64_t fm = width >= 64 ? ~0ull : (1ull << width) - 1;
    ld_x(j, RAX, rn, sf);
    if (opc == 2) {  // UBFM
        if (imms >= immr) { shift_ri(j, 5, RAX, immr, sf); if (imms != ds - 1) and_imm(j, RAX, fm, sf); }
        else { if (pos + width != ds) and_imm(j, RAX, fm, sf); shift_ri(j, 4, RAX, pos, sf); }
    } else if (opc == 0) {  // SBFM
        shift_ri(j, 4, RAX, ds - 1 - imms, sf);
        shift_ri(j, 7, RAX, imms >= immr ? ds - 1 - imms + immr : immr - 1 - imms, sf);
    } else {  // BFM
        if (imms >= immr) { shift_ri(j, 5, RAX, immr, sf); if (width != ds) and_imm(j, RAX, fm, sf); }
        else { and_imm(j, RAX, fm, sf); shift_ri(j, 4, RAX, pos, sf); }
        ld_x(j, RCX, rd, sf);
        and_imm(j, RCX, ~(fm << pos), sf);
        alu_rr(j, 0x09, RCX, RAX, sf);
        mov_rr(j, RAX, RCX, 1);
    }
    st_x(j, rd, RAX);
    return JI_OK;
}
static JI emit_bitfield(JC *j, uint32_t i) {   // UBFM / SBFM (LSL, LSR, ASR, UBFX, SBFX, SXTB, SXTH, SXTW, UXTB, UXTH): on the host registers; BFM and a zero source by way of rax
    int sf = B(i, 31, 31), opc = B(i, 30, 29), N = B(i, 22, 22), immr = B(i, 21, 16), imms = B(i, 15, 10), rn = B(i, 9, 5), rd = B(i, 4, 0);
    int ds = sf ? 64 : 32;
    if (opc == 3 || N != sf || immr >= ds || imms >= ds) return JI_NO;
    if (opc == 1 || rn == 31) return emit_bitfield_via_scratch(j, i);
    if (rd == 31) return JI_OK;
    int width, pos;
    if (imms >= immr) { width = imms - immr + 1; pos = 0; } else { width = imms + 1; pos = ds - immr; }
    uint64_t fm = width >= 64 ? ~0ull : (1ull << width) - 1;
    int a = hr(j, rn), d = hw(j, rd);
    if (immr == 0 && opc == 2 && (imms == 7 || imms == 15 || imms == 31)) { movzx_rr(j, d, a, imms == 7 ? 1 : imms == 15 ? 2 : 4); return JI_OK; }   // UXTB / UXTH / (MOV Wd, Wn)
    if (immr == 0 && opc == 0 && (imms == 7 || imms == 15 || (imms == 31 && sf))) { movsx_rr(j, d, a, imms == 7 ? 1 : imms == 15 ? 2 : 4, sf); return JI_OK; }   // SXTB / SXTH / SXTW
    if (d != a) mov_rr(j, d, a, sf);
    if (opc == 2) {  // UBFM
        if (imms >= immr) { shift_ri(j, 5, d, immr, sf); if (imms != ds - 1) and_imm(j, d, fm, sf); }
        else { if (pos + width != ds) and_imm(j, d, fm, sf); shift_ri(j, 4, d, pos, sf); }
    } else {  // SBFM
        shift_ri(j, 4, d, ds - 1 - imms, sf);
        shift_ri(j, 7, d, imms >= immr ? ds - 1 - imms + immr : immr - 1 - imms, sf);
    }
    return JI_OK;
}
static JI emit_extr(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), lsb = B(i, 15, 10), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (B(i, 22, 22) != sf || B(i, 30, 29) != 0 || (!sf && lsb >= 32)) return JI_NO;
    ld_x(j, RAX, rn, sf); ld_x(j, RDX, rm, sf);
    shrd_ri(j, RDX, RAX, lsb, sf);       // rdx = (rax:rdx) >> lsb
    st_x(j, rd, RDX);
    return JI_OK;
}
static JI emit_csel(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op = B(i, 30, 30), op2 = B(i, 10, 10), rm = B(i, 20, 16), cond = B(i, 15, 12), rn = B(i, 9, 5), rd = B(i, 4, 0);
    ld_x(j, RAX, rm, sf);                                  // the value when the condition fails
    if (op) unary(j, 2, RAX, sf);
    if (op2) lea_rm(j, RAX, RAX, 1, sf);
    ld_x(j, RCX, rn, sf);                                  // the value when it holds
    int cc = cond_cc(j, cond);
    if (cc == CC_ALWAYS) mov_rr(j, RAX, RCX, sf);
    else if (cc != CC_NEVER) cmov(j, cc, RAX, RCX, sf);
    st_x(j, rd, RAX);
    return JI_OK;
}
static JI emit_ccmp(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op = B(i, 30, 30), rn = B(i, 9, 5), cond = B(i, 15, 12), nzcv = B(i, 3, 0);
    int cc = cond_cc(j, cond);
    Fix skip = {0}, done = {0};
    bool have_else = cc != CC_ALWAYS;
    if (cc == CC_NEVER) { mov_m8_imm(j, R15, GN, nzcv >> 3 & 1); mov_m8_imm(j, R15, GZ, nzcv >> 2 & 1); mov_m8_imm(j, R15, GC, nzcv >> 1 & 1); mov_m8_imm(j, R15, GV, nzcv & 1); return JI_OK; }
    if (cc != CC_ALWAYS) skip = jcc_fwd(j, cc ^ 1);
    ld_x(j, RAX, rn, sf);
    if (B(i, 11, 11)) { alu_ri(j, op ? 7 : 0, RAX, B(i, 20, 16), sf); }
    else { ld_x(j, RCX, B(i, 20, 16), sf); alu_rr(j, op ? 0x39 : 0x01, RAX, RCX, sf); }
    flags_from_x86(j, op ? 1 : 2);
    if (have_else) {
        done = jmp_fwd(j);
        fix_here(j, skip);
        mov_m8_imm(j, R15, GN, nzcv >> 3 & 1); mov_m8_imm(j, R15, GZ, nzcv >> 2 & 1); mov_m8_imm(j, R15, GC, nzcv >> 1 & 1); mov_m8_imm(j, R15, GV, nzcv & 1);
        fix_here(j, done);
        j->flagkind = 0;
    }
    return JI_OK;
}
static JI emit_adc(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op = B(i, 30, 30), S = B(i, 29, 29), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    ld_x(j, RAX, rn, sf); ld_x(j, RDX, rm, sf);
    if (op) unary(j, 2, RDX, sf);
    movzx8_rm(j, RCX, R15, GC);
    e1(j, 0x80); modrm_rr(j, 0, RCX); e1(j, 0xFF);        // add cl, 0xff: CF = old C
    alu_rr(j, 0x11, RAX, RDX, sf);                         // adc
    if (S) flags_out(j, 2);
    st_x(j, rd, RAX);
    return JI_OK;
}
static JI emit_shiftv(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op = B(i, 15, 10), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    static const int ext[4] = {4, 5, 7, 1};
    ld_x(j, RAX, rn, sf); ld_x(j, RCX, rm, sf);
    shift_cl(j, ext[op - 8], RAX, sf);
    st_x(j, rd, RAX);
    return JI_OK;
}
static JI emit_div(JC *j, uint32_t i) {   // UDIV / SDIV: a zero divisor gives 0 (x86 would trap), INT_MIN / -1 gives INT_MIN (x86 would trap)
    int sf = B(i, 31, 31), sg = B(i, 10, 10), rm = B(i, 20, 16), rn = B(i, 9, 5), rd = B(i, 4, 0);
    ld_x(j, RAX, rn, sf); ld_x(j, RCX, rm, sf);
    alu_rr(j, 0x85, RCX, RCX, sf);
    Fix zero = jcc_fwd(j, CC_E);
    Fix neg = {0};
    if (sg) { alu_ri(j, 7, RCX, -1, sf); neg = jcc_fwd(j, CC_E); cdq_cqo(j, sf); unary(j, 7, RCX, sf); }
    else { alu_rr(j, 0x31, RDX, RDX, 0); unary(j, 6, RCX, sf); }
    Fix d1 = jmp_fwd(j), d2 = {0};
    if (sg) { fix_here(j, neg); unary(j, 3, RAX, sf); d2 = jmp_fwd(j); }
    fix_here(j, zero); alu_rr(j, 0x31, RAX, RAX, 0);
    fix_here(j, d1); if (sg) fix_here(j, d2);
    st_x(j, rd, RAX);
    return JI_OK;
}
static JI emit_dp3(JC *j, uint32_t i) {
    int sf = B(i, 31, 31), op31 = B(i, 23, 21), o0 = B(i, 15, 15), rm = B(i, 20, 16), ra = B(i, 14, 10), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if (op31 == 0) {  // MADD / MSUB
        ld_x(j, RAX, rn, sf);
        if (rm == 31) mov_ri_nf(j, RAX, 0); else imul_rr(j, RAX, hr(j, rm), sf);
        if (o0) unary(j, 3, RAX, sf);                                  // -(n * m)
        if (ra != 31) alu_rr(j, 0x01, RAX, hr(j, ra), sf);
        st_x(j, rd, RAX);
        return JI_OK;
    }
    if (!sf) return JI_NO;
    if (op31 == 1 || op31 == 5) {  // SMADDL / SMSUBL / UMADDL / UMSUBL: 32 x 32 -> 64
        if (op31 == 1) { ld_x(j, RAX, rn, 1); movsx_rr(j, RAX, RAX, 4, 1); ld_x(j, RCX, rm, 1); movsx_rr(j, RCX, RCX, 4, 1); }
        else { ld_x(j, RAX, rn, 0); ld_x(j, RCX, rm, 0); }
        imul_rr(j, RAX, RCX, 1);
        if (o0) unary(j, 3, RAX, 1);
        if (ra != 31) alu_rr(j, 0x01, RAX, hr(j, ra), 1);
        st_x(j, rd, RAX);
        return JI_OK;
    }
    if (op31 == 2 || op31 == 6) {  // SMULH / UMULH
        ld_x(j, RAX, rn, 1); ld_x(j, RCX, rm, 1);
        unary(j, op31 == 2 ? 5 : 4, RCX, 1);      // rdx:rax = rax * rcx
        st_x(j, rd, RDX);
        return JI_OK;
    }
    return JI_NO;
}
static JI emit_dp1(JC *j, uint32_t i) {  // REV family on whole registers; the rest by exec_slow
    int sf = B(i, 31, 31), op = B(i, 15, 10), rn = B(i, 9, 5), rd = B(i, 4, 0);
    if ((op == 2 && sf) || (op == 3 && sf) || (op == 2 && !sf)) {  // REV32 (X: each word), REV64, REV (W)
        ld_x(j, RAX, rn, sf);
        if (op == 3) bswap(j, RAX, 1);
        else if (!sf) bswap(j, RAX, 0);
        else { bswap(j, RAX, 1); shift_ri(j, 1, RAX, 32, 1); }   // bswap64 then swap the halves = bswap each 32-bit half
        st_x(j, rd, RAX);
        return JI_OK;
    }
    return JI_NO;
}

// ----- loads and stores

// the effective address in rcx (displacement returned); writeback value in rdx when *wb is set
static JI ldst_address(JC *j, uint32_t i, int size, int sc, int32_t *disp, int *wb) {
    int rn = B(i, 9, 5);
    *wb = 0;
    ld_xsp(j, RCX, rn, 1);
    if (B(i, 25, 24) == 1) { *disp = (int32_t)(B(i, 21, 10) << sc); return JI_OK; }
    if (B(i, 21, 21)) {
        int opt = B(i, 15, 13);
        if (B(i, 11, 10) != 2 || (opt & 2) == 0) return JI_NO;
        ld_x(j, RDX, B(i, 20, 16), 1);
        if (opt == 2) movzx_rr(j, RDX, RDX, 4); else if (opt == 6) movsx_rr(j, RDX, RDX, 4, 1);
        if (B(i, 12, 12)) shift_ri(j, 4, RDX, sc, 1);
        alu_rr(j, 0x01, RCX, RDX, 1);
        *disp = 0; return JI_OK;
    }
    int idx = B(i, 11, 10);
    int32_t imm = (int32_t)sext(B(i, 20, 12), 9);
    if (idx == 2) return JI_NO;
    if (idx == 0) { *disp = imm; return JI_OK; }
    lea_rm(j, RDX, RCX, imm, 1); *wb = 1;
    if (idx == 1) { *disp = 0; return JI_OK; }          // post-index: access at the old base
    mov_rr(j, RCX, RDX, 1); *disp = 0; return JI_OK;     // pre-index: access at the new one
}
static void emit_load_sized(JC *j, int size, int opc, int base, int32_t disp) {  // value into rax
    if (opc == 1) {
        if (size == 3) mov_rm(j, RAX, base, disp, 1); else if (size == 2) mov_rm(j, RAX, base, disp, 0);
        else if (size == 1) movzx16_rm(j, RAX, base, disp); else movzx8_rm(j, RAX, base, disp);
    } else {  // sign-extending: opc 2 to 64 bits, opc 3 to 32
        movsx_rm(j, RAX, base, disp, size == 0 ? 1 : size == 1 ? 2 : 4, opc == 2);
    }
}
// ---- general-purpose loads and stores with host registers as the address
typedef struct { int base, idx, scale; int32_t disp; } Mem;   // idx < 0: no index; scale is the shift (0..3)
static void mem_op(JC *j, int w, int twobyte, uint8_t opc, int reg, Mem m, int byte) {   // op reg, [mem] (or the store form of the opcode)
    uint8_t r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((m.idx >= 0 && (m.idx & 8)) ? 2 : 0) | ((m.base & 8) ? 1 : 0);
    if (r != 0x40 || (byte && reg >= 4)) e1(j, r);
    if (twobyte) e1(j, 0x0F);
    e1(j, opc);
    if (m.idx < 0) { modrm_mem(j, reg, m.base, m.disp); return; }
    jsite_add(j);
    int mod = (m.disp == 0 && (m.base & 7) != 5) ? 0 : (m.disp >= -128 && m.disp <= 127) ? 1 : 2;
    e1(j, (mod << 6) | ((reg & 7) << 3) | 4);
    e1(j, (m.scale << 6) | ((m.idx & 7) << 3) | (m.base & 7));
    if (mod == 1) e1(j, (uint8_t)m.disp); else if (mod == 2) e4(j, (uint32_t)m.disp);
}
static Mem mem_plus(Mem m, int32_t d) { m.disp += d; return m; }
static void mem_load(JC *j, int size, int opc, int dst, Mem m) {   // opc 1: zero-extending, 2: sign-extending to 64 bits, 3: to 32 bits
    if (opc == 1) {
        if (size == 3) mem_op(j, 1, 0, 0x8B, dst, m, 0); else if (size == 2) mem_op(j, 0, 0, 0x8B, dst, m, 0);
        else mem_op(j, 0, 1, size == 1 ? 0xB7 : 0xB6, dst, m, 0);
    } else if (size == 2) mem_op(j, 1, 0, 0x63, dst, m, 0);
    else mem_op(j, opc == 2, 1, size == 1 ? 0xBF : 0xBE, dst, m, 0);
}
static void mem_store(JC *j, int size, int src, Mem m) {
    if (size == 1) e1(j, 0x66);
    mem_op(j, size == 3, 0, size == 0 ? 0x88 : 0x89, src, m, size == 0);
}
// the address of a load or store: *post is the amount to add to the base register afterwards (post-index); pre-index is done here
static JI ldst_mem(JC *j, uint32_t i, int size, Mem *m, int32_t *post) {
    int rn = B(i, 9, 5);
    *post = 0;
    if (B(i, 25, 24) == 1) { *m = (Mem){hr(j, rn), -1, 0, (int32_t)(B(i, 21, 10) << size)}; return JI_OK; }
    if (B(i, 21, 21)) {   // register offset
        int opt = B(i, 15, 13);
        if (B(i, 11, 10) != 2 || (opt & 2) == 0) return JI_NO;
        int rm = B(i, 20, 16), sc = B(i, 12, 12) ? size : 0;
        int base = hr(j, rn);
        if (rm == 31) { *m = (Mem){base, -1, 0, 0}; return JI_OK; }
        int idx = hr(j, rm);
        if (opt == 2) { movzx_rr(j, RDX, idx, 4); idx = RDX; } else if (opt == 6) { movsx_rr(j, RDX, idx, 4, 1); idx = RDX; }
        *m = (Mem){base, idx, sc, 0};
        return JI_OK;
    }
    int idx = B(i, 11, 10);
    int32_t imm = (int32_t)sext(B(i, 20, 12), 9);
    if (idx == 2) return JI_NO;
    if (idx == 0) { *m = (Mem){hr(j, rn), -1, 0, imm}; return JI_OK; }
    int base = hrw(j, rn);
    if (idx == 3) alu_ri(j, 0, base, imm, 1);     // pre-index: the base changes before the access
    else *post = imm;
    *m = (Mem){base, -1, 0, 0};
    return JI_OK;
}
static JI emit_ldst_gpr(JC *j, uint32_t i) {
    int size = B(i, 31, 30), opc = B(i, 23, 22), rt = B(i, 4, 0), rn = B(i, 9, 5);
    if (opc >= 2 && size == 3) { if (B(i, 25, 24) == 1 || (!B(i, 21, 21) && B(i, 11, 10) == 0) || (B(i, 21, 21) && B(i, 11, 10) == 2)) return JI_OK; return JI_NO; }   // PRFM / PRFUM: nothing to do (immediate, unscaled, register offset)
    if (opc == 3 && size == 2) return JI_NO;
    if (rt == rn && rt != 31 && B(i, 25, 24) == 0 && !B(i, 21, 21) && (B(i, 11, 10) & 1)) {   // a register that is both the base with writeback and the transferred one: the writeback wins, a store writes the old value
        int32_t disp; int wb;
        JI r = ldst_address(j, i, size, size, &disp, &wb);
        if (r != JI_OK) return r;
        if (opc == 0) { ld_x(j, RAX, rt, 1); st_sized(j, RCX, disp, RAX, 1 << size); }
        else emit_load_sized(j, size, opc, RCX, disp);
        st_xsp(j, rn, RDX);
        return JI_OK;
    }
    Mem m; int32_t post;
    JI r = ldst_mem(j, i, size, &m, &post);
    if (r != JI_OK) return r;
    if (opc == 0) {
        int v = RAX;
        if (rt == 31) mov_ri_nf(j, RAX, 0); else v = hr(j, rt);
        mem_store(j, size, v, m);
    } else {
        int d = rt == 31 ? RAX : hw(j, rt);   // (a load into xzr is still made, and faults on a bad address, as on the hardware)
        mem_load(j, size, opc, d, m);
    }
    if (post) alu_ri(j, 0, m.base, post, 1);
    return JI_OK;
}
static JI emit_ldst_pair(JC *j, uint32_t i) {
    int opc = B(i, 31, 30), L = B(i, 22, 22), idx = B(i, 24, 23), rt = B(i, 4, 0), rt2 = B(i, 14, 10), rn = B(i, 9, 5);
    if (opc == 3 || (opc == 1 && !L)) return JI_NO;   // (opc 1 is LDPSW)
    int esz = opc == 2 ? 3 : 2, sz = 1 << esz;
    int32_t imm = (int32_t)sext(B(i, 21, 15), 7) * (1 << esz);
    if ((idx == 1 || idx == 3) && rn != 31 && (rt == rn || rt2 == rn)) {   // the base with writeback is also transferred: the writeback wins
        ld_xsp(j, RCX, rn, 1);
        int32_t disp = idx == 1 ? 0 : imm;
        if (L) {
            if (opc == 1) { movsx_rm(j, RAX, RCX, disp, 4, 1); movsx_rm(j, RDX, RCX, disp + sz, 4, 1); }
            else { mov_rm(j, RAX, RCX, disp, esz == 3); mov_rm(j, RDX, RCX, disp + sz, esz == 3); }
            st_x(j, rt, RAX); st_x(j, rt2, RDX);
        } else {
            ld_x(j, RAX, rt, 1); ld_x(j, RDX, rt2, 1);
            st_sized(j, RCX, disp, RAX, sz); st_sized(j, RCX, disp + sz, RDX, sz);
        }
        lea_rm(j, RDX, RCX, imm, 1); st_xsp(j, rn, RDX);
        return JI_OK;
    }
    int base = (idx == 1 || idx == 3) ? hrw(j, rn) : hr(j, rn);
    if (idx == 3) alu_ri(j, 0, base, imm, 1);
    int32_t disp = idx == 1 || idx == 3 ? 0 : imm;
    Mem m = {base, -1, 0, disp};
    if (L) {
        // both loads read the base register: a destination that is the base goes through rax / rdx
        int d1 = rt == 31 || rt == rn ? RAX : hw(j, rt), d2 = rt2 == 31 || rt2 == rn ? RDX : hw(j, rt2);
        mem_load(j, opc == 1 ? 2 : esz, opc == 1 ? 2 : 1, d1, m); mem_load(j, opc == 1 ? 2 : esz, opc == 1 ? 2 : 1, d2, mem_plus(m, sz));
        if (d1 == RAX && rt != 31) mov_rr(j, hw(j, rt), RAX, 1);
        if (d2 == RDX && rt2 != 31) mov_rr(j, hw(j, rt2), RDX, 1);
    } else {
        int v1 = RAX, v2 = RDX;
        if (rt == 31) mov_ri_nf(j, RAX, 0); else v1 = hr(j, rt);
        if (rt2 == 31) mov_ri_nf(j, RDX, 0); else v2 = hr(j, rt2);
        mem_store(j, esz, v1, m); mem_store(j, esz, v2, mem_plus(m, sz));
    }
    if (idx == 1) alu_ri(j, 0, base, imm, 1);
    return JI_OK;
}
static JI emit_ldr_lit(JC *j, uint32_t i) {
    int opc = B(i, 31, 30), rt = B(i, 4, 0);
    if (opc == 3) return JI_OK;
    uint64_t addr = j->pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
    mov_ri_nf(j, RCX, addr);
    if (opc == 0) mov_rm(j, RAX, RCX, 0, 0); else if (opc == 1) mov_rm(j, RAX, RCX, 0, 1); else movsx_rm(j, RAX, RCX, 0, 4, 1);
    st_x(j, rt, RAX);
    return JI_OK;
}
// SIMD&FP register loads and stores: the unsigned-offset, unscaled and register-offset forms of 1, 2, 4, 8 and 16 bytes, and LDP/STP
static JI emit_ldst_vec(JC *j, uint32_t i) {
    int size = B(i, 31, 30), opc = B(i, 23, 22), rt = B(i, 4, 0), rn = B(i, 9, 5);
    int q = (opc & 2) != 0;
    if (q && size != 0) return JI_NO;
    int bytes = q ? 16 : 1 << size, sc = q ? 4 : size;
    int32_t disp; int wb;
    JI r = ldst_address(j, i, size, sc, &disp, &wb);
    if (r != JI_OK) return r;
    int32_t vo = (int32_t)(offsetof(CPU, vr) + 16 * rt);
    if (opc & 1) {  // load
        if (q) op_rm(j, 0, 1, 0x10, 0, RCX, disp);   // movups xmm0, [rcx+disp]
        else emit_load_sized(j, size, 1, RCX, disp);
        if (q) { rex(j, 0, 0, R15, 0); e1(j, 0x0F); e1(j, 0x11); modrm_mem(j, 0, R15, vo); }  // movups [vr], xmm0
        else { mov_mr(j, R15, vo, RAX, 1); mov_ri_nf(j, RSI, 0); mov_mr(j, R15, vo + 8, RSI, 1); }
    } else {
        if (q) { rex(j, 0, 0, R15, 0); e1(j, 0x0F); e1(j, 0x10); modrm_mem(j, 0, R15, vo); op_rm(j, 0, 1, 0x11, 0, RCX, disp); }
        else { mov_rm(j, RAX, R15, vo, 1); st_sized(j, RCX, disp, RAX, bytes); }
    }
    if (wb) st_xsp(j, rn, RDX);
    return JI_OK;
}

#include "jitfp.h"
#include "jitmem.h"
#include "jitneon.h"

// ----- branches and the per-instruction router

// The stub of a plain host function without side effects on the bridge (memcpy, strlen, pthread_getspecific, ...), as translated code can call it
// itself: the arguments go to the SysV registers, the result comes back in x0.  A call that goes through the dispatcher instead costs ~60 ns.
static void emit_leaf_call_body(JC *j, void *fn, int nargs, int zext8) {
    static const uint8_t argreg[6] = {RDI, RSI, RDX, RCX, R8, R9};
    rc_flush(j);
    for (int k = 0; k < nargs; k++) { int sl = j->rc_slot_of[k] - 1; if (sl >= 0 && sl < g_npin) mov_rr(j, argreg[k], RC_HOST[sl], 1); else mov_rm(j, argreg[k], R15, GXR(k), 1); }   // (a pinned register is not in memory)
    mov_ri_nf(j, RAX, (uint64_t)fn); call_reg(j, RAX);
    if (zext8) movzx_rr(j, RAX, RAX, 1);                          // (a bool result leaves the upper bits of rax undefined)
    { int sl = j->rc_slot_of[0] - 1; if (sl >= 0 && sl < g_npin) mov_rr(j, RC_HOST[sl], RAX, 1); else mov_mr(j, R15, GXR(0), RAX, 1); }
    rc_clear(j);
}
// A lazy-binding stub of the guest (adrp x16, got@page; ldr x16, [x16, got@off]; br x16) whose slot holds such an import: the block of the stub
// itself makes the call and returns to x30.
static bool jit_leaf_stub(uint64_t pc, void **fn, int *nargs, int *zext8) {
    if (g_jit_noleaf || (pc & 4095) > 4096 - 12) return false;
    const uint32_t *w = (const uint32_t *)pc;
    if ((w[0] & 0x9f00001f) != 0x90000010 || (w[1] & 0xffc003ff) != 0xf9400210 || w[2] != 0xd61f0200) return false;
    uint64_t imm = ((uint64_t)B(w[0], 23, 5) << 2) | B(w[0], 30, 29);
    uint64_t slot = ((pc & ~4095ull) + (uint64_t)(sext(imm, 21) << 12)) + (uint64_t)B(w[1], 21, 10) * 8;
    if (slot - g_guest_lo >= g_guest_size) return false;
    return jit_leaf_import(*(const uint64_t *)slot, fn, nargs, zext8);
}
static JI emit_branch_insn(JC *j, uint32_t i) {
    uint64_t pc = j->pc;
    if (B(i, 30, 26) == 0x5) {  // B / BL
        uint64_t target = pc + (uint64_t)(sext(B(i, 25, 0), 26) * 4);
        if (B(i, 31, 31)) mov_ri_nf(j, hw(j, 30), pc + 4);
        if (B(i, 31, 31) && g_jit_ras && target - g_stub_base >= g_stub_size) { emit_call_ras(j, target, pc + 4); return JI_END; }
        emit_goto(j, target);
        return JI_END;
    }
    if ((i & 0xff000010) == 0x54000000) {  // B.cond
        uint64_t target = pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
        int cc = cond_cc(j, B(i, 3, 0));
        bool pend = j->fl_pend_pc == pc && j->flagkind;   // the stores of the flags were left to this branch (flags_deferrable)
        if (pend && (cc == CC_ALWAYS || cc == CC_NEVER || !side_exit_room(j))) { flags_from_x86(j, j->fl_pend_kind); j->kind_set = false; pend = false; }   // (not expected: store them now)
        if (cc == CC_ALWAYS) emit_goto(j, target);
        else if (cc == CC_NEVER) return JI_OK;
        else if (side_exit_room(j)) {
            side_exit(j, cc, target);
            if (pend) {
                j->sx[j->nsx - 1].fkind = j->fl_pend_taken ? (uint8_t)j->fl_pend_kind : g_flagpoison ? 0x10 : 0;
                if (j->fl_pend_fall) { flags_from_x86(j, j->fl_pend_kind); j->kind_set = false; }
                else if (g_flagpoison) flags_poison(j);
            }
            return JI_OK;
        }
        else { Fix t = jcc_fwd(j, cc); emit_goto(j, pc + 4); fix_here(j, t); emit_goto(j, target); }
        return JI_END;
    }
    if (B(i, 30, 25) == 0x1a) {  // CBZ / CBNZ
        int sf = B(i, 31, 31), nz = B(i, 24, 24), rt = B(i, 4, 0);
        uint64_t target = pc + (uint64_t)(sext(B(i, 23, 5), 19) * 4);
        int h = RAX;
        if (rt == 31) mov_ri_nf(j, RAX, 0); else h = hr(j, rt);
        alu_rr(j, 0x85, h, h, sf);
        if (side_exit_room(j)) { side_exit(j, nz ? CC_NE : CC_E, target); return JI_OK; }
        Fix t = jcc_fwd(j, nz ? CC_NE : CC_E);
        emit_goto(j, pc + 4); fix_here(j, t); emit_goto(j, target);
        return JI_END;
    }
    if (B(i, 30, 25) == 0x1b) {  // TBZ / TBNZ
        int bit = (B(i, 31, 31) << 5) | B(i, 23, 19), nz = B(i, 24, 24), rt = B(i, 4, 0);
        uint64_t target = pc + (uint64_t)(sext(B(i, 18, 5), 14) * 4);
        int h = RAX;
        if (rt == 31) mov_ri_nf(j, RAX, 0); else h = hr(j, rt);
        bt_ri(j, h, bit);
        if (side_exit_room(j)) { side_exit(j, nz ? CC_B : CC_AE, target); return JI_OK; }
        Fix t = jcc_fwd(j, nz ? CC_B : CC_AE);
        emit_goto(j, pc + 4); fix_here(j, t); emit_goto(j, target);
        return JI_END;
    }
    if ((i & 0xfe1ffc1f) == 0xd61f0000) {  // BR / BLR / RET
        int opc = B(i, 24, 21), rn = B(i, 9, 5);
        if (opc > 2 || rn == 31) return JI_NO;
        mov_rr(j, RAX, hr(j, rn), 1);
        if (opc == 1) mov_ri_nf(j, hw(j, 30), pc + 4);
        if (opc == 1 && g_jit_ras) { emit_blr_ras(j, pc + 4); return JI_END; }
        if (opc == 2 && g_jit_ras) {   // RET: the guest return pc is on top of the return-address stack
            rc_flush(j);
            op_rm(j, 1, 0, 0x3B, RAX, RSP, 8);                // cmp rax, [rsp+8]
            Fix other = jcc_fwd(j, CC_NE);
            e1(j, 0xC2); e1(j, 8); e1(j, 0);                  // ret 8
            fix_here(j, other);
        }
        g_ind_kind = opc;
        emit_indirect(j, RAX);
        return JI_END;
    }
    return JI_NO;
}

static JI jit_emit(JC *j, uint32_t i) {
    switch (B(i, 28, 25)) {
    case 0x8: case 0x9: {
        JI r = JI_NO;
        if (B(i, 28, 23) == 0x22) r = emit_addsub_imm(j, i);
        else if (B(i, 28, 23) == 0x24) r = emit_logic_imm(j, i);
        else if (B(i, 28, 23) == 0x25) r = emit_movw(j, i);
        else if (B(i, 28, 23) == 0x26) r = emit_bitfield(j, i);
        else if (B(i, 28, 23) == 0x27) r = emit_extr(j, i);
        else if (B(i, 28, 24) == 0x10) r = emit_adr(j, i);
        if (r != JI_NO) return r;
        break;
    }
    case 0xa: case 0xb: {
        JI r = emit_branch_insn(j, i);
        if (r != JI_NO) return r;
        if (i == 0xd503201f) return JI_OK;                                        // NOP
        if ((i & 0xfffff000) == 0xd5033000) {   // DMB / DSB fence, ISB does not.  x86 keeps loads and stores in order on its own: only a full barrier (CRm = both loads and stores) orders a store before a later load
            if (B(i, 7, 5) != 6 && (g_nodmbopt || (B(i, 11, 8) & 3) == 3)) { if (g_nodmbopt) mfence_(j); else { e1(j, 0xF0); e1(j, 0x83); e1(j, 0x0C); e1(j, 0x24); e1(j, 0x00); } }   // lock or dword [rsp], 0: a full fence, cheaper than mfence
            return JI_OK;
        }
        if ((i & 0xffffffc0) == 0xd53bd040) {   // MRS Xt, TPIDR_EL0 / TPIDRRO_EL0 (the thread-local-variable thunk reads the latter on every access)
            mov_rm(j, RAX, R15, (i & 0x20) ? (int32_t)offsetof(CPU, tpidrro) : (int32_t)offsetof(CPU, tpidr), 1); st_x(j, B(i, 4, 0), RAX); return JI_OK;
        }
        if ((i & 0xfffff000) == 0xd5032000 || (i & 0xffe0001f) == 0xd4000001 || (i & 0xffd00000) == 0xd5100000) { emit_helper(j, jit_h_slow, i); return JI_OK; }
        return JI_NO;
    }
    case 0x4: case 0x6: case 0xc: case 0xe: {
        int V = B(i, 26, 26);
        if (B(i, 29, 24) == 0x08 || B(i, 29, 24) == 0x19 || (B(i, 29, 27) == 7 && B(i, 25, 24) == 0 && B(i, 21, 21) && B(i, 11, 10) == 0)) {   // exclusives, acquire/release, atomics
            JI r = emit_ordered(j, i);
            if (r == JI_NO) r = emit_atomic(j, i);
            if (r != JI_NO) return r;
            break;
        }
        if (B(i, 29, 27) == 0x7 && B(i, 25, 24) <= 1) {
            JI r = V ? emit_ldst_vec(j, i) : emit_ldst_gpr(j, i);
            if (r != JI_NO) return r;
            break;
        }
        if (B(i, 29, 27) == 0x5 && !B(i, 25, 25)) { JI r = V ? emit_ldst_pair_vec(j, i) : emit_ldst_pair(j, i); if (r != JI_NO) return r; if (V) { emit_helper(j, jit_h_vls, i); return JI_OK; } break; }
        if (B(i, 29, 27) == 0x3 && B(i, 25, 24) == 0 && !V) { JI r = emit_ldr_lit(j, i); if (r != JI_NO) return r; break; }
        if (V && (i & 0xbf208000) == 0x0c000000) { JI r = emit_ld1_multi(j, i); if (r != JI_NO) return r; }
        if (V && (i & 0xbf200000) == 0x0d000000) { JI r = emit_ld1_lane(j, i); if (r != JI_NO) return r; }
        if (V) { emit_helper(j, jit_h_vls, i); return JI_OK; }
        break;
    }
    case 0x5: case 0xd: {
        JI r = JI_NO;
        if (B(i, 28, 24) == 0x0b) r = emit_addsub_reg(j, i);
        else if (B(i, 28, 24) == 0x0a) r = emit_logic_reg(j, i);
        else if (B(i, 28, 21) == 0xd0 && B(i, 15, 10) == 0) r = emit_adc(j, i);
        else if (B(i, 29, 21) == 0x1d2 && !B(i, 10, 10) && !B(i, 4, 4)) r = emit_ccmp(j, i);
        else if (B(i, 29, 21) == 0xd4 && !B(i, 11, 11)) r = emit_csel(j, i);
        else if (B(i, 30, 21) == 0x0d6 && B(i, 15, 10) >= 8 && B(i, 15, 10) <= 11) r = emit_shiftv(j, i);
        else if (B(i, 30, 21) == 0x0d6 && (B(i, 15, 10) == 2 || B(i, 15, 10) == 3)) r = emit_div(j, i);
        else if (B(i, 30, 21) == 0x2d6 && !B(i, 20, 16)) { r = emit_dp1(j, i); if (r == JI_NO) r = emit_bitcount(j, i); }
        else if (B(i, 28, 24) == 0x1b) r = emit_dp3(j, i);
        if (r != JI_NO) return r;
        break;
    }
    case 0x7: case 0xf: {
        JI r = (B(i, 31, 24) & 0x7f) == 0x1e || B(i, 31, 24) == 0x1f ? emit_fp_scalar(j, i) : emit_neon(j, i);
        if (r != JI_NO) return r;
        emit_helper(j, jit_h_simd, i);
        return JI_OK;
    }
    default: return JI_NO;
    }
    emit_helper(j, jit_h_slow, i);   // no template: the interpreter's own code, one instruction
    return JI_OK;
}

// ------------------------------------------------------------------------------------------------ the block cache

static void jit_insert(uint64_t pc, uint8_t *code) {   // under g_jit_mu
    unsigned h = jit_hash(pc);
    for (int k = 0; k < 4096; k++) {
        JEnt *e = &g_jit_tab[(h + k) & ((1 << JIT_TAB_BITS) - 1)];
        if (e->pc == 0 || e->pc == 1) { e->code = code; __atomic_store_n(&e->pc, pc, __ATOMIC_RELEASE); return; }   // (1: the entry of a block that was invalidated, free again)
    }
}
static inline void jit_ic_set(uint64_t pc, uint8_t *code) { __atomic_store_n(&g_jit_ic[(pc >> 2) & ((1 << JIT_IC_BITS) - 1)], code, __ATOMIC_RELEASE); }

// REBAR_JIT_ONLY=<lo>-<hi>[,...] / REBAR_JIT_SKIP=<lo>-<hi>[,...] (hex guest addresses): translate only / everything but the blocks that start in the ranges.
// A bisection tool: when a program misbehaves only with the translator, halving the ranges finds the code it mistranslates.
static struct { uint64_t lo, hi; } g_jit_only[16], g_jit_skip[16];
static int g_jit_nonly, g_jit_nskip;
static void jit_parse_ranges(const char *env, int *n, struct { uint64_t lo, hi; } *r) {
    const char *s = getenv(env);
    while (s && *s && *n < 16) {
        char *e;
        uint64_t lo = strtoull(s, &e, 16);
        if (*e != '-') break;
        uint64_t hi = strtoull(e + 1, &e, 16);
        r[*n].lo = lo; r[*n].hi = hi; (*n)++;
        if (*e != ',') break;
        s = e + 1;
    }
}
static bool jit_allowed(uint64_t pc) {
    if (g_jit_nonly) { bool in = false; for (int k = 0; k < g_jit_nonly; k++) if (pc >= g_jit_only[k].lo && pc < g_jit_only[k].hi) in = true; if (!in) return false; }
    for (int k = 0; k < g_jit_nskip; k++) if (pc >= g_jit_skip[k].lo && pc < g_jit_skip[k].hi) return false;
    return true;
}

static __thread bool t_jit_busy;   // this thread is inside jit_compile / jit_invalidate and holds g_jit_mu
static __thread struct { uint64_t lo, hi; } t_jit_pend[8]; static __thread int t_jit_npend;   // invalidations asked for from a signal handler that interrupted the translator
static void jit_invalidate(uint64_t lo, uint64_t hi);
static void jit_run_pending(void) { while (t_jit_npend > 0) { int k = --t_jit_npend; jit_invalidate(t_jit_pend[k].lo, t_jit_pend[k].hi); } }
static bool g_jit_budget_all;   // REBAR_JIT_BUDGET_ALL=1: every block counts down the budget (to compare)
static bool jit_needs_budget(uint64_t pc, int maxn) {   // could a loop go through the block that starts at pc?  (scans the guest instructions it may cover)
    for (int k = 0; k < maxn; k++) {
        uint64_t p = pc + 4 * (uint64_t)k; uint32_t i = *(uint32_t *)p;
        if (B(i, 30, 26) == 0x5) return sext(B(i, 25, 0), 26) <= 0;   // B / BL: the block ends here
        if ((i & 0xfe1ffc1f) == 0xd61f0000) return true;                        // BR / BLR / RET
        if ((i & 0xff000010) == 0x54000000 && sext(B(i, 23, 5), 19) <= 0) return true;
        if (B(i, 30, 25) == 0x1a && sext(B(i, 23, 5), 19) <= 0) return true;
        if (B(i, 30, 25) == 0x1b && sext(B(i, 18, 5), 14) <= 0) return true;
    }
    return false;
}
static uint8_t *jit_compile(uint64_t pc) {
    if (t_jit_busy) return NULL;   // a guest signal handler that interrupted this thread inside the translator: interpret, do not wait for the lock this thread holds
    pthread_mutex_lock(&g_jit_mu); t_jit_busy = true;
    JEnt *ex = jit_probe(pc);
    if (ex) { uint8_t *code = ex->code; t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu); return code; }
    if (g_jit_end - g_jit_top < 65536 || ((g_jit_nonly || g_jit_nskip) && !jit_allowed(pc))) { jit_insert(pc, NULL); t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu); return NULL; }
    JC j; memset(&j, 0, sizeof j); rc_pin_apply(&j);
    uint8_t *start = (uint8_t *)(((uintptr_t)g_jit_top + (g_jit_align - 1)) & ~(uintptr_t)(g_jit_align - 1)) + g_jit_align;   // (the guest pc sits in the 8 bytes before the code)
    memcpy(start - 8, &pc, 8);
    j.p = start; j.limit = g_jit_end; j.blockpc = pc;
    int n = 0, maxn = g_jit_maxn, room = (int)((4096 - (pc & 4095)) / 4);
    if (room < maxn) maxn = room;
    // every block that a loop can run through counts down the budget (a backward branch, an indirect branch or a return in it); a block that only
    // branches forward cannot be part of a cycle by itself, so it skips the read-modify-write on the CPU structure
    void *leaf_fn = NULL; int leaf_nargs = 0, leaf_z = 0;
    bool leaf = room >= 3 && jit_leaf_stub(pc, &leaf_fn, &leaf_nargs, &leaf_z);
    bool counts = !leaf && (g_jit_budget_all || jit_needs_budget(pc, maxn));
    uint8_t *nptr = NULL; Fix budget = {0};
    if (counts) {
        rex(&j, 0, 0, R15, 0); e1(&j, 0x83); modrm_mem(&j, 5, R15, GBUDGET); nptr = j.p; e1(&j, 0);   // sub dword [r15+budget], n
        budget = jcc_fwd(&j, CC_S);
    } else { e1(&j, 0x0F); e1(&j, 0x1F); e1(&j, 0x44); e1(&j, 0x00); e1(&j, 0x00); }   // a 5-byte nop: a dead block's entry is overwritten by a jmp, which must not reach into code that another thread is already running
    size_t blkid = g_jit_reg_n;
    if (g_jit_count && blkid < (1u << 20)) { mov_ri_nf(&j, R9, (uint64_t)&g_jit_cnt[blkid]); e1(&j, 0x49); e1(&j, 0xFF); e1(&j, 0x01); }   // inc qword [r9]
    uint64_t cur = pc; bool ended = false;
    bool precise = pc >= g_jit_precise_lo && pc < g_jit_precise_hi;
    if (leaf) {   // the stub of a leaf import: call it and return to x30 (the three words are what the block stands for)
        emit_leaf_call_body(&j, leaf_fn, leaf_nargs, leaf_z);
        mov_rm(&j, RAX, R15, GXR(30), 1);
        emit_indirect(&j, RAX);
        n = 3; ended = true;
    }
    while (!leaf && n < maxn) {
        j.pc = cur; j.kind_set = false; j.rc_pin = 0; j.fresh = 0; j.insn_left = maxn - n - 1;
        if (precise && (*(uint32_t *)cur & 0x0a000000) == 0x08000000) {   // a load or store: the state a signal handler would see
            mov_ri_nf(&j, RAX, cur); mov_mr(&j, R15, (int32_t)offsetof(CPU, pc), RAX, 1);
            rc_sync_out(&j); rc_flush(&j);
        }
        JI r = jit_emit(&j, *(uint32_t *)cur);
        if (n < 64) g_jit_offs[n] = (uint32_t)(j.p - start);
        if (!j.kind_set) j.flagkind = 0;
        if (r == JI_NO) break;
        n++; cur += 4;
        if (r == JI_END) { ended = true; break; }
    }
    if (n == 0) { jit_insert(pc, NULL); t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu); return NULL; }
    if (!ended) emit_goto(&j, cur);
    emit_side_exits(&j);
    emit_blr_stub(&j);
    if (j.cs_rel) { int32_t d = (int32_t)(j.p - (j.cs_rel + 4)); memcpy(j.cs_rel, &d, 4); emit_exit_to(&j, j.cs_target); }   // until the callee is translated the call leads to the dispatcher
    if (counts) fix_here(&j, budget);
    uint32_t exit_off = (uint32_t)(j.p - start);
    emit_exit_to(&j, pc);
    if (counts) *nptr = (uint8_t)n;
    g_jit_top = j.p;
    g_jit_nblocks++; g_jit_ninsns += n;
    if (g_jit_reg_n == g_jit_reg_cap) { g_jit_reg_cap = g_jit_reg_cap ? g_jit_reg_cap * 2 : 65536; g_jit_reg = realloc(g_jit_reg, g_jit_reg_cap * sizeof(JReg)); }
    g_jit_reg[g_jit_reg_n++] = (JReg){start, (uint32_t)(j.p - start), (uint32_t)n, pc, precise, j.sites, (uint32_t)j.nsites};
    if (g_jit_map) fprintf(g_jit_map, "%llx %u %llx %d\n", (unsigned long long)start, (unsigned)(j.p - start), (unsigned long long)pc, n);
    if (g_jit_dump) {   // REBAR_JITDUMP=<file>: "pc n bytes guest-words... | x86-bytes" for every block (analysis aid)
        fprintf(g_jit_dump, "%llx %d %u", (unsigned long long)pc, n, (unsigned)(j.p - start));
        for (int k = 0; k < n; k++) fprintf(g_jit_dump, " %08x", *(uint32_t *)(pc + 4 * k));
        fprintf(g_jit_dump, " @"); for (int k = 0; k < n && k < 64; k++) fprintf(g_jit_dump, "%u,", g_jit_offs[k]);
        fprintf(g_jit_dump, " |"); for (uint8_t *q = start; q < j.p; q++) fprintf(g_jit_dump, "%02x", *q);
        fprintf(g_jit_dump, "\n");
    }
    // publish
    jit_insert(pc, start);
    JBlk *b = jpool_alloc(1, sizeof *b); b->pc = pc; b->bytes = (uint32_t)(n * 4); b->exit_off = exit_off; b->code = start;
    unsigned ph = (unsigned)((pc >> 12) & ((1 << JIT_PAGE_BITS) - 1));
    b->next = g_jit_pages[ph]; g_jit_pages[ph] = b;
    // chain the blocks that were waiting for this one
    unsigned qh = (unsigned)((pc >> 2) * 0x9E3779B97F4A7C15ull >> (64 - JIT_PEND_BITS));
    JPend **pp = &g_jit_pend[qh];
    while (*pp) {
        JPend *w = *pp;
        if (w->target == pc) {
            int32_t d = (int32_t)(start - (w->site + 5));
            __atomic_store_n((int32_t *)(w->site + 1), d, __ATOMIC_RELEASE);
            *pp = w->next; jpool_free(0, w);
        } else pp = &w->next;
    }
    t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu);
    if (t_jit_npend) jit_run_pending();
    return start;
}

// The guest changed or unmapped code in [lo, hi): forget the blocks translated from it.
static void jit_invalidate(uint64_t lo, uint64_t hi) {
    if (!g_jit_on || hi <= lo) return;
    if (t_jit_busy) {   // asked for by a guest signal handler that interrupted this thread inside the translator: after it
        if (t_jit_npend < 8) { t_jit_pend[t_jit_npend].lo = lo; t_jit_pend[t_jit_npend].hi = hi; t_jit_npend++; } else { t_jit_pend[7].lo = 0; t_jit_pend[7].hi = ~0ull; }
        return;
    }
    pthread_mutex_lock(&g_jit_mu); t_jit_busy = true;
    g_ji_calls++;
    // a huge range (a reservation of gigabytes) is cheaper to answer by looking at every bucket than at every page in it
    bool all = ((hi - 1) >> 12) - (lo >> 12) >= (1u << JIT_PAGE_BITS);
    uint64_t first = all ? 0 : lo >> 12, last = all ? (1u << JIT_PAGE_BITS) - 1 : (hi - 1) >> 12;
    for (uint64_t page = first; page <= last; page++) {
        JBlk **pp = &g_jit_pages[page & ((1 << JIT_PAGE_BITS) - 1)];
        while (*pp) {
            JBlk *b = *pp;
            if ((all || (b->pc >> 12) == page) && b->pc < hi && b->pc + b->bytes > lo) {
                JEnt *e = jit_probe(b->pc);
                if (e && e->code == b->code) __atomic_store_n(&e->pc, 1, __ATOMIC_SEQ_CST);        // never matches a real pc; the probe chain stays intact
                uint8_t **ic = &g_jit_ic[(b->pc >> 2) & ((1 << JIT_IC_BITS) - 1)];
                if (*ic == b->code) __atomic_store_n(ic, NULL, __ATOMIC_SEQ_CST);
                // the entry of a dead block goes back to the dispatcher (chained jumps may still arrive here): it jumps to the sequence
                // that leaves with c->pc = the block's pc
                uint8_t *exitseq = b->code + b->exit_off;
                int32_t jd = (int32_t)(exitseq - (b->code + 5));
                uint8_t patch[5] = {0xE9}; memcpy(patch + 1, &jd, 4);
                memcpy(b->code, patch, 5);
                *pp = b->next; jpool_free(1, b); g_ji_killed++;
                continue;
            }
            pp = &b->next;
        }
    }
    t_jit_busy = false; pthread_mutex_unlock(&g_jit_mu);
    if (t_jit_npend) jit_run_pending();
}

static inline int jit_run(CPU *c, uint8_t *code) {
    c->jit_budget = JIT_BUDGET;
    ((void (*)(CPU *, uint8_t *))g_jit_enter_thunk)(c, code);
    int left = c->jit_budget;
    if (__builtin_expect(g_jit_count, 0)) { g_jr_runs++; if (c->pc - g_stub_base < g_stub_size) g_jr_stub++; else if (left < 0) g_jr_budget++; else g_jr_other++; }
    return left < 0 ? JIT_BUDGET : JIT_BUDGET - left;
}

// Called by the dispatcher at every control-flow join. True when translated code ran (c->pc has moved on).
static inline bool jit_try(CPU *c, uint64_t *ticks) {
    uint64_t pc = c->pc;
    JEnt *e = jit_probe(pc);
    uint8_t *code;
    if (e) { code = e->code; if (!code) return false; if (__builtin_expect(g_jit_count, 0)) g_jr_found++; }
    else {
        if (__builtin_expect(g_jit_count, 0)) g_jr_cold++;
        unsigned h = (unsigned)(((pc >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - JIT_HOT_BITS));
        if (g_jit_hot[h] < g_jit_threshold) { g_jit_hot[h]++; return false; }
        code = jit_compile(pc);
        if (!code) return false;
    }
    jit_ic_set(pc, code);
    *ticks += (uint64_t)jit_run(c, code);
    return true;
}

// ------------------------------------------------------------------------------------------------ start-up

static void jit_make_thunks(void) {
    JC j; memset(&j, 0, sizeof j);
    j.p = g_jit_top;
    g_jit_enter_thunk = j.p;
    e1(&j, 0x53); e1(&j, 0x55); e1(&j, 0x41); e1(&j, 0x54); e1(&j, 0x41); e1(&j, 0x55); e1(&j, 0x41); e1(&j, 0x56); e1(&j, 0x41); e1(&j, 0x57);   // push rbx, rbp, r12-r15
    mov_rr(&j, R15, RDI, 1);
    // two stack words: [rsp] = 1, which no guest pc equals (the bottom of the return-address stack), [rsp+8] = the hsp of an outer entry, restored when this one ends
    { const uint8_t sub16[] = {0x48, 0x83, 0xEC, 0x10}, set1[] = {0x48, 0xC7, 0x04, 0x24, 1, 0, 0, 0}, savehsp[] = {0x48, 0x89, 0x44, 0x24, 0x08}, lea8[] = {0x48, 0x8D, 0x44, 0x24, 0xF8},
              leaLim[] = {0x48, 0x8D, 0x84, 0x24, 0xF8, 0xFF, 0xFE, 0xFF};
      for (size_t q = 0; q < sizeof sub16; q++) e1(&j, sub16[q]);
      for (size_t q = 0; q < sizeof set1; q++) e1(&j, set1[q]);
      mov_rm(&j, RAX, R15, GHSP, 1); for (size_t q = 0; q < sizeof savehsp; q++) e1(&j, savehsp[q]);
      for (size_t q = 0; q < sizeof lea8; q++) e1(&j, lea8[q]); mov_mr(&j, R15, GHSP, RAX, 1);                              // hsp = rsp at the first instruction of the translated code (after the call below)
      for (size_t q = 0; q < sizeof leaLim; q++) e1(&j, leaLim[q]); mov_mr(&j, R15, GHLIM, RAX, 1); }                       // hlim = hsp - 64 KB
    rc_sync_in(&j);                                                                                                       // the pinned registers come from the CPU structure ...
    call_reg(&j, RSI);
    rc_sync_out(&j);                                                                                                      // ... and go back whenever translated code returns to the dispatcher
    { const uint8_t ldhsp[] = {0x48, 0x8B, 0x44, 0x24, 0x08}, add16[] = {0x48, 0x83, 0xC4, 0x10}, leaLim2[] = {0x48, 0x8D, 0x80, 0x00, 0x00, 0xFF, 0xFF};
      for (size_t q = 0; q < sizeof ldhsp; q++) e1(&j, ldhsp[q]);
      mov_mr(&j, R15, GHSP, RAX, 1);                                                                                        // (the outer entry's hsp again)
      for (size_t q = 0; q < sizeof leaLim2; q++) e1(&j, leaLim2[q]); mov_mr(&j, R15, GHLIM, RAX, 1);                        // lea rax, [rax - 0x10000]
      for (size_t q = 0; q < sizeof add16; q++) e1(&j, add16[q]); }
    e1(&j, 0x41); e1(&j, 0x5F); e1(&j, 0x41); e1(&j, 0x5E); e1(&j, 0x41); e1(&j, 0x5D); e1(&j, 0x41); e1(&j, 0x5C); e1(&j, 0x5D); e1(&j, 0x5B);   // pop r15..r12, rbp, rbx
    ret_(&j);
    while ((uintptr_t)j.p & 15) e1(&j, 0xCC);
    g_jit_exit_stub = j.p;
    mov_rm(&j, RSP, R15, GHSP, 1);   // (whatever frames the return-address stack holds are dropped)
    ret_(&j);
    while ((uintptr_t)j.p & 15) e1(&j, 0xCC);
    // indirect branch: look the target up in the direct-mapped cache, leave through the exit stub on a miss
    g_jit_ind_stub = j.p;
    mov_rm(&j, RAX, R15, GPC, 1);
    mov_rr(&j, RDX, RAX, 1); shift_ri(&j, 5, RDX, 2, 1); alu_ri(&j, 4, RDX, (1 << JIT_IC_BITS) - 1, 0); shift_ri(&j, 4, RDX, 3, 1);
    mov_ri(&j, RCX, (uint64_t)g_jit_ic); alu_rr(&j, 0x01, RDX, RCX, 1);          // rdx = &ic[hash]
    mov_rm(&j, RSI, RDX, 0, 1);                                                  // the block (or 0)
    alu_rr(&j, 0x85, RSI, RSI, 1);                                               // test rsi, rsi
    Fix miss1 = jcc_fwd(&j, CC_E);
    op_rm(&j, 1, 0, 0x3B, RAX, RSI, -8);                                         // cmp rax, [rsi-8]: is this block's guest pc the target?
    Fix miss2 = jcc_fwd(&j, CC_NE);
    mov_mr(&j, R8, 0, RSI, 1);                                                   // refill the calling site's cell
    jmp_reg(&j, RSI);
    fix_here(&j, miss1); fix_here(&j, miss2);
    mov_rm(&j, RSP, R15, GHSP, 1);
    ret_(&j);
    while ((uintptr_t)j.p & 7) e1(&j, 0xCC);
    e8(&j, 0); g_jit_dummy = j.p; e1(&j, 0xCC);                                  // the dummy block: pc 0, never entered
    g_jit_top = (uint8_t *)(((uintptr_t)j.p + 63) & ~63ull);
}

static void jit_init(void) {
    const char *e = getenv("REBAR_JIT");
    if (e && !strcmp(e, "0")) return;                      // REBAR_JIT=0: interpret only
    if (getenv("REBAR_TRACE_STUBS") || g_dbg) return;
    void *a = mmap(NULL, JIT_ARENA, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (a == MAP_FAILED) { fprintf(stderr, "rebar: cannot allocate the JIT arena\n"); return; }
    g_jit_arena = g_jit_top = a; g_jit_end = g_jit_arena + JIT_ARENA - (8ull << 20); g_jit_sink = g_jit_end; g_jit_cell_top = g_jit_end + 8; g_jit_cell_end = g_jit_arena + JIT_ARENA;
    g_jit_tab = mmap(NULL, sizeof(JEnt) << JIT_TAB_BITS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    g_jit_ic = mmap(NULL, sizeof(uint8_t *) << JIT_IC_BITS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    g_jit_hot = mmap(NULL, 1 << JIT_HOT_BITS, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (g_jit_tab == MAP_FAILED || g_jit_ic == MAP_FAILED || g_jit_hot == MAP_FAILED) return;
    if (getenv("REBAR_JIT_NOFLAGSKIP")) g_noflagskip = true;
    if (getenv("REBAR_JIT_NOFLAGSKIP_RANGE")) { char *e; g_noskip_lo = strtoull(getenv("REBAR_JIT_NOFLAGSKIP_RANGE"), &e, 16); if (*e == '-') g_noskip_hi = strtoull(e + 1, NULL, 16); }
    if (getenv("REBAR_JIT_FLAGPOISON")) g_flagpoison = true;
    if (getenv("REBAR_JIT_NOFP")) g_nofp = true;
    if (getenv("REBAR_JIT_BUDGET_ALL")) g_jit_budget_all = true;
    if (getenv("REBAR_JIT_NOLEAF")) g_jit_noleaf = true;
    if (getenv("REBAR_JIT_NOCALLSCAN")) g_fl_callscan = false;
    if (getenv("REBAR_JIT_RAS")) g_jit_ras = atoi(getenv("REBAR_JIT_RAS")) != 0;
    if (getenv("REBAR_JIT_ALIGN")) g_jit_align = (uintptr_t)atoi(getenv("REBAR_JIT_ALIGN"));
    if (getenv("REBAR_JIT_NODMBOPT")) g_nodmbopt = true;
    if (getenv("REBAR_JIT_MAXN")) g_jit_maxn = atoi(getenv("REBAR_JIT_MAXN"));
    if (getenv("REBAR_JIT_FLBUDGET")) g_fl_budget = atoi(getenv("REBAR_JIT_FLBUDGET"));
    if (getenv("REBAR_JIT_NOFLAGDEFER")) g_noflagdefer = true;
    if (getenv("REBAR_JIT_FLAGSKIP_MODE")) g_skipmode = atoi(getenv("REBAR_JIT_FLAGSKIP_MODE"));
    if (getenv("REBAR_JIT_FLAGSKIP")) { char *e; g_flagskip_lo = strtoull(getenv("REBAR_JIT_FLAGSKIP"), &e, 16); if (*e == '-') g_flagskip_hi = strtoull(e + 1, NULL, 16); }
    if (getenv("REBAR_JIT_FLAGDBG")) { g_fl_dbg = true; atexit(fl_dbg_dump); }
    if (getenv("REBAR_JITCOUNT")) { g_jit_count = true; g_jit_cnt = calloc(1u << 20, 8); }
    if (getenv("REBAR_JITMAP")) { char nm[PATH_MAX]; snprintf(nm, sizeof nm, "%s.%d", getenv("REBAR_JITMAP"), (int)getpid()); g_jit_map = fopen(nm, "w"); if (g_jit_map) setvbuf(g_jit_map, NULL, _IOLBF, 0); }
    if (getenv("REBAR_JITDUMP")) { char nm[PATH_MAX]; snprintf(nm, sizeof nm, "%s.%d", getenv("REBAR_JITDUMP"), (int)getpid()); g_jit_dump = fopen(nm, "w"); }
    jit_parse_ranges("REBAR_JIT_ONLY", &g_jit_nonly, g_jit_only); jit_parse_ranges("REBAR_JIT_SKIP", &g_jit_nskip, g_jit_skip);
    if (getenv("REBAR_JIT_THRESHOLD")) g_jit_threshold = (unsigned)atoi(getenv("REBAR_JIT_THRESHOLD"));
    if (g_jit_threshold > 250) g_jit_threshold = 250;
    g_have_f16c = __builtin_cpu_supports("f16c"); g_have_sse41 = __builtin_cpu_supports("sse4.1"); g_have_ssse3 = __builtin_cpu_supports("ssse3"); g_have_fma = __builtin_cpu_supports("fma") && __builtin_cpu_supports("avx"); g_have_avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("sse4.2") && g_have_ssse3 && g_have_sse41;
    {   // REBAR_JIT_PIN=<guest register numbers, comma separated | none>: the pinned registers (at most four, among x0..x28)
        const char *e = getenv("REBAR_JIT_PIN"); if (!e) e = "8,9,0";
        g_npin = 0;
        for (const char *q = e; *q && g_npin < 5;) { char *end; long v = strtol(q, &end, 10); if (end == q) break; if (v >= 0 && v <= 28) g_pin[g_npin++] = (uint8_t)v; q = *end == ',' ? end + 1 : end; }
    }
    jit_make_thunks();
    g_jit_on = true;
}
