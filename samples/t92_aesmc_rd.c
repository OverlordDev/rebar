// AESMC / AESIMC with Rd != Rn (OpenSSL's 3-block CTR loop says `aesmc v17.16b, v18.16b`): the state is Rn. FIPS-197 MixColumns column vectors, and the inverse brings them back.
#include <stdio.h>
#include <string.h>
int main(void) {
    static const unsigned char in[16] = {0xdb,0x13,0x53,0x45, 0xf2,0x0a,0x22,0x5c, 0x01,0x01,0x01,0x01, 0xc6,0xc6,0xc6,0xc6};
    unsigned char out[16], back[16], junk[16]; memset(junk, 0x5a, 16);
    __asm__ volatile("ld1 {v18.16b}, [%2]\n ld1 {v17.16b}, [%3]\n aesmc v17.16b, v18.16b\n st1 {v17.16b}, [%0]\n aesimc v16.16b, v17.16b\n st1 {v16.16b}, [%1]\n"
                     : : "r"(out), "r"(back), "r"(in), "r"(junk) : "v16", "v17", "v18", "memory");
    for (int i = 0; i < 16; i++) printf("%02x", out[i]); printf("\n"); printf("roundtrip %s\n", memcmp(back, in, 16) ? "BAD" : "ok"); return 0; }
