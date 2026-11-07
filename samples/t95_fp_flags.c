// FCMP / FCMPE flags and their consumers (CSEL / FCSEL / B.cond) for every ARM condition,
// with ordered and unordered operands.  Self-checking: prints "all ok" or the first mismatch.
//
// This is the regression test for the JIT's flag kind 4 (the FP comparison left in the x86
// flags).  Build for arm64 and run under rebar; the native x86_64 build is not needed.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;
static double vals[] = {-1.0, -0.0, 0.0, 1.0, 1.5, -1.5, INFINITY, -INFINITY, NAN};
#define NV ((int)(sizeof vals / sizeof vals[0]))

#define CHECK(cc, a, b) do { \
    uint64_t got; \
    __asm__ volatile("fcmp %d1, %d2\n\tcset %0, " #cc : "=r"(got) : "w"(a), "w"(b)); \
    int exp = cond_##cc(a, b); \
    if ((int)got != exp) { if (fails++ < 20) printf("fcmp %s a=%g b=%g got %d want %d\n", #cc, (a), (b), (int)got, exp); } \
} while (0)

static int unord(double a, double b) { return isnan(a) || isnan(b); }
static int cond_eq(double a, double b) { return !unord(a, b) && a == b; }
static int cond_ne(double a, double b) { return unord(a, b) || a != b; }
static int cond_cs(double a, double b) { return unord(a, b) || a >= b; }
static int cond_cc(double a, double b) { return !unord(a, b) && a < b; }
static int cond_mi(double a, double b) { return !unord(a, b) && a < b; }
static int cond_pl(double a, double b) { return unord(a, b) || a >= b; }
static int cond_vs(double a, double b) { return unord(a, b); }
static int cond_vc(double a, double b) { return !unord(a, b); }
static int cond_hi(double a, double b) { return unord(a, b) || a > b; }
static int cond_ls(double a, double b) { return !unord(a, b) && a <= b; }
static int cond_ge(double a, double b) { return !unord(a, b) && a >= b; }
static int cond_lt(double a, double b) { return unord(a, b) || a < b; }
static int cond_gt(double a, double b) { return !unord(a, b) && a > b; }
static int cond_le(double a, double b) { return unord(a, b) || a <= b; }

#define CHECK_ALL(a, b) do { \
    CHECK(eq, a, b); CHECK(ne, a, b); CHECK(cs, a, b); CHECK(cc, a, b); \
    CHECK(mi, a, b); CHECK(pl, a, b); CHECK(vs, a, b); CHECK(vc, a, b); \
    CHECK(hi, a, b); CHECK(ls, a, b); CHECK(ge, a, b); CHECK(lt, a, b); \
    CHECK(gt, a, b); CHECK(le, a, b); \
} while (0)

// FCSEL picks rn when the condition holds, rm otherwise.
#define CHECK_SEL(cc, a, b) do { \
    double got; \
    __asm__ volatile("fcmp %d1, %d2\n\tfcsel %d0, %d1, %d2, " #cc : "=w"(got) : "w"(a), "w"(b)); \
    double want = cond_##cc(a, b) ? (a) : (b); \
    if (memcmp(&got, &want, 8)) { if (fails++ < 20) printf("fcsel %s a=%g b=%g got %g want %g\n", #cc, (a), (b), got, want); } \
} while (0)

#define CHECK_SEL_ALL(a, b) do { \
    CHECK_SEL(eq, a, b); CHECK_SEL(ne, a, b); CHECK_SEL(cs, a, b); CHECK_SEL(cc, a, b); \
    CHECK_SEL(mi, a, b); CHECK_SEL(pl, a, b); CHECK_SEL(vs, a, b); CHECK_SEL(vc, a, b); \
    CHECK_SEL(hi, a, b); CHECK_SEL(ls, a, b); CHECK_SEL(ge, a, b); CHECK_SEL(lt, a, b); \
    CHECK_SEL(gt, a, b); CHECK_SEL(le, a, b); \
} while (0)

// B.cond consumes the same flags through a side exit / deferred store, not through CSEL.
#define CHECK_BR(cc, a, b) do { \
    int got; \
    __asm__ volatile("fcmp %d1, %d2\n\tb." #cc " 1f\n\tmov %w0, #0\n\tb 2f\n1:\tmov %w0, #1\n2:" \
                     : "=r"(got) : "w"(a), "w"(b) : "cc"); \
    int exp = cond_##cc(a, b); \
    if (got != exp) { if (fails++ < 20) printf("b.%s a=%g b=%g got %d want %d\n", #cc, (a), (b), got, exp); } \
} while (0)

// A branch whose fall-through still reads the comparison flags (the taken path does not):
// this is the "deferred flags" path, where cond_cc's FP sequence must not destroy the flags
// before the fall-through consumer reads them.
static int defer_gt(double a, double b) {
    int r;
    __asm__ volatile("fcmp %d1, %d2\n\t"
                     "b.gt 1f\n\t"
                     "cset %w0, eq\n\t"
                     "b 2f\n"
                     "1:\tmov %w0, #0\n"
                     "2:"
                     : "=r"(r) : "w"(a), "w"(b) : "cc");
    return r;
}

int main(void) {
    for (int i = 0; i < NV; i++) for (int j = 0; j < NV; j++) {
        double a = vals[i], b = vals[j];
        CHECK_ALL(a, b);
        CHECK_SEL_ALL(a, b);
        CHECK_BR(eq, a, b); CHECK_BR(ne, a, b); CHECK_BR(cs, a, b); CHECK_BR(cc, a, b);
        CHECK_BR(mi, a, b); CHECK_BR(pl, a, b); CHECK_BR(vs, a, b); CHECK_BR(vc, a, b);
        CHECK_BR(hi, a, b); CHECK_BR(ls, a, b); CHECK_BR(ge, a, b); CHECK_BR(lt, a, b);
        CHECK_BR(gt, a, b); CHECK_BR(le, a, b);
        int dg = defer_gt(a, b), de = cond_gt(a, b) ? 0 : cond_eq(a, b);
        if (dg != de) { if (fails++ < 20) printf("defer.gt a=%g b=%g got %d want %d\n", a, b, dg, de); }
    }
    // FCMP against zero: the #0.0 form.
    for (int i = 0; i < NV; i++) {
        double a = vals[i];
        uint64_t got;
        __asm__ volatile("fcmp %d1, #0.0\n\tcset %0, eq" : "=r"(got) : "w"(a));
        if ((int)got != (!unord(a, 0.0) && a == 0.0)) { if (fails++ < 20) printf("fcmp zero eq a=%g got %d\n", a, (int)got); }
        __asm__ volatile("fcmp %d1, #0.0\n\tcset %0, ge" : "=r"(got) : "w"(a));
        if ((int)got != (!unord(a, 0.0) && a >= 0.0)) { if (fails++ < 20) printf("fcmp zero ge a=%g got %d\n", a, (int)got); }
        __asm__ volatile("fcmp %d1, #0.0\n\tcset %0, mi" : "=r"(got) : "w"(a));
        if ((int)got != (!unord(a, 0.0) && a < 0.0)) { if (fails++ < 20) printf("fcmp zero mi a=%g got %d\n", a, (int)got); }
    }
    if (fails) printf("FAILED %d\n", fails); else printf("all ok\n");
    return fails != 0;
}
