// ARM ABI integration check: all clock calls route through the native adapter.
typedef unsigned long long u64;
struct timespec {long seconds,nanoseconds;};
extern int clock_gettime(unsigned,struct timespec*);
extern u64 clock_gettime_nsec_np(unsigned);
extern int usleep(unsigned);
struct timeval {long seconds; int microseconds;};
extern int gettimeofday(struct timeval*,void*);
extern int puts(const char*);
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
    puts("PASS: Darwin monotonic clocks agree, never reverse across second boundaries, preserve other clock IDs, and gettimeofday matches realtime");
    return 0;
}
