#include <indium/device.private.hpp>
#include <indium/dynamic-vk.hpp>
@interface NSObject { Class isa; } @end
@interface NSString : NSObject
+ (id)stringWithUTF8String:(const char *)s;
- (signed char)writeToFile:(id)path atomically:(signed char)atomic encoding:(unsigned long)encoding error:(id *)error;
@end
@interface NSError : NSObject + (id)errorWithDomain:(id)domain code:(long)code userInfo:(id)info; @end
@interface MTLDeviceInternal : NSObject
- (std::shared_ptr<Indium::Device>)device;
@end
@implementation MTLDeviceInternal (LinuxDeviceInfo)
- (id)name { return [NSString stringWithUTF8String:[self device]->name().c_str()]; }
- (signed char)supportsTextureSampleCount:(unsigned long)count { return count==1; }
- (signed char)isLowPower {
    auto dev=std::static_pointer_cast<Indium::PrivateDevice>([self device]);
    return dev->properties().deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}
- (signed char)hasUnifiedMemory { return [self isLowPower]; }
- (unsigned long)currentAllocatedSize {
    auto dev=std::static_pointer_cast<Indium::PrivateDevice>([self device]);
    return Indium::DynamicVK::allocatedSize(dev->device());
}
- (unsigned long)maxBufferLength {
    auto dev=std::static_pointer_cast<Indium::PrivateDevice>([self device]);
    return dev->properties().limits.maxStorageBufferRange;
}
- (unsigned long)recommendedMaxWorkingSetSize {
    auto dev=std::static_pointer_cast<Indium::PrivateDevice>([self device]);
    unsigned long largest=0;
    const auto& memory=dev->memoryProperties();
    for (unsigned i=0;i<memory.memoryHeapCount;i++)
        if(memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            largest=std::max(largest,static_cast<unsigned long>(memory.memoryHeaps[i].size));
    // ponytail: static 80% heap budget; use VK_EXT_memory_budget when accounting is added.
    return largest/5*4;
}
- (signed char)isDepth24Stencil8PixelFormatSupported {
    // MTLDeviceInternal is constructed only from Indium::PrivateDevice.
    auto dev=std::static_pointer_cast<Indium::PrivateDevice>([self device]);
    if (!dev) return 0;
    VkFormatProperties properties{};
    static Indium::DynamicVK::DynamicFunction<PFN_vkGetPhysicalDeviceFormatProperties> query("vkGetPhysicalDeviceFormatProperties");
    query(dev->physicalDevice(),VK_FORMAT_D24_UNORM_S8_UINT,&properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)!=0;
}
- (signed char)supportsFeatureSet:(unsigned long)feature {
    // ponytail: baseline Mac GPU family 1 only; higher tiers need API coverage tests.
    return feature==10000;
}
@end

#include <indium/texture.hpp>
@interface NSObject (Lifetime)
+ (Class)class; + (id)alloc; + (id)allocWithZone:(void *)zone;
- (id)init; - (id)autorelease;
@end
@interface MTLTextureDescriptor : NSObject @end
@interface LinuxTextureDescriptor : MTLTextureDescriptor {
@public Indium::TextureDescriptor value;
}
@end
@interface MTLTextureInternal : NSObject
- (id)initWithTexture:(std::shared_ptr<Indium::Texture>)texture device:(id)device resourceOptions:(unsigned long)options;
@end
@implementation MTLTextureDescriptor (LinuxDescriptor)
+ (id)allocWithZone:(void *)zone {
    return self==[MTLTextureDescriptor class] ? [LinuxTextureDescriptor allocWithZone:zone] : [super allocWithZone:zone];
}
+ (id)texture2DDescriptorWithPixelFormat:(unsigned long)format width:(unsigned long)width height:(unsigned long)height mipmapped:(signed char)mipmapped {
    LinuxTextureDescriptor *d=[[self alloc] init];
    d->value=Indium::TextureDescriptor::texture2DDescriptor(static_cast<Indium::PixelFormat>(format),width,height,mipmapped);
    return [d autorelease];
}
@end
@implementation LinuxTextureDescriptor
#define TEX_PROPERTY(name,Name) \
- (unsigned long)name {return static_cast<unsigned long>(value.name);} \
- (void)set##Name:(unsigned long)n {value.name=static_cast<decltype(value.name)>(n);}
TEX_PROPERTY(textureType,TextureType)
TEX_PROPERTY(pixelFormat,PixelFormat)
TEX_PROPERTY(width,Width)
TEX_PROPERTY(height,Height)
TEX_PROPERTY(depth,Depth)
TEX_PROPERTY(mipmapLevelCount,MipmapLevelCount)
TEX_PROPERTY(sampleCount,SampleCount)
TEX_PROPERTY(arrayLength,ArrayLength)
TEX_PROPERTY(resourceOptions,ResourceOptions)
TEX_PROPERTY(usage,Usage)
#undef TEX_PROPERTY
- (unsigned long)storageMode {return (static_cast<unsigned long>(value.resourceOptions)>>4)&15;}
- (void)setStorageMode:(unsigned long)n {value.resourceOptions=static_cast<Indium::ResourceOptions>((static_cast<unsigned long>(value.resourceOptions)&~0xf0ul)|(n<<4));}
- (unsigned long)cpuCacheMode {return static_cast<unsigned long>(value.resourceOptions)&15;}
- (void)setCpuCacheMode:(unsigned long)n {value.resourceOptions=static_cast<Indium::ResourceOptions>((static_cast<unsigned long>(value.resourceOptions)&~15ul)|n);}
- (id)copyWithZone:(void *)zone {LinuxTextureDescriptor *d=[[LinuxTextureDescriptor allocWithZone:zone] init];d->value=value;return d;}
@end
@implementation MTLDeviceInternal (LinuxTextures)
- (id)newTextureWithDescriptor:(LinuxTextureDescriptor *)descriptor {
    auto texture=[self device]->newTexture(descriptor->value);
    if (!texture) return nullptr;
    return [[MTLTextureInternal alloc] initWithTexture:texture device:self resourceOptions:static_cast<unsigned long>(descriptor->value.resourceOptions)];
}
@end

#include <indium/command-queue.hpp>
#include <objc/runtime.h>
#include <dlfcn.h>
@interface MTLCommandQueueInternal : NSObject - (id)commandBuffer; @end
@interface MTLCommandBufferInternal : NSObject
- (id)initWithCommandBuffer:(std::shared_ptr<Indium::CommandBuffer>)commandBuffer commandQueue:(id)commandQueue;
@end
@implementation MTLCommandQueueInternal (LinuxCommandBuffers)
- (id)commandBufferWithUnretainedReferences {
    // The client keeps the buffers and textures it encodes alive until
    // completion, so the renderer skips retaining them per draw.
    static const ptrdiff_t queueOffset = [] {
        Ivar ivar = class_getInstanceVariable(objc_getClass("MTLCommandQueueInternal"), "_queue");
        return ivar ? ivar_getOffset(ivar) : ptrdiff_t(-1);
    }();
    // Only the optimized renderer has the setter; other builds keep retaining.
    static const auto setRetained = reinterpret_cast<void (*)(Indium::CommandBuffer&, bool)>(
        dlsym(RTLD_DEFAULT, "_ZN6Indium21setRetainedReferencesERNS_13CommandBufferEb"));
    if (queueOffset < 0 || !setRetained) return [self commandBuffer];
    const auto& queue = *reinterpret_cast<std::shared_ptr<Indium::CommandQueue>*>(reinterpret_cast<char*>(self) + queueOffset);
    auto buffer = queue->commandBuffer();
    if (!buffer) return nullptr;
    setRetained(*buffer, false);
    return [[[MTLCommandBufferInternal alloc] initWithCommandBuffer:buffer commandQueue:self] autorelease];
}
@end
