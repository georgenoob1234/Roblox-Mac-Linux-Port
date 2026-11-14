// Guest probe for madvise(MADV_FREE / MADV_FREE_REUSABLE / MADV_FREE_REUSE).
// An allocator that hands memory back with these calls (macOS malloc, and mimalloc
// built for macOS, which the Roblox client embeds) must really give the pages
// back to the host, and memory that is not private anonymous must keep its
// contents. Run by check-madvise.py.
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MiB = 1024 * 1024, SIZE = 64 * MiB };

// The guest runs natively on Linux: ask the host for this process's resident size.
static long host_call(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return result;
}
static long resident_kb(void) {
    char text[128];
    long fd = host_call(2, (long)"/proc/self/statm", 0, 0);
    if (fd < 0) return -1;
    long n = host_call(0, fd, (long)text, sizeof text - 1);
    host_call(3, fd, 0, 0);
    if (n <= 0) return -1;
    text[n] = 0;
    long size, resident;
    return sscanf(text, "%ld %ld", &size, &resident) == 2 ? resident * 4 : -1;
}

static int failures;
static void expect(int ok, const char *what) {
    if (!ok) { printf("FAIL %s\n", what); failures++; }
}
static int all_bytes(const unsigned char *p, size_t n, unsigned char a, unsigned char b) {
    for (size_t i = 0; i < n; i++) if (p[i] != a && p[i] != b) return 0;
    return 1;
}

// Touch SIZE bytes of private anonymous memory, give them back with `advice`,
// and report how many kB of resident memory went away.
static long released_by(int advice, const char *name) {
    unsigned char *p = mmap(0, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { expect(0, "mmap"); return 0; }
    memset(p, 0xA5, SIZE);
    long before = resident_kb();
    errno = 0;
    int result = madvise(p, SIZE, advice);
    long after = resident_kb();
    char what[96];
    snprintf(what, sizeof what, "%s returns 0 on private anonymous memory (got %d, errno %d)", name, result, errno);
    expect(result == 0, what);
    // Darwin leaves the contents undefined: old bytes or zeroes, never a fault.
    snprintf(what, sizeof what, "%s leaves old bytes or zeroes", name);
    expect(all_bytes(p, SIZE, 0xA5, 0), what);
    if (advice == MADV_FREE_REUSABLE) expect(madvise(p, SIZE, MADV_FREE_REUSE) == 0, "MADV_FREE_REUSE returns 0");
    memset(p, 0x5A, SIZE);
    snprintf(what, sizeof what, "memory is usable again after %s", name);
    expect(all_bytes(p, SIZE, 0x5A, 0x5A), what);
    munmap(p, SIZE);
    return before - after;
}

int main(void) {
    if (resident_kb() < 0) { puts("FAIL cannot read /proc/self/statm"); return 1; }
    long reusable = released_by(MADV_FREE_REUSABLE, "MADV_FREE_REUSABLE");
    long freed = released_by(MADV_FREE, "MADV_FREE");
    char what[128];
    snprintf(what, sizeof what, "MADV_FREE_REUSABLE releases the pages (%ld of %d kB)", reusable, SIZE / 1024);
    expect(reusable >= SIZE / 1024 * 9 / 10, what);
    snprintf(what, sizeof what, "MADV_FREE releases the pages (%ld of %d kB)", freed, SIZE / 1024);
    expect(freed >= SIZE / 1024 * 9 / 10, what);

    // Modified pages of a private file mapping and pages of shared memory are not
    // the allocator's to throw away: the contents must survive every advice.
    char path[] = "/tmp/rbx-madvise-probe.XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0 && ftruncate(fd, MiB) == 0, "temporary file");
    unsigned char *file = mmap(0, MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    unsigned char *shared = mmap(0, MiB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    expect(file != MAP_FAILED && shared != MAP_FAILED, "file and shared mappings");
    if (file != MAP_FAILED && shared != MAP_FAILED) {
        const int advices[] = { MADV_FREE, MADV_FREE_REUSABLE, MADV_FREE_REUSE };
        for (unsigned i = 0; i < sizeof advices / sizeof *advices; i++) {
            memset(file, 0xC3, MiB); memset(shared, 0x3C, MiB);
            madvise(file, MiB, advices[i]); madvise(shared, MiB, advices[i]);
            snprintf(what, sizeof what, "advice %d keeps a private file mapping's modified pages", advices[i]);
            expect(all_bytes(file, MiB, 0xC3, 0xC3), what);
            snprintf(what, sizeof what, "advice %d keeps shared anonymous memory", advices[i]);
            expect(all_bytes(shared, MiB, 0x3C, 0x3C), what);
        }
    }
    if (fd >= 0) { close(fd); unlink(path); }

    // Advice the host must never see under a different meaning: Darwin's values
    // 5..10 are other requests on Linux (5 = MADV_HWPOISON-range values, 8 = MADV_FREE,
    // 9 = MADV_REMOVE, which would punch holes).
    unsigned char *q = mmap(0, MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (q != MAP_FAILED) {
        memset(q, 0x77, MiB);
        expect(madvise(q, MiB, MADV_ZERO_WIRED_PAGES) == 0 && madvise(q, MiB, MADV_CAN_REUSE) == 0, "other Darwin-only advice still returns 0");
        expect(all_bytes(q, MiB, 0x77, 0x77), "other Darwin-only advice changes nothing");
        expect(madvise(q, MiB, MADV_DONTNEED) == 0 && madvise(q, MiB, MADV_WILLNEED) == 0, "MADV_DONTNEED and MADV_WILLNEED still work");
    }
    if (!failures) printf("PASS madvise: MADV_FREE_REUSABLE gave back %ld kB and MADV_FREE %ld kB of %d kB; "
                          "file-backed and shared memory keep their contents\n", reusable, freed, SIZE / 1024);
    return failures != 0;
}
