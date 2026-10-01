#pragma once
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
static void *cached_native_symbol(const char *name) {
    // Only resident native APIs use this cache; guest dlsym retains its scope
    // and live lookup semantics. C TLS avoids C++ static-initializer recursion.
    static __thread struct {char name[64];void *value;} cache[64];
    unsigned hash=2166136261u;
    for(const unsigned char *p=(const void*)name;*p;++p)hash=(hash^*p)*16777619u;
    unsigned slot=hash&63;
    if(cache[slot].value && !strcmp(cache[slot].name,name))return cache[slot].value;
    int saved=errno;void *value=dlsym(RTLD_DEFAULT,name);errno=saved;
    // Retry missing symbols after later dlopen; never retain a failed lookup.
    if(value && strlen(name)<sizeof(cache[slot].name)) {
        strcpy(cache[slot].name,name);cache[slot].value=value;
    }
    return value;
}
