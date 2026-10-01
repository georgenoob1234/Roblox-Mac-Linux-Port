// Contended timed wait + notify, matching the game receiver/update wakeup pair.
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
extern int __ulock_wait2(uint32_t,void *,uint64_t,uint64_t,uint64_t);
struct pair {pthread_mutex_t mutex;pthread_cond_t changed;int turn;unsigned progress;};
static struct pair pairs[4];
static void expired(int sig) {
    char line[160];int n=snprintf(line,sizeof(line),"FAIL timed-wait wakeups stalled: %u %u %u %u\n",pairs[0].progress,pairs[1].progress,pairs[2].progress,pairs[3].progress);
    write(2,line,n);_exit(124);
}
struct job {struct pair *pair;int turn;};
static pthread_mutex_t cancelMutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cancelCond=PTHREAD_COND_INITIALIZER;
static unsigned cleanupRan;
static void cleanup(void *unused) {assert(!pthread_mutex_unlock(&cancelMutex));cleanupRan=1;}
static void *cancel_waiter(void *unused) {
    assert(!pthread_mutex_lock(&cancelMutex));
    pthread_cleanup_push(cleanup,0);
    for(;;)assert(!pthread_cond_wait(&cancelCond,&cancelMutex));
    pthread_cleanup_pop(1);
    return 0;
}
static void *exchange(void *context) {
    struct job *job=context;struct pair *p=job->pair;
    for(unsigned i=0;i<10000;i++) {
        assert(!pthread_mutex_lock(&p->mutex));
        while(p->turn!=job->turn) {
            struct timeval now;gettimeofday(&now,0);
            struct timespec until={now.tv_sec,now.tv_usec*1000+1000000};
            if(until.tv_nsec>=1000000000){until.tv_nsec-=1000000000;until.tv_sec++;}
            int rc=pthread_cond_timedwait(&p->changed,&p->mutex,&until);
            assert(rc==0 || rc==ETIMEDOUT);
        }
        p->turn=1-job->turn;__atomic_add_fetch(&p->progress,1,__ATOMIC_RELAXED);
        assert(!pthread_mutex_unlock(&p->mutex));
        assert(!pthread_cond_signal(&p->changed));
    }
    return 0;
}
int main(void) {
    signal(SIGALRM,expired);alarm(20);
    if(getenv("PTHREAD_MUTEX_USE_ULOCK") && !strcmp(getenv("PTHREAD_MUTEX_USE_ULOCK"),"1")) {
        uint32_t word=0;
        errno=EDOM;assert(__ulock_wait2(1|0x01000000,&word,0,1100000,0)==-ETIMEDOUT && errno==EDOM);
        errno=0;assert(__ulock_wait2(1,&word,0,1100000,0)==-1 && errno==ETIMEDOUT);
        assert(__ulock_wait2(1|0x01000000,&word,0,1,1)==-EINVAL);
        // A changed value must return immediately, even for a very long timeout.
        assert(__ulock_wait2(1|0x01000000,&word,1,UINT64_MAX,0)>=0);
        pthread_t canceled;void *result;
        assert(!pthread_create(&canceled,0,cancel_waiter,0));usleep(10000);
        assert(!pthread_cancel(canceled));assert(!pthread_join(canceled,&result));
        assert(result==PTHREAD_CANCELED && cleanupRan);
        puts("PASS nanosecond wait timeout/errno, changed-value wakeup, and cancellation cleanup");
    }
    pthread_t threads[8];struct job jobs[8];
    for(unsigned i=0;i<4;i++) {
        assert(!pthread_mutex_init(&pairs[i].mutex,0));assert(!pthread_cond_init(&pairs[i].changed,0));
        for(unsigned j=0;j<2;j++){jobs[i*2+j]=(struct job){&pairs[i],j};assert(!pthread_create(&threads[i*2+j],0,exchange,&jobs[i*2+j]));}
    }
    for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],0));
    alarm(0);
    for(unsigned i=0;i<4;i++){assert(pairs[i].progress==20000);assert(!pthread_cond_destroy(&pairs[i].changed));assert(!pthread_mutex_destroy(&pairs[i].mutex));}
    puts("PASS 80000 contended timed-wait notifications across four receiver/update pairs");
}
