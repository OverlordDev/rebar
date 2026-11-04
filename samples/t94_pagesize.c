// An arm64 program sees the 16 KiB pages of Apple Silicon everywhere, and the VM statistics are counted in them.
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
    int p = 0; size_t l = sizeof p; sysctlbyname("hw.pagesize", &p, &l, 0, 0);
    vm_size_t ps; host_page_size(mach_host_self(), &ps);
    vm_statistics64_data_t v; mach_msg_type_number_t c = HOST_VM_INFO64_COUNT; host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &c);
    uint64_t mem = 0; l = sizeof mem; sysctlbyname("hw.memsize", &mem, &l, 0, 0);
    printf("same %d\n", getpagesize() == p && p == (int)ps && p == (int)vm_page_size);
    printf("pages fit in memory %d\n", ((uint64_t)(v.free_count + v.active_count + v.inactive_count + v.wire_count)) * ps <= mem);
    return 0;
}
