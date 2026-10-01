#include "cpu-query.h"
#include "cpu-topology.h"
#include <sys/sysctl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>

static void *cpu_original(void **slot,const char *name) {
    int error=errno;
    void *value=__atomic_load_n(slot,__ATOMIC_ACQUIRE);
    if(!value) {
        value=dlsym(RTLD_NEXT,name);
        if(value)__atomic_store_n(slot,value,__ATOMIC_RELEASE);
    }
    errno=error;return value;
}
static int cpu_kind(const char *name,size_t size) {
    static const char *names[]={"hw.ncpu","hw.availcpu","hw.logicalcpu","hw.logicalcpu_max","hw.physicalcpu","hw.physicalcpu_max"};
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i)
        if(size==strlen(names[i]) && !memcmp(name,names[i],size))return i>=4?2:1;
    return 0;
}
static int cpu_value(int kind,void *old,size_t *size) {
    if(!size){errno=EFAULT;return -1;}
    // Preserve Darling's integer query ABI: short buffers return EINVAL without
    // modifying length/data; a size-only query succeeds; wide buffers keep tails.
    if(!old){*size=sizeof(int);return 0;}
    if(*size<sizeof(int)){errno=EINVAL;return -1;}
    int saved=errno,value=trackb_cpu_count(kind==2);
    if(value<0)return -1;
    memcpy(old,&value,sizeof(value));*size=sizeof(value);errno=saved;return 0;
}
static int cpu_mib_kind(int *name,unsigned length) {
    int kind=0;
    if(name && length==2 && name[0]==CTL_HW) {
        // Darling's sysctl_hw.c defines these four private OIDs (1000..1003).
        if(name[1]==HW_NCPU || name[1]==HW_AVAILCPU || name[1]==1002 || name[1]==1003)kind=1;
        else if(name[1]==1000 || name[1]==1001)kind=2;
    }
    return kind;
}
int trackb_sysctl(int *name,unsigned length,void *old,size_t *size,void *new_value,size_t new_size) {
    int kind=cpu_mib_kind(name,length);
    if(kind && !new_value && !new_size)return cpu_value(kind,old,size);
    static void *slot;
    int (*original)(int*,unsigned,void*,size_t*,void*,size_t)=cpu_original(&slot,"sysctl");
    if(!original){errno=ENOSYS;return -1;}
    return original(name,length,old,size,new_value,new_size);
}
int trackb___sysctl(int *name,unsigned length,void *old,size_t *size,void *new_value,size_t new_size) {
    int kind=cpu_mib_kind(name,length);
    if(kind && !new_value && !new_size)return cpu_value(kind,old,size);
    static void *slot;
    int (*original)(int*,unsigned,void*,size_t*,void*,size_t)=cpu_original(&slot,"__sysctl");
    if(!original){errno=ENOSYS;return -1;}
    return original(name,length,old,size,new_value,new_size);
}
int trackb_sysctlbyname(const char *name,void *old,size_t *size,void *new_value,size_t new_size) {
#ifdef TRACKA_CPU_FEATURE_QUERY
    int feature_result;
    if(TRACKA_CPU_FEATURE_QUERY(name,name?strlen(name):0,old,size,new_value,new_size,&feature_result))return feature_result;
#endif
    int kind=name?cpu_kind(name,strlen(name)):0;
    if(kind && !new_value && !new_size)return cpu_value(kind,old,size);
    static void *slot;
    int (*original)(const char*,void*,size_t*,void*,size_t)=cpu_original(&slot,"sysctlbyname");
    if(!original){errno=ENOSYS;return -1;}
    return original(name,old,size,new_value,new_size);
}
int trackb___sysctlbyname(const char *name,size_t length,void *old,size_t *size,void *new_value,size_t new_size) {
#ifdef TRACKA_CPU_FEATURE_QUERY
    int feature_result;
    if(TRACKA_CPU_FEATURE_QUERY(name,length,old,size,new_value,new_size,&feature_result))return feature_result;
#endif
    int kind=name?cpu_kind(name,length):0;
    if(kind && !new_value && !new_size)return cpu_value(kind,old,size);
    static void *slot;
    int (*original)(const char*,size_t,void*,size_t*,void*,size_t)=cpu_original(&slot,"__sysctlbyname");
    if(!original){errno=ENOSYS;return -1;}
    return original(name,length,old,size,new_value,new_size);
}
long trackb_sysconf(int name) {
    if(name==_SC_NPROCESSORS_CONF || name==_SC_NPROCESSORS_ONLN) {
        int saved=errno,value=trackb_cpu_count(0);
        if(value>0)errno=saved;
        return value;
    }
    static void *slot;
    long (*original)(int)=cpu_original(&slot,"sysconf");
    if(!original){errno=ENOSYS;return -1;}
    return original(name);
}
#ifndef TRACKB_CPU_HOST
// Insert this small native library for framework-side calls, including early
// startup queries. Guest bindings also work when linked directly into the host.
int sysctl(int *n,unsigned l,void *o,size_t *s,void *v,size_t z){return trackb_sysctl(n,l,o,s,v,z);}
int __sysctl(int *n,unsigned l,void *o,size_t *s,void *v,size_t z){return trackb___sysctl(n,l,o,s,v,z);}
int sysctlbyname(const char *n,void *o,size_t *s,void *v,size_t z){return trackb_sysctlbyname(n,o,s,v,z);}
int __sysctlbyname(const char *n,size_t l,void *o,size_t *s,void *v,size_t z){return trackb___sysctlbyname(n,l,o,s,v,z);}
long sysconf(int n){return trackb_sysconf(n);}
#endif
