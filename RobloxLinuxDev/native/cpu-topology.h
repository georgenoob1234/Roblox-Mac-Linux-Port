#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

// This is the native x86_64 Linux/Darling adapter, never ARM guest code.
#if !defined(__x86_64__)
#error CPU topology helper requires the native x86_64 host
#endif
static long cpu_linux_call(long number,long a,long b,long c) {
    long result;
    __asm__ volatile("syscall":"=a"(result):"a"(number),"D"(a),"S"(b),"d"(c):"rcx","r11","memory","cc");
    return result;
}
static int cpu_topology_id(unsigned cpu,const char *field) {
    char path[128],value[32];
    snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%u/topology/%s",cpu,field);
    long fd=cpu_linux_call(2,(long)path,02000000,0); // Linux open, O_CLOEXEC
    if(fd<0)return -1;
    long bytes=cpu_linux_call(0,fd,(long)value,sizeof(value)-1);
    cpu_linux_call(3,fd,0,0);
    if(bytes<=0)return -1;
    value[bytes]=0;char *end;long id=strtol(value,&end,10);
    return end==value || (*end && *end!='\n') || id<0 || id>INT32_MAX?-1:(int)id;
}
static int trackb_cpu_count(int physical) {
    // ponytail: bounded at 8192 CPUs; grow the mask if sched_getaffinity rejects
    // it. Fail instead of silently truncating an affinity mask on larger hosts.
    uint64_t mask[128]={0};
    long bytes=cpu_linux_call(204,0,sizeof(mask),(long)mask);
    if(bytes<0){errno=bytes==-22?EOVERFLOW:EIO;return -1;}
    unsigned count=0;
    if(!physical) {
        for(unsigned i=0;i<(unsigned)bytes/sizeof(*mask);++i)count+=__builtin_popcountll(mask[i]);
    }else {
        // Package/core IDs are stable across affinity changes and CPU hotplug.
        // No TLS, allocation, locks or constructors: libmalloc may query early.
        static uint64_t topology[8192];
        for(unsigned i=0;i<(unsigned)bytes*8;++i)if(mask[i/64]&(UINT64_C(1)<<(i%64))) {
            uint64_t key=__atomic_load_n(&topology[i],__ATOMIC_RELAXED);
            if(!key) {
                int package=cpu_topology_id(i,"physical_package_id"),core=cpu_topology_id(i,"core_id");
                if(package<0 || core<0){errno=ENOTSUP;return -1;}
                key=(((uint64_t)(unsigned)package<<32)|(unsigned)core)+1;
                __atomic_store_n(&topology[i],key,__ATOMIC_RELAXED);
            }
            // ponytail: linear scan of earlier CPU IDs; use a set on sparse,
            // many-core hosts. Avoid a large stack or allocating during startup.
            unsigned j=0;
            for(;j<i;++j)if((mask[j/64]&(UINT64_C(1)<<(j%64))) &&
                __atomic_load_n(&topology[j],__ATOMIC_RELAXED)==key)break;
            if(j==i)++count;
        }
    }
    if(!count){errno=EIO;return -1;}
    return (int)count;
}
