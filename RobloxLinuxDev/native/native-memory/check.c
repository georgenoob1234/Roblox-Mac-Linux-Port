#include <assert.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../runtime/src/darling/src/startup/mldr/elfcalls/elfcalls.h"
extern struct elf_calls *_elfcalls;
int main(void) {
    void (*install)(void*,void*)=dlsym(RTLD_DEFAULT,"trackb_install_memory");
    if(install && !getenv("TRACKB_MEMORY_TEST_FALLBACK")){
        void *libc=_elfcalls->dlopen("libc.so.6");assert(libc);
        void *move=_elfcalls->dlsym(libc,"memmove"),*compare=_elfcalls->dlsym(libc,"memcmp");assert(move&&compare);
        install(move,compare);
    }
    void *(*move)(void*,const void*,size_t)=dlsym(RTLD_DEFAULT,"memmove");
    int (*compare)(const void*,const void*,size_t)=dlsym(RTLD_DEFAULT,"memcmp");
    void *(*fill)(void*,int,size_t)=dlsym(RTLD_DEFAULT,"memset");
    assert(move && compare && fill);
    Dl_info info;assert(dladdr(move,&info));printf("Memory implementation: %s\n",info.dli_fname);
    unsigned char a[2048],b[2048];
    for(size_t n=0;n<=1024;n+=n<64?1:31)for(size_t offset=0;offset<32;++offset)for(int direction=0;direction<2;++direction){
        for(size_t i=0;i<sizeof(a);++i)a[i]=b[i]=(unsigned char)(i*13+7);
        size_t dst=direction?offset:0,src=direction?0:offset;
        unsigned char copy[1024];for(size_t i=0;i<n;++i)copy[i]=b[src+i];
        for(size_t i=0;i<n;++i)b[dst+i]=copy[i];
        assert(move(a+dst,a+src,n)==a+dst);assert(compare(a,b,sizeof(a))==0);
        assert(fill(a+offset,0xA5,n)==a+offset);
        for(size_t i=0;i<n;++i)b[offset+i]=0xA5;
        assert(compare(a,b,sizeof(a))==0);
    }
    for(size_t n=1;n<=1024;++n){
        fill(a,0,n);fill(b,0,n);a[n-1]=255;
        assert(compare(a,b,n)>0 && compare(b,a,n)<0);
    }
    size_t page=getpagesize();unsigned char *guard=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);assert(guard!=MAP_FAILED);
    assert(!mprotect(guard+page,page,PROT_READ|PROT_WRITE));
    for(size_t n=0;n<=1024;++n){fill(guard+2*page-n,0x77,n);move(a,guard+2*page-n,n);assert(!compare(a,guard+2*page-n,n));}
    assert(!munmap(guard,page*3));
    size_t size=1024*1024;unsigned char *src=malloc(size+64),*dst=malloc(size+64);assert(src&&dst);fill(src,37,size+64);
    mach_timebase_info_data_t tb;mach_timebase_info(&tb);
    for(int run=0;run<6;++run){uint64_t start=mach_absolute_time();for(int i=0;i<128;++i)move(dst+1,src+3,size);
        double ms=(mach_absolute_time()-start)*(double)tb.numer/tb.denom/1e6;
        assert(!compare(dst+1,src+3,size));printf("128 MiB unaligned copy: %.3f ms\n",ms);
    }
    free(src);free(dst);puts("PASS overlap, alignment, bounds and fill");fflush(stdout);_Exit(0);
}
