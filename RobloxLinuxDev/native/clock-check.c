// ARM ABI integration check: all clock calls route through the native adapter.
typedef unsigned long long u64;
struct timespec {long seconds,nanoseconds;};
extern int clock_gettime(unsigned,struct timespec*);
extern u64 clock_gettime_nsec_np(unsigned);
extern int usleep(unsigned);
struct timeval {long seconds; int microseconds;};
extern int gettimeofday(struct timeval*,void*);
extern long time(long*);
extern int puts(const char*);
extern void *dlopen(const char*,int);
extern void *dlsym(void*,const char*);
extern u64 mach_absolute_time(void),mach_continuous_time(void),mach_approximate_time(void),mach_continuous_approximate_time(void);
// The adapter's mach clocks must stay on the time line of libsystem_kernel's
// own functions: every reading lies between two readings of the original.
static int brackets(void *kernel,const char *original,u64 (*adapter)(void)) {
    u64 (*stock)(void)=(u64 (*)(void))dlsym(kernel,original);
    if(!stock)return 0;
    for(unsigned i=0;i<200000;++i) {
        u64 before=stock(),value=adapter(),after=stock();
        if(!before || value<before || value>after)return 0;
    }
    return 1;
}
static u64 read_time(unsigned clock) {
    struct timespec t;
    if(clock_gettime(clock,&t) || t.seconds<0 || t.nanoseconds<0 || t.nanoseconds>=1000000000)return 0;
    return (u64)t.seconds*1000000000+t.nanoseconds;
}
int main(void) {
    // Darwin IDs, deliberately different from Linux: realtime=0, monotonic=6,
    // uptime_raw=8. Sampling >2 seconds crosses multiple rounding boundaries.
    u64 start=read_time(6),raw=read_time(8),previous=start;
    if(!start || !raw || read_time(0)<1000000000000000000ULL)return 1;
    for(unsigned i=0;i<240;++i) {
        usleep(10000);
        u64 now=read_time(6),ns=clock_gettime_nsec_np(6),last=read_time(6);
        if(now<previous || ns<now || last<ns || !now)return 2;
        previous=last;
    }
    u64 elapsed=previous-start,raw_elapsed=read_time(8)-raw;
    if(elapsed<2000000000ULL || raw_elapsed<2000000000ULL)return 3;
    if(elapsed>raw_elapsed+100000000 || raw_elapsed>elapsed+100000000)return 4;
    if(clock_gettime(999, &(struct timespec){0})!=-1 || clock_gettime_nsec_np(999)!=0)return 5;
    if(clock_gettime(6,(void*)0)!=-1)return 6;
    // gettimeofday (vDSO in the native adapter) must agree with realtime.
    for(unsigned i=0;i<1000;++i) {
        u64 before=read_time(0);struct timeval tv={0,-1};
        if(gettimeofday(&tv,(void*)0) || tv.microseconds<0 || tv.microseconds>=1000000)return 7;
        u64 at=(u64)tv.seconds*1000000000+(u64)tv.microseconds*1000,after=read_time(0);
        if(at+1000<before || at>after)return 8;
    }
    // time() (vDSO in the native adapter) is the realtime clock in whole seconds.
    for(unsigned i=0;i<1000;++i) {
        long stored=-1,before=(long)(read_time(0)/1000000000),value=time(&stored),after=(long)(read_time(0)/1000000000);
        if(value<before || value>after || stored!=value || time((void*)0)<value)return 15;
    }
    void *kernel=dlopen("/usr/lib/system/libsystem_kernel.dylib",1);
    if(!kernel)return 9;
    // On Darling the continuous base is 0 and approximate time is not published,
    // so all four share mach_absolute_time's line; each is checked against its own original.
    if(!brackets(kernel,"mach_absolute_time",mach_absolute_time))return 10;
    if(!brackets(kernel,"mach_continuous_time",mach_continuous_time))return 11;
    if(!brackets(kernel,"mach_approximate_time",mach_approximate_time))return 12;
    if(!brackets(kernel,"mach_continuous_approximate_time",mach_continuous_approximate_time))return 13;
    // CLOCK_MONOTONIC_RAW (4) is mach_continuous_time in nanoseconds.
    for(unsigned i=0;i<1000;++i) {
        u64 before=mach_continuous_time(),raw_ns=clock_gettime_nsec_np(4),after=mach_continuous_time();
        if(raw_ns<before || raw_ns>after)return 14;
    }
    puts("PASS: Darwin monotonic clocks agree, never reverse across second boundaries, preserve other clock IDs, gettimeofday and time match realtime, and the mach clock family stays on the original time line");
    return 0;
}
