// Debug-only: print a backtrace at every C++ throw, then let the throw proceed.
// Roblox dies with an uncaught std::bad_alloc before it opens its own log, so this is the only
// way to see where. Insert it with DYLD_INSERT_LIBRARIES; flat namespace makes our __cxa_throw
// the one everybody calls, and RTLD_NEXT gets us back to the real one.
extern int backtrace(void **, int);
extern void backtrace_symbols_fd(void *const *, int, int);
extern void *dlsym(void *, const char *);
extern long write(int, const void *, unsigned long);

typedef struct { const char *fname; void *fbase; const char *sname; void *saddr; } Dl_info;
extern int dladdr(const void *, Dl_info *);
struct DebugElfCalls {void *(*open)(const char*);int (*close)(void*);void *(*symbol)(void*,const char*);};
extern struct DebugElfCalls *_elfcalls;
static int (*host_dladdr)(const void*,Dl_info*);
__attribute__((constructor)) static void resolve_host_symbols(void) {
    if(_elfcalls){void *lib=_elfcalls->open("libc.so.6");if(lib)host_dladdr=_elfcalls->symbol(lib,"dladdr");}
}
static void describe_address(const void *address,Dl_info *info) {
    if(!dladdr(address,info) && host_dladdr)host_dladdr(address,info);
}
extern int snprintf(char *, unsigned long, const char *, ...);
extern char *strstr(const char *, const char *);
extern int strcmp(const char *, const char *);

typedef void (*throw_fn)(void *, void *, void *);

void __cxa_throw(void *ex, void *tinfo, void *dest) {
    static const char hdr[] = "=== __cxa_throw ===\n";
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info origin={0};dladdr(__builtin_return_address(0),&origin);
    const char *type=((const char **)tinfo)[1];
    char detail[512];
    write(2,detail,snprintf(detail,sizeof(detail),"THROW type=%s origin=%s + %p\n",type,origin.fname ? origin.fname : "?",(void*)((char*)__builtin_return_address(0)-(char*)origin.fbase)));
    // Read what() only for an exact standard exception emitted by our runtime.
    if(!strcmp(type,"St13runtime_error")) {
        const char *message=((const char *(*)(void *))(*(void ***)ex)[2])(ex);
        if((origin.fname && strstr(origin.fname,"libindium.dylib")) || strstr(message,"shader") || strstr(message,"Shader") || strstr(message,"Metal") || strstr(message,"render"))
            write(2,detail,snprintf(detail,sizeof(detail),"GRAPHICS ERROR: %s\n",message));
    }
    write(2, hdr, sizeof(hdr) - 1);
    backtrace_symbols_fd(bt, n, 2);
    ((throw_fn)dlsym((void *)-1L, "__cxa_throw"))(ex, tinfo, dest);
    __builtin_trap();
}


// Startup trace. Roblox brings up a Crashpad-style handler very early — a spawned helper plus a
// Mach exception port — and the whole Darling container dies around that point. These wrappers
// chain to the real function via RTLD_NEXT; they only report.
extern int snprintf(char *, unsigned long, const char *, ...);

#define CHAIN(name) static void *r; if (!r) r = dlsym((void *)-1L, name)

extern char *getenv(const char *);
extern char *strstr(const char *, const char *);

// RBX_NO_CRASH_HANDLER=1 makes the Crashpad handler spawn fail instead of deadlocking, to see
// whether Roblox treats "no handler" differently from "no crash database".
static int refuse(const char *path) {
    if (!path || !getenv("RBX_NO_CRASH_HANDLER")) return 0;
    return strstr(path, "RobloxCrashHandler") != 0;
}

static void say(const char *tag, const char *arg) {
    char b[512];
    write(2, b, snprintf(b, sizeof b, ">> %s%s%s\n", tag, arg ? " " : "", arg ? arg : ""));
}

int posix_spawn(int *pid, const char *path, const void *fa, const void *at,
                char *const av[], char *const ev[]) {
    CHAIN("posix_spawn"); say("posix_spawn", path);
    if (refuse(path)) return 2;                                /* ENOENT */
    return ((int (*)(int *, const char *, const void *, const void *, char *const[], char *const[]))r)
           (pid, path, fa, at, av, ev);
}

int posix_spawnp(int *pid, const char *path, const void *fa, const void *at,
                 char *const av[], char *const ev[]) {
    CHAIN("posix_spawnp"); say("posix_spawnp", path);
    if (refuse(path)) return 2;                                /* ENOENT */
    return ((int (*)(int *, const char *, const void *, const void *, char *const[], char *const[]))r)
           (pid, path, fa, at, av, ev);
}

int execvp(const char *path, char *const av[]) {
    CHAIN("execvp"); say("execvp", path);
    return ((int (*)(const char *, char *const[]))r)(path, av);
}

int fork(void) {
    CHAIN("fork"); say("fork", 0);
    return ((int (*)(void))r)();
}

int task_set_exception_ports(unsigned task, unsigned mask, unsigned port, int behavior, int flavor) {
    CHAIN("task_set_exception_ports"); say("task_set_exception_ports", 0);
    return ((int (*)(unsigned, unsigned, unsigned, int, int))r)(task, mask, port, behavior, flavor);
}

void exit(int code) {
    CHAIN("exit"); say("exit", 0);
    ((void (*)(int))r)(code);
    __builtin_trap();
}

void abort(void) {
    CHAIN("abort"); say("abort", 0);
    ((void (*)(void))r)();
    __builtin_trap();
}

// Nothing is printed when the client dies, and the whole Darling container goes with it.
// backtrace() returns nothing from inside a Darling signal handler, so read the faulting
// address straight out of the ucontext and symbolise it with dladdr.
//   ucontext: uc_mcontext at +48   |   mcontext64: __es (16 bytes) then __ss, rip at +144
extern void _exit(int);

struct sigact { void *handler; void *mask_and_flags[2]; };

static void *mainThread, *joinTarget;
extern void *pthread_self(void);
extern int pthread_kill(void *,int);
int pthread_join(void *thread, void **result) {
    CHAIN("pthread_join");
    if (pthread_self()==mainThread) joinTarget=thread;
    int rc=((int (*)(void *,void **))r)(thread,result);
    if (pthread_self()==mainThread) joinTarget=0;
    return rc;
}
static void onsig(int sig, void *info, void *uctx) {
    unsigned long mctx = *(unsigned long *)((char *)uctx + 48);
    void *fault = (void *)*(unsigned long *)(mctx + 8);
    void *rip = (void *)*(unsigned long *)(mctx + 144);
    Dl_info d = {0};
    char b[512];
    describe_address(rip, &d);
    write(2, b, snprintf(b, sizeof b, "!! signal %d at rip %p (%s + %p), fault addr %p\n",
                         sig, rip, d.fname ? d.fname : "?",
                         (void *)((char *)rip - (char *)(d.fbase ? d.fbase : 0)), fault));

    write(2,b,snprintf(b,sizeof b,"   rdi=%p rsi=%p r10=%p r11=%p\n",*(void **)(mctx+48),*(void **)(mctx+56),*(void **)(mctx+96),*(void **)(mctx+104)));

    // Optimized renderer frames omit RBP; symbolise potential return addresses
    // on the current stack as well (not a reconstructed call chain).
    unsigned long *rsp = (unsigned long *)*(unsigned long *)(mctx + 72);
    for (int i=0; i<48 && rsp; ++i) {
        Dl_info f={0}; describe_address((void *)rsp[i], &f);
        if(f.fname)write(2,b,snprintf(b,sizeof b,"   stack[%d] %s + %p %s\n",i,f.fname,
            (void *)(rsp[i]-(unsigned long)f.fbase),f.sname?f.sname:""));
    }

    // Walk the frame-pointer chain by hand; backtrace() comes back empty in a Darling handler.
    unsigned long *rbp = (unsigned long *)*(unsigned long *)(mctx + 64);
    for (int i = 0; i < 12 && rbp; i++) {
        void *ret = (void *)rbp[1];
        if (!ret) break;
        Dl_info f = {0};
        describe_address(ret, &f);
        write(2, b, snprintf(b, sizeof b, "   #%-2d %p  %s + %p  %s\n", i, ret,
                             f.fname ? f.fname : "?",
                             (void *)((char *)ret - (char *)(f.fbase ? f.fbase : 0)),
                             f.sname ? f.sname : ""));
        rbp = (unsigned long *)rbp[0];
    }
    if (sig==14 && joinTarget && pthread_self()==mainThread) {
        pthread_kill(joinTarget,14);return;
    }
    _exit(128 + sig);
}

extern int sigaction(int, const void *, void *);
extern unsigned alarm(unsigned);
extern long strtol(const char *,char **,int);

__attribute__((constructor)) static void install_sig(void) {
    static const int fatal[] = {4, 6, 7, 8, 10, 11};   // ILL ABRT EMT FPE BUS SEGV
    struct { void *h; unsigned mask; int flags; } sa = {(void *)onsig, 0, 0x0040 /* SA_SIGINFO */};
    for (unsigned i = 0; i < sizeof fatal / sizeof *fatal; i++) sigaction(fatal[i], &sa, 0);
    if (getenv("RBX_TRACE_TIMEOUT")) { mainThread=pthread_self(); sigaction(14, &sa, 0); long seconds=strtol(getenv("RBX_TRACE_TIMEOUT"),0,10); alarm(seconds>1 && seconds<=300 ? (unsigned)seconds : 15); }
}

// Network trace. Roblox's bundled libcurl 8.19 reports "HttpError: DnsResolve" while a probe in the
// same guest resolves the same host fine (probes/dns.c), so these report what the client itself
// asks the resolver and the socket layer, and what it gets back.
struct rbx_addrinfo { int ai_flags, ai_family, ai_socktype, ai_protocol; unsigned ai_addrlen;
                      char *ai_canonname; void *ai_addr; struct rbx_addrinfo *ai_next; };

/* getaddrinfo tracing moved out: shims/dns now owns that symbol (see shims/dns/dns.c). */

int connect(int fd, const void *addr, unsigned len) {
    CHAIN("connect");
    int rc = ((int (*)(int, const void *, unsigned))r)(fd, addr, len);
    const unsigned char *a = addr;
    char b[256];
    if (len >= 8 && a[1] == 2)      /* sockaddr_in: sa_len, sa_family=AF_INET, port, addr */
        write(2, b, snprintf(b, sizeof b, ">> connect fd=%d %u.%u.%u.%u:%u -> rc=%d\n",
                             fd, a[4], a[5], a[6], a[7], (unsigned)(a[2] << 8 | a[3]), rc));
    else
        write(2, b, snprintf(b, sizeof b, ">> connect fd=%d family=%u len=%u -> rc=%d\n",
                             fd, len ? a[1] : 0, len, rc));
    return rc;
}

struct event_trace { unsigned long ident; short filter; unsigned short flags; unsigned fflags; long data; void *udata; } __attribute__((packed));
int kevent(int fd,const struct event_trace *changes,int nc,struct event_trace *events,int ne,const void *timeout) {
    CHAIN("kevent");
    if (getenv("RBX_TRACE_TIMEOUT")) {
        for(int i=0;i<nc;i++) {
            char b[200];write(2,b,snprintf(b,sizeof b,"KEVENT fd=%d id=%lu filter=%d flags=%x fflags=%x data=%ld ne=%d\n",fd,changes[i].ident,changes[i].filter,changes[i].flags,changes[i].fflags,changes[i].data,ne));
        }
    }
    int result=((int (*)(int,const void *,int,void *,int,const void *))r)(fd,changes,nc,events,ne,timeout);
    if (getenv("RBX_TRACE_TIMEOUT") && (nc || result)) { char b[100];write(2,b,snprintf(b,sizeof b,"KEVENT RETURN fd=%d nc=%d ne=%d result=%d\n",fd,nc,ne,result)); }
    return result;
}
