// Run in the Darwin guest with librbxidentity inserted. Asks for the host's memory
// statistics the way the Roblox client does and works out the two "free memory" figures
// the client derives from them:
//   budget    = (free - speculative, not below 0) + purgeable + external pages
//               (client 0.747, the function that feeds "Harmony memory (MB): freeOs=")
//   reclaim   = free + inactive pages
// On a Mac both count the file cache. Both must therefore follow Linux's MemAvailable,
// not MemFree: a Linux machine that has been up for a while has little MemFree and
// most of its memory in the file cache.
#include <mach/mach.h>
#include <mach/host_info.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int meminfo(unsigned long long *free_kb, unsigned long long *available_kb) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) file = fopen("/Volumes/SystemRoot/proc/meminfo", "r");
    if (!file) return 0;
    char line[256]; int found = 0;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "MemFree: %llu kB", free_kb) == 1) found |= 1;
        if (sscanf(line, "MemAvailable: %llu kB", available_kb) == 1) found |= 2;
    }
    fclose(file);
    return found == 3;
}

int main(void) {
    // The client passes a count of 40 integers (160 bytes) and a zeroed buffer.
    union { vm_statistics64_data_t stats; integer_t raw[40]; } info;
    unsigned long long free1, available1, free2, available2;
    vm_size_t page = 0;
    host_t host = mach_host_self();
    if (host_page_size(host, &page) || !page) { puts("FAIL host_page_size"); return 1; }
    if (!meminfo(&free1, &available1)) { puts("FAIL cannot read /proc/meminfo"); return 1; }
    memset(&info, 0, sizeof info);
    mach_msg_type_number_t count = 40;
    kern_return_t result = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&info, &count);
    if (!meminfo(&free2, &available2)) { puts("FAIL cannot read /proc/meminfo"); return 1; }
    if (result) { printf("FAIL host_statistics64 returned %d\n", result); return 1; }

    const vm_statistics64_data_t *s = &info.stats;
    uint64_t rest = s->free_count > s->speculative_count ? s->free_count - s->speculative_count : 0;
    unsigned long long budget = (rest + s->purgeable_count + s->external_page_count) * (uint64_t)page / 1024;
    unsigned long long reclaim = ((uint64_t)s->free_count + s->inactive_count) * (uint64_t)page / 1024;
    unsigned long long pages = (uint64_t)s->free_count + s->active_count + s->inactive_count + s->wire_count;
    unsigned long long low = available1 < available2 ? available1 : available2;
    unsigned long long high = available1 > available2 ? available1 : available2;
    const unsigned long long slack = 64 * 1024; // kB; the two readings are microseconds apart
    printf("host MemFree %llu MB, MemAvailable %llu MB; client budget figure %llu MB, free+inactive %llu MB (count %u)\n",
           free1 / 1024, available1 / 1024, budget / 1024, reclaim / 1024, count);
    int failed = 0;
    if (budget + slack < low || budget > high + slack) {
        printf("FAIL the client's budget figure is %llu MB, MemAvailable is %llu MB\n", budget / 1024, low / 1024);
        failed = 1;
    }
    if (reclaim + slack < low || reclaim > high + slack) {
        printf("FAIL free+inactive is %llu MB, MemAvailable is %llu MB\n", reclaim / 1024, low / 1024);
        failed = 1;
    }
    // File-backed pages are a part of the pages in use, never more than all of them.
    if ((uint64_t)s->external_page_count > (uint64_t)s->active_count + s->inactive_count) {
        puts("FAIL more file-backed pages than active and inactive pages together");
        failed = 1;
    }
    if (pages * (uint64_t)page < (1ull << 30)) { puts("FAIL less than 1 GiB of memory reported"); failed = 1; }
    if (failed) return 1;
    if (high - (free1 < free2 ? free1 : free2) < 4 * slack)
        puts("note: MemFree and MemAvailable are within 256 MB of each other here; the check cannot tell them apart");
    printf("PASS host memory: the client's free-memory figures follow MemAvailable (%llu MB)\n", low / 1024);
    return 0;
}
