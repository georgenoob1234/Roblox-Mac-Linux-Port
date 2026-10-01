#include <sys/event.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct timespec timeout={0,100000000};
    for(int wide=0;wide<2;wide++) {
        int q=kqueue();assert(q>=0);
        struct kevent ev={0},out={0};struct kevent64_s ev64={0},out64={0};
        ev.ident=42;ev.filter=EVFILT_USER;ev.flags=EV_ADD|EV_ENABLE|EV_CLEAR;
        ev64.ident=42;ev64.filter=EVFILT_USER;ev64.flags=ev.flags;
        assert((wide ? kevent64(q,&ev64,1,0,0,0,0) : kevent(q,&ev,1,0,0,0))==0);
        for(int i=0;i<3;i++) {
            ev.flags=EV_CLEAR;ev.fflags=NOTE_TRIGGER;ev64.flags=ev.flags;ev64.fflags=ev.fflags;
            assert((wide ? kevent64(q,&ev64,1,0,0,0,0) : kevent(q,&ev,1,0,0,0))==0);
            assert((wide ? kevent64(q,0,0,&out64,1,0,&timeout) : kevent(q,0,0,&out,1,&timeout))==1);
            assert((wide ? out64.ident : out.ident)==42);
            assert((wide ? kevent64(q,0,0,&out64,1,0,&timeout) : kevent(q,0,0,&out,1,&timeout))==0);
        }
        close(q);
    }
    puts("PASS kevent and kevent64 EV_CLEAR trigger, delivery, reset and retrigger");
}
