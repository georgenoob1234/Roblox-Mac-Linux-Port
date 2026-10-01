// Darling-specific fork adapter, following its sys_fork.c and libSystem/init.c.
// Private symbol names are read from the bundled library, never hardcoded code
// offsets. This development bridge must be replaced by a public Darling export
// before supporting arbitrary Darling versions.
#include <mach-o/dyld.h>
#include <mach-o/nlist.h>
#include "generated/darling-tsd.h"
#include <poll.h>
static void *kernel_private(const char *name){
    for(uint32_t i=0;i<_dyld_image_count();++i){
        const char *path=_dyld_get_image_name(i);
        if(!path || !strstr(path,"/libsystem_kernel.dylib"))continue;
        const struct mach_header_64 *header=(void*)_dyld_get_image_header(i);
        if(!header || header->magic!=MH_MAGIC_64 || header->sizeofcmds>1048576)return NULL;
        const char *end=(const char*)(header+1)+header->sizeofcmds;
        const struct load_command *lc=(void*)(header+1);
        const struct symtab_command *symbols=NULL;const struct segment_command_64 *link=NULL;
        for(uint32_t j=0;j<header->ncmds;++j){
            if((const char*)(lc+1)>end || lc->cmdsize<sizeof(*lc) || lc->cmdsize>(size_t)(end-(const char*)lc))return NULL;
            if(lc->cmd==LC_SYMTAB && lc->cmdsize>=sizeof(*symbols))symbols=(void*)lc;
            if(lc->cmd==LC_SEGMENT_64 && lc->cmdsize>=sizeof(*link) && !strcmp(((const struct segment_command_64*)lc)->segname,"__LINKEDIT"))link=(void*)lc;
            lc=(void*)((const char*)lc+lc->cmdsize);
        }
        if(!symbols || !link || symbols->symoff<link->fileoff || symbols->stroff<link->fileoff ||
           (uint64_t)symbols->symoff+(uint64_t)symbols->nsyms*sizeof(struct nlist_64)>link->fileoff+link->filesize ||
           (uint64_t)symbols->stroff+symbols->strsize>link->fileoff+link->filesize)return NULL;
        intptr_t slide=_dyld_get_image_vmaddr_slide(i);
        uintptr_t base=slide+link->vmaddr-link->fileoff;
        const struct nlist_64 *entries=(void*)(base+symbols->symoff);const char *strings=(void*)(base+symbols->stroff);
        void *result=NULL;
        for(uint32_t j=0;j<symbols->nsyms;++j){
            const struct nlist_64 *entry=&entries[j];uint32_t offset=entry->n_un.n_strx;
            if((entry->n_type&N_STAB) || (entry->n_type&N_TYPE)!=N_SECT || offset>=symbols->strsize)continue;
            if(!memchr(strings+offset,0,symbols->strsize-offset))return NULL;
            if(!strcmp(strings+offset,name)){if(result)return NULL;result=(void*)(slide+entry->n_value);}
        }
        return result;
    }
    return NULL;
}
struct NativeFork {
    void (*handlers_prepare)(void),(*handlers_parent)(void),(*prepare)(void),(*parent)(void),(*child)(void);
    void (*guard_child)(void);
    int (*guard_add)(int,unsigned,void*),(*checkin)(_Bool,void*,int),(*convert_errno)(int);
    int *cached_pid;
    int (*fork)(void),(*error)(void);
    void (*failed_parent[7])(void);
    int linux_error,prepared,inject_failure;
};
static int create_native_process(void *pointer){
    struct NativeFork *state=pointer;state->prepare();state->prepared=1;
    int pid=state->inject_failure?-1:state->fork();
    if(pid<0)state->linux_error=state->inject_failure?11:state->error(); // Linux EAGAIN in the failure regression.
    return pid;
}
static int guest_atfork(armrt_cpu *cpu,void *unused){
    (void)unused;void *callbacks[3];
    for(unsigned i=0;i<3;++i){uint64_t guest=p_armrt_register(cpu,i);callbacks[i]=guest?p_armrt_callback(cpu,guest,"v"):NULL;if(guest && !callbacks[i])return -1;}
    int (*register_handlers)(void(*)(void),void(*)(void),void(*)(void))=dlsym(RTLD_DEFAULT,"pthread_atfork");
    return register_handlers?p_armrt_set_register(cpu,0,register_handlers(callbacks[0],callbacks[1],callbacks[2])):p_armrt_fail(cpu,"native pthread_atfork unavailable");
}
static int guest_fork(armrt_cpu *cpu,void *unused){
    (void)unused;struct NativeFork state={0};
    if(p_armrt_native_active()){errno=EDEADLK;return p_armrt_set_register(cpu,0,(uint64_t)-1);}
    void *libc=_elfcalls->dlopen("libc.so.6");
    if(!libc)return p_armrt_fail(cpu,"cannot load native fork");
    state.fork=_elfcalls->dlsym(libc,"fork");state.error=_elfcalls->get_errno;
    state.handlers_prepare=dlsym(RTLD_DEFAULT,"_pthread_atfork_prepare_handlers");
    state.handlers_parent=dlsym(RTLD_DEFAULT,"_pthread_atfork_parent_handlers");
    state.prepare=dlsym(RTLD_DEFAULT,"libSystem_posix_spawn_prepare");
    state.parent=dlsym(RTLD_DEFAULT,"libSystem_atfork_parent");state.child=dlsym(RTLD_DEFAULT,"libSystem_atfork_child");
    state.guard_child=kernel_private("_guard_table_postfork_child");state.guard_add=kernel_private("_guard_table_add");
    state.checkin=kernel_private("_dserver_rpc_checkin");state.convert_errno=kernel_private("_errno_linux_to_bsd");state.cached_pid=kernel_private("__current_pid");
    const char *cleanup[]={"_pthread_atfork_parent","_malloc_fork_parent","cc_atfork_parent","_dyld_atfork_parent","dispatch_atfork_parent","xpc_atfork_parent","_libSC_info_fork_parent"};
    int valid=state.fork && state.error && state.handlers_prepare && state.handlers_parent && state.prepare && state.parent && state.child && state.guard_child && state.guard_add && state.checkin && state.convert_errno && state.cached_pid;
    for(unsigned i=0;i<7;++i){state.failed_parent[i]=dlsym(RTLD_DEFAULT,cleanup[i]);valid=valid && state.failed_parent[i];}
    if(!valid){_elfcalls->dlclose(libc);return p_armrt_fail(cpu,"bundled Darling fork interface mismatch");}
    int (*host_pipe)(int*)=_elfcalls->dlsym(libc,"pipe");
    int (*host_close)(int)=_elfcalls->dlsym(libc,"close");
    ssize_t (*host_read)(int,void*,size_t)=_elfcalls->dlsym(libc,"read");
    ssize_t (*host_write)(int,const void*,size_t)=_elfcalls->dlsym(libc,"write");
    int (*host_poll)(struct pollfd*,unsigned long,int)=_elfcalls->dlsym(libc,"poll");
    int (*host_kill)(int,int)=_elfcalls->dlsym(libc,"kill");
    int (*host_wait)(int,int*,int)=_elfcalls->dlsym(libc,"waitpid");
    int (*host_clock)(int,struct timespec*)=_elfcalls->dlsym(libc,"clock_gettime");
    if(!host_pipe || !host_close || !host_read || !host_write || !host_poll || !host_kill || !host_wait || !host_clock){_elfcalls->dlclose(libc);return p_armrt_fail(cpu,"native fork status interface unavailable");}
    int status_pipe[2];
    if(host_pipe(status_pipe)){errno=state.convert_errno(state.error());_elfcalls->dlclose(libc);return p_armrt_set_register(cpu,0,(uint64_t)-1);}
    state.inject_failure=getenv("TRACKB_FORK_FAIL_CHECK")!=NULL;
    state.handlers_prepare();
    // Darling's per-thread directory may change inside a prepare callback.
    int cwd=pthread_getspecific(__PTK_DARLING_KEY1)?(int)(intptr_t)pthread_getspecific(__PTK_DARLING_KEY2):-1;
    int timed_out=0;
    int pid=p_armrt_fork(cpu,create_native_process,&state);
    if(pid==0){
        host_close(status_pipe[0]);
        state.guard_child();_elfcalls->dserver_per_thread_socket_refresh();
        int readfd=_elfcalls->dserver_process_lifetime_pipe_refresh();
        struct {void (*close)(int);} options={_elfcalls->dserver_close_socket};
        int failed=state.guard_add(_elfcalls->dserver_per_thread_socket(),3,&options);
        if(readfd!=-1){options.close=_elfcalls->dserver_close_process_lifetime_pipe;failed|=state.guard_add(_elfcalls->dserver_get_process_lifetime_pipe(),3,&options);}
        int stack_hint;
        unsigned char checked=!failed && !getenv("TRACKB_CHECKIN_FAIL_CHECK") && state.checkin(1,&stack_hint,readfd)>=0;
        ssize_t sent;do{sent=host_write(status_pipe[1],&checked,1);}while(sent<0 && state.error()==4);
        host_close(status_pipe[1]);
        if(!checked || sent!=1)_Exit(126);
        if(cwd>=0 && fchdir(cwd))_Exit(126);
        _elfcalls->dserver_close_process_lifetime_pipe(readfd);
        *state.cached_pid=0;state.child();
    }else{
        host_close(status_pipe[1]);
        if(pid>0){
            struct pollfd ready={status_pipe[0],1,0};int polled=-1;
            struct timespec now;int clock_ok=!host_clock(1,&now); // Linux CLOCK_MONOTONIC.
            int64_t deadline=clock_ok?(int64_t)now.tv_sec*1000+now.tv_nsec/1000000+10000:0;
            while(clock_ok){
                int64_t remaining=deadline-((int64_t)now.tv_sec*1000+now.tv_nsec/1000000);
                if(remaining<=0){polled=0;break;}
                polled=host_poll(&ready,1,(int)remaining);
                if(polled>=0 || state.error()!=4)break;
                clock_ok=!host_clock(1,&now);
            }
            unsigned char checked=0;ssize_t received=-1;
            if(polled>0)do{received=host_read(status_pipe[0],&checked,1);}while(received<0 && state.error()==4);
            if(received==1 && checked){state.parent();}
            else{
                timed_out=received!=1 || checked!=0;
                host_kill(pid,9);int status;while(host_wait(pid,&status,0)<0 && state.error()==4){}
                state.linux_error=5;pid=-1;
            }
        }
        host_close(status_pipe[0]);
        if(pid<0){
        // The stock parent hook waits for a child check-in even after failed
        // fork. Release the same subsystem locks, omitting that wait only.
        if(state.prepared)for(unsigned i=0;i<7;++i)state.failed_parent[i]();
        state.handlers_parent();
        }
    }
    _elfcalls->dlclose(libc);
    if(timed_out)return p_armrt_fail(cpu,"child check-in status uncertain; stopping to avoid stale fork synchronization");
    if(pid==-2)return -1;
    if(pid<0)errno=state.convert_errno(state.linux_error);
    return p_armrt_set_register(cpu,0,pid);
}
