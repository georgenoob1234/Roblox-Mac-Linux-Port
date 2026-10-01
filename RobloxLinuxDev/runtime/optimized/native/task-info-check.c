#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mach/mach.h>
#include <dlfcn.h>
static void *test_dlsym(void *,const char *);
#define dlsym test_dlsym
#define task_info checked_task_info
#include "task-info.c"
#undef dlsym
#undef task_info
static int response,calls;
static task_flavor_t seen;
static kern_return_t fake(task_name_t task,task_flavor_t flavor,task_info_t value,mach_msg_type_number_t *count) {
    (void)task;(void)value;(void)count;seen=flavor;++calls;return response;
}
static void *test_dlsym(void *handle,const char *name){(void)handle;assert(!strcmp(name,"task_info"));return fake;}
int main(void) {
    task_name_t self=mach_task_self();
    task_vm_info_data_t info={0};info.resident_size=8000000000ULL;
    mach_msg_type_number_t count=TASK_VM_INFO_REV1_COUNT;
    // Simulate the shared scanner's cache without starting the test copy's worker.
    refresher_state=2;cached_estimate=1300000000ULL;
    errno=E2BIG;
    assert(!checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,&count));
    assert(info.phys_footprint==cached_estimate && info.resident_size==8000000000ULL && errno==E2BIG);
    assert(count==TASK_VM_INFO_REV1_COUNT);
    info.phys_footprint=99;
    assert(!checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,&count) && info.phys_footprint==cached_estimate);
    assert(estimated_roblox_bytes()==cached_estimate); // cached polling
    // Failed scans preserve nonzero reports, or fall back to raw RSS when zero.
    cached_estimate=UINT64_MAX;info.phys_footprint=99;
    assert(!checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,&count) && info.phys_footprint==99);
    info.phys_footprint=0;
    assert(!checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,&count) && info.phys_footprint==info.resident_size);
    cached_estimate=42;info.phys_footprint=99;count=TASK_VM_INFO_REV0_COUNT;
    assert(!checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,&count) && info.phys_footprint==99);
    count=TASK_VM_INFO_COUNT;response=KERN_INVALID_ARGUMENT;
    assert(checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,&count)==response && info.phys_footprint==99);
    response=0;
    assert(!checked_task_info(self,18,(task_info_t)&info,&count) && seen==18 && info.phys_footprint==99);
    assert(!checked_task_info(self+1,TASK_VM_INFO,(task_info_t)&info,&count) && info.phys_footprint==99);
    assert(!checked_task_info(self,TASK_VM_INFO,NULL,&count));
    assert(!checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,NULL));
    // A published snapshot avoids every synchronous Mach query, respects ABI
    // revisions, and never overwrites the caller's short buffer.
    cached_report.resident_size=123;cached_report.phys_footprint=456;
    cached_count=TASK_VM_INFO_COUNT;
    const unsigned revisions[]={TASK_VM_INFO_REV1_COUNT,TASK_VM_INFO_REV2_COUNT,
        TASK_VM_INFO_REV3_COUNT,TASK_VM_INFO_REV4_COUNT,TASK_VM_INFO_REV5_COUNT};
    int before=calls;
    for(unsigned i=0;i<sizeof(revisions)/sizeof(*revisions);++i) {
        memset(&info,0xa5,sizeof(info));count=revisions[i];errno=E2BIG;
        assert(!checked_task_info(self,TASK_VM_INFO,(task_info_t)&info,&count));
        assert(count==revisions[i] && info.resident_size==123 && info.phys_footprint==456 && errno==E2BIG);
        for(unsigned j=count*sizeof(integer_t);j<sizeof(info);++j)assert(((unsigned char*)&info)[j]==0xa5);
    }
    assert(calls==before);
    refresh_estimate();assert(calls==before && cached_count==TASK_VM_INFO_COUNT);
    assert(cached_report.page_size==4096 && cached_report.resident_size && cached_report.virtual_size);
    restart_after_fork();assert(!refresher_state && cached_estimate==UINT64_MAX && !cached_count);
    puts("PASS task_info: exact cached estimate, nonzero override, scan failure fallback, raw RSS/errno preservation, self-only and short/error forwarding");
    // Exercise the real adapter and its independent cache through the loader.
    Dl_info image;assert(dladdr((void*)task_info,&image));
    assert(strstr(image.dli_fname,"libtracka-taskinfo.dylib"));
    memset(&info,0,sizeof(info));count=TASK_VM_INFO_COUNT;
    assert(!task_info(self,TASK_VM_INFO,(task_info_t)&info,&count));
    assert(count>=TASK_VM_INFO_REV1_COUNT && info.resident_size && info.phys_footprint);
    FILE *smaps=fopen("/proc/self/smaps","re");RbxMemoryReading reading;
    assert(smaps && rbx_read_memory(smaps,&reading));fclose(smaps);
    // The cached value and this fresh scan can differ as the worker starts.
    uint64_t estimate=rbx_memory_estimate(&reading);
    uint64_t delta=estimate>info.phys_footprint?estimate-info.phys_footprint:info.phys_footprint-estimate;
    assert(delta<4*1024*1024);
    printf("Live task_info: raw resident %.2f MB, shared-parser estimate %.2f MB, reported footprint %.2f MB\n",
           info.resident_size/1e6,estimate/1e6,info.phys_footprint/1e6);
    puts("PASS live native task_info uses the profiler estimate without opening the profiler");
}
