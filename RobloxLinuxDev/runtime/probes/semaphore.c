// In-process Mach semaphores (shims/kqueue/semaphore.c): Mach semantics,
// libdispatch through them, timeout withdrawal, destroy, slot reuse, fork.
#include <assert.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/semaphore.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static semaphore_t a, b, shared;
static int done, successes;

static uint64_t now_ns(void) { return mach_absolute_time(); } // timebase 1/1 on Darling
static mach_timespec_t ms(unsigned value) { mach_timespec_t t = {value / 1000, (value % 1000) * 1000000}; return t; }

static void *pong(void *arg) {
    for (long i = 0; i < (long)arg; ++i) { assert(semaphore_wait(a) == KERN_SUCCESS); assert(semaphore_signal(b) == KERN_SUCCESS); }
    return NULL;
}
static void *consumer(void *arg) {
    for (long i = 0; i < (long)arg; ++i) assert(semaphore_wait(shared) == KERN_SUCCESS);
    return NULL;
}
static void *timed_consumer(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        kern_return_t result = semaphore_timedwait(shared, ms(1));
        assert(result == KERN_SUCCESS || result == KERN_OPERATION_TIMED_OUT);
        if (result == KERN_SUCCESS) __atomic_add_fetch(&successes, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
static void *terminated_waiter(void *unused) {
    (void)unused;
    assert(semaphore_wait(shared) == KERN_TERMINATED);
    return NULL;
}
static void *released_waiter(void *unused) {
    (void)unused;
    assert(semaphore_wait(shared) == KERN_SUCCESS);
    __atomic_add_fetch(&successes, 1, __ATOMIC_RELAXED);
    return NULL;
}
static void settle(void) { usleep(100000); }
static int custom_done;
static void *custom_stack_thread(void *unused) {
    (void)unused;
    usleep(50000);
    __atomic_store_n(&custom_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void) {
    alarm(50);
    setvbuf(stdout, NULL, _IONBF, 0);
    Dl_info info;
    assert(dladdr(dlsym(RTLD_DEFAULT, "semaphore_signal"), &info) && strstr(info.dli_fname, "librbxkqueue.dylib"));
    task_t self = mach_task_self();

    // libc sleeps wait on its own semaphore through __semwait_signal.
    uint64_t slept = now_ns();
    for (int i = 0; i < 5; ++i) assert(!usleep(20000));
    struct timespec request = {0, 20000000};
    for (int i = 0; i < 5; ++i) assert(!nanosleep(&request, NULL));
    slept = (now_ns() - slept) / 10;
    assert(slept >= 19500000 && slept < 40000000);
    // pthread_join of a custom-stack thread waits on a cached semaphore.
    static char stack[1 << 20] __attribute__((aligned(4096)));
    pthread_attr_t attributes; pthread_t custom;
    assert(!pthread_attr_init(&attributes) && !pthread_attr_setstack(&attributes, stack, sizeof(stack)));
    assert(!pthread_create(&custom, &attributes, custom_stack_thread, NULL));
    assert(!pthread_join(custom, NULL) && __atomic_load_n(&custom_done, __ATOMIC_ACQUIRE));

    // Counting, polling and timeouts.
    assert(semaphore_create(self, &a, SYNC_POLICY_FIFO, 2) == KERN_SUCCESS && (a & 0xff) == 0);
    assert(semaphore_wait(a) == KERN_SUCCESS && semaphore_wait(a) == KERN_SUCCESS);
    assert(semaphore_timedwait(a, ms(0)) == KERN_OPERATION_TIMED_OUT);
    uint64_t start = now_ns();
    assert(semaphore_timedwait(a, ms(20)) == KERN_OPERATION_TIMED_OUT);
    assert(now_ns() - start >= 19000000);
    mach_timespec_t bad = {0, 1000000000};
    assert(semaphore_timedwait(a, bad) == KERN_INVALID_VALUE);
    assert(semaphore_signal(a) == KERN_SUCCESS && semaphore_timedwait(a, ms(0)) == KERN_SUCCESS);
    assert(semaphore_signal_thread(a, MACH_PORT_NULL) == KERN_NOT_WAITING);

    // Ping-pong latency between two threads.
    assert(semaphore_create(self, &b, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
    long rounds = 50000; pthread_t thread;
    assert(!pthread_create(&thread, NULL, pong, (void *)rounds));
    start = now_ns();
    for (long i = 0; i < rounds; ++i) { assert(semaphore_signal(a) == KERN_SUCCESS); assert(semaphore_wait(b) == KERN_SUCCESS); }
    uint64_t round_trip = (now_ns() - start) / rounds;
    assert(!pthread_join(thread, NULL));

    // Every signal consumed exactly once by concurrent waiters.
    assert(semaphore_create(self, &shared, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
    pthread_t threads[8];
    for (int i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, consumer, (void *)25000L));
    for (int i = 0; i < 200000; ++i) assert(semaphore_signal(shared) == KERN_SUCCESS);
    for (int i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(semaphore_timedwait(shared, ms(0)) == KERN_OPERATION_TIMED_OUT);

    // Timed waiters withdraw without losing or duplicating a signal.
    for (int i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, timed_consumer, NULL));
    for (int i = 0; i < 20000; ++i) { assert(semaphore_signal(shared) == KERN_SUCCESS); if (!(i % 64)) usleep(50); }
    settle(); settle();
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    int left = 0;
    while (semaphore_timedwait(shared, ms(0)) == KERN_SUCCESS) ++left;
    assert(successes + left == 20000);

    // signal_all releases current waiters only.
    successes = 0;
    for (int i = 0; i < 4; ++i) assert(!pthread_create(&threads[i], NULL, released_waiter, NULL));
    settle();
    assert(semaphore_signal_all(shared) == KERN_SUCCESS);
    for (int i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    assert(successes == 4 && semaphore_signal_all(shared) == KERN_SUCCESS);
    assert(semaphore_timedwait(shared, ms(0)) == KERN_OPERATION_TIMED_OUT);

    // Destroy wakes waiters with KERN_TERMINATED and makes the name stale.
    assert(!pthread_create(&thread, NULL, terminated_waiter, NULL));
    settle();
    semaphore_t stale = shared;
    assert(semaphore_destroy(self, shared) == KERN_SUCCESS);
    assert(!pthread_join(thread, NULL));
    assert(semaphore_signal(stale) == KERN_INVALID_ARGUMENT && semaphore_destroy(self, stale) == KERN_INVALID_ARGUMENT);

    // Slots are recycled well past the table size.
    for (int i = 0; i < 10000; ++i) {
        semaphore_t s;
        assert(semaphore_create(self, &s, SYNC_POLICY_FIFO, 1) == KERN_SUCCESS);
        assert(semaphore_wait(s) == KERN_SUCCESS && semaphore_destroy(self, s) == KERN_SUCCESS);
    }

    // libdispatch semaphores and groups ride on the same calls.
    dispatch_semaphore_t ds = dispatch_semaphore_create(0);
    assert(dispatch_semaphore_wait(ds, dispatch_time(DISPATCH_TIME_NOW, 10000000)) != 0);
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_group_t group = dispatch_group_create();
    __block int ran = 0;
    for (int i = 0; i < 1000; ++i) dispatch_group_async(group, queue, ^{ __atomic_add_fetch(&ran, 1, __ATOMIC_RELAXED); dispatch_semaphore_signal(ds); });
    assert(!dispatch_group_wait(group, DISPATCH_TIME_FOREVER) && ran == 1000);
    for (int i = 0; i < 1000; ++i) assert(!dispatch_semaphore_wait(ds, DISPATCH_TIME_FOREVER));

    // A forked child keeps working semaphores.
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        semaphore_t s; assert(semaphore_create(self, &s, SYNC_POLICY_FIFO, 0) == KERN_SUCCESS);
        assert(semaphore_signal(s) == KERN_SUCCESS && semaphore_wait(s) == KERN_SUCCESS);
        _exit(0);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    printf("PASS in-process Mach semaphores: sleeps %.2f ms/20 ms, custom-stack join, counts, timeouts, %llu ns ping-pong, 200000 signals/8 waiters, timed withdrawal, signal_all, destroy, 10000 recycled slots, libdispatch, fork\n",
           slept / 1e6, (unsigned long long)round_trip);
    return 0;
}
