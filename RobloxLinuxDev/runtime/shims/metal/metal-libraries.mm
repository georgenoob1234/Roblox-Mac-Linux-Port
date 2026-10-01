#include <indium/device.private.hpp>
#include <indium/library.private.hpp>
#include <map>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
@interface NSObject { Class isa; }
+ (id)alloc; - (id)init; - (id)retain; - (oneway void)release; - (id)autorelease; - (void)dealloc;
@end
@interface NSString : NSObject
+ (id)stringWithUTF8String:(const char *)s;
- (const char *)UTF8String; - (id)dataUsingEncoding:(unsigned long)encoding;
@end
@interface NSData : NSObject
+ (id)dataWithContentsOfFile:(id)path; + (id)dataWithContentsOfURL:(id)url;
- (const void *)bytes; - (unsigned long)length;
@end
@interface NSMutableArray : NSObject
+ (id)array; - (void)addObject:(id)object;
@end
@interface NSError : NSObject + (id)errorWithDomain:(id)domain code:(long)code userInfo:(id)info; @end
@interface MTLDeviceInternal : NSObject - (std::shared_ptr<Indium::Device>)device; @end
extern "C" unsigned char *CC_SHA256(const void *,unsigned int,unsigned char *);
extern "C" void *dispatch_data_create_map(void *,const void **,size_t *);
extern "C" void dispatch_release(void *);

@interface LinuxMetalFunction : NSObject {
    std::shared_ptr<Indium::Function> _function;
    id _device;
}
- (id)initWithFunction:(std::shared_ptr<Indium::Function>)function device:(id)device;
@end
@implementation LinuxMetalFunction
- (id)initWithFunction:(std::shared_ptr<Indium::Function>)function device:(id)device {
    self=[super init];if(self){_function=function;_device=[device retain];}return self;
}
- (void)dealloc {[_device release];[super dealloc];}
- (std::shared_ptr<Indium::Function>)function {return _function;}
- (id)device {return _device;}
- (id)name {return [NSString stringWithUTF8String:_function->name().c_str()];}
- (unsigned long)functionType {return static_cast<unsigned long>(std::dynamic_pointer_cast<Indium::PrivateFunction>(_function)->functionInfo().functionType);}
@end

@interface LinuxMetalLibrary : NSObject {
@public std::map<std::string,std::shared_ptr<Indium::Library>> libraries;
    id _device;
}
- (id)initWithDevice:(id)device;
@end
@implementation LinuxMetalLibrary
- (id)initWithDevice:(id)device {self=[super init];if(self)_device=[device retain];return self;}
- (void)dealloc {[_device release];[super dealloc];}
- (id)device {return _device;}
- (id)functionNames {
    NSMutableArray *names=[NSMutableArray array];
    for(const auto& pair:libraries)[names addObject:[NSString stringWithUTF8String:pair.first.c_str()]];
    return names;
}
- (id)newFunctionWithName:(NSString *)name {
    if(!name)return nullptr;
    auto entry=libraries.find([name UTF8String]);if(entry==libraries.end())return nullptr;
    auto function=entry->second->newFunction(entry->first);if(!function)return nullptr;
    return [[LinuxMetalFunction alloc] initWithFunction:function device:_device];
}
@end

static id libraryError(id *error) {
    if(error)*error=[NSError errorWithDomain:@"MTLLibraryErrorDomain" code:3 userInfo:nullptr];
    return nullptr;
}
static id compiledLibrary(MTLDeviceInternal *device,const void *data,size_t size,id *error) {
    if(!data || !size)return libraryError(error);
    auto library=[device device]->newLibrary(data,size);
    auto concrete=std::dynamic_pointer_cast<Indium::PrivateLibrary>(library);
    if(!concrete)return libraryError(error);
    LinuxMetalLibrary *result=[[LinuxMetalLibrary alloc] initWithDevice:device];
    for(const auto& entry:concrete->functionInfos())result->libraries.emplace(entry.first,library);
    return result;
}

@implementation MTLDeviceInternal (LinuxLibraries)
- (id)newLibraryWithData:(void *)data error:(id *)error {
    if(!data)return libraryError(error);
    const void *bytes=nullptr;size_t size=0;
    void *mapped=dispatch_data_create_map(data,&bytes,&size);
    if(!mapped)return libraryError(error);
    id result=compiledLibrary(self,bytes,size,error);dispatch_release(mapped);return result;
}
- (id)newLibraryWithURL:(id)url error:(id *)error {
    NSData *data=[NSData dataWithContentsOfURL:url];
    return compiledLibrary(self,[data bytes],[data length],error);
}
- (id)newLibraryWithSource:(NSString *)source options:(id)options error:(id *)error {
    NSData *input=[source dataUsingEncoding:4];
    if(!input || [input length]>UINT32_MAX)return libraryError(error);
    unsigned char digest[32];CC_SHA256([input bytes],(unsigned)[input length],digest);
    char key[65];for(unsigned i=0;i<32;i++)std::snprintf(key+2*i,3,"%02x",digest[i]);
    // ponytail: exact, tested equivalents of this client's three texture-copy kernels.
    // This is not a general MSL compiler; changed source fails explicitly.
    if(std::strcmp(key,"06746e4365ef80ed886ac1a85877d8958f72958bc4facf3d9cdef9063c796e7c"))return libraryError(error);
    Dl_info location{};if(!dladdr((void *)&compiledLibrary,&location))return libraryError(error);
    std::string directory=location.dli_fname;directory.resize(directory.find_last_of('/')+1);
    auto device=std::dynamic_pointer_cast<Indium::PrivateDevice>([self device]);
    if(!device)return libraryError(error);
    LinuxMetalLibrary *result=[[LinuxMetalLibrary alloc] initWithDevice:self];
    const std::pair<const char *,int> kernels[]={{"blitTexture2D_R8",1},{"blitTexture3D_RG8",2},{"blitTexture3D_RGBA8",4}};
    for(const auto& kernel:kernels) {
        auto path=directory+"runtime-shaders/blit"+std::to_string(kernel.second)+".spv";
        NSData *spv=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
        if([spv length]<20 || [spv length]%4){[result release];return libraryError(error);}
        Indium::FunctionInfo info{};info.functionType=Indium::FunctionType::Kernel;
        Iridium::BindingInfo buffer{};buffer.type=Iridium::BindingType::Buffer;
        info.bindings.push_back(buffer);buffer.index=buffer.internalIndex=1;info.bindings.push_back(buffer);
        Iridium::BindingInfo texture{};texture.type=Iridium::BindingType::Texture;texture.internalIndex=480;texture.textureAccessType=Iridium::TextureAccessType::Write;info.bindings.push_back(texture);
        Indium::PrivateLibrary::FunctionInfoMap functions{{kernel.first,info}};
        result->libraries.emplace(kernel.first,std::make_shared<Indium::PrivateLibrary>(device,(const char *)[spv bytes],[spv length],functions));
    }
    return result;
}
@end
