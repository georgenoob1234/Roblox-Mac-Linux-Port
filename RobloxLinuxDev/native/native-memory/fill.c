// A byte fill lets the compiler vectorize instead of calling memmove per four
// bytes through memset_pattern4. No host functions are needed during dyld startup.
#include <platform/string.h>

void *_platform_memset(void *dst, int value, size_t size) {
    unsigned char *bytes = dst;
    for (size_t i = 0; i < size; ++i) bytes[i] = (unsigned char)value;
    return dst;
}

void *memset(void *dst, int value, size_t size) {
    return _platform_memset(dst, value, size);
}

// Retain Darling's bzero/__bzero/platform aliases and signatures.
#undef _PLATFORM_OPTIMIZED_MEMSET
#define _PLATFORM_OPTIMIZED_MEMSET 1
#include "bzero.c"
