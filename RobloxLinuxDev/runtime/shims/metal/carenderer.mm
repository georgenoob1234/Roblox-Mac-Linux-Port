// Adapted from Darling/Cocotron QuartzCore/CARenderer.m (bundled source).
// Empty container layers must traverse their children without uploading a nonexistent image.
#define GL_GLEXT_PROTOTYPES 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <objc/runtime.h>
#include <cstdlib>
#include <indium/dynamic-vk.hpp>
#include <mutex>
#include <chrono>
#include <cstdio>
extern "C" void *dlopen(const char *,int);
extern "C" void *dlsym(void *,const char *);
// Darling forces linear tiling for an old AMD workaround. NVIDIA exposes only
// optimal RGBA8 tiling. Both Vulkan allocation and GL import must agree.
static bool needsOptimalSharing() {
    GLint tilings[8]={},count=0;
    glGetInternalformativ(GL_TEXTURE_2D,GL_RGBA8,GL_NUM_TILING_TYPES_EXT,1,&count);
    if(count<=0 || count>8)return false;
    glGetInternalformativ(GL_TEXTURE_2D,GL_RGBA8,GL_TILING_TYPES_EXT,count,tilings);
    bool optimal=false;
    for(int i=0;i<count;i++) {
        if(tilings[i]==GL_LINEAR_TILING_EXT)return false;
        if(tilings[i]==GL_OPTIMAL_TILING_EXT)optimal=true;
    }
    return optimal;
}
static PFN_vkCreateImage createImage;
static VkResult createSharedImage(VkDevice device,const VkImageCreateInfo *info,const VkAllocationCallbacks *callbacks,VkImage *image) {
    auto selected=*info;
    if(info->format==VK_FORMAT_R8G8B8A8_UNORM && info->tiling==VK_IMAGE_TILING_LINEAR) {
        for(auto p=(const VkBaseInStructure *)info->pNext;p;p=p->pNext) {
            if(p->sType==VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO &&
               ((const VkExternalMemoryImageCreateInfo *)p)->handleTypes==VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT && needsOptimalSharing())
                selected.tiling=VK_IMAGE_TILING_OPTIMAL;
        }
    }
    return createImage(device,&selected,callbacks,image);
}
extern "C" void glTextureParameteri(GLuint texture,GLenum pname,GLint param) {
    using Set=void (*)(GLuint,GLenum,GLint);
    static auto original=(Set)dlsym(dlopen("/System/Library/Frameworks/OpenGL.framework/Libraries/libGL.dylib",1),"glTextureParameteri");
    if(!original)std::abort();
    if(pname==GL_TEXTURE_TILING_EXT && param==GL_LINEAR_TILING_EXT && needsOptimalSharing())param=GL_OPTIMAL_TILING_EXT;
    original(texture,pname,param);
}

// CAMetalDrawableTexture is this runtime's only GL memory importer; every image
// it exports uses VkMemoryDedicatedAllocateInfo. EXT_external_objects requires
// the matching GL flag BEFORE import, while memory parameters remain mutable.
extern "C" void glImportMemoryFdEXT(GLuint memory,GLuint64 size,GLenum type,GLint fd) {
    using Import=void (*)(GLuint,GLuint64,GLenum,GLint);
    static auto original=(Import)dlsym(dlopen("/System/Library/Frameworks/OpenGL.framework/Libraries/libGL.dylib",1),"glImportMemoryFdEXT");
    if(!original)std::abort();
    GLint dedicated=GL_TRUE;
    glMemoryObjectParameterivEXT(memory,GL_DEDICATED_MEMORY_OBJECT_EXT,&dedicated);
    original(memory,size,type,fd);
}
typedef double CGFloat, CFTimeInterval;
struct CGPoint {double x,y;};
struct CGSize {double width,height;};
struct CGRect {CGPoint origin;CGSize size;};
static CGPoint CGPointMake(double x,double y){return {x,y};}
static CGRect CGRectMake(double x,double y,double w,double h){return {{x,y},{w,h}};}
typedef void *CGImageRef;
@interface NSObject
+ (Class)class; - (id)valueForKey:(id)key; - (signed char)isKindOfClass:(Class)cls;
@end
@interface NSString : NSObject - (signed char)isEqualToString:(id)string; @end
@interface NSNumber : NSObject - (float)floatValue; - (unsigned)unsignedIntValue; @end
@interface NSValue : NSObject - (CGPoint)pointValue; - (CGRect)rectValue; @end
struct EnumState {unsigned long state;id *itemsPtr;unsigned long *mutationsPtr;unsigned long extra[5];};
@interface NSArray : NSObject
- (unsigned long)countByEnumeratingWithState:(EnumState *)state objects:(id *)items count:(unsigned long)count;
@end
@interface CAMediaTimingFunction : NSObject
+ (id)functionWithName:(id)name; - (void)getControlPointAtIndex:(unsigned long)index values:(CGFloat *)values;
@end
@interface CAAnimation : NSObject
- (CFTimeInterval)beginTime; - (CFTimeInterval)duration; - (CAMediaTimingFunction *)timingFunction;
@end
@interface CABasicAnimation : CAAnimation - (id)fromValue; - (id)toValue; @end
@interface CALayer : NSObject
- (NSNumber *)_textureId; - (CAAnimation *)animationForKey:(id)key;
@property(readonly) id contents;
@property(readonly) NSString *minificationFilter,*magnificationFilter;
@property(readonly) NSArray *sublayers;
@end
@interface CARenderer : NSObject - (CALayer *)layer; @end
@interface CALayer (MetalPresentation) - (void)prepareRender; @end
extern "C" { extern NSString *const kCAMediaTimingFunctionDefault,*const kCAFilterLinear,*const kCAFilterNearest;
void CATexImage2DCGImage(CGImageRef image); }
static inline CGFloat cubed(CGFloat value) {
    return value * value * value;
}

static inline CGFloat squared(CGFloat value) {
    return value * value;
}

static CGFloat applyMediaTimingFunction(CAMediaTimingFunction *function,
                                        CGFloat t)
{
    CGFloat result;
    CGFloat cp1[2];
    CGFloat cp2[2];

    [function getControlPointAtIndex: 1 values: cp1];
    [function getControlPointAtIndex: 2 values: cp2];

    double x = cubed(1.0 - t) * 0.0 + 3 * squared(1 - t) * t * cp1[0] +
               3 * (1 - t) * squared(t) * cp2[0] + cubed(t) * 1.0;
    double y = cubed(1.0 - t) * 0.0 + 3 * squared(1 - t) * t * cp1[1] +
               3 * (1 - t) * squared(t) * cp2[1] + cubed(t) * 1.0;

    // this is wrong
    return y;
}

static CGFloat mediaTimingScale(CAAnimation *animation,
                                CFTimeInterval currentTime)
{
    CFTimeInterval begin = [animation beginTime];
    CFTimeInterval duration = [animation duration];
    CFTimeInterval delta = currentTime - begin;
    double zeroToOne = delta / duration;
    CAMediaTimingFunction *function = [animation timingFunction];

    if (function == nil)
        function = [CAMediaTimingFunction
                functionWithName: kCAMediaTimingFunctionDefault];

    return applyMediaTimingFunction(function, zeroToOne);
}

static CGFloat interpolateFloatInLayerKey(CALayer *layer, NSString *key,
                                          CFTimeInterval currentTime)
{
    CAAnimation *animation = [layer animationForKey: key];

    if (animation == nil)
        return [[layer valueForKey: key] floatValue];

    if ([animation isKindOfClass: [CABasicAnimation class]]) {
        CABasicAnimation *basic = (CABasicAnimation *) animation;

        id fromValue = [basic fromValue];
        id toValue = [basic toValue];

        if (toValue == nil)
            toValue = [layer valueForKey: key];

        CGFloat fromFloat = [fromValue floatValue];
        CGFloat toFloat = [toValue floatValue];

        CGFloat resultFloat;
        double timingScale = mediaTimingScale(animation, currentTime);

        resultFloat = fromFloat + (toFloat - fromFloat) * timingScale;

        return resultFloat;
    }

    return 0;
}

static CGPoint interpolatePointInLayerKey(CALayer *layer, NSString *key,
                                          CFTimeInterval currentTime)
{
    CAAnimation *animation = [layer animationForKey: key];

    if (animation == nil)
        return [[layer valueForKey: key] pointValue];

    if ([animation isKindOfClass: [CABasicAnimation class]]) {
        CABasicAnimation *basic = (CABasicAnimation *) animation;

        id fromValue = [basic fromValue];
        id toValue = [basic toValue];

        if (toValue == nil)
            toValue = [layer valueForKey: key];

        CGPoint fromPoint = [fromValue pointValue];
        CGPoint toPoint = [toValue pointValue];

        CGPoint resultPoint;
        double timingScale = mediaTimingScale(animation, currentTime);

        resultPoint.x = fromPoint.x + (toPoint.x - fromPoint.x) * timingScale;
        resultPoint.y = fromPoint.y + (toPoint.y - fromPoint.y) * timingScale;

        return resultPoint;
    }

    return CGPointMake(0, 0);
}

static CGRect interpolateRectInLayerKey(CALayer *layer, NSString *key,
                                        CFTimeInterval currentTime)
{
    CAAnimation *animation = [layer animationForKey: key];

    if (animation == nil) {
        return [[layer valueForKey: key] rectValue];
    }

    if ([animation isKindOfClass: [CABasicAnimation class]]) {
        CABasicAnimation *basic = (CABasicAnimation *) animation;

        id fromValue = [basic fromValue];
        id toValue = [basic toValue];

        if (toValue == nil)
            toValue = [layer valueForKey: key];

        CGRect fromRect = [fromValue rectValue];
        CGRect toRect = [toValue rectValue];

        double timingScale = mediaTimingScale(animation, currentTime);

        CGRect resultRect;

        resultRect.origin.x =
                fromRect.origin.x +
                (toRect.origin.x - fromRect.origin.x) * timingScale;
        resultRect.origin.y =
                fromRect.origin.y +
                (toRect.origin.y - fromRect.origin.y) * timingScale;
        resultRect.size.width =
                fromRect.size.width +
                (toRect.size.width - fromRect.size.width) * timingScale;
        resultRect.size.height =
                fromRect.size.height +
                (toRect.size.height - fromRect.size.height) * timingScale;

        return resultRect;
    }

    return CGRectMake(0, 0, 0, 0);
}

static GLint interpolationFromName(NSString *name) {
    if (name == kCAFilterLinear)
        return GL_LINEAR;
    else if (name == kCAFilterNearest)
        return GL_NEAREST;
    else if ([name isEqualToString: kCAFilterLinear])
        return GL_LINEAR;
    else if ([name isEqualToString: kCAFilterNearest])
        return GL_NEAREST;
    else
        return GL_LINEAR;
}


@implementation CARenderer (LinuxEmptyLayers)
- (void) _renderLayer: (CALayer *) layer
                    z: (CGFloat) z
          currentTime: (CFTimeInterval) currentTime
{
    // CALayerContext prepares only a Metal root; nested Metal layers need the same step.
    if (layer != [self layer] && [layer isKindOfClass:objc_getClass("CAMetalLayerInternal")])
        [layer prepareRender];
    NSNumber *textureId = [layer _textureId];
    GLuint texture = [textureId unsignedIntValue];
    GLboolean loadPixelData = GL_FALSE;

    if (texture == 0)
        loadPixelData = GL_TRUE;
    else {

        if (glIsTexture(texture) == GL_FALSE) {
            loadPixelData = GL_TRUE;
        }
        glBindTexture(GL_TEXTURE_2D, texture);
    }

    bool drawContents = layer.contents != nil;
    if (!drawContents && texture) {
        GLint width = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        drawContents = width > 0;
    }
    if (drawContents && loadPixelData) {
        CGImageRef image = (CGImageRef)layer.contents;

        CATexImage2DCGImage(image);

        GLint minFilter = interpolationFromName(layer.minificationFilter);
        GLint magFilter = interpolationFromName(layer.magnificationFilter);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    CGPoint anchorPoint =
            interpolatePointInLayerKey(layer, @"anchorPoint", currentTime);
    CGPoint position =
            interpolatePointInLayerKey(layer, @"position", currentTime);
    CGRect bounds = interpolateRectInLayerKey(layer, @"bounds", currentTime);
    CGFloat opacity =
            interpolateFloatInLayerKey(layer, @"opacity", currentTime);

    GLfloat textureVertices[4 * 2];
    GLfloat vertices[4 * 3];

    textureVertices[0] = 0;
    textureVertices[1] = 1;
    textureVertices[2] = 1;
    textureVertices[3] = 1;
    textureVertices[4] = 0;
    textureVertices[5] = 0;
    textureVertices[6] = 1;
    textureVertices[7] = 0;

    vertices[0] = 0;
    vertices[1] = 0;
    vertices[2] = z;

    vertices[3] = bounds.size.width;
    vertices[4] = 0;
    vertices[5] = z;

    vertices[6] = 0;
    vertices[7] = bounds.size.height;
    vertices[8] = z;

    vertices[9] = bounds.size.width;
    vertices[10] = bounds.size.height;
    vertices[11] = z;

    glPushMatrix();
    //  glTranslatef(width/2,height/2,0);
    glTexCoordPointer(2, GL_FLOAT, 0, textureVertices);
    glVertexPointer(3, GL_FLOAT, 0, vertices);

    glTranslatef(position.x - (bounds.size.width * anchorPoint.x),
                 position.y - (bounds.size.height * anchorPoint.y), 0);
    // glTranslatef(position.x,position.y,0);
    // glScalef(bounds.size.width,bounds.size.height,1);

    //  glRotatef(1,0,0,1);
    glColor4f(opacity, opacity, opacity, opacity);

    if (drawContents) glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    for (CALayer *child in layer.sublayers)
        [self _renderLayer: child z: z + 1 currentTime: currentTime];

    glPopMatrix();
}


@end

@interface NSObject (Lifetime)
- (id)retain; - (void)release;
@end
@interface CALayerContext : NSObject - (void)render; - (void)flush; @end
@interface CALayer (Context)
- (CALayerContext *)_context; - (void)display;
@end
@interface CAMetalLayerInternal : CALayer
- (id)device; - (void)recreateDrawables; - (void)rbxRecreateDrawables;
@end
extern "C" char _dispatch_main_q;
extern "C" void dispatch_async_f(void *queue,void *context,void (*work)(void *));
static void presentMetalLayer(void *object) {
    @autoreleasepool {
    using Clock=std::chrono::steady_clock;
    const bool timing=getenv("RBX_PRESENT_TIMING")!=nullptr;
    const auto start=timing ? Clock::now() : Clock::time_point{};
    CALayerContext *context=(CALayerContext *)object;
    [context render];[context flush];[context release];
    if(timing) {
        static auto epoch=start;static unsigned frames=0;static double total=0,slowest=0;
        auto end=Clock::now();double ms=std::chrono::duration<double,std::milli>(end-start).count();
        total+=ms;if(ms>slowest)slowest=ms;++frames;
        double seconds=std::chrono::duration<double>(end-epoch).count();
        if(seconds>=2){fprintf(stderr,"PRESENT %.1f fps CPU+swap avg=%.2fms max=%.2fms\n",frames/seconds,total/frames,slowest);epoch=end;frames=0;total=slowest=0;}
    }
    }
}
@implementation CAMetalLayerInternal (LinuxFrameUpdates)
+ (void)load {
    Class cls=objc_getClass("CAMetalLayerInternal");
    method_exchangeImplementations(class_getInstanceMethod(cls,@selector(recreateDrawables)),class_getInstanceMethod(cls,@selector(rbxRecreateDrawables)));
}
- (void)rbxRecreateDrawables {
    if(![self device]){[self rbxRecreateDrawables];return;}
    static std::once_flag installed;
    std::call_once(installed,[] {
        auto& entry=Indium::DynamicVK::vkCreateImage;
        if(!entry.resolve())std::abort();
        createImage=(PFN_vkCreateImage)entry.pointer;
        entry.pointer=(void *)createSharedImage;
    });
    [self rbxRecreateDrawables];
}
- (void)display {
    [super display];
    // queuePresent calls display, but Darling's CALayer only calls its delegate.
    // Defer rendering until this commit has finished importing its presentation semaphore.
    CALayerContext *context=[self _context];
    if(context)dispatch_async_f(&_dispatch_main_q,[context retain],presentMetalLayer);
}
@end
