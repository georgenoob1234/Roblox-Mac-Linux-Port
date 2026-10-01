// Track A alone supplies the client's ISA queries; keep shared CPU count/fallback behavior.
#include "runtime-sidecar/cpu-features.h"
#define TRACKA_CPU_FEATURE_QUERY tracka_cpu_feature_query
#include "../../../native/cpu-query.c"
#undef TRACKA_CPU_FEATURE_QUERY
