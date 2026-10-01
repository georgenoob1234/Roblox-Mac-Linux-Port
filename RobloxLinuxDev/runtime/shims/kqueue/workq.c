#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>

static int (*original)(int,void *,int,int);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void resolve(void) {
    original=dlsym(RTLD_NEXT,"__workq_kernreturn");
    if(!original)abort();
}
extern void rbx_workq_restack(int,void *,int,int,void *,void *) __attribute__((noreturn));
int __workq_kernreturn(int operation,void *item,int affinity,int priority) {
    pthread_once(&once,resolve);
    // Ordinary workers have finished their job and never return from operation 4.
    // Darling's reuse jump leaves the old frames on the stack. Reclaim them here,
    // before parking, as the kernel would. Kevent returns can contain stack pointers
    // and deliberately keep their existing path.
    if(operation==4 && item==NULL && affinity==0 && priority==0)
        rbx_workq_restack(operation,item,affinity,priority,(void *)original,pthread_get_stackaddr_np(pthread_self()));
    return original(operation,item,affinity,priority);
}
