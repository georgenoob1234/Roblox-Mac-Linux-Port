#include <indium/render-pipeline.hpp>
#include <objc/runtime.h>
#include <array>
#include <stdexcept>
@interface NSObject {Class isa;}
+ (Class)class; + (id)alloc; + (id)allocWithZone:(void *)zone;
- (id)init; - (id)autorelease; - (void)release; - (void)dealloc;
@end
@interface MTLVertexDescriptor : NSObject @end
@interface LinuxVertexAttribute : NSObject {@public Indium::VertexAttributeDescriptor value;} @end
@interface LinuxVertexLayout : NSObject {@public Indium::VertexBufferLayoutDescriptor value;} @end
#define PROPERTY(name,Name) \
- (unsigned long)name {return static_cast<unsigned long>(value.name);} \
- (void)set##Name:(unsigned long)n {value.name=static_cast<decltype(value.name)>(n);}
@implementation LinuxVertexAttribute
PROPERTY(format,Format)
PROPERTY(offset,Offset)
PROPERTY(bufferIndex,BufferIndex)
@end
@implementation LinuxVertexLayout
PROPERTY(stride,Stride)
PROPERTY(stepRate,StepRate)
PROPERTY(stepFunction,StepFunction)
@end
#undef PROPERTY
@interface LinuxVertexArray : NSObject {
@public std::array<id,31> slots;
    bool attributes;
}
- (id)objectAtIndexedSubscript:(unsigned long)index;
@end
@implementation LinuxVertexArray
- (id)init {self=[super init];if(self)slots.fill(nullptr);return self;}
- (void)dealloc {for(id item:slots)[item release];[super dealloc];}
- (id)objectAtIndexedSubscript:(unsigned long)index {
    if(index>=slots.size())throw std::out_of_range("Metal vertex descriptor index");
    if(!slots[index])slots[index]=attributes ? [[LinuxVertexAttribute alloc] init] : [[LinuxVertexLayout alloc] init];
    return slots[index];
}
@end
@interface LinuxVertexDescriptor : MTLVertexDescriptor {
    LinuxVertexArray *_attributes,*_layouts;
}
- (Indium::VertexDescriptor)asIndiumDescriptor;
@end
@implementation MTLVertexDescriptor (LinuxVertexAllocation)
+ (id)allocWithZone:(void *)zone {return self==[MTLVertexDescriptor class] ? [LinuxVertexDescriptor allocWithZone:zone] : [super allocWithZone:zone];}
+ (id)vertexDescriptor {return [[[self alloc] init] autorelease];}
@end
@implementation LinuxVertexDescriptor
- (id)init {
    self=[super init];if(self){_attributes=[[LinuxVertexArray alloc] init];_attributes->attributes=true;_layouts=[[LinuxVertexArray alloc] init];}return self;
}
- (void)dealloc {[_attributes release];[_layouts release];[super dealloc];}
- (id)attributes {return _attributes;}
- (id)layouts {return _layouts;}
- (void)reset {
    for(LinuxVertexAttribute *item:_attributes->slots)if(item)item->value={};
    for(LinuxVertexLayout *item:_layouts->slots)if(item)item->value={};
}
- (id)copyWithZone:(void *)zone {
    LinuxVertexDescriptor *copy=[[LinuxVertexDescriptor allocWithZone:zone] init];
    for(unsigned i=0;i<31;i++) {
        if(_attributes->slots[i])((LinuxVertexAttribute *)[copy->_attributes objectAtIndexedSubscript:i])->value=((LinuxVertexAttribute *)_attributes->slots[i])->value;
        if(_layouts->slots[i])((LinuxVertexLayout *)[copy->_layouts objectAtIndexedSubscript:i])->value=((LinuxVertexLayout *)_layouts->slots[i])->value;
    }
    return copy;
}
- (Indium::VertexDescriptor)asIndiumDescriptor {
    Indium::VertexDescriptor result;
    for(unsigned i=0;i<31;i++) {
        LinuxVertexAttribute *attr=_attributes->slots[i];
        if(!attr || attr->value.format==Indium::VertexFormat::Invalid)continue;
        result.attributes.emplace(i,attr->value);
        LinuxVertexLayout *layout=[_layouts objectAtIndexedSubscript:attr->value.bufferIndex];
        result.layouts.emplace(attr->value.bufferIndex,layout->value);
    }
    return result;
}
@end
@interface MTLRenderPipelineDescriptor : NSObject
- (LinuxVertexDescriptor *)vertexDescriptor;
- (Indium::RenderPipelineDescriptor)asIndiumDescriptor;
@end
@interface MTLRenderPipelineDescriptor (LinuxVertexPipeline)
- (Indium::RenderPipelineDescriptor)rbxVertexDescriptor;
@end
@implementation MTLRenderPipelineDescriptor (LinuxVertexPipeline)
+ (void)load {
    Method original=class_getInstanceMethod(self,@selector(asIndiumDescriptor));
    Method replacement=class_getInstanceMethod(self,@selector(rbxVertexDescriptor));
    if(!original || !replacement)throw std::runtime_error("Darling render descriptor ABI unavailable");
    method_exchangeImplementations(original,replacement);
}
- (Indium::RenderPipelineDescriptor)rbxVertexDescriptor {
    auto result=[self rbxVertexDescriptor]; // original conversion after exchange
    LinuxVertexDescriptor *vertex=[self vertexDescriptor];
    if(vertex)result.vertexDescriptor=[vertex asIndiumDescriptor];
    return result;
}
@end
