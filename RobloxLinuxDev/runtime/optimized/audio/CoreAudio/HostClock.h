#pragma once
#include <MacTypes.h>
#ifdef __cplusplus
extern "C" {
#endif
UInt64 AudioGetCurrentHostTime(void);
Float64 AudioGetHostClockFrequency(void);
UInt32 AudioGetHostClockMinimumTimeDelta(void);
UInt64 AudioConvertHostTimeToNanos(UInt64 hostTime);
UInt64 AudioConvertNanosToHostTime(UInt64 nanos);
#ifdef __cplusplus
}
#endif
