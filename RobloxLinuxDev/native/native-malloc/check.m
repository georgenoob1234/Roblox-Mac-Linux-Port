#include <malloc/malloc.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <Foundation/NSAutoreleasePool.h>
#include <Foundation/NSData.h>
#include <Foundation/NSString.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d errno=%d: %s\n",__LINE__,errno,#x);exit(1);}}while(0)
static const size_t sizes[]={0,1,15,16,17,255,256,257,1008,1024,4096,16384,32768,65536,131072,1048576};
static void allocations(malloc_zone_t *zone) {
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i) {
        size_t size=sizes[i];unsigned char *p=zone?malloc_zone_malloc(zone,size):malloc(size);
        CHECK(p && !((uintptr_t)p&15) && malloc_size(p)>=size);
        CHECK(malloc_zone_from_ptr(p) && malloc_good_size(size)>=size);
        if(zone)CHECK(malloc_zone_from_ptr(p)==zone);
        memset(p,0x5a,size);
        unsigned char *q=zone?malloc_zone_realloc(zone,p,size+100):realloc(p,size+100);
        CHECK(q && malloc_size(q)>=size+100);
        for(size_t j=0;j<size;++j)CHECK(q[j]==0x5a);
        if(zone)malloc_zone_free(zone,q);else free(q);
        p=zone?malloc_zone_calloc(zone,1,size):calloc(1,size);
        CHECK(p && malloc_size(p)>=size);
        for(size_t j=0;j<size;++j)CHECK(!p[j]);
        if(zone)malloc_zone_free(zone,p);else free(p);
    }
}
static void *worker(void *value) {
    void **transfer=value;
    malloc_zone_t *zone=malloc_create_zone(0,0);CHECK(zone);
    for(unsigned i=0;i<1000;++i) {
        size_t size=16+(i*7919)%65536;
        char *p=malloc(size);CHECK(p);memset(p,i,size);
        char *q=realloc(p,size+16);CHECK(q && q[0]==(char)i);free(q);
        p=malloc_zone_malloc(zone,size);CHECK(p);malloc_zone_free(zone,p);
    }
    *transfer=malloc(8192);CHECK(*transfer);memset(*transfer,0x41,8192);
    CHECK(malloc_zone_check(zone));malloc_destroy_zone(zone);
    return NULL;
}
static void benchmark(void) {
    mach_timebase_info_data_t timebase;CHECK(!mach_timebase_info(&timebase));
    static const size_t mixed[]={16,48,256,1008,4096,16384};
    for(unsigned workload=0;workload<3;++workload) {
        double samples[5];
        for(unsigned sample=0;sample<5;++sample) {
            unsigned char *live[128]={0};
            uint64_t begin=mach_absolute_time();
            for(unsigned i=0;i<262144;++i) {
                unsigned slot=i%128;
                free(live[slot]);
                size_t size=workload==0?64:workload==1?4096:mixed[i%6];
                live[slot]=malloc(size);CHECK(live[slot]);
                live[slot][0]=0x5a;live[slot][size-1]=0xa5;
            }
            for(unsigned i=0;i<128;++i)free(live[i]);
            uint64_t elapsed=mach_absolute_time()-begin;
            samples[sample]=(double)elapsed*timebase.numer/timebase.denom/262144;
        }
        for(unsigned i=1;i<5;++i)for(unsigned j=i;j && samples[j]<samples[j-1];--j) {
            double swap=samples[j];samples[j]=samples[j-1];samples[j-1]=swap;
        }
        printf("BENCH %s allocations=262144 live=128 samples=5 ns_per_pair=%.3f,%.3f,%.3f,%.3f,%.3f median=%.3f\n",
               workload==0?"64B":workload==1?"4096B":"mixed16B-16KiB",
               samples[0],samples[1],samples[2],samples[3],samples[4],samples[2]);
    }
}
int main(int argc,char **argv) {
    CHECK(argc==2 || (argc==3 && !strcmp(argv[2],"--benchmark")));alarm(30);
    struct rlimit no_core={0,0};CHECK(!setrlimit(RLIMIT_CORE,&no_core));
    Dl_info library;CHECK(dladdr((void*)malloc,&library) && strstr(library.dli_fname,argv[1]));
    unsigned allocators=0;
    for(unsigned i=0;i<_dyld_image_count();++i)if(strstr(_dyld_get_image_name(i),"libsystem_malloc.dylib"))++allocators;
    CHECK(allocators==1);
    malloc_zone_t *zone=malloc_create_zone(0,0);CHECK(zone && zone->introspect);
    printf("ABI zone=%zu introspection=%zu version=%u offsets=%zu,%zu,%zu allocator=%s\n",
           sizeof(*zone),sizeof(*zone->introspect),zone->version,offsetof(malloc_zone_t,introspect),
           offsetof(malloc_zone_t,version),offsetof(malloc_zone_t,claimed_address),library.dli_fname);
    malloc_set_zone_name(zone,"staged-malloc-check");CHECK(!strcmp(malloc_get_zone_name(zone),"staged-malloc-check"));
    allocations(NULL);allocations(zone);
    void *p=zone->malloc(zone,96);CHECK(p && zone->size(zone,p)>=96);zone->free(zone,p);
    for(size_t alignment=16;alignment<=65536;alignment*=2) {
        CHECK(!posix_memalign(&p,alignment,123) && p && !((uintptr_t)p&(alignment-1)));free(p);
        p=malloc_zone_memalign(zone,alignment,123);CHECK(p && !((uintptr_t)p&(alignment-1)));malloc_zone_free(zone,p);
    }
    p=valloc(123);CHECK(p && !((uintptr_t)p&4095));free(p);
    void *batch[32];unsigned count=malloc_zone_batch_malloc(zone,48,batch,32);CHECK(count<=32);
    for(unsigned i=0;i<count;++i)CHECK(batch[i] && malloc_size(batch[i])>=48);
    malloc_zone_batch_free(zone,batch,count);
    malloc_statistics_t stats;malloc_zone_statistics(zone,&stats);CHECK(stats.size_allocated>=stats.size_in_use);
    CHECK(malloc_zone_check(zone));malloc_zone_pressure_relief(zone,0);
    vm_address_t *zones;unsigned zone_count;
    CHECK(!malloc_get_all_zones(mach_task_self(),NULL,&zones,&zone_count) && zone_count>=2);
    unsigned found=0;for(unsigned i=0;i<zone_count;++i)if(zones[i]==(vm_address_t)zone)found=1;CHECK(found);
    malloc_zone_unregister(zone);malloc_zone_register(zone);
    p=malloc_zone_malloc(zone,80);CHECK(p && malloc_zone_from_ptr(p)==zone);free(p);
    malloc_destroy_zone(zone);free(NULL);CHECK(!malloc_size(NULL));
    volatile size_t huge=SIZE_MAX;
    errno=0;CHECK(!malloc(huge) && errno==ENOMEM);
    errno=0;CHECK(!calloc(huge,2) && errno==ENOMEM);
    p=malloc(32);CHECK(p);memset(p,0x5a,32);errno=0;
    CHECK(!realloc(p,huge) && errno==ENOMEM && ((unsigned char*)p)[0]==0x5a);free(p);
    p=(void*)0x1234;errno=123;CHECK(posix_memalign(&p,3,32)==EINVAL && p==(void*)0x1234 && errno==123);
    pthread_t threads[4];void *transfer[4]={0};
    for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,worker,&transfer[i]));
    for(unsigned i=0;i<4;++i){CHECK(!pthread_join(threads[i],NULL));CHECK(((char*)transfer[i])[8191]==0x41);free(transfer[i]);}
    @autoreleasepool {
        NSMutableData *data=[[NSMutableData alloc] initWithLength:128];CHECK(data && [data length]==128);
        memset([data mutableBytes],0x44,128);[data setLength:1024];CHECK(((char*)[data bytes])[0]==0x44);[data release];
        NSString *value=[[NSString alloc] initWithFormat:@"native-zone-%d",42];CHECK(!strcmp([value UTF8String],"native-zone-42"));[value release];
    }
    pid_t child=fork();CHECK(child>=0);
    if(!child){void *memory=malloc(8192);if(!memory)_exit(2);memset(memory,1,8192);free(memory);_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(malloc_zone_check(NULL));
    puts("PASS native allocator: identity, zone ABI/ownership, sizes, alignment, realloc/calloc, errors, batch/introspection, four threads/cross-thread frees, Foundation, fork");
    if(argc==3)benchmark();
}
