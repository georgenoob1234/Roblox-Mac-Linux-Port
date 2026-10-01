// Reuse startup-safe fallbacks and atomic publication from Track B unchanged.
#define trackb_install_memory tracka_install_memory
#include "../../../native/native-memory/memory.c"
#include "../../src/darling/src/startup/mldr/elfcalls/elfcalls.h"
#include <stdlib.h>
#include <errno.h>
extern struct elf_calls *_elfcalls;

int tracka_memory_active(void) {
    return __atomic_load_n(&native_move,__ATOMIC_ACQUIRE) != NULL &&
           __atomic_load_n(&native_compare,__ATOMIC_ACQUIRE) != NULL;
}

__attribute__((constructor)) static void install_host_memory(void) {
    const char *enabled=getenv("TRACKA_NATIVE_MEMORY");
    if(!enabled || enabled[0]!='1' || enabled[1] || !_elfcalls)return;
    int saved=errno;
    void *libc=_elfcalls->dlopen("libc.so.6");
    if(libc){
        void *move=_elfcalls->dlsym(libc,"memmove");
        void *compare=_elfcalls->dlsym(libc,"memcmp");
        if(move && compare)tracka_install_memory(move,compare);
        // Keep the handle resident for the published pointers' lifetime.
        else _elfcalls->dlclose(libc);
    }
    errno=saved;
}
