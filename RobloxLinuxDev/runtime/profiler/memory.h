#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

enum { RBX_MEMORY_GPU=4, RBX_MEMORY_MACOS=5, RBX_MEMORY_CATEGORIES=6 };
typedef struct {
    uint64_t rss[RBX_MEMORY_CATEGORIES],mapped,gpu_mapped,pss_ram,swap;
} RbxMemoryReading;

// Shared by the Linux profiler and Darwin task_info adapter.
static inline unsigned rbx_memory_category(char *path) {
    if(!*path || *path=='[')return 3;
    if(!strncmp(path,"/dev/nvidia",11) || !strncmp(path,"/dev/dri/",9))return RBX_MEMORY_GPU;
    size_t length=strlen(path);
    if(length>=10 && !strcmp(path+length-10," (deleted)"))path[length-10]=0;
    const char *name=strrchr(path,'/');name=name?name+1:path;
    if(strstr(path,"/RobloxPlayer.app/") || !strcmp(name,"RobloxPlayer") || strstr(name,"mimalloc"))return 0;
    if(!strncmp(name,"libsystem_",10) || !strncmp(name,"libobjc.",8) || !strncmp(name,"libc++",6))return RBX_MEMORY_MACOS;
    if(strcmp(name,"Metal") && (strstr(path,"/optimized/") || strstr(path,"/shims/") || strstr(name,"indium") || strstr(name,"roblox-wayland")))return 1;
    length=strlen(name);
    if(!strcmp(name,"dyld") || strstr(path,".framework/") || (length>=6 && !strcmp(name+length-6,".dylib")))return RBX_MEMORY_MACOS;
    return 2;
}
// ponytail: includes unattributed runtime/driver heaps. Allocation-owner tracking
// is required for an exact guest-only total; library mappings alone cannot do it.
static inline uint64_t rbx_memory_estimate(const RbxMemoryReading *reading) {
    return reading->rss[0]+reading->rss[3];
}
static inline int rbx_memory_add(uint64_t *total,uint64_t bytes) {
    if(UINT64_MAX-*total<bytes)return 0;
    *total+=bytes;return 1;
}
// smaps RSS is resident bytes, not the virtual address range of a mapping.
// https://docs.kernel.org/filesystems/proc.html
// Publish only complete scans; callers retain the underlying report on failure.
static inline int rbx_read_memory(FILE *input,RbxMemoryReading *out) {
    RbxMemoryReading result={0};
    char *line=NULL;size_t capacity=0,count=0;unsigned category=0,fields=0;
    int mapping=0,complete=0;
    if(!input)return 0;
    while(getline(&line,&capacity,input)>=0) {
        unsigned long long start,end,offset,inode;char perms[5],device[32];int consumed=0;
        if(sscanf(line,"%llx-%llx %4s %llx %31s %llu %n",&start,&end,perms,&offset,device,&inode,&consumed)==6) {
            if(mapping || end<=start)goto done;
            line[strcspn(line,"\n")]=0;
            mapping=1;fields=0;++count;category=rbx_memory_category(line+consumed);
            if(!rbx_memory_add(&result.mapped,end-start))goto done;
            if(category==RBX_MEMORY_GPU && !rbx_memory_add(&result.gpu_mapped,end-start))goto done;
            continue;
        }
        if(!mapping)goto done;
        if(!strncmp(line,"VmFlags:",8)){if(fields!=7)goto done;mapping=0;continue;}
        unsigned bit=!strncmp(line,"Rss:",4)?1:!strncmp(line,"Pss:",4)?2:!strncmp(line,"Swap:",5)?4:0;
        if(!bit)continue;
        char *value=strchr(line,':')+1,*next;
        while(*value==' ' || *value=='\t')++value;
        if(*value<'0' || *value>'9')goto done;
        errno=0;unsigned long long kb=strtoull(value,&next,10);
        if(errno || kb>UINT64_MAX/1024 || (fields&bit))goto done;
        while(*next==' ' || *next=='\t')++next;
        if(strncmp(next,"kB",2) || next[2+strspn(next+2," \t\r\n")])goto done;
        fields|=bit;uint64_t bytes=kb*1024;
        if(bit==1 && !rbx_memory_add(&result.rss[category],bytes))goto done;
        if(bit==2 && category!=RBX_MEMORY_GPU && !rbx_memory_add(&result.pss_ram,bytes))goto done;
        if(bit==4 && !rbx_memory_add(&result.swap,bytes))goto done;
    }
    complete=feof(input) && !ferror(input) && !mapping && count && UINT64_MAX-result.rss[0]>=result.rss[3];
    if(complete)*out=result;
done:
    free(line);return complete;
}
