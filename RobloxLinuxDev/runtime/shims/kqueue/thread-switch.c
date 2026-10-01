// Darling's thread_switch ignores its option and always sleeps 1 ms. libdispatch,
// libmalloc and os_lock call it to yield while contended, so every contended
// spin became a millisecond stall. Follow XNU: depress options yield, wait
// options sleep for the requested time. No directed handoff to `thread`.
#include <mach/mach.h>
#include <mach/thread_switch.h>
#include <time.h>
// XNU values; the SDK keeps these options private.
#ifndef SWITCH_OPTION_DISPATCH_CONTENTION
#define SWITCH_OPTION_DISPATCH_CONTENTION 3
#define SWITCH_OPTION_OSLOCK_DEPRESS 4
#define SWITCH_OPTION_OSLOCK_WAIT 5
#endif

static long linux_syscall(long number, const void *first, void *second) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first), "S"(second) : "rcx", "r11", "memory");
    return result;
}
enum { linux_sched_yield = 24, linux_nanosleep = 35 };

kern_return_t thread_switch(mach_port_name_t thread, int option, mach_msg_timeout_t time) {
    (void)thread;
    struct timespec delay = {0, 0};
    switch (option) {
    case SWITCH_OPTION_NONE:
    case SWITCH_OPTION_DEPRESS:
    case SWITCH_OPTION_OSLOCK_DEPRESS:
        linux_syscall(linux_sched_yield, 0, 0);
        return KERN_SUCCESS;
    case SWITCH_OPTION_DISPATCH_CONTENTION: // time in microseconds
        delay.tv_sec = time / 1000000; delay.tv_nsec = (long)(time % 1000000) * 1000;
        break;
    case SWITCH_OPTION_WAIT:
    case SWITCH_OPTION_OSLOCK_WAIT:         // time in milliseconds
        delay.tv_sec = time / 1000; delay.tv_nsec = (long)(time % 1000) * 1000000;
        break;
    default:
        return KERN_INVALID_ARGUMENT;
    }
    linux_syscall(linux_nanosleep, &delay, 0);
    return KERN_SUCCESS;
}
