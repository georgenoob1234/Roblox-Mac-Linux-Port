// Preserve Apple's other Darwin clocks; correct the MONOTONIC epoch at source.
// The included implementation retains its Apple Public Source License header.
#include <stdint.h>
extern uint64_t mach_boottime_usec(void);
#define clock_gettime trackb_original_clock_gettime
#define clock_gettime_nsec_np trackb_original_clock_gettime_nsec_np
#define clock_getres trackb_original_clock_getres
#include "clock_gettime.c"
#undef clock_gettime
#undef clock_gettime_nsec_np
#undef clock_getres
int clock_gettime(clockid_t clock,struct timespec *value) {
    if(clock!=CLOCK_MONOTONIC)return trackb_original_clock_gettime(clock,value);
    if(!value){errno=EFAULT;return -1;}
    // No dlsym or constructor: this must also work during dyld/libc startup.
    // Linux BOOTTIME includes suspend, matching Darwin MONOTONIC.
#ifdef TRACKA_NATIVE_CLOCK
    extern int tracka_linux_clock_gettime(int, struct timespec *) __attribute__((visibility("hidden")));
    long status = tracka_linux_clock_gettime(7, value);
#else
    long status;
    __asm__ volatile("syscall":"=a"(status):"a"(228L),"D"(7L),"S"(value):"rcx","r11","memory","cc");
#endif
    if(status<0){errno=status==-38?ENOSYS:(int)-status;return -1;}
    return 0;
}
uint64_t clock_gettime_nsec_np(clockid_t clock) {
    if(clock!=CLOCK_MONOTONIC)return trackb_original_clock_gettime_nsec_np(clock);
    struct timespec value;
    return clock_gettime(clock,&value)?0:(uint64_t)value.tv_sec*1000000000+value.tv_nsec;
}
