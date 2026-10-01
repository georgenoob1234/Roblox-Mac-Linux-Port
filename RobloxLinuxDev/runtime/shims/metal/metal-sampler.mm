#include <indium/indium.hpp>
@interface NSObject {Class isa;}
+ (id)alloc; + (id)allocWithZone:(void *)zone; + (Class)class;
- (id)init; - (id)copy; - (id)retain; - (void)release; - (void)dealloc;
@end
@interface MTLSamplerDescriptor : NSObject @end
@interface LinuxSamplerDescriptor : MTLSamplerDescriptor {
@public Indium::SamplerDescriptor value;
    id _label;
}
@end
@implementation MTLSamplerDescriptor (LinuxSampler)
+ (id)allocWithZone:(void *)zone {
    return self==[MTLSamplerDescriptor class] ? [LinuxSamplerDescriptor allocWithZone:zone] : [super allocWithZone:zone];
}
@end
@implementation LinuxSamplerDescriptor
#define PROPERTY(type,name,Name) \
- (type)name {return static_cast<type>(value.name);} \
- (void)set##Name:(type)n {value.name=static_cast<decltype(value.name)>(n);}
PROPERTY(unsigned long,minFilter,MinFilter)
PROPERTY(unsigned long,magFilter,MagFilter)
PROPERTY(unsigned long,mipFilter,MipFilter)
PROPERTY(unsigned long,maxAnisotropy,MaxAnisotropy)
PROPERTY(unsigned long,sAddressMode,SAddressMode)
PROPERTY(unsigned long,tAddressMode,TAddressMode)
PROPERTY(unsigned long,rAddressMode,RAddressMode)
PROPERTY(unsigned long,borderColor,BorderColor)
PROPERTY(unsigned long,compareFunction,CompareFunction)
PROPERTY(signed char,normalizedCoordinates,NormalizedCoordinates)
PROPERTY(signed char,supportArgumentBuffers,SupportArgumentBuffers)
PROPERTY(float,lodMinClamp,LodMinClamp)
PROPERTY(float,lodMaxClamp,LodMaxClamp)
#undef PROPERTY
- (id)label {return _label;}
- (void)setLabel:(id)label {id next=[label copy];[_label release];_label=next;}
- (id)copyWithZone:(void *)zone {
    LinuxSamplerDescriptor *d=[[LinuxSamplerDescriptor allocWithZone:zone] init];d->value=value;[d setLabel:_label];return d;
}
- (void)dealloc {[_label release];[super dealloc];}
@end
@interface LinuxSamplerState : NSObject {
@public std::shared_ptr<Indium::SamplerState> value;
    id _device,_label;
}
- (id)initWithState:(std::shared_ptr<Indium::SamplerState>)state device:(id)device label:(id)label;
@end
@implementation LinuxSamplerState
- (id)initWithState:(std::shared_ptr<Indium::SamplerState>)state device:(id)device label:(id)label {
    self=[super init];if(self){value=state;_device=[device retain];_label=[label copy];}return self;
}
- (std::shared_ptr<Indium::SamplerState>)state {return value;}
- (id)device {return _device;}
- (id)label {return _label;}
- (void)dealloc {[_device release];[_label release];[super dealloc];}
@end
@interface MTLDeviceInternal : NSObject - (std::shared_ptr<Indium::Device>)device; @end
@implementation MTLDeviceInternal (LinuxSamplers)
- (id)newSamplerStateWithDescriptor:(LinuxSamplerDescriptor *)descriptor {
    if(!descriptor)return nullptr;
    auto sampler=[self device]->newSamplerState(descriptor->value);
    return sampler ? [[LinuxSamplerState alloc] initWithState:sampler device:self label:[descriptor label]] : nullptr;
}
@end
