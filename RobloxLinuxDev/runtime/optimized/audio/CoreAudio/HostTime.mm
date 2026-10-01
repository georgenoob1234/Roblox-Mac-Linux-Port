#include "HostClock.h"
#include <mach/mach_time.h>

static const mach_timebase_info_data_t& hostTimebase() {
 static const auto info=[] {mach_timebase_info_data_t value={};mach_timebase_info(&value);return value;}();
 return info;
}

UInt64 AudioGetCurrentHostTime(void)
{
    return mach_absolute_time();
}

Float64 AudioGetHostClockFrequency(void) {
 const auto& t=hostTimebase();return 1000000000.0*double(t.denom)/double(t.numer);
}
UInt32 AudioGetHostClockMinimumTimeDelta(void) {return 1;}
UInt64 AudioConvertHostTimeToNanos(UInt64 hostTime) {
 const auto& t=hostTimebase();return UInt64(__uint128_t(hostTime)*t.numer/t.denom);
}
UInt64 AudioConvertNanosToHostTime(UInt64 nanos) {
 const auto& t=hostTimebase();return UInt64(__uint128_t(nanos)*t.denom/t.numer);
}
