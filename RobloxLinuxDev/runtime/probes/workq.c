#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
static dispatch_semaphore_t done;
static uintptr_t maximum;
static void job(void *unused) {
    (void)unused;
    char marker;
    uintptr_t used=(uintptr_t)pthread_get_stackaddr_np(pthread_self())-(uintptr_t)&marker;
    if(used>maximum)maximum=used;
    dispatch_semaphore_signal(done);
}
int main(void) {
    done=dispatch_semaphore_create(0);
    dispatch_queue_t queue=dispatch_get_global_queue(0,0);
    for(int i=0;i<1500;i++) {
        dispatch_async_f(queue,NULL,job);
        if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC)))return 3;
        if(maximum>65536){fprintf(stderr,"FAIL workqueue stack grew to %lu bytes at job %d\n",(unsigned long)maximum,i);return 2;}
        usleep(1000); // let the worker park, then exercise its reuse path
    }
    fprintf(stderr,"PASS 1500 workqueue reuse cycles, maximum stack depth %lu bytes\n",(unsigned long)maximum);
    return 0;
}
