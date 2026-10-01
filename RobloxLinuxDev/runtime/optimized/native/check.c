// Use the existing overlap, alignment and guard-page regression unchanged.
#define main trackb_memory_check
#include "../../../native/native-memory/check.c"
#undef main
#include <pthread.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <malloc/malloc.h>
static int stop,ready,prepared,parent_called,child_called;
static void prepare(void){++prepared;}
static void parent(void){++parent_called;}
static void child(void){++child_called;}
static void *allocate(void *unused) {
    (void)unused;
    __atomic_add_fetch(&ready,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&stop,__ATOMIC_ACQUIRE)) {
        char *p=malloc(4096);assert(p);memset(p,0x5a,4096);
        char *q=realloc(p,8192);assert(q && q[4095]==0x5a);free(q);
    }
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);alarm(30);
    struct rlimit limit={0,0};assert(!setrlimit(RLIMIT_CORE,&limit));
    int (*active)(void)=dlsym(RTLD_DEFAULT,"tracka_memory_active");
    assert(active && active()==atoi(argv[1]));
    const char *names[]={"memmove","memcpy","memcmp"};
    for(unsigned i=0;i<3;++i){
        Dl_info info;assert(dladdr(dlsym(RTLD_DEFAULT,names[i]),&info));
        assert(strstr(info.dli_fname,"libtracka-memory.dylib"));
    }
    // Existing check does not recognize the renamed Track A install function,
    // so this proves constructor installation (or fallback) without test setup.
    assert(!pthread_atfork(prepare,parent,child));
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,allocate,NULL));
    while(__atomic_load_n(&ready,__ATOMIC_ACQUIRE)!=4)usleep(1000);
    for(int i=1;i<=8;++i){
        pid_t pid=fork();assert(pid>=0);
        if(!pid){
            assert(prepared==i && child_called==1 && parent_called==i-1);
            char *p=malloc(8192);assert(p);memset(p,0x37,8192);free(p);
            assert(malloc_zone_check(NULL));_exit(0);
        }
        int status;assert(waitpid(pid,&status,0)==pid);
        assert(WIFEXITED(status) && !WEXITSTATUS(status));
        assert(prepared==i && parent_called==i && !child_called);
    }
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    puts("PASS native constructor/identity and eight forks during four-thread allocation; atfork ordering and child allocator locks");
    return trackb_memory_check();
}
