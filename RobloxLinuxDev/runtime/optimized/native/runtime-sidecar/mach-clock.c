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

// The rest of the mach clock family (libsyscall/wrappers/mach_continuous_time.c
// and mach_approximate_time.c). libsystem_kernel's own versions call its
// mach_absolute_time directly, so the override above never sees them and each
// call was a real clock_gettime system call (~380 ns instead of ~20 ns);
// clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) lands here. Same commpage fields
// and arithmetic as the originals, so the values and the epoch are unchanged.
#include <i386/cpu_capabilities.h>

uint64_t mach_continuous_time(void) {
    volatile uint64_t *base = (volatile uint64_t *)_COMM_PAGE_CONT_TIMEBASE;
    uint64_t before, absolute;
    do {
        before = *base;
        absolute = mach_absolute_time();
    } while (__builtin_expect(before != *base, 0));
    return absolute + before;
}

uint64_t mach_approximate_time(void) {
    if (*(volatile uint8_t *)_COMM_PAGE_APPROX_TIME_SUPPORTED) return *(volatile uint64_t *)_COMM_PAGE_APPROX_TIME;
    return mach_absolute_time();
}

uint64_t mach_continuous_approximate_time(void) {
    // No retry, as in the original: a slightly old base is within "approximate".
    uint64_t base = *(volatile uint64_t *)_COMM_PAGE_CONT_TIMEBASE;
    return base + mach_approximate_time();
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
