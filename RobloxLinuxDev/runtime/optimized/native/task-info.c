// Native form of Track B's guest_task_info footprint correction. No ARM OIDs.
#include <mach/mach.h>
#include <mach/task_info.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

#include "../../profiler/memory.h"

// Same resident-mapping estimate as the profiler, available with F8 hidden.
// UINT64_MAX means no complete reading: preserve the underlying task report.
static uint64_t cached_estimate=UINT64_MAX;
static kern_return_t (*original)(task_name_t,task_flavor_t,task_info_t,mach_msg_type_number_t*);
static pthread_mutex_t report_mutex=PTHREAD_MUTEX_INITIALIZER;
static task_vm_info_data_t cached_report;
static mach_msg_type_number_t cached_count;
static mach_msg_type_number_t report_count(mach_msg_type_number_t capacity) {
    const mach_msg_type_number_t revisions[]={TASK_VM_INFO_REV5_COUNT,TASK_VM_INFO_REV4_COUNT,
        TASK_VM_INFO_REV3_COUNT,TASK_VM_INFO_REV2_COUNT,TASK_VM_INFO_REV1_COUNT};
    for(unsigned i=0;i<sizeof(revisions)/sizeof(*revisions);++i)
        if(capacity>=revisions[i])return revisions[i];
    return 0;
}
static int refresher_state; // 0 none, 1 first scan running, 2 refresher running
static void refresh_estimate(void) {
    FILE *smaps=fopen("/proc/self/smaps","re");
    RbxMemoryReading reading;
    int complete=rbx_read_memory(smaps,&reading);
    if(smaps)fclose(smaps);
    uint64_t estimate=complete?rbx_memory_estimate(&reading):UINT64_MAX;
    __atomic_store_n(&cached_estimate,estimate,__ATOMIC_RELEASE);
    // Darling fills only these four TASK_VM_INFO fields. Derive the same
    // report from our completed scan instead of another synchronous RPC.
    task_vm_info_data_t report={0};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    int valid=complete;
    if(valid) {
        report.page_size=4096;
        report.virtual_size=reading.mapped;
        for(unsigned i=0;i<RBX_MEMORY_CATEGORIES;++i)report.resident_size+=reading.rss[i];
        report.resident_size_peak=report.resident_size;
        report.phys_footprint=estimate;
    }
    pthread_mutex_lock(&report_mutex);
    if(valid)cached_report=report;
    cached_count=valid?count:0;
    pthread_mutex_unlock(&report_mutex);
}
static void *refresh_loop(void *unused) {
    (void)unused;
    pthread_setname_np("tracka task_info");
    for(;;) {
        // ponytail: 1s refresh; the client polls this far more often than
        // the number moves. Shorten it if a caller needs fresher bytes.
        struct timespec delay={1,0};
        nanosleep(&delay,NULL);
        refresh_estimate();
    }
    return NULL;
}
// A forked child has no refresher thread; its next caller starts one.
static void restart_after_fork(void) {
    __atomic_store_n(&cached_estimate,UINT64_MAX,__ATOMIC_RELEASE);
    __atomic_store_n(&refresher_state,0,__ATOMIC_RELEASE);
    cached_count=0;
    report_mutex=(pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}
static void register_fork_handler(void) { pthread_atfork(NULL,NULL,restart_after_fork); }
// Only the first query scans inline. The existing worker refreshes thereafter.
static uint64_t estimated_roblox_bytes(void) {
    int state=__atomic_load_n(&refresher_state,__ATOMIC_ACQUIRE),expected=0;
    if(state!=2) {
        if(__atomic_compare_exchange_n(&refresher_state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            static pthread_once_t atfork=PTHREAD_ONCE_INIT;
            pthread_once(&atfork,register_fork_handler);
            refresh_estimate();
            pthread_attr_t attributes;pthread_t thread;
            pthread_attr_init(&attributes);
            pthread_attr_setdetachstate(&attributes,PTHREAD_CREATE_DETACHED);
            int started=!pthread_create(&thread,&attributes,refresh_loop,NULL);
            pthread_attr_destroy(&attributes);
            // Without a refresher, the next caller scans again rather than never.
            __atomic_store_n(&refresher_state,started?2:0,__ATOMIC_RELEASE);
        } else {
            // Another thread's first scan is in flight: wait for its total
            // instead of reporting device memory as the client's own.
            while(__atomic_load_n(&refresher_state,__ATOMIC_ACQUIRE)==1)sched_yield();
        }
    }
    return __atomic_load_n(&cached_estimate,__ATOMIC_ACQUIRE);
}
kern_return_t task_info(task_name_t task,task_flavor_t flavor,task_info_t value,
                        mach_msg_type_number_t *count) {
    int saved=errno;
    __typeof__(original) call=__atomic_load_n(&original,__ATOMIC_ACQUIRE);
    if(!call){
        call=dlsym(RTLD_NEXT,"task_info");
        if(call)__atomic_store_n(&original,call,__ATOMIC_RELEASE);
    }
    errno=saved;
    if(!call)return KERN_FAILURE;
    mach_msg_type_number_t capacity=count?*count:0;
    if(task==mach_task_self() && flavor==TASK_VM_INFO && value && count && capacity>=TASK_VM_INFO_REV1_COUNT) {
        estimated_roblox_bytes();
        pthread_mutex_lock(&report_mutex);
        mach_msg_type_number_t length=report_count(capacity<cached_count?capacity:cached_count);
        if(length) {memcpy(value,&cached_report,length*sizeof(integer_t));*count=length;}
        pthread_mutex_unlock(&report_mutex);
        errno=saved;
        if(length)return KERN_SUCCESS;
    }
    kern_return_t result=call(task,flavor,value,count);
    if(result==KERN_SUCCESS && task==mach_task_self() && flavor==TASK_VM_INFO && value && count &&
       capacity>=TASK_VM_INFO_REV1_COUNT && *count>=TASK_VM_INFO_REV1_COUNT){
        task_vm_info_t info=(task_vm_info_t)value;
        int query_errno=errno;
        uint64_t estimate=estimated_roblox_bytes();
        // resident_size remains the OS's raw RSS for diagnostics; Roblox reads
        // phys_footprint. A successful scan replaces even a nonzero footprint.
        if(estimate!=UINT64_MAX)info->phys_footprint=estimate;
        else if(!info->phys_footprint)info->phys_footprint=info->resident_size;
        errno=query_errno;
    }
    return result;
}
