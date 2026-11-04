// A buffer sized for the cipher suites of the guest SDK (uint16_t on arm64) must not be overrun by the host framework (uint32_t on x86_64).
#include <Security/SecureTransport.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    SSLContextRef ctx = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    size_t n = 0, n2 = 0;
    OSStatus r1 = SSLGetNumberEnabledCiphers(ctx, &n);
    size_t cap = n;
    SSLCipherSuite *buf = malloc(cap * sizeof(SSLCipherSuite) + 256);
    memset(buf, 0xAB, cap * sizeof(SSLCipherSuite) + 256);
    n2 = cap;
    OSStatus r2 = SSLGetEnabledCiphers(ctx, buf, &n2);
    size_t bad = 0; unsigned char *g = (unsigned char *)buf + cap * sizeof(SSLCipherSuite);
    for (int i = 0; i < 256; i++) if (g[i] != 0xAB) bad++;
    printf("number=%zu (r=%d) get: n2=%zu (r=%d) bytes written past the buffer: %zu\n", n, (int)r1, n2, (int)r2, bad);
    return 0;
}
