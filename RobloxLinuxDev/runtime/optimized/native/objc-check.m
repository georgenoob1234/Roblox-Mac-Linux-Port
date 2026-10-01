#include <assert.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <string.h>
#define main reused_objc_check
#include "../../../native/objc-check.m"
#undef main
int main(void) {
    Dl_info info;assert(dladdr(dlsym(RTLD_DEFAULT,"objc_getClass"),&info));
    assert(strstr(info.dli_fname,"/mnt/lib/libobjc.A.dylib"));
    unsigned count=0;
    for(unsigned i=0;i<_dyld_image_count();++i)
        if(strstr(_dyld_get_image_name(i),"libobjc.A.dylib"))++count;
    assert(count==1);
    // Same source, compiled for native x86_64: no translator or callbacks.
    return reused_objc_check();
}
