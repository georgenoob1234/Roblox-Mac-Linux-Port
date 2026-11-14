// Darwin's "these pages are free" advice. Darling answers every advice above
// MADV_DONTNEED with success and does nothing, so an allocator that hands
// memory back with MADV_FREE or MADV_FREE_REUSABLE believes the pages are gone
// while the host keeps every one of them resident. macOS malloc returns memory
// that way, and so does mimalloc when built for macOS; the Roblox client embeds
// mimalloc and purges freed memory after one second. The process then never
// shrinks below its high-water mark.
//
// What xnu does (osfmk/vm/vm_map.c, vm_map_behavior_set):
// - MADV_FREE discards the pages of anonymous memory that nobody else shares;
//   they read as zeroes afterwards. Other memory is left alone.
// - MADV_FREE_REUSABLE does the same for private, unshared anonymous memory
//   (vm_map_reusable_pages) and the pages stop counting as resident at once.
//   The caller announces reuse with MADV_FREE_REUSE, which only repairs the
//   accounting.
//
// On Linux that is MADV_DONTNEED on a private anonymous mapping: the pages are
// freed immediately and read as zeroes. MADV_DONTNEED must not reach any other
// kind of mapping, where it would drop a private file mapping's modified pages
// back to the file's contents. Linux's own MADV_FREE accepts exactly private
// anonymous memory (EINVAL for file-backed, shared, locked and device
// mappings), so it serves as the test: only when it succeeds for the whole
// range are the pages dropped. Anything else keeps Darling's answer, success
// without an effect; xnu would return EINVAL for MADV_FREE_REUSABLE there, and
// no caller has been seen to depend on that.
//
// Every other advice keeps Darling's implementation.
#include <sys/mman.h>
#include <dlfcn.h>

enum { linux_madvise = 28, linux_madv_dontneed = 4, linux_madv_free = 8 };

// Guest addresses are the host's addresses.
static long host_madvise(void *address, size_t length, int advice) {
    long result;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"((long)linux_madvise), "D"((long)address), "S"((long)length), "d"((long)advice)
        : "rcx", "r11", "memory", "cc");
    return result;
}

int madvise(void *address, size_t length, int advice) {
    if (advice == MADV_FREE || advice == MADV_FREE_REUSABLE) {
        if (host_madvise(address, length, linux_madv_free) == 0)
            host_madvise(address, length, linux_madv_dontneed);
        return 0;
    }
    static int (*original)(void *, size_t, int);
    int (*call)(void *, size_t, int) = __atomic_load_n(&original, __ATOMIC_ACQUIRE);
    if (!call) {
        call = (int (*)(void *, size_t, int))dlsym(RTLD_NEXT, "madvise");
        __atomic_store_n(&original, call, __ATOMIC_RELEASE);
    }
    return call(address, length, advice);
}
