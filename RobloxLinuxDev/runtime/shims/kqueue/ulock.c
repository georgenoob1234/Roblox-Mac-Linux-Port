// Darwin's newer pthread implementation calls ulock_wait2 (nanoseconds), but
// Darling only implements ulock_wait (microseconds). Reuse its Linux futex path.
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <dlfcn.h>
enum {no_errno=0x01000000,cancel=0x00020000};
static uint64_t monotonic_ns(void);
// Darling's syscall glue turns its encoded NO_ERRNO return into errno+2048.
// Ask for ordinary errno, then implement the documented negative-error form.
int __ulock_wait(uint32_t operation,void *address,uint64_t value,uint32_t timeout) {
    static int (*original)(uint32_t,void *,uint64_t,uint32_t);
    int (*call)(uint32_t,void *,uint64_t,uint32_t)=__atomic_load_n(&original,__ATOMIC_ACQUIRE);
    if(!call){call=dlsym(RTLD_NEXT,"__ulock_wait");__atomic_store_n(&original,call,__ATOMIC_RELEASE);}
    int saved=errno;
    uint64_t start=(operation&cancel) && timeout?monotonic_ns():0;
    uint32_t remaining=timeout;
    for(;;) {
        if(operation&cancel)pthread_testcancel();
        // ponytail: Darwin cancellation cannot interrupt a Linux futex through
        // psynch. Poll only cancellation points at 100 ms; use a native cancel
        // wake hook if that idle wakeup cost becomes measurable.
        uint32_t chunk=(operation&cancel) && (!remaining || remaining>100000)?100000:remaining;
        int rc=call(operation&~no_errno,address,value,chunk),error=errno;
        if(operation&cancel)pthread_testcancel();
        if(rc<0 && error==ETIMEDOUT && (operation&cancel) && chunk!=remaining) {
            if(!timeout)continue;
            uint64_t elapsed=(monotonic_ns()-start)/1000;
            if(elapsed<timeout){remaining=timeout-elapsed;continue;}
        }
        if(operation&no_errno){errno=saved;return rc<0?-error:rc;}
        if(rc<0)errno=error;
        return rc;
    }
}
int __ulock_wake(uint32_t operation,void *address,uint64_t value) {
    static int (*original)(uint32_t,void *,uint64_t);
    int (*call)(uint32_t,void *,uint64_t)=__atomic_load_n(&original,__ATOMIC_ACQUIRE);
    if(!call){call=dlsym(RTLD_NEXT,"__ulock_wake");__atomic_store_n(&original,call,__ATOMIC_RELEASE);}
    int saved=errno,rc=call(operation&~no_errno,address,value),error=errno;
    if(operation&no_errno){errno=saved;return rc<0?-error:rc;}
    return rc;
}
static uint64_t monotonic_ns(void) {
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000000ull+now.tv_nsec;
}
int __ulock_wait2(uint32_t operation,void *address,uint64_t value,uint64_t timeout,uint64_t value2) {
    if(value2) {if(operation&no_errno)return -EINVAL;errno=EINVAL;return -1;}
    uint64_t start=timeout?monotonic_ns():0,remaining=timeout;
    for(;;) {
        if(operation&cancel)pthread_testcancel();
        uint64_t micros=remaining/1000+(remaining%1000!=0);
        uint32_t chunk=micros>UINT_MAX?UINT_MAX:(uint32_t)micros;
        int rc=__ulock_wait(operation|no_errno,address,value,chunk);
        if(operation&cancel)pthread_testcancel();
        if(rc==-ETIMEDOUT && micros>UINT_MAX) {
            uint64_t elapsed=monotonic_ns()-start;
            if(elapsed<timeout){remaining=timeout-elapsed;continue;}
        }
        if(rc<0 && !(operation&no_errno)){errno=-rc;return -1;}
        return rc;
    }
}
