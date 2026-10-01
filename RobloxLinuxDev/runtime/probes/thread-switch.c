// thread_switch (shims/kqueue/thread-switch.c): depress options yield instead
// of sleeping 1 ms, wait options still sleep, contended os_unfair_lock is exact.
#include <assert.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_switch.h>
#ifndef SWITCH_OPTION_DISPATCH_CONTENTION
#define SWITCH_OPTION_DISPATCH_CONTENTION 3
#define SWITCH_OPTION_OSLOCK_DEPRESS 4
#endif
#include <os/lock.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static uint64_t now_ns(void) { return mach_absolute_time(); } // timebase 1/1 on Darling
static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
static long counter;
static void *contend(void *unused) {
    (void)unused;
    for (int i = 0; i < 200000; ++i) { os_unfair_lock_lock(&lock); ++counter; os_unfair_lock_unlock(&lock); }
    return NULL;
}

int main(void) {
    Dl_info info;
    assert(dladdr(dlsym(RTLD_DEFAULT, "thread_switch"), &info) && strstr(info.dli_fname, "librbxkqueue.dylib"));
    uint64_t start = now_ns();
    for (int i = 0; i < 1000; ++i) assert(thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1) == KERN_SUCCESS);
    const double depress = (now_ns() - start) / 1000.0;
    assert(depress < 50000); // Darling's own version slept 1 ms per call
    for (int option = SWITCH_OPTION_NONE; option <= SWITCH_OPTION_OSLOCK_DEPRESS; option += SWITCH_OPTION_OSLOCK_DEPRESS)
        assert(thread_switch(MACH_PORT_NULL, option, 0) == KERN_SUCCESS);
    start = now_ns();
    assert(thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 20) == KERN_SUCCESS);
    const uint64_t wait = now_ns() - start;
    assert(wait >= 19000000);
    start = now_ns();
    assert(thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DISPATCH_CONTENTION, 500) == KERN_SUCCESS);
    const uint64_t contention = now_ns() - start;
    assert(contention >= 450000 && contention < 20000000);
    assert(thread_switch(MACH_PORT_NULL, 99, 0) == KERN_INVALID_ARGUMENT);
    pthread_t threads[4];
    start = now_ns();
    for (int i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, contend, NULL));
    for (int i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(counter == 800000);
    printf("PASS thread_switch: depress %.0f ns/call, wait 20 ms -> %.1f ms, contention 500 us -> %.2f ms, 4-thread unfair lock exact in %.1f ms\n",
           depress, wait / 1e6, contention / 1e6, (now_ns() - start) / 1e6);
    return 0;
}
