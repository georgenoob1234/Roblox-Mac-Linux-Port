// CoreMedia/CoreVideo/VideoToolbox's playback subset, backed by the host FFmpeg decoder.
// Darling's stock functions return success without objects; every success here owns a real result.
#import <Foundation/NSObject.h>
#import <Foundation/NSString.h>
#import <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import <CoreFoundation/CFRuntime.h>
// SDK Metal headers are incomplete; declare the existing renderer's public ABI.
typedef NSUInteger MTLPixelFormat;
enum { MTLPixelFormatR8Unorm=10, MTLPixelFormatRG8Unorm=30, MTLPixelFormatBGRA8Unorm=80,
       MTLTextureUsageShaderRead=1, MTLStorageModeManaged=1 };
typedef struct { NSUInteger x,y,z; } VideoOrigin;
typedef struct { NSUInteger width,height,depth; } VideoSize;
typedef struct { VideoOrigin origin; VideoSize size; } MTLRegion;
static MTLRegion MTLRegionMake2D(NSUInteger x,NSUInteger y,NSUInteger w,NSUInteger h){return (MTLRegion){{x,y,0},{w,h,1}};}
@interface MTLTextureDescriptor:NSObject
+ (id)texture2DDescriptorWithPixelFormat:(NSUInteger)format width:(NSUInteger)width height:(NSUInteger)height mipmapped:(BOOL)mip;
@property NSUInteger usage,storageMode;
@end
@interface NSObject (TrackAVideoMetal)
- (id)newTextureWithDescriptor:(id)descriptor;
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)stride;
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)stride fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level;
@end
extern id MTLCreateSystemDefaultDevice(void);
#include <CoreMedia/CMTime.h>
#include <CoreGraphics/CGGeometry.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <math.h>
#include <limits.h>
#include "video-host.h"

typedef struct { int32_t width,height; } Dimensions;
typedef struct { CMTime duration, presentationTimeStamp, decodeTimeStamp; } Timing;
typedef struct { uint32_t version; void *(*allocate)(void *,size_t); void (*free)(void *,void *,size_t); void *refcon; } BlockSource;
typedef void (*Output)(void *,void *,int32_t,uint32_t,void *,CMTime,CMTime);
typedef struct { Output function; void *refcon; } Callback;
enum { Block=1,Format,Sample,Pixel,Session,Cache,Texture };
typedef struct Video {
    CFRuntimeBase base;
    int kind,width,height,nalLength; uint32_t codec,pixelFormat;
    uint8_t *bytes; size_t size,stride[3],offset[3];
    CFTypeRef first,second; CFMutableArrayRef attachments; CFArrayRef parameters;
    void *original; size_t originalSize; CFAllocatorRef owner; BlockSource source;
    CMTime pts,duration; long samples; Boolean ready;
    void *decoder; Callback callback; pthread_mutex_t lock; BOOL invalid;
    id device,texture;
} Video;
static void *(*hostOpen)(uint32_t,const uint8_t *,size_t,uint32_t);
static void (*hostClose)(void *);
static int (*hostSend)(void *,const uint8_t *,size_t,const VideoStamp *);
static int (*hostReceive)(void *,VideoFrame *);
static int (*hostDimensions)(void *,int *,int *);
struct ElfCalls {void *(*dlopen)(const char *);int (*dlclose)(void *);void *(*dlsym)(void *,const char *);};
extern struct ElfCalls *_elfcalls;
static void loadHost(void) {
    const char *path=getenv("ROBLOX_MAC_VIDEO_HELPER");
    void *lib=_elfcalls&&path?_elfcalls->dlopen(path):NULL;
    if(!lib){fprintf(stderr,"tracka-video: unable to load host decoder (%s)\n",path?:"no helper path");return;}
    hostOpen=_elfcalls->dlsym(lib,"tracka_video_open");hostClose=_elfcalls->dlsym(lib,"tracka_video_close");
    hostSend=_elfcalls->dlsym(lib,"tracka_video_send");hostReceive=_elfcalls->dlsym(lib,"tracka_video_receive");
    hostDimensions=_elfcalls->dlsym(lib,"tracka_video_dimensions");
}
static BOOL backend(void) {
    static dispatch_once_t once;dispatch_once(&once,^{loadHost();});
    return hostOpen&&hostClose&&hostSend&&hostReceive&&hostDimensions;
}
static void finalize(CFTypeRef ref) {
    Video *v=(Video *)ref;
    if(v->decoder)hostClose(v->decoder);
    if(v->kind==Session)pthread_mutex_destroy(&v->lock);
    if(v->first)CFRelease(v->first);if(v->second)CFRelease(v->second);
    if(v->attachments)CFRelease(v->attachments);if(v->parameters)CFRelease(v->parameters);
    if(v->original) {
        if(v->source.free)v->source.free(v->source.refcon,v->original,v->originalSize);
        else CFAllocatorDeallocate(v->owner,v->original);
    }
    if(v->owner)CFRelease(v->owner);
    [v->texture release];[v->device release];free(v->bytes);
}
static CFTypeID videoType(void) {
    static CFTypeID type;static dispatch_once_t once;
    dispatch_once(&once,^{static const CFRuntimeClass cls={.className="TrackAVideo",.finalize=finalize};type=_CFRuntimeRegisterClass(&cls);});
    return type;
}
static Video *make(int kind) {
    Video *v=(Video *)_CFRuntimeCreateInstance(NULL,videoType(),sizeof(Video)-sizeof(CFRuntimeBase),NULL);
    if(v){v->kind=kind;if(kind==Session)pthread_mutex_init(&v->lock,NULL);}return v;
}
static BOOL valid(const Video *v,int kind){return v&&CFGetTypeID(v)==videoType()&&v->kind==kind;}
static CFTypeRef attribute(CFDictionaryRef dict,CFStringRef key,CFStringRef legacy) {
    if(!dict||CFGetTypeID(dict)!=CFDictionaryGetTypeID())return NULL;
    return CFDictionaryGetValue(dict,key)?:CFDictionaryGetValue(dict,legacy);
}
static uint32_t pixelFormat(CFDictionaryRef attrs) {
    CFTypeRef value=attribute(attrs,CFSTR("PixelFormatType"),CFSTR("kCVPixelBufferPixelFormatTypeKey"));
    if(value&&CFGetTypeID(value)==CFArrayGetTypeID())value=CFArrayGetCount(value)?CFArrayGetValueAtIndex(value,0):NULL;
    int32_t format=0;if(value&&CFGetTypeID(value)==CFNumberGetTypeID())CFNumberGetValue(value,kCFNumberSInt32Type,&format);
    return format?:'420v';
}
static BOOL keyIs(CFTypeRef key,CFStringRef name) {
    return key&&CFGetTypeID(key)==CFStringGetTypeID()&&CFStringHasSuffix(key,name);
}
static void trace(const char *event,int width,int height) {
    if(getenv("ROBLOX_MAC_VIDEO_TRACE"))fprintf(stderr,"tracka-video: %s %dx%d\n",event,width,height);
}

CMTime CMTimeMake(int64_t value,int32_t scale){return (CMTime){value,scale,scale>0?1:0,0};}
double CMTimeGetSeconds(CMTime t) {
    if(!(t.flags&1))return NAN;if(t.flags&4)return INFINITY;if(t.flags&8)return -INFINITY;
    return (t.flags&16)||t.timescale<=0?NAN:(double)t.value/t.timescale;
}

int32_t CMBlockBufferCreateWithMemoryBlock(CFAllocatorRef alloc,void *memory,size_t length,CFAllocatorRef owner,
    const BlockSource *source,size_t offset,size_t size,uint32_t flags,Video **out) {
    trace("block",length,size);
    if(!out)return -12706;*out=NULL;
    if(offset>length||size>length-offset||size>INT_MAX||(!memory&&!length)||(source&&source->version))return -12706;
    Video *v=make(Block);if(!v)return -12700;
    v->bytes=malloc(size?:1);if(!v->bytes){CFRelease(v);return -12700;}
    if(memory)memcpy(v->bytes,(uint8_t *)memory+offset,size);else memset(v->bytes,0,size);
    v->size=size;
    if(memory){v->original=memory;v->originalSize=length;v->owner=owner?CFRetain(owner):NULL;if(source)v->source=*source;}
    *out=v;return 0;
}
int32_t CMBlockBufferCreateContiguous(CFAllocatorRef alloc,Video *input,CFAllocatorRef owner,const BlockSource *source,
    size_t offset,size_t size,uint32_t flags,Video **out) {
    if(!out)return -12706;*out=NULL;
    if(!valid(input,Block)||offset>input->size)return -12706;
    if(!size)size=input->size-offset;
    return CMBlockBufferCreateWithMemoryBlock(alloc,input->bytes,input->size,kCFAllocatorNull,NULL,offset,size,flags,out);
}
int32_t CMBlockBufferGetDataPointer(Video *v,size_t offset,size_t *atOffset,size_t *total,char **pointer) {
    if(pointer)*pointer=NULL;if(atOffset)*atOffset=0;if(total)*total=0;
    if(!valid(v,Block)||offset>v->size)return -12706;
    if(pointer)*pointer=(char *)v->bytes+offset;if(atOffset)*atOffset=v->size-offset;if(total)*total=v->size;return 0;
}
static int32_t formatFromSets(uint32_t codec,size_t count,const uint8_t *const *sets,const size_t *sizes,int nalLength,Video **out) {
    if(!out)return -12710;*out=NULL;
    if(!sets||!sizes||count<2||count>64||(nalLength!=1&&nalLength!=2&&nalLength!=4))return -12710;
    size_t size=0;for(size_t i=0;i<count;i++){if(!sets[i]||!sizes[i]||sizes[i]>1024*1024||size>INT_MAX-4-sizes[i])return -12710;size+=4+sizes[i];}
    Video *v=make(Format);if(!v)return -12711;
    v->codec=codec;v->nalLength=nalLength;v->size=size;v->bytes=malloc(size);
    CFMutableArrayRef params=CFArrayCreateMutable(NULL,count,&kCFTypeArrayCallBacks);v->parameters=params;
    if(!v->bytes||!params){CFRelease(v);return -12711;}
    size_t offset=0;for(size_t i=0;i<count;i++) {
        memcpy(v->bytes+offset,"\0\0\0\1",4);memcpy(v->bytes+offset+4,sets[i],sizes[i]);offset+=4+sizes[i];
        CFDataRef data=CFDataCreate(NULL,sets[i],sizes[i]);if(!data){CFRelease(v);return -12711;}CFArrayAppendValue(params,data);CFRelease(data);
    }
    if(backend()) {
        void *d=hostOpen(codec,v->bytes,v->size,'420v');
        int status=d?hostDimensions(d,&v->width,&v->height):-1;if(d)hostClose(d);
        if(status){CFRelease(v);return -12710;}
    }
    *out=v;trace("format",v->width,v->height);return 0;
}
int32_t CMVideoFormatDescriptionCreateFromH264ParameterSets(CFAllocatorRef alloc,size_t count,const uint8_t *const *sets,const size_t *sizes,int nalLength,Video **out) {
    return formatFromSets('avc1',count,sets,sizes,nalLength,out);
}
int32_t CMVideoFormatDescriptionCreateFromHEVCParameterSets(CFAllocatorRef alloc,size_t count,const uint8_t *const *sets,const size_t *sizes,int nalLength,CFDictionaryRef extensions,Video **out) {
    return formatFromSets('hvc1',count,sets,sizes,nalLength,out);
}
static int32_t getParameter(Video *v,size_t index,const uint8_t **data,size_t *size,size_t *count,int *nalLength) {
    if(data)*data=NULL;if(size)*size=0;if(count)*count=0;if(nalLength)*nalLength=0;
    if(!valid(v,Format)||!v->parameters)return -12710;
    size_t n=CFArrayGetCount(v->parameters);if(count)*count=n;if(nalLength)*nalLength=v->nalLength;
    if(index>=n)return -12710;
    CFDataRef param=CFArrayGetValueAtIndex(v->parameters,index);if(data)*data=CFDataGetBytePtr(param);if(size)*size=CFDataGetLength(param);return 0;
}
int32_t CMVideoFormatDescriptionGetH264ParameterSetAtIndex(Video *v,size_t index,const uint8_t **data,size_t *size,size_t *count,int *nalLength){return getParameter(v,index,data,size,count,nalLength);}
int32_t CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(Video *v,size_t index,const uint8_t **data,size_t *size,size_t *count,int *nalLength){return getParameter(v,index,data,size,count,nalLength);}
int32_t CMVideoFormatDescriptionCreate(CFAllocatorRef allocator,uint32_t codec,int32_t width,int32_t height,CFDictionaryRef extensions,Video **out) {
    if(!out)return -12710;*out=NULL;if(width<=0||height<=0||width>16384||height>16384)return -12710;
    CFTypeRef atoms=attribute(extensions,CFSTR("SampleDescriptionExtensionAtoms"),CFSTR("kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms"));
    CFDataRef config=attribute(atoms,codec=='avc1'?CFSTR("avcC"):CFSTR("hvcC"),CFSTR(""));
    if(config&&CFGetTypeID(config)==CFDataGetTypeID()) {
        const uint8_t *p=CFDataGetBytePtr(config),*end=p+CFDataGetLength(config),*sets[64];size_t sizes[64],count=0;int nalLength=0;
        if(codec=='avc1'&&end-p>=7&&p[0]==1) {
            nalLength=(p[4]&3)+1;size_t n=p[5]&31;p+=6;
            for(int group=0;group<2;group++) {
                for(size_t i=0;i<n;i++){if(end-p<2||count==64)return -12710;size_t s=p[0]*256+p[1];p+=2;if((size_t)(end-p)<s)return -12710;sets[count]=p;sizes[count++]=s;p+=s;}
                if(group==0){if(p==end)return -12710;n=*p++;}
            }
        } else if((codec=='hvc1'||codec=='hev1')&&end-p>=23&&p[0]==1) {
            nalLength=(p[21]&3)+1;unsigned arrays=p[22];p+=23;
            for(unsigned a=0;a<arrays;a++){if(end-p<3)return -12710;unsigned n=p[1]*256+p[2];p+=3;
                for(unsigned i=0;i<n;i++){if(end-p<2||count==64)return -12710;size_t s=p[0]*256+p[1];p+=2;if((size_t)(end-p)<s)return -12710;sets[count]=p;sizes[count++]=s;p+=s;}}
        }
        int32_t status=formatFromSets(codec,count,sets,sizes,nalLength,out);if(status)return status;
    } else {*out=make(Format);if(!*out)return -12711;(*out)->codec=codec;}
    (*out)->width=width;(*out)->height=height;return 0;
}
uint32_t CMFormatDescriptionGetMediaSubType(Video *v){return valid(v,Format)?v->codec:0;}
Dimensions CMVideoFormatDescriptionGetDimensions(Video *v){return valid(v,Format)?(Dimensions){v->width,v->height}:(Dimensions){0,0};}
int32_t CMSampleBufferCreate(CFAllocatorRef alloc,Video *block,Boolean ready,void *makeReady,void *refcon,Video *format,
    long samples,long timingCount,const Timing *timing,long sizeCount,const size_t *sizes,Video **out) {
    trace("sample",samples,ready);
    if(!out)return -12731;*out=NULL;
    if(!valid(block,Block)||!valid(format,Format)||samples<0||samples>65536||timingCount<0||sizeCount<0||(timingCount&&!timing)||(sizeCount&&!sizes))return -12731;
    if(!ready)return -12733; // No lazy producer is needed by the compressed-video path.
    Video *v=make(Sample);if(!v)return -12730;
    v->first=CFRetain(block);v->second=CFRetain(format);v->samples=samples;v->ready=ready;
    if(timingCount){v->pts=timing[0].presentationTimeStamp;v->duration=timing[0].duration;}*out=v;return 0;
}
Boolean CMSampleBufferDataIsReady(Video *v){return valid(v,Sample)&&v->ready;}
Boolean CMSampleBufferIsValid(Video *v){return valid(v,Sample)&&!v->invalid;}
void *CMSampleBufferGetDataBuffer(Video *v){return valid(v,Sample)?(void *)v->first:NULL;}
void *CMSampleBufferGetFormatDescription(Video *v){return valid(v,Sample)?(void *)v->second:NULL;}
void *CMSampleBufferGetImageBuffer(Video *v){return NULL;}
long CMSampleBufferGetNumSamples(Video *v){return valid(v,Sample)?v->samples:0;}
CMTime CMSampleBufferGetPresentationTimeStamp(Video *v){return valid(v,Sample)?v->pts:(CMTime){0};}
CFArrayRef CMSampleBufferGetSampleAttachmentsArray(Video *v,Boolean create) {
    if(!valid(v,Sample))return NULL;
    if(!v->attachments&&create) {
        v->attachments=CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
        for(long i=0;i<v->samples;i++){CFMutableDictionaryRef d=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);CFArrayAppendValue(v->attachments,d);CFRelease(d);}
    }return v->attachments;
}
CFDictionaryRef CMCopyDictionaryOfAttachments(CFAllocatorRef alloc,CFTypeRef object,uint32_t mode){return NULL;}

int32_t CVPixelBufferCreate(CFAllocatorRef alloc,size_t width,size_t height,uint32_t format,CFDictionaryRef attributes,Video **out) {
    if(!out)return -6661;*out=NULL;
    if(!width||!height||width>16384||height>16384||(format!='420v'&&format!='420f'&&format!='BGRA'&&format!='y420'&&format!='f420'))return -6680;
    Video *v=make(Pixel);if(!v)return -6662;
    v->width=width;v->height=height;v->pixelFormat=format;v->stride[0]=(width*(format=='BGRA'?4:1)+63)&~63;
    BOOL planar=format=='y420'||format=='f420';
    v->stride[1]=v->stride[2]=((width+1)/2*(planar?1:2)+63)&~63;v->offset[1]=v->stride[0]*height;
    v->offset[2]=v->offset[1]+v->stride[1]*((height+1)/2);
    v->size=format=='BGRA'?v->offset[1]:planar?v->offset[2]+v->stride[2]*((height+1)/2):v->offset[2];v->bytes=calloc(1,v->size);
    if(!v->bytes){CFRelease(v);return -6662;}*out=v;return 0;
}
void *CVBufferRetain(Video *v){return v?(void *)CFRetain(v):NULL;}
void CVBufferRelease(Video *v){if(v)CFRelease(v);}
void CVPixelBufferRelease(Video *v){CVBufferRelease(v);}
void *CVPixelBufferRetain(Video *v){return CVBufferRetain(v);}
size_t CVPixelBufferGetWidth(Video *v){return valid(v,Pixel)?v->width:0;}
size_t CVPixelBufferGetHeight(Video *v){return valid(v,Pixel)?v->height:0;}
uint32_t CVPixelBufferGetPixelFormatType(Video *v){return valid(v,Pixel)?v->pixelFormat:0;}
size_t CVPixelBufferGetPlaneCount(Video *v){return valid(v,Pixel)&&v->pixelFormat!='BGRA'?(v->pixelFormat=='y420'||v->pixelFormat=='f420'?3:2):0;}
size_t CVPixelBufferGetWidthOfPlane(Video *v,size_t plane){return plane<CVPixelBufferGetPlaneCount(v)?(plane?(v->width+1)/2:v->width):0;}
size_t CVPixelBufferGetHeightOfPlane(Video *v,size_t plane){return plane<CVPixelBufferGetPlaneCount(v)?(plane?(v->height+1)/2:v->height):0;}
void *CVPixelBufferGetBaseAddress(Video *v){return valid(v,Pixel)?v->bytes:NULL;}
void *CVPixelBufferGetBaseAddressOfPlane(Video *v,size_t plane){return plane<CVPixelBufferGetPlaneCount(v)?v->bytes+v->offset[plane]:NULL;}
size_t CVPixelBufferGetBytesPerRow(Video *v){return valid(v,Pixel)?v->stride[0]:0;}
size_t CVPixelBufferGetBytesPerRowOfPlane(Video *v,size_t plane){return plane<CVPixelBufferGetPlaneCount(v)?v->stride[plane]:0;}
int32_t CVPixelBufferLockBaseAddress(Video *v,uint64_t flags){return valid(v,Pixel)?0:-6661;}
int32_t CVPixelBufferUnlockBaseAddress(Video *v,uint64_t flags){return valid(v,Pixel)?0:-6661;}
CGSize CVImageBufferGetEncodedSize(Video *v){return CGSizeMake(CVPixelBufferGetWidth(v),CVPixelBufferGetHeight(v));}
int32_t CVMetalTextureCacheCreate(CFAllocatorRef alloc,CFDictionaryRef attrs,id device,CFDictionaryRef texAttrs,Video **out) {
    if(!out)return -6661;*out=NULL;if(!device)return -6661;
    Video *v=make(Cache);if(!v)return -6662;v->device=[device retain];*out=v;return 0;
}
int32_t CVMetalTextureCacheCreateTextureFromImage(CFAllocatorRef alloc,Video *cache,Video *pixel,CFDictionaryRef attrs,
    MTLPixelFormat format,size_t width,size_t height,size_t plane,Video **out) {
    if(!out)return -6661;*out=NULL;if(!valid(cache,Cache)||!valid(pixel,Pixel))return -6661;
    BOOL bgra=pixel->pixelFormat=='BGRA',planar=pixel->pixelFormat=='y420'||pixel->pixelFormat=='f420';
    if((bgra&&(plane||format!=MTLPixelFormatBGRA8Unorm))||(!bgra&&(plane>=CVPixelBufferGetPlaneCount(pixel)||format!=(!planar&&plane?MTLPixelFormatRG8Unorm:MTLPixelFormatR8Unorm))))return -6680;
    if(width!=(bgra?(size_t)pixel->width:CVPixelBufferGetWidthOfPlane(pixel,plane))||height!=(bgra?(size_t)pixel->height:CVPixelBufferGetHeightOfPlane(pixel,plane)))return -6661;
    @autoreleasepool {
        MTLTextureDescriptor *desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        desc.usage=MTLTextureUsageShaderRead;desc.storageMode=MTLStorageModeManaged;
        id texture=[cache->device newTextureWithDescriptor:desc];if(!texture)return -6662;
        [texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:pixel->bytes+pixel->offset[plane] bytesPerRow:pixel->stride[plane]];
        Video *v=make(Texture);if(!v){[texture release];return -6662;}v->texture=texture;v->first=CFRetain(pixel);*out=v;
    }
    trace("texture",width,height);return 0;
}
id CVMetalTextureGetTexture(Video *v){return valid(v,Texture)?v->texture:nil;}
void CVMetalTextureCacheFlush(Video *cache,uint64_t options){}

Boolean VTIsHardwareDecodeSupported(uint32_t codec){return false;}
void VTRegisterSupplementalVideoDecoderIfAvailable(uint32_t codec){}
int32_t VTDecompressionSessionCreate(CFAllocatorRef alloc,Video *format,CFDictionaryRef specification,CFDictionaryRef attrs,const Callback *callback,Video **out) {
    if(!out)return -12902;*out=NULL;if(!valid(format,Format))return -12902;
    CFTypeRef required=attribute(specification,CFSTR("RequireHardwareAcceleratedVideoDecoder"),CFSTR("kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder"));
    if(getenv("ROBLOX_MAC_VIDEO_TRACE")){fprintf(stderr,"tracka-video: create codec=%08x extra=%zu hardware-required=%d backend=%d\n",format->codec,format->size,required==kCFBooleanTrue,backend());}
    if(required==kCFBooleanTrue||!backend())return -12906;
    uint32_t pf=pixelFormat(attrs);if(pf!='420v'&&pf!='420f'&&pf!='BGRA'&&pf!='y420'&&pf!='f420')return -12910;
    void *decoder=hostOpen(format->codec,format->bytes,format->size,pf);if(!decoder)return -12906;
    Video *v=make(Session);if(!v){hostClose(decoder);return -12904;}
    v->decoder=decoder;v->first=CFRetain(format);v->pixelFormat=pf;if(callback)v->callback=*callback;
    v->attachments=CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);
    if(!v->attachments){CFRelease(v);return -12904;}
    *out=v;trace("decoder created",format->width,format->height);return 0;
}
int32_t VTSessionCopyProperty(Video *v,CFStringRef key,CFAllocatorRef alloc,CFTypeRef *out) {
    if(!out)return -12902;*out=NULL;if(!valid(v,Session)||v->invalid)return -12903;if(!key)return -12902;
    if(keyIs(key,CFSTR("UsingHardwareAcceleratedVideoDecoder"))){*out=CFRetain(kCFBooleanFalse);return 0;}return -12900;
}
int32_t VTSessionSetProperty(Video *v,CFStringRef key,CFTypeRef value) {
    if(!valid(v,Session)||v->invalid)return -12903;if(!key)return -12902;
    if(keyIs(key,CFSTR("UsingHardwareAcceleratedVideoDecoder")))return -12901;
    return keyIs(key,CFSTR("RealTime"))||keyIs(key,CFSTR("ThreadCount"))?0:-12900;
}
Boolean VTDecompressionSessionCanAcceptFormatDescription(Video *v,Video *f) {
    if(!valid(v,Session)||v->invalid||!valid(f,Format))return false;Video *old=(Video *)v->first;
    return old->codec==f->codec&&old->nalLength==f->nalLength&&old->size==f->size&&(!f->size||!memcmp(old->bytes,f->bytes,f->size));
}
static int32_t receiveFrames(Video *v) {
    VideoFrame frame;int status;
    while((status=hostReceive(v->decoder,&frame))>0) {
        Video *pixel=NULL;int32_t error=CVPixelBufferCreate(NULL,frame.width,frame.height,v->pixelFormat,NULL,&pixel);
        if(error)return -12904;
        BOOL bgra=v->pixelFormat=='BGRA',planar=v->pixelFormat=='y420'||v->pixelFormat=='f420';
        for(int p=0;p<(bgra?1:planar?3:2);p++) {
            size_t rows=p?(frame.height+1)/2:frame.height,bytes=p?(frame.width+1)/2*(planar?1:2):frame.width*(bgra?4:1);
            for(size_t row=0;row<rows;row++)memcpy(pixel->bytes+pixel->offset[p]+row*pixel->stride[p],frame.data[p]+row*frame.stride[p],bytes);
        }
        CMTime pts,duration;memcpy(&pts,&frame.stamp.pts,sizeof(pts));memcpy(&duration,&frame.stamp.duration,sizeof(duration));
        // Keep the submitted sample alive until its callback, including B-frame delay.
        CFIndex pending=CFArrayGetFirstIndexOfValue(v->attachments,CFRangeMake(0,CFArrayGetCount(v->attachments)),frame.stamp.sample);
        if(v->callback.function)v->callback.function(v->callback.refcon,frame.stamp.refcon,0,frame.stamp.flags&2?2:0,frame.stamp.flags&2?NULL:pixel,pts,duration);
        if(pending!=kCFNotFound)CFArrayRemoveValueAtIndex(v->attachments,pending);
        trace("decoded frame",frame.width,frame.height);CFRelease(pixel);
    }return status<0?-12909:0;
}
int32_t VTDecompressionSessionDecodeFrame(Video *v,Video *sample,uint32_t flags,void *refcon,uint32_t *info) {
    trace("decode call",flags,0);
    if(info)*info=0;if(!valid(v,Session)||v->invalid)return -12903;
    if(!valid(sample,Sample)||!sample->ready||sample->samples!=1)return -12902;
    Video *block=(Video *)sample->first,*format=(Video *)sample->second;
    if(!VTDecompressionSessionCanAcceptFormatDescription(v,format))return -12910;
    // Convert length-prefixed AVC/HEVC packets into Annex B. Validate before allocating/copying.
    size_t offset=0,total=0;unsigned n=format->nalLength;if(!n)return -12909;
    while(offset<block->size) {
        if(block->size-offset<n)return -12909;uint32_t size=0;for(unsigned i=0;i<n;i++)size=size*256+block->bytes[offset++];
        if(!size||size>block->size-offset||total>INT_MAX-4-size)return -12909;total+=4+size;offset+=size;
    }
    if(!total)return -12909;uint8_t *data=malloc(total);if(!data)return -12904;
    offset=0;size_t dest=0;while(offset<block->size){uint32_t size=0;for(unsigned i=0;i<n;i++)size=size*256+block->bytes[offset++];memcpy(data+dest,"\0\0\0\1",4);memcpy(data+dest+4,block->bytes+offset,size);dest+=4+size;offset+=size;}
    VideoStamp stamp={.refcon=refcon,.sample=sample,.flags=flags};memcpy(&stamp.pts,&sample->pts,sizeof(stamp.pts));memcpy(&stamp.duration,&sample->duration,sizeof(stamp.duration));
    pthread_mutex_lock(&v->lock);
    // Bound retained samples even when corrupt input never produces frames.
    if(v->invalid||CFArrayGetCount(v->attachments)>=64){free(data);pthread_mutex_unlock(&v->lock);return v->invalid?-12903:-12909;}
    CFArrayAppendValue(v->attachments,sample);
    int sent=hostSend(v->decoder,data,total,&stamp);
    if(sent==1){receiveFrames(v);sent=hostSend(v->decoder,data,total,&stamp);}free(data);
    int32_t result=sent?-12909:receiveFrames(v);
    if(sent)CFArrayRemoveValueAtIndex(v->attachments,CFArrayGetCount(v->attachments)-1);
    else if(info&&CFArrayContainsValue(v->attachments,CFRangeMake(0,CFArrayGetCount(v->attachments)),sample))*info|=1;
    pthread_mutex_unlock(&v->lock);return result;
}
int32_t VTDecompressionSessionFinishDelayedFrames(Video *v) {
    if(!valid(v,Session)||v->invalid)return -12903;
    pthread_mutex_lock(&v->lock);
    // A flush emits B frames; reopen to permit subsequent decoding after a seek.
    hostSend(v->decoder,NULL,0,NULL);int32_t result=receiveFrames(v);hostClose(v->decoder);CFArrayRemoveAllValues(v->attachments);
    Video *f=(Video *)v->first;v->decoder=hostOpen(f->codec,f->bytes,f->size,v->pixelFormat);if(!v->decoder){v->invalid=YES;result=-12906;}
    pthread_mutex_unlock(&v->lock);return result;
}
int32_t VTDecompressionSessionWaitForAsynchronousFrames(Video *v) {
    if(!valid(v,Session))return -12903;pthread_mutex_lock(&v->lock);int32_t result=v->invalid?-12903:0;pthread_mutex_unlock(&v->lock);return result;
}
void VTDecompressionSessionInvalidate(Video *v) {
    if(!valid(v,Session))return;pthread_mutex_lock(&v->lock);v->invalid=YES;
    if(v->decoder){hostClose(v->decoder);v->decoder=NULL;}CFArrayRemoveAllValues(v->attachments);pthread_mutex_unlock(&v->lock);
}
