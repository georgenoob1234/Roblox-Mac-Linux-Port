#pragma once
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if !defined(__x86_64__)
#error Track A CPU features require native x86_64
#endif

enum tracka_feature { TRACKA_SSE, TRACKA_SSE41, TRACKA_AVX, TRACKA_AVX2,
    TRACKA_AVX512F, TRACKA_X86_64, TRACKA_ARM, TRACKA_ADVSIMD };
static const char *const tracka_feature_names[] = {
    "hw.optional.sse", "hw.optional.sse4_1", "hw.optional.avx1_0",
    "hw.optional.avx2_0", "hw.optional.avx512f", "hw.optional.x86_64",
    "hw.optional.arm", "hw.optional.AdvSIMD"
};
struct tracka_cpu_caps { uint32_t ecx1, edx1, ebx7; uint64_t xcr0; };
static void tracka_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
    __asm__ volatile("cpuid" : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                     : "a"(leaf), "c"(subleaf));
}
static struct tracka_cpu_caps tracka_read_cpu_caps(void) {
    uint32_t regs[4]; struct tracka_cpu_caps caps = {0};
    tracka_cpuid(0, 0, regs); uint32_t max_leaf = regs[0];
    if (max_leaf >= 1) {
        tracka_cpuid(1, 0, regs); caps.ecx1 = regs[2]; caps.edx1 = regs[3];
        // Never execute XGETBV unless CPU XSAVE and OSXSAVE are both present.
        if ((caps.ecx1 & (3u << 26)) == (3u << 26)) {
            uint32_t lo, hi;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
            caps.xcr0 = ((uint64_t)hi << 32) | lo;
        }
    }
    if (max_leaf >= 7) { tracka_cpuid(7, 0, regs); caps.ebx7 = regs[1]; }
    return caps;
}
static int tracka_feature_value(enum tracka_feature feature, struct tracka_cpu_caps caps) {
    int avx = (caps.ecx1 & (7u << 26)) == (7u << 26) && (caps.xcr0 & 6) == 6;
    switch (feature) {
        case TRACKA_SSE: return !!(caps.edx1 & (1u << 25));
        case TRACKA_SSE41: return !!(caps.ecx1 & (1u << 19));
        case TRACKA_AVX: return avx;
        case TRACKA_AVX2: return avx && !!(caps.ebx7 & (1u << 5));
        case TRACKA_AVX512F: return avx && !!(caps.ebx7 & (1u << 16)) && (caps.xcr0 & 0xe6) == 0xe6;
        case TRACKA_X86_64: return 1;
        case TRACKA_ARM: case TRACKA_ADVSIMD: return 0;
    }
    return 0;
}
// A handled flag keeps unknown-name/count fallback wholly in shared cpu-query.c.
static int tracka_cpu_feature_query(const char *name, size_t length, void *old,
        size_t *size, void *new_value, size_t new_size, int *result) {
    if (!name) return 0;
    unsigned feature;
    for (feature = 0; feature < sizeof(tracka_feature_names) / sizeof(*tracka_feature_names); ++feature)
        if (length == strlen(tracka_feature_names[feature]) && !memcmp(name, tracka_feature_names[feature], length)) break;
    if (feature == sizeof(tracka_feature_names) / sizeof(*tracka_feature_names)) return 0;
    int error = new_value || new_size ? EPERM : !size ? EFAULT : old && *size < sizeof(int) ? EINVAL : 0;
    if (error) { errno = error; *result = -1; return 1; }
    if (old) {
        int value = tracka_feature_value((enum tracka_feature)feature, tracka_read_cpu_caps());
        memcpy(old, &value, sizeof(value));
    }
    *size = sizeof(int); *result = 0;
    return 1;
}
