#include <indium/indium.hpp>
@interface NSObject {Class isa;}
+ (id)alloc; + (id)allocWithZone:(void *)zone; + (Class)class;
- (id)init; - (id)copy; - (id)retain; - (void)release; - (void)dealloc;
@end
@interface MTLStencilDescriptor : NSObject @end
@interface LinuxStencilDescriptor : MTLStencilDescriptor {@public Indium::StencilDescriptor value;} @end
@implementation MTLStencilDescriptor (LinuxStencil)
+ (id)allocWithZone:(void *)zone {return self==[MTLStencilDescriptor class] ? [LinuxStencilDescriptor allocWithZone:zone] : [super allocWithZone:zone];}
@end
#define PROPERTY(type,name,Name) \
- (type)name {return static_cast<type>(value.name);} \
- (void)set##Name:(type)n {value.name=static_cast<decltype(value.name)>(n);}
@implementation LinuxStencilDescriptor
PROPERTY(unsigned long,stencilCompareFunction,StencilCompareFunction)
PROPERTY(unsigned long,stencilFailureOperation,StencilFailureOperation)
PROPERTY(unsigned long,depthFailureOperation,DepthFailureOperation)
PROPERTY(unsigned long,depthStencilPassOperation,DepthStencilPassOperation)
PROPERTY(unsigned,readMask,ReadMask)
PROPERTY(unsigned,writeMask,WriteMask)
- (id)copyWithZone:(void *)zone {LinuxStencilDescriptor *d=[[LinuxStencilDescriptor allocWithZone:zone] init];d->value=value;return d;}
@end
@interface MTLDepthStencilDescriptor : NSObject @end
@interface LinuxDepthDescriptor : MTLDepthStencilDescriptor {
    Indium::DepthStencilDescriptor value;
    LinuxStencilDescriptor *_front,*_back;
    id _label;
}
- (Indium::DepthStencilDescriptor)asIndiumDescriptor;
@end
@implementation MTLDepthStencilDescriptor (LinuxDepth)
+ (id)allocWithZone:(void *)zone {return self==[MTLDepthStencilDescriptor class] ? [LinuxDepthDescriptor allocWithZone:zone] : [super allocWithZone:zone];}
@end
@implementation LinuxDepthDescriptor
PROPERTY(unsigned long,depthCompareFunction,DepthCompareFunction)
PROPERTY(signed char,depthWriteEnabled,DepthWriteEnabled)
- (signed char)isDepthWriteEnabled {return value.depthWriteEnabled;}
- (id)frontFaceStencil {return _front;}
- (id)backFaceStencil {return _back;}
- (void)setFrontFaceStencil:(id)stencil {id next=[stencil copy];[_front release];_front=next;}
- (void)setBackFaceStencil:(id)stencil {id next=[stencil copy];[_back release];_back=next;}
- (id)label {return _label;}
- (void)setLabel:(id)label {id next=[label copy];[_label release];_label=next;}
- (Indium::DepthStencilDescriptor)asIndiumDescriptor {
    auto result=value;result.frontFaceStencil=_front ? std::make_optional(_front->value) : std::nullopt;
    result.backFaceStencil=_back ? std::make_optional(_back->value) : std::nullopt;return result;
}
- (id)copyWithZone:(void *)zone {
    LinuxDepthDescriptor *d=[[LinuxDepthDescriptor allocWithZone:zone] init];d->value=value;
    [d setFrontFaceStencil:_front];[d setBackFaceStencil:_back];[d setLabel:_label];return d;
}
- (void)dealloc {[_front release];[_back release];[_label release];[super dealloc];}
@end
#undef PROPERTY
@interface LinuxDepthState : NSObject {
    std::shared_ptr<Indium::DepthStencilState> _state;
    id _device,_label;
}
- (id)initWithState:(std::shared_ptr<Indium::DepthStencilState>)state device:(id)device label:(id)label;
@end
@implementation LinuxDepthState
- (id)initWithState:(std::shared_ptr<Indium::DepthStencilState>)state device:(id)device label:(id)label {
    self=[super init];if(self){_state=state;_device=[device retain];_label=[label copy];}return self;
}
- (std::shared_ptr<Indium::DepthStencilState>)state {return _state;}
- (id)device {return _device;}
- (id)label {return _label;}
- (void)dealloc {[_device release];[_label release];[super dealloc];}
@end
@interface MTLDeviceInternal : NSObject - (std::shared_ptr<Indium::Device>)device; @end
@implementation MTLDeviceInternal (LinuxDepth)
- (id)newDepthStencilStateWithDescriptor:(LinuxDepthDescriptor *)descriptor {
    if(!descriptor)return nullptr;
    auto state=[self device]->newDepthStencilState([descriptor asIndiumDescriptor]);
    return state ? [[LinuxDepthState alloc] initWithState:state device:self label:[descriptor label]] : nullptr;
}
@end
@interface MTLRenderCommandEncoderInternal : NSObject - (std::shared_ptr<Indium::RenderCommandEncoder>)encoder; @end
@implementation MTLRenderCommandEncoderInternal (LinuxDepth)
- (void)setDepthStencilState:(LinuxDepthState *)state {[self encoder]->setDepthStencilState(state ? [state state] : nullptr);}
@end

// Apple ABI is x,y,width,height. Darling's public header and Indium store
// height,width,x,y; keep that internal ABI and translate at the guest boundary.
struct AppleScissorRect {unsigned long x,y,width,height;};
@implementation MTLRenderCommandEncoderInternal (LinuxScissor)
- (void)setScissorRect:(AppleScissorRect)rect {
    [self encoder]->setScissorRect({rect.height,rect.width,rect.x,rect.y});
}
- (void)setScissorRects:(const AppleScissorRect *)rects count:(unsigned long)count {
    std::vector<Indium::ScissorRect> converted;
    for(unsigned long i=0;i<count;i++)converted.push_back({rects[i].height,rects[i].width,rects[i].x,rects[i].y});
    [self encoder]->setScissorRects(converted);
}
@end

// Metal's attachment getters return mutable descriptors even before a target is
// assigned. Darling's synthesized getters return nil, silently losing setters.
#include <objc/runtime.h>
@interface MTLRenderPassDepthAttachmentDescriptor : NSObject @end
@interface MTLRenderPassStencilAttachmentDescriptor : NSObject @end
@interface MTLRenderPassDescriptor : NSObject
- (id)depthAttachment; - (id)stencilAttachment;
- (void)setDepthAttachment:(id)value; - (void)setStencilAttachment:(id)value;
- (Indium::RenderPassDescriptor)asIndiumDescriptor;
@end
static id (*depthAttachmentGetter)(id,SEL),(*stencilAttachmentGetter)(id,SEL);
static Indium::RenderPassDescriptor (*passDescriptor)(id,SEL);
static id depthAttachment(id self,SEL sel) {
    id value=depthAttachmentGetter(self,sel);
    if(!value){value=[[MTLRenderPassDepthAttachmentDescriptor alloc] init];[self setDepthAttachment:value];[value release];value=depthAttachmentGetter(self,sel);}
    return value;
}
static id stencilAttachment(id self,SEL sel) {
    id value=stencilAttachmentGetter(self,sel);
    if(!value){value=[[MTLRenderPassStencilAttachmentDescriptor alloc] init];[self setStencilAttachment:value];[value release];value=stencilAttachmentGetter(self,sel);}
    return value;
}
static Indium::RenderPassDescriptor renderPass(id self,SEL sel) {
    auto result=passDescriptor(self,sel);
    if(result.depthAttachment && !result.depthAttachment->texture)result.depthAttachment.reset();
    if(result.stencilAttachment && !result.stencilAttachment->texture)result.stencilAttachment.reset();
    return result;
}
@implementation MTLRenderPassDescriptor (LinuxAttachments)
+ (void)load {
    depthAttachmentGetter=(decltype(depthAttachmentGetter))method_setImplementation(class_getInstanceMethod(self,@selector(depthAttachment)),(IMP)depthAttachment);
    stencilAttachmentGetter=(decltype(stencilAttachmentGetter))method_setImplementation(class_getInstanceMethod(self,@selector(stencilAttachment)),(IMP)stencilAttachment);
    passDescriptor=(decltype(passDescriptor))method_setImplementation(class_getInstanceMethod(self,@selector(asIndiumDescriptor)),(IMP)renderPass);
}
@end
