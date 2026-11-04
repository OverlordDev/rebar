// Read-only ID registers that memset / cache-maintenance code reads with MRS (DCZID_EL0, CTR_EL0).  arm64 only: the test compares with a fixed text.
#include <stdint.h>
#include <stdio.h>
int main() {
    uint64_t dczid, ctr;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    printf("dczid dzp=%d\n", (int)((dczid >> 4) & 1));
    printf("ctr dminline=%d iminline=%d\n", (int)((ctr >> 16) & 15), (int)(ctr & 15));
    return 0;
}
