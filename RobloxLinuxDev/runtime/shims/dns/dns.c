// Linux resolver bridge; legacy gethostbyname remains serialized below.
extern void *dlsym(void *, const char *);
extern int pthread_mutex_lock(void *);
extern int pthread_mutex_unlock(void *);
extern char *getenv(const char *);
extern int snprintf(char *,unsigned long,const char *,...);
extern long write(int,const void *,unsigned long);
struct timespec {long seconds,nanoseconds;};
extern int clock_gettime(int,struct timespec *);
static double monotonic(void) {struct timespec t;clock_gettime(8 /* Darwin CLOCK_UPTIME_RAW */,&t);return t.seconds+t.nanoseconds/1e9;}

/* Darwin's pthread_mutex_t: 8-byte signature + 56 opaque bytes, PTHREAD_MUTEX_INITIALIZER sig. */
typedef struct { long sig; char opaque[56]; } darwin_mutex;
static darwin_mutex lookup_lock = { 0x32AAABA7L, {0} };

#define REAL(name) static void *fn; if (!fn) fn = dlsym((void *)-1L, name)

// glibc uses the host resolver and its thread-safe NSS implementation. Darling
// can return EAI_NONAME even when the same host lookup succeeds immediately.
// Copy results into Darwin allocations so its existing freeaddrinfo owns them.
struct darwin_addrinfo { int flags,family,socktype,protocol; unsigned addrlen;
    char *canonname; void *addr; struct darwin_addrinfo *next; };
struct linux_addrinfo { int flags,family,socktype,protocol; unsigned addrlen;
    void *addr; char *canonname; struct linux_addrinfo *next; };
struct ElfCalls {void *(*dlopen)(const char *);int (*dlclose)(void *);void *(*dlsym)(void *,const char *);};
extern struct ElfCalls *_elfcalls;
extern void *calloc(unsigned long,unsigned long),*malloc(unsigned long),*memcpy(void *,const void *,unsigned long);
extern char *strdup(const char *);
extern void freeaddrinfo(struct darwin_addrinfo *);
extern void dispatch_once_f(long *,void *,void (*)(void *));
static int (*host_lookup)(const char *,const char *,const struct linux_addrinfo *,struct linux_addrinfo **);
static void (*host_free)(struct linux_addrinfo *);
static void load_host_resolver(void *unused) {
    (void)unused;
    if(!_elfcalls)return;
    void *libc=_elfcalls->dlopen("libc.so.6");
    if(!libc)return;
    host_lookup=_elfcalls->dlsym(libc,"getaddrinfo");
    host_free=_elfcalls->dlsym(libc,"freeaddrinfo");
}
int getaddrinfo(const char *node,const char *service,const struct darwin_addrinfo *hints,struct darwin_addrinfo **result) {
    static long once;
    dispatch_once_f(&once,0,load_host_resolver);
    int flags=hints ? hints->flags : 0;
    int family=hints ? hints->family : 0;
    // Preserve unsupported Darwin extensions through the original implementation.
    if(!host_lookup || !host_free || (flags & ~0x1f07) || (family!=0 && family!=2 && family!=30)) {
        REAL("getaddrinfo");
        pthread_mutex_lock(&lookup_lock);
        int rc=((int (*)(const char *,const char *,const void *,void **))fn)(node,service,hints,(void **)result);
        pthread_mutex_unlock(&lookup_lock);
        return rc;
    }
    if(!result)return 4;
    *result=0;
    struct linux_addrinfo request={0},*native=0;
    request.flags=(flags&7) | ((flags&0xa00)?8:0) | ((flags&0x100)?16:0) |
                  ((flags&0x400)?32:0) | ((flags&0x1000)?0x400:0);
    request.family=family==30 ? 10 : family;
    request.socktype=hints ? hints->socktype : 0;
    request.protocol=hints ? hints->protocol : 0;
    int timing=getenv("RBX_DNS_TIMING")!=0;
    double start=timing ? monotonic() : 0;
    int rc=host_lookup(node,service,&request,&native);
    if(timing){char line[512];int n=snprintf(line,sizeof(line),"DNS host lookup=%.1fms rc=%d host=%.253s\n",(monotonic()-start)*1000,rc,node ? node : "(null)");if(n>0 && n<(int)sizeof(line))write(2,line,n);}
    if(rc) {
        // Linux EAI_* values are negative, Darwin's are positive and reordered.
        static const int errors[]={0,3,8,2,4,7,5,10,9,1,6,4,14};
        return rc<0 && rc>=-12 ? errors[-rc] : 4;
    }
    struct darwin_addrinfo **tail=result;
    for(struct linux_addrinfo *a=native;a;a=a->next) {
        if((a->family!=2 && a->family!=10) || !a->addr || a->addrlen!=(a->family==2 ? 16u : 28u)){rc=4;break;}
        struct darwin_addrinfo *copy=calloc(1,sizeof(*copy));
        if(!copy){rc=6;break;}
        *tail=copy;tail=&copy->next;
        copy->flags=flags;copy->family=a->family==10 ? 30 : 2;
        copy->socktype=a->socktype;copy->protocol=a->protocol;copy->addrlen=a->addrlen;
        copy->addr=malloc(a->addrlen);
        if(a->canonname)copy->canonname=strdup(a->canonname);
        if(!copy->addr || (a->canonname && !copy->canonname)){rc=6;break;}
        memcpy(copy->addr,a->addr,a->addrlen);
        ((unsigned char *)copy->addr)[0]=a->addrlen;
        ((unsigned char *)copy->addr)[1]=copy->family;
    }
    host_free(native);
    if(rc){freeaddrinfo(*result);*result=0;}
    return rc;
}

void *gethostbyname(const char *name) {
    REAL("gethostbyname");
    pthread_mutex_lock(&lookup_lock);
    void *h = ((void *(*)(const char *))fn)(name);
    pthread_mutex_unlock(&lookup_lock);
    return h;
}
