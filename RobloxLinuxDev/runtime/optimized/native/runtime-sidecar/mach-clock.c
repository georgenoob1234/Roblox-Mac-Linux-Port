// Native x86_64 only: preserve Darling mach_absolute_time's Linux clock-1 epoch.
#include <stdint.h>
#include <time.h>
#include <elfcalls.h>
#if !defined(__x86_64__)
#error mach clock adapter requires native x86_64
#endif

extern struct elf_calls *_elfcalls;
typedef int (*linux_clock_fn)(int, struct timespec *);
static linux_clock_fn monotonic_clock;

#ifndef TRACKA_CLOCK_TEST
__attribute__((constructor))
#endif
static void initialize_mach_clock(void) {
    if (!_elfcalls || !_elfcalls->dlopen || !_elfcalls->dlsym) return;
    void *vdso = _elfcalls->dlopen("linux-vdso.so.1");
    if (!vdso) return;
    // Keep the handle for process lifetime. Calls bypass the ELF bridge and do
    // not depend on host libc TLS. The kernel mapping remains valid after fork.
    linux_clock_fn clock = (linux_clock_fn)_elfcalls->dlsym(vdso, "__vdso_clock_gettime");
    if (clock) __atomic_store_n(&monotonic_clock, clock, __ATOMIC_RELEASE);
}

// Shared by both native Track A clock entry points. vDSO is safe with Darwin
// TLS; it does not enter host libc. Preserve raw negative Linux errors.
__attribute__((visibility("hidden")))
int tracka_linux_clock_gettime(int clock_id, struct timespec *value) {
    linux_clock_fn clock = __atomic_load_n(&monotonic_clock, __ATOMIC_ACQUIRE);
    if (clock && !clock(clock_id, value)) return 0;
    long status;
    __asm__ volatile("syscall" : "=a"(status)
        : "a"(228L), "D"((long)clock_id), "S"(value) : "rcx", "r11", "memory", "cc");
    return (int)status;
}

uint64_t mach_absolute_time(void) {
    struct timespec value;
    if (tracka_linux_clock_gettime(1, &value)) return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

// Darling's gettimeofday is a raw syscall behind its -O0 syscall glue
// (~650 ns); Linux answers it from the vDSO. Darwin's timeval has a 32-bit
// tv_usec, so convert rather than pass the Linux structure through.
// Time zone requests keep Darling's implementation.
#include <dlfcn.h>
#include <sys/time.h>
int gettimeofday(struct timeval *restrict value, void *restrict zone) {
    struct timespec now;
    if (value && !zone && !tracka_linux_clock_gettime(0, &now)) {
        value->tv_sec = now.tv_sec;
        value->tv_usec = (suseconds_t)(now.tv_nsec / 1000);
        return 0;
    }
    static int (*original)(struct timeval *restrict, void *restrict);
    int (*call)(struct timeval *restrict, void *restrict) = __atomic_load_n(&original, __ATOMIC_ACQUIRE);
    if (!call) {
        call = (int (*)(struct timeval *restrict, void *restrict))dlsym(RTLD_NEXT, "gettimeofday");
        __atomic_store_n(&original, call, __ATOMIC_RELEASE);
    }
    return call(value, zone);
}
