#include "CoreAudio/HostClock.h"
#include <mach/mach_time.h>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <thread>
#include <vector>
#include <unistd.h>
template<class T> T resolve(const char* name) {
 auto p=dlsym(RTLD_DEFAULT,name);Dl_info info={};assert(p&&dladdr(p,&info));
 printf("AUDIO clock symbol=%s provider=%s\n",name,info.dli_fname);
 return reinterpret_cast<T>(p);
}
int main() {
 setvbuf(stdout,nullptr,_IOLBF,0);
 auto current=resolve<UInt64(*)(void)>("AudioGetCurrentHostTime");
 auto frequency=resolve<Float64(*)(void)>("AudioGetHostClockFrequency");
 auto delta=resolve<UInt32(*)(void)>("AudioGetHostClockMinimumTimeDelta");
 auto nanos=resolve<UInt64(*)(UInt64)>("AudioConvertHostTimeToNanos");
 auto ticks=resolve<UInt64(*)(UInt64)>("AudioConvertNanosToHostTime");
 mach_timebase_info_data_t t={};assert(!mach_timebase_info(&t)&&t.numer&&t.denom);
 auto worker=[&] {
  for(unsigned j=0;j<1000;j++) {
   auto before=mach_absolute_time(),now=current(),after=mach_absolute_time();
   assert(before<=now&&now<=after&&now);
   assert(fabs(frequency()-1e9*double(t.denom)/t.numer)<.01&&delta()==1);
   for(UInt64 value:{UInt64(0),UInt64(1),UInt64(1000000000),UInt64(1)<<53,UINT64_MAX}) {
    assert(nanos(value)==UInt64(__uint128_t(value)*t.numer/t.denom));
    assert(ticks(value)==UInt64(__uint128_t(value)*t.denom/t.numer));
   }
  }
 };
 std::vector<std::thread> threads;
 for(int j=0;j<8;j++)threads.emplace_back(worker);
 for(auto& thread:threads)thread.join();
 auto start=nanos(current());usleep(20000);auto end=nanos(current());assert(end>start&&end-start>=10000000);
 printf("PASS CoreAudio clock ABI, 8 concurrent readers, Mach bounds, frequency and 64-bit conversions; sleep_ns=%llu\n",end-start);
}
