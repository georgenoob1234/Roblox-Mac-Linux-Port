#include <iridium/iridium.hpp>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <cstdio>
#include <unistd.h>
@interface NSObject + (id)alloc; - (id)init; - (void)release; @end
@interface NSString : NSObject
+ (id)stringWithUTF8String:(const char *)s; - (const char *)UTF8String;
@end
@interface NSData : NSObject
+ (id)dataWithContentsOfFile:(id)path;
+ (id)dataWithBytesNoCopy:(void *)bytes length:(unsigned long)length freeWhenDone:(signed char)free; - (const void *)bytes; - (unsigned long)length;
@end
@interface NSDictionary : NSObject - (id)objectForKey:(id)key; @end
@interface NSArray : NSObject - (unsigned long)count; - (id)objectAtIndex:(unsigned long)i; @end
@interface NSNumber : NSObject - (unsigned long)unsignedLongValue; @end
@interface NSJSONSerialization : NSObject
+ (id)JSONObjectWithData:(id)data options:(unsigned long)options error:(id *)error;
@end
@interface NSAutoreleasePool : NSObject @end
extern "C" unsigned char *CC_SHA256(const void *,unsigned int,unsigned char *);
static std::string cacheDirectory() {
    const char *cache=std::getenv("ROBLOX_MAC_SHADER_CACHE");
    return cache ? cache : "";
}
// Shader cache paths name host files. Avoid a Darwin/server round trip for
// every Foundation stat/read while loading thousands of precompiled shaders.
struct ElfCalls {void *(*dlopen)(const char *);int (*dlclose)(void *);void *(*dlsym)(void *,const char *);};
extern "C" ElfCalls *_elfcalls;
extern "C" void dispatch_once_f(long *,void *,void (*)(void *));
static int (*hostOpen)(const char *,int);
static long (*hostRead)(int,void *,size_t),(*hostSeek)(int,long,int);
static int (*hostClose)(int);
static int *(*hostErrno)();
static void loadHostIO(void *) {
    if(!_elfcalls)return;void *libc=_elfcalls->dlopen("libc.so.6");if(!libc)return;
    hostOpen=(decltype(hostOpen))_elfcalls->dlsym(libc,"open");
    hostRead=(decltype(hostRead))_elfcalls->dlsym(libc,"read");
    hostSeek=(decltype(hostSeek))_elfcalls->dlsym(libc,"lseek");
    hostClose=(decltype(hostClose))_elfcalls->dlsym(libc,"close");
    hostErrno=(decltype(hostErrno))_elfcalls->dlsym(libc,"__errno_location");
}
static NSData *cacheData(const std::string& path) {
    static long once;dispatch_once_f(&once,nullptr,loadHostIO);
    constexpr const char *prefix="/Volumes/SystemRoot";
    if(!hostOpen || !hostRead || !hostSeek || !hostClose || path.compare(0,std::strlen(prefix),prefix) || path.size()<=std::strlen(prefix) || path[std::strlen(prefix)]!='/')
        return [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
    int fd=hostOpen(path.c_str()+std::strlen(prefix),0x80000 /* Linux O_CLOEXEC */);
    if(fd<0)return nullptr;
    long size=hostSeek(fd,0,2); // regular, bounded shader/reflection cache file
    if(size<=0 || size>32*1024*1024 || hostSeek(fd,0,0)<0){hostClose(fd);return nullptr;}
    void *bytes=std::malloc(size);size_t offset=0;
    if(bytes)while(offset<(size_t)size){long n=hostRead(fd,(char *)bytes+offset,size-offset);if(n<0 && hostErrno && *hostErrno()==4)continue;if(n<=0)break;offset+=n;}
    hostClose(fd);
    if(offset!=(size_t)size){std::free(bytes);return nullptr;}
    return [NSData dataWithBytesNoCopy:bytes length:size freeWhenDone:1];
}
namespace Iridium {
bool init() { const auto path=cacheDirectory();return !path.empty() && access(path.c_str(),R_OK)==0; }
void finit() {}
void *translate(const void *input,size_t size,size_t& outSize,OutputInfo& info) {
    outSize=0;
    if (!input || size<88 || size>UINT_MAX || std::memcmp(input,"MTLB",4)) return nullptr;
    unsigned char digest[32];CC_SHA256(input,(unsigned)size,digest);
    char key[65];for (size_t i=0;i<32;++i)std::snprintf(key+2*i,3,"%02x",digest[i]);
    const auto base=cacheDirectory()+"/"+key;
    NSAutoreleasePool *pool=[[NSAutoreleasePool alloc] init];
    NSData *binary=cacheData(base+".spv");
    NSData *json=cacheData(base+".json");
    NSDictionary *meta=json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nullptr] : nullptr;
    if (!binary || !meta || [[meta objectForKey:@"version"] unsignedLongValue]!=1) {
        std::fprintf(stderr,"metal2vulkan cache miss/invalid entry: %s\n",key);[pool release];return nullptr;
    }
    FunctionInfo function{};
    auto stage=[[meta objectForKey:@"stage"] unsignedLongValue];
    NSString *entry=[meta objectForKey:@"entry"];
    if (stage<1 || stage>3 || !entry || [binary length]<20 || [binary length]%4) { [pool release];return nullptr; }
    function.type=static_cast<FunctionType>(stage);
    NSArray *bindings=[meta objectForKey:@"bindings"];
    for (unsigned long i=0;i<[bindings count];++i) {
        NSArray *b=[bindings objectAtIndex:i];
        if ([b count]!=4) { [pool release];return nullptr; }
        auto type=[[b objectAtIndex:0] unsignedLongValue];
        auto access=[[b objectAtIndex:3] unsignedLongValue];
        if (type>2 || access>3) { [pool release];return nullptr; }
        BindingInfo binding{};binding.type=static_cast<BindingType>(type);
        binding.index=[[b objectAtIndex:1] unsignedLongValue];
        binding.internalIndex=[[b objectAtIndex:2] unsignedLongValue];
        binding.textureAccessType=static_cast<TextureAccessType>(access);
        function.bindings.push_back(binding);
    }
    void *result=std::malloc([binary length]);
    if (result) {
        std::memcpy(result,[binary bytes],[binary length]);outSize=[binary length];
        info.functionInfos.emplace([entry UTF8String],std::move(function));
    }
    [pool release];return result;
}
}
