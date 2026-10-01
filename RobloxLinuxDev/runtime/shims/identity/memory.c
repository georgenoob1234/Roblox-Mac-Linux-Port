// Darling's HOST_VM_INFO/HOST_VM_INFO64 stubs succeed with all-zero counts.
// Report live Linux memory instead, so clients do not constantly evict assets.
#include <mach/mach.h>
#include <mach/host_info.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <dlfcn.h>

// This package runs native x86_64 on Linux. Reading procfs directly avoids
// a Darling VFS round trip on every frame's host-memory statistics query.
static long memory_linux_call(long number,long a,long b,long c) {
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(number),"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");
    return result;
}
static int memory_pages(natural_t pages[4]) {
    long fd=memory_linux_call(2,(long)"/proc/meminfo",02000000,0);
    if(fd<0)return 0;
    char buffer[8192];size_t used=0;long bytes=0;
    while(used<sizeof(buffer)-1) {
        bytes=memory_linux_call(0,fd,(long)(buffer+used),sizeof(buffer)-1-used);
        if(bytes==-4)continue; // Linux EINTR; guest errno values need not match.
        if(bytes<=0)break;
        used+=(size_t)bytes;
    }
    memory_linux_call(3,fd,0,0);
    if(used==sizeof(buffer)-1 || bytes<0)return 0;
    buffer[used]=0;
    char *line=buffer,*next,key[64];unsigned long long value;
    uint64_t total=0,available=0,free=0,active=0;int found=0;
    for(;line && *line;line=next) {
        next=strchr(line,'\n');if(next)*next++=0;
        if(sscanf(line,"%63s %llu kB",key,&value)!=2)continue;
        if(!strcmp(key,"MemTotal:"))total=value;
        else if(!strcmp(key,"MemAvailable:")){available=value;found=1;}
        else if(!strcmp(key,"MemFree:"))free=value;
        else if(!strcmp(key,"Active:"))active=value;
    }
    if(!total || !found)return 0;
    if(available>total)available=total;
    if(free>available)free=available;
    if(active>total-available)active=total-available;
    // Linux and Darwin page categories differ. Account reclaimable RAM as
    // inactive; free+inactive then equals Linux's MemAvailable estimate.
    // Remaining non-reclaimable RAM is wired. Guest pages are always 4096 bytes.
    uint64_t kb[]={free,active,available-free,total-available-active};
    for(unsigned i=0;i<4;i++)pages[i]=kb[i]/4>UINT_MAX?UINT_MAX:(natural_t)(kb[i]/4);
    return 1;
}

kern_return_t host_statistics(host_t host,host_flavor_t flavor,host_info_t info,mach_msg_type_number_t *count) {
    if(flavor!=HOST_VM_INFO) {
        kern_return_t (*original)(host_t,host_flavor_t,host_info_t,mach_msg_type_number_t*)=dlsym(RTLD_NEXT,"host_statistics");
        return original?original(host,flavor,info,count):KERN_FAILURE;
    }
    if(!host || !info || !count)return KERN_INVALID_ARGUMENT;
    if(*count<HOST_VM_INFO_REV0_COUNT)return KERN_FAILURE;
    natural_t pages[4];if(!memory_pages(pages))return KERN_FAILURE;
    vm_statistics_data_t stats={0};
    stats.free_count=pages[0];stats.active_count=pages[1];stats.inactive_count=pages[2];stats.wire_count=pages[3];
    unsigned size=*count>=HOST_VM_INFO_COUNT?HOST_VM_INFO_COUNT:
        *count>=HOST_VM_INFO_REV1_COUNT?HOST_VM_INFO_REV1_COUNT:HOST_VM_INFO_REV0_COUNT;
    memcpy(info,&stats,size*sizeof(integer_t));*count=size;return KERN_SUCCESS;
}
kern_return_t host_statistics64(host_t host,host_flavor_t flavor,host_info64_t info,mach_msg_type_number_t *count) {
    if(flavor==HOST_VM_INFO)return host_statistics(host,flavor,(host_info_t)info,count);
    if(flavor!=HOST_VM_INFO64) {
        kern_return_t (*original)(host_t,host_flavor_t,host_info64_t,mach_msg_type_number_t*)=dlsym(RTLD_NEXT,"host_statistics64");
        return original?original(host,flavor,info,count):KERN_FAILURE;
    }
    if(!host || !info || !count)return KERN_INVALID_ARGUMENT;
    if(*count<HOST_VM_INFO64_REV0_COUNT)return KERN_FAILURE;
    natural_t pages[4];if(!memory_pages(pages))return KERN_FAILURE;
    vm_statistics64_data_t stats={0};
    stats.free_count=pages[0];stats.active_count=pages[1];stats.inactive_count=pages[2];stats.wire_count=pages[3];
    unsigned size=*count>=HOST_VM_INFO64_COUNT?HOST_VM_INFO64_COUNT:HOST_VM_INFO64_REV0_COUNT;
    memcpy(info,&stats,size*sizeof(integer_t));*count=size;return KERN_SUCCESS;
}
