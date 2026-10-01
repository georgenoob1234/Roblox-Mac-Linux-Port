#include <time.h>
#include <stdint.h>
#include <errno.h>
// Darling's kern.boottime fallback subtracts rounded sysinfo.uptime from
// realtime on every query. Its changing epoch makes CLOCK_MONOTONIC jump.
// Route both ARM imports through one adapter. Do not interpose libc during
// dyld startup: resolving its original implementation there can deadlock.
static int trackb_clock_gettime(clockid_t clock,struct timespec *time) {
    if(clock==CLOCK_MONOTONIC) {
        if(!time){errno=EFAULT;return -1;}
        int result=trackb_boottime(time);
        if(result<0){errno=result==-38?ENOSYS:-result;return -1;}
        return 0;
    }
    return clock_gettime(clock,time);
}
static uint64_t trackb_clock_gettime_nsec_np(clockid_t clock) {
    if(clock==CLOCK_MONOTONIC) {
        struct timespec time;
        return trackb_clock_gettime(clock,&time)?0:(uint64_t)time.tv_sec*1000000000+time.tv_nsec;
    }
    return clock_gettime_nsec_np(clock);
}
