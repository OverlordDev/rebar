// A callback the host calls with eleven arguments: the last three (rdata, ttl, context) are on the stack of an arm64 callee but come from the stack of an x86 caller.
// mDNSResponder calls it from DNSServiceProcessResult, as Bun's resolver does.
#include <dns_sd.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
static int done, calls;
static void cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifindex, DNSServiceErrorType err, const char *name, uint16_t rrtype, uint16_t rrclass,
               uint16_t rdlen, const void *rdata, uint32_t ttl, void *ctx) {
    calls++;
    if (err == kDNSServiceErr_NoError && rrtype == kDNSServiceType_A && rdata && rdlen == 4 && ctx == (void *)0x1234abcd && ttl < 1000000) {
        const unsigned char *a = rdata;
        printf("answer %s %u.%u.%u.%u rrclass %u\n", name, a[0], a[1], a[2], a[3], rrclass);
        done = 1;
    } else if (err != kDNSServiceErr_NoError && calls > 3) done = -1;
    else if (err == kDNSServiceErr_NoError) printf("bad callback: type %u rdlen %u rdata %p ctx %p ttl %u\n", rrtype, rdlen, rdata, ctx, ttl), done = -1;
}
int main() {
    DNSServiceRef ref;
    if (DNSServiceCreateConnection(&ref) != kDNSServiceErr_NoError) { puts("no mDNSResponder"); return 1; }
    DNSServiceRef q = ref;
    if (DNSServiceQueryRecord(&q, kDNSServiceFlagsShareConnection | kDNSServiceFlagsForceMulticast * 0, 0, "localhost.", kDNSServiceType_A, kDNSServiceClass_IN, cb, (void *)0x1234abcd) != kDNSServiceErr_NoError) { puts("query failed"); return 1; }
    int fd = DNSServiceRefSockFD(ref);
    for (int i = 0; i < 40 && !done; i++) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) DNSServiceProcessResult(ref);
    }
    puts(done > 0 ? "ok" : "no good answer");
    return 0;
}
