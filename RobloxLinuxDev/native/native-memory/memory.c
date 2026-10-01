// Keep Darling's routines available during dyld startup. The embedding host
// can install libc's CPU-selected implementations after native loading ends.
#undef VARIANT_STATIC
#define VARIANT_STATIC 0
#define _platform_memmove trackb_fallback_memmove
#include "memmove.c"
#undef _platform_memmove
#define _platform_memcmp trackb_fallback_memcmp
#include "memcmp.c"
#undef _platform_memcmp

typedef void *(*Move)(void *,const void *,size_t);
typedef int (*Compare)(const void *,const void *,size_t);
static Move native_move;
static Compare native_compare;
void trackb_install_memory(void *move,void *compare) {
    if(move)__atomic_store_n(&native_move,(Move)move,__ATOMIC_RELEASE);
    if(compare)__atomic_store_n(&native_compare,(Compare)compare,__ATOMIC_RELEASE);
}
void *_platform_memmove(void *dst,const void *src,size_t size) {
    Move move=__atomic_load_n(&native_move,__ATOMIC_ACQUIRE);
    return move?move(dst,src,size):trackb_fallback_memmove(dst,src,size);
}
int _platform_memcmp(const void *a,const void *b,size_t size) {
    Compare compare=__atomic_load_n(&native_compare,__ATOMIC_ACQUIRE);
    return compare?compare(a,b,size):trackb_fallback_memcmp(a,b,size);
}
void *memmove(void *dst,const void *src,size_t size){return _platform_memmove(dst,src,size);}
void *memcpy(void *dst,const void *src,size_t size){return _platform_memmove(dst,src,size);}
int memcmp(const void *a,const void *b,size_t size){return _platform_memcmp(a,b,size);}
