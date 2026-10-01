// Build this exact query check as native Darwin and translated ARM64.
typedef unsigned long size_t;
extern int sysctl(int*,unsigned,void*,size_t*,void*,size_t);
extern int __sysctl(int*,unsigned,void*,size_t*,void*,size_t);
extern int sysctlbyname(const char*,void*,size_t*,void*,size_t);
extern int __sysctlbyname(const char*,size_t,void*,size_t*,void*,size_t);
extern int sysctlnametomib(const char*,int*,size_t*);
extern long sysconf(int);
extern size_t strlen(const char*);
extern int atoi(const char*);
extern int printf(const char*,...);
extern int *__error(void);
extern void *dlsym(void*,const char*);
#define CHECK(test) do{if(!(test)){printf("FAIL CPU query line %d errno=%d\n",__LINE__,*__error());return 1;}}while(0)
@interface NSProcessInfo
+ (id)processInfo;
- (unsigned long)processorCount;
- (unsigned long)activeProcessorCount;
@end
int main(int argc,char **argv) {
    CHECK(argc==3);
    int physical=atoi(argv[1]),logical=atoi(argv[2]);CHECK(physical>0 && logical>=physical);
    const char *names[]={"hw.ncpu","hw.availcpu","hw.logicalcpu","hw.logicalcpu_max","hw.physicalcpu","hw.physicalcpu_max"};
    int (*dynamic)(const char*,void*,size_t*,void*,size_t)=dlsym((void*)-2,"sysctlbyname");
    CHECK(dynamic);
    for(unsigned i=0;i<6;++i) {
        int expected=i>=4?physical:logical,mib[8];size_t mib_size=8;
        CHECK(!sysctlnametomib(names[i],mib,&mib_size) && mib_size==2);
        for(unsigned route=0;route<5;++route) {
            struct {int count;unsigned guard;} value={-1,0x12345678};size_t size=sizeof(value);
            *__error()=123;
            int result=route==0?sysctlbyname(names[i],&value,&size,0,0):
                route==1?sysctl(mib,2,&value,&size,0,0):
                route==2?__sysctl(mib,2,&value,&size,0,0):
                route==3?__sysctlbyname(names[i],strlen(names[i]),&value,&size,0,0):
                dynamic(names[i],&value,&size,0,0);
            CHECK(!result && value.count==expected && size==sizeof(int) && value.guard==0x12345678 && *__error()==123);
        }
        size_t size=0;CHECK(!sysctlbyname(names[i],0,&size,0,0) && size==sizeof(int));
        unsigned value=0xaabbccdd;size=3;
        CHECK(sysctlbyname(names[i],&value,&size,0,0)==-1 && *__error()==22 && size==3 && value==0xaabbccdd);
        size=3;CHECK(sysctl(mib,2,&value,&size,0,0)==-1 && *__error()==22 && size==3 && value==0xaabbccdd);
    }
    *__error()=123;
    CHECK(sysconf(57)==logical && *__error()==123); // Darwin IDs, not Linux IDs.
    CHECK(sysconf(58)==logical && *__error()==123);
    CHECK([[NSProcessInfo processInfo] processorCount]==(unsigned)logical);
    CHECK([[NSProcessInfo processInfo] activeProcessorCount]==(unsigned)logical);
    // Unrelated queries still reach the original functions without recursion.
    int page=0;size_t size=sizeof(page);CHECK(!sysctlbyname("hw.pagesize",&page,&size,0,0) && page>0);
    CHECK(sysconf(29)==page);
    size=sizeof(page);CHECK(sysctlbyname("hw.trackb_missing_cpu_query",&page,&size,0,0)==-1 && *__error()!=0);
    printf("PASS CPU queries: physical=%d logical=%d; named/MIB/private/dlsym/sysconf/NSProcessInfo; sizes/errno/fallback\n",physical,logical);
    return 0;
}
