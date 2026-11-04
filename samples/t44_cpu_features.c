// The CPU feature names of an arm64 Mac that Qt, Rust and JavaScript engines ask for.
#include <stdio.h>
#include <sys/sysctl.h>
int main(void) {
    const char *names[] = {"hw.optional.neon", "hw.optional.armv8_crc32", "hw.optional.armv8_1_atomics", "hw.optional.arm.FEAT_AES", "hw.optional.arm.FEAT_CRC32", "hw.ncpu"};
    for (int i = 0; i < 6; i++) {
        int v = -1; size_t n = sizeof v;
        int r = sysctlbyname(names[i], &v, &n, NULL, 0);
        printf("%s: %s\n", names[i], r != 0 ? "(missing)" : (v > 0 ? "yes" : "no"));
    }
    return 0;
}
