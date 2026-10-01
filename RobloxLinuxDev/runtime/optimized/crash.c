// Preserve the original Darwin fault before Crashpad/container shutdown loses it.
// Only registers, stack addresses and module mappings are recorded; no game URLs.
#include <signal.h>
#include <sys/ucontext.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct CrashElfCalls {void *(*open)(const char *);int (*close)(void *);void *(*symbol)(void *,const char *);};
extern struct CrashElfCalls *_elfcalls;
struct CrashIOVec {void *base;size_t length;};
static int report=-1;
static int (*hostOpen)(const char *,int,...),(*hostClose)(int),(*hostPid)(void);
static long (*hostRead)(int,void *,size_t),(*hostWrite)(int,const void *,size_t);
static long (*hostReadMemory)(int,const struct CrashIOVec *,unsigned long,const struct CrashIOVec *,unsigned long,unsigned long);
static struct sigaction previous[32];
static volatile sig_atomic_t reporting;
static void put(const char *bytes,size_t length) {
    while(length){long n=hostWrite(report,bytes,length);if(n<=0)return;bytes+=n;length-=n;}
}
static void number(const char *name,uintptr_t value) {
    char buffer[96];size_t n=0;while(*name&&n<60)buffer[n++]=*name++;
    buffer[n++]='=';buffer[n++]='0';buffer[n++]='x';int started=0;
    for(int shift=60;shift>=0;shift-=4){unsigned digit=(value>>shift)&15;if(digit||started||!shift){buffer[n++]="0123456789abcdef"[digit];started=1;}}
    buffer[n++]='\n';put(buffer,n);
}
static int readMemory(uintptr_t address,void *bytes,size_t length) {
    struct CrashIOVec local={bytes,length},remote={(void *)address,length};
    return hostReadMemory(hostPid(),&local,1,&remote,1,0)==(long)length;
}
static void recordFault(int sig,siginfo_t *info,void *context) {
    if(!__sync_lock_test_and_set(&reporting,1)) {
        ucontext_t *uc=context;
        put("\nTRACKA ORIGINAL FAULT\n",23);number("pid",hostPid());number("signal",sig);
        number("fault",(uintptr_t)info->si_addr);
        number("rip",uc->uc_mcontext->__ss.__rip);number("rsp",uc->uc_mcontext->__ss.__rsp);number("rbp",uc->uc_mcontext->__ss.__rbp);
        number("rax",uc->uc_mcontext->__ss.__rax);number("rbx",uc->uc_mcontext->__ss.__rbx);
        number("rdi",uc->uc_mcontext->__ss.__rdi);number("rsi",uc->uc_mcontext->__ss.__rsi);
        number("rdx",uc->uc_mcontext->__ss.__rdx);number("rcx",uc->uc_mcontext->__ss.__rcx);
        number("r8",uc->uc_mcontext->__ss.__r8);number("r9",uc->uc_mcontext->__ss.__r9);
        number("r10",uc->uc_mcontext->__ss.__r10);number("r11",uc->uc_mcontext->__ss.__r11);
        number("r12",uc->uc_mcontext->__ss.__r12);number("r13",uc->uc_mcontext->__ss.__r13);
        number("r14",uc->uc_mcontext->__ss.__r14);number("r15",uc->uc_mcontext->__ss.__r15);
        number("rflags",uc->uc_mcontext->__ss.__rflags);
        uintptr_t frame=uc->uc_mcontext->__ss.__rbp,pair[2];
        for(int i=0;i<24&&frame&&!(frame&7);i++) {
            if(!readMemory(frame,pair,sizeof(pair)))break;
            number("return",pair[1]);if(pair[0]<=frame||pair[0]-frame>8*1024*1024)break;frame=pair[0];
        }
        // Optimized native frames can omit frame pointers. These are candidates, not a backtrace.
        uintptr_t stack[64];
        if(readMemory(uc->uc_mcontext->__ss.__rsp,stack,sizeof(stack)))
            for(unsigned i=0;i<64;i++)number("stack",stack[i]);
        put("MAPS\n",5);int maps=hostOpen("/proc/self/maps",0x80000 /* O_CLOEXEC */);
        if(maps>=0){char buffer[4096];long n;size_t total=0;
            while(total<512*1024&&(n=hostRead(maps,buffer,sizeof(buffer)))>0){put(buffer,n);total+=n;}
            hostClose(maps);
        }
        put("END FAULT\n",10);
    }
    // Preserve termination/Crashpad behavior after the original context has been saved.
    sigaction(sig,&previous[sig],NULL);raise(sig);
}
__attribute__((constructor)) static void installCrashReport(void) {
    const char *program=getprogname(),*path=getenv("ROBLOX_MAC_CRASH_LOG");
    if(!program||(strcmp(program,"RobloxPlayer")&&strcmp(program,"crash-check"))||!path||!_elfcalls)return;
    void *lib=_elfcalls->open("libc.so.6");if(!lib)return;
    hostOpen=_elfcalls->symbol(lib,"open");hostClose=_elfcalls->symbol(lib,"close");
    hostRead=_elfcalls->symbol(lib,"read");hostWrite=_elfcalls->symbol(lib,"write");hostPid=_elfcalls->symbol(lib,"getpid");
    hostReadMemory=_elfcalls->symbol(lib,"process_vm_readv");
    if(!hostOpen||!hostClose||!hostRead||!hostWrite||!hostPid||!hostReadMemory)return;
    report=hostOpen(path,0xa0441 /* WRONLY|CREAT|APPEND|NOFOLLOW|CLOEXEC */,0600);if(report<0)return;
    const int fatal[]={SIGILL,SIGABRT,SIGFPE,SIGBUS,SIGSEGV};
    struct sigaction action={0};action.sa_sigaction=recordFault;action.sa_flags=SA_SIGINFO;
    sigemptyset(&action.sa_mask);for(unsigned i=0;i<sizeof(fatal)/sizeof(*fatal);i++)sigaddset(&action.sa_mask,fatal[i]);
    for(unsigned i=0;i<sizeof(fatal)/sizeof(*fatal);i++)sigaction(fatal[i],&action,&previous[fatal[i]]);
}
#ifdef TRACKA_CRASH_CHECK
__attribute__((noinline)) static void fault(void){*(volatile unsigned *)(uintptr_t)1=7;}
int main(void){fault();return 97;}
#endif
