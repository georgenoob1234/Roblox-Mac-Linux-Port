#include <indium/indium.hpp>
#include <objc/runtime.h>
#include <stdexcept>
@interface NSObject { Class isa; }
+ (id)alloc; - (id)init; - (id)autorelease; - (id)retain; - (void)release;
@end

@interface MTLTextureInternal : NSObject - (std::shared_ptr<Indium::Texture>)texture; @end
@interface LinuxSamplerState : NSObject - (std::shared_ptr<Indium::SamplerState>)state; @end
@interface MTLRenderCommandEncoderInternal : NSObject
- (std::shared_ptr<Indium::RenderCommandEncoder>)encoder;
@end
static std::shared_ptr<Indium::RenderCommandEncoder>& renderEncoder(id object) {
    static Ivar slot=class_getInstanceVariable(objc_getClass("MTLRenderCommandEncoderInternal"),"_encoder");
    if(!slot)throw std::runtime_error("Darling render encoder ABI is unavailable");
    return *reinterpret_cast<std::shared_ptr<Indium::RenderCommandEncoder> *>((char *)object+ivar_getOffset(slot));
}
@implementation MTLRenderCommandEncoderInternal (LinuxTextures)
// The command retains the backend through GPU completion; the ended wrapper
// must not extend the lifetime of inline buffers and descriptor arenas.
- (void)endEncoding {auto& encoder=renderEncoder(self);encoder->endEncoding();encoder.reset();}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {[self encoder]->setDepthBias(bias,slope,clamp);}
- (void)setDepthClipMode:(unsigned long)mode {[self encoder]->setDepthClipMode(static_cast<Indium::DepthClipMode>(mode));}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {[self encoder]->setBlendColor(r,g,b,a);}
- (void)setStencilReferenceValue:(unsigned)value {[self encoder]->setStencilReferenceValue(value);}
- (void)setStencilFrontReferenceValue:(unsigned)front backReferenceValue:(unsigned)back {[self encoder]->setStencilReferenceValue(front,back);}
- (void)setVertexSamplerState:(LinuxSamplerState *)sampler atIndex:(unsigned long)index {[self encoder]->setVertexSamplerState(sampler ? [sampler state] : nullptr,index);}
- (void)setFragmentSamplerState:(LinuxSamplerState *)sampler atIndex:(unsigned long)index {[self encoder]->setFragmentSamplerState(sampler ? [sampler state] : nullptr,index);}
- (void)setVertexSamplerState:(LinuxSamplerState *)sampler lodMinClamp:(float)minimum lodMaxClamp:(float)maximum atIndex:(unsigned long)index {[self encoder]->setVertexSamplerState(sampler ? [sampler state] : nullptr,minimum,maximum,index);}
- (void)setFragmentSamplerState:(LinuxSamplerState *)sampler lodMinClamp:(float)minimum lodMaxClamp:(float)maximum atIndex:(unsigned long)index {[self encoder]->setFragmentSamplerState(sampler ? [sampler state] : nullptr,minimum,maximum,index);}
- (void)setVertexTexture:(MTLTextureInternal *)texture atIndex:(unsigned long)index {[self encoder]->setVertexTexture(texture ? [texture texture] : nullptr,index);}
- (void)setFragmentTexture:(MTLTextureInternal *)texture atIndex:(unsigned long)index {[self encoder]->setFragmentTexture(texture ? [texture texture] : nullptr,index);}
- (void)setVertexTextures:(MTLTextureInternal *const *)textures withRange:(Indium::Range<size_t>)range {
    for(size_t i=0;i<range.length;i++)[self setVertexTexture:textures[i] atIndex:range.start+i];
}
- (void)setFragmentTextures:(MTLTextureInternal *const *)textures withRange:(Indium::Range<size_t>)range {
    for(size_t i=0;i<range.length;i++)[self setFragmentTexture:textures[i] atIndex:range.start+i];
}
@end
@interface MTLBufferInternal : NSObject - (std::shared_ptr<Indium::Buffer>)buffer; @end
@interface MTLCommandBufferInternal : NSObject @end

@interface LinuxBlitEncoder : NSObject {
@public std::shared_ptr<Indium::BlitCommandEncoder> encoder;
}
@end
static std::shared_ptr<Indium::CommandBuffer>& commandBuffer(id object) {
    // Darling exposes no commandBuffer getter. Resolve the matching runtime ivar instead
    // of assuming an offset; the bundled framework and this bridge use the same C++ ABI.
    static Ivar slot=class_getInstanceVariable(objc_getClass("MTLCommandBufferInternal"),"_commandBuffer");
    if(!slot)throw std::runtime_error("Darling Metal command-buffer ABI is unavailable");
    return *reinterpret_cast<std::shared_ptr<Indium::CommandBuffer> *>((char *)object+ivar_getOffset(slot));
}
extern "C" void *_Block_copy(const void *),_Block_release(const void *);
@implementation MTLCommandBufferInternal (LinuxBlit)
- (void)addScheduledHandler:(void (^)(id))handler {
    if(!handler)throw std::invalid_argument("Metal scheduled handler is null");
    std::shared_ptr<void> block(_Block_copy(handler),_Block_release);
    std::shared_ptr<void> owner([self retain],[](void *object){[(id)object release];});
    commandBuffer(self)->addScheduledHandler([block,owner](std::shared_ptr<Indium::CommandBuffer>) {
        ((void (^)(id))block.get())((id)owner.get());
    });
}
- (id)blitCommandEncoder {
    auto& command=commandBuffer(self);
    LinuxBlitEncoder *result=[[LinuxBlitEncoder alloc] init];
    result->encoder=command->blitCommandEncoder();
    return [result autorelease];
}
@end
@implementation LinuxBlitEncoder
// The command owns the encoder through completion; this autoreleased wrapper
// must not keep its staging buffers alive after the GPU has finished.
- (void)endEncoding {encoder->endEncoding();encoder.reset();}
- (void)copyFromBuffer:(MTLBufferInternal *)source sourceOffset:(unsigned long)offset toBuffer:(MTLBufferInternal *)destination destinationOffset:(unsigned long)destOffset size:(unsigned long)size {
    encoder->copy([source buffer],offset,[destination buffer],destOffset,size);
}
- (void)fillBuffer:(MTLBufferInternal *)buffer range:(Indium::Range<size_t>)range value:(unsigned char)value {
    encoder->fillBuffer([buffer buffer],range,value);
}
- (void)copyFromBuffer:(MTLBufferInternal *)source sourceOffset:(unsigned long)offset sourceBytesPerRow:(unsigned long)row sourceBytesPerImage:(unsigned long)image sourceSize:(Indium::Size)size toTexture:(MTLTextureInternal *)destination destinationSlice:(unsigned long)slice destinationLevel:(unsigned long)level destinationOrigin:(Indium::Origin)origin options:(unsigned long)options {
    encoder->copy([source buffer],offset,row,image,size,[destination texture],slice,level,origin,static_cast<Indium::BlitOption>(options));
}
- (void)copyFromBuffer:(MTLBufferInternal *)source sourceOffset:(unsigned long)offset sourceBytesPerRow:(unsigned long)row sourceBytesPerImage:(unsigned long)image sourceSize:(Indium::Size)size toTexture:(MTLTextureInternal *)destination destinationSlice:(unsigned long)slice destinationLevel:(unsigned long)level destinationOrigin:(Indium::Origin)origin {
    [self copyFromBuffer:source sourceOffset:offset sourceBytesPerRow:row sourceBytesPerImage:image sourceSize:size toTexture:destination destinationSlice:slice destinationLevel:level destinationOrigin:origin options:0];
}
- (void)copyFromTexture:(MTLTextureInternal *)source sourceSlice:(unsigned long)slice sourceLevel:(unsigned long)level sourceOrigin:(Indium::Origin)origin sourceSize:(Indium::Size)size toBuffer:(MTLBufferInternal *)destination destinationOffset:(unsigned long)offset destinationBytesPerRow:(unsigned long)row destinationBytesPerImage:(unsigned long)image options:(unsigned long)options {
    encoder->copy([source texture],slice,level,origin,size,[destination buffer],offset,row,image,static_cast<Indium::BlitOption>(options));
}
- (void)copyFromTexture:(MTLTextureInternal *)source sourceSlice:(unsigned long)slice sourceLevel:(unsigned long)level sourceOrigin:(Indium::Origin)origin sourceSize:(Indium::Size)size toBuffer:(MTLBufferInternal *)destination destinationOffset:(unsigned long)offset destinationBytesPerRow:(unsigned long)row destinationBytesPerImage:(unsigned long)image {
    [self copyFromTexture:source sourceSlice:slice sourceLevel:level sourceOrigin:origin sourceSize:size toBuffer:destination destinationOffset:offset destinationBytesPerRow:row destinationBytesPerImage:image options:0];
}
- (void)copyFromTexture:(MTLTextureInternal *)source toTexture:(MTLTextureInternal *)destination {encoder->copy([source texture],[destination texture]);}
- (void)copyFromTexture:(MTLTextureInternal *)source sourceSlice:(unsigned long)slice sourceLevel:(unsigned long)level toTexture:(MTLTextureInternal *)destination destinationSlice:(unsigned long)destSlice destinationLevel:(unsigned long)destLevel sliceCount:(unsigned long)slices levelCount:(unsigned long)levels {
    encoder->copy([source texture],slice,level,[destination texture],destSlice,destLevel,slices,levels);
}
- (void)copyFromTexture:(MTLTextureInternal *)source sourceSlice:(unsigned long)slice sourceLevel:(unsigned long)level sourceOrigin:(Indium::Origin)origin sourceSize:(Indium::Size)size toTexture:(MTLTextureInternal *)destination destinationSlice:(unsigned long)destSlice destinationLevel:(unsigned long)destLevel destinationOrigin:(Indium::Origin)destOrigin {
    encoder->copy([source texture],slice,level,origin,size,[destination texture],destSlice,destLevel,destOrigin);
}
- (void)generateMipmapsForTexture:(MTLTextureInternal *)texture {encoder->generateMipmapsForTexture([texture texture]);}
@end

@interface MTLComputeCommandEncoderInternal : NSObject @end
static std::shared_ptr<Indium::ComputeCommandEncoder>& computeEncoder(id object) {
    static Ivar slot=class_getInstanceVariable(objc_getClass("MTLComputeCommandEncoderInternal"),"_encoder");
    if(!slot)throw std::runtime_error("Darling compute encoder ABI is unavailable");
    return *reinterpret_cast<std::shared_ptr<Indium::ComputeCommandEncoder> *>((char *)object+ivar_getOffset(slot));
}
@implementation MTLComputeCommandEncoderInternal (LinuxResources)
- (void)endEncoding {auto& encoder=computeEncoder(self);encoder->endEncoding();encoder.reset();}
- (void)setTexture:(MTLTextureInternal *)texture atIndex:(unsigned long)index {computeEncoder(self)->setTexture(texture ? [texture texture] : nullptr,index);}
- (void)setTextures:(MTLTextureInternal *const *)textures withRange:(Indium::Range<size_t>)range {
    std::vector<std::shared_ptr<Indium::Texture>> values;values.reserve(range.length);
    for(size_t i=0;i<range.length;i++)values.push_back(textures[i] ? [textures[i] texture] : nullptr);
    computeEncoder(self)->setTextures(values,range);
}
- (void)setSamplerState:(LinuxSamplerState *)sampler atIndex:(unsigned long)index {computeEncoder(self)->setSamplerState(sampler ? [sampler state] : nullptr,index);}
- (void)setSamplerState:(LinuxSamplerState *)sampler lodMinClamp:(float)minimum lodMaxClamp:(float)maximum atIndex:(unsigned long)index {computeEncoder(self)->setSamplerState(sampler ? [sampler state] : nullptr,minimum,maximum,index);}
- (void)setThreadgroupMemoryLength:(unsigned long)length atIndex:(unsigned long)index {computeEncoder(self)->setThreadgroupMemoryLength(length,index);}
@end
