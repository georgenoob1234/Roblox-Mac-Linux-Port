#include <sys/event.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void *kevent_native,*kevent64_native;
static void *next_symbol(void **slot,const char *name) {
    int error=errno;
    void *result=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if(!result) {
        result=dlsym(RTLD_NEXT,name);
        // These libsystem_kernel entry points remain loaded for process life.
        // No once/constructor: loader reentry must never wait on itself.
        if(result)__atomic_store_n(slot,result,__ATOMIC_RELEASE);
    }
    errno=error;return result;
}

/* Darling's kevent_copyin_one skips EV_CLEAR-only modifications. EV_CLEAR is
 * already persistent on the registered knote; a trigger must still reach kn_modify. */
static void *normalize(const void *changes,int count,size_t stride) {
    void *copy=NULL;
    for(int i=0;i<count;i++) {
        const struct kevent *e=(const void *)((const char *)changes+i*stride);
        if(e->filter==EVFILT_USER && e->flags==EV_CLEAR && (e->fflags & NOTE_TRIGGER)) {
            if(!copy) {
                copy=malloc((size_t)count*stride);
                if(!copy) return (void *)-1;
                memcpy(copy,changes,(size_t)count*stride);
            }
            ((struct kevent *)((char *)copy+i*stride))->flags=0;
        }
    }
    return copy;
}
int kevent(int fd,const struct kevent *changes,int count,struct kevent *events,int capacity,const struct timespec *timeout) {
    int (*original)(int,const struct kevent *,int,struct kevent *,int,const struct timespec *)=next_symbol(&kevent_native,"kevent");
    if(!original){errno=ENOSYS;return -1;}
    void *copy=changes && count>0 ? normalize(changes,count,sizeof(*changes)) : NULL;
    if(copy==(void *)-1) {errno=ENOMEM;return -1;}
    int result=original(fd,copy ? copy : changes,count,events,capacity,timeout);
    int error=errno;free(copy);errno=error;return result;
}
int kevent64(int fd,const struct kevent64_s *changes,int count,struct kevent64_s *events,int capacity,unsigned flags,const struct timespec *timeout) {
    int (*original)(int,const struct kevent64_s *,int,struct kevent64_s *,int,unsigned,const struct timespec *)=next_symbol(&kevent64_native,"kevent64");
    if(!original){errno=ENOSYS;return -1;}
    void *copy=changes && count>0 ? normalize(changes,count,sizeof(*changes)) : NULL;
    if(copy==(void *)-1) {errno=ENOMEM;return -1;}
    int result=original(fd,copy ? copy : changes,count,events,capacity,flags,timeout);
    int error=errno;free(copy);errno=error;return result;
}
