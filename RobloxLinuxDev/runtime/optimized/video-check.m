// Run with video-check.py: generated AVC/HEVC clips, reordered timestamps, CF lifetime, GPU readback.
#include "video.m"
#include <assert.h>
#include <objc/runtime.h>
#include <unistd.h>
static id fakeDisplay(id self,SEL selector){return self;}
static CFMutableArrayRef frames;
static int received,scale;static int64_t previous;
static void decoded(void *context,void *source,int32_t status,uint32_t flags,void *image,CMTime pts,CMTime duration) {
    assert(!status&&image&&context==frames);
    assert(pts.flags&1);assert(pts.timescale==scale&&pts.value>previous);
    assert(CMSampleBufferIsValid(source));assert(CMSampleBufferGetPresentationTimeStamp(source).value==pts.value);assert(duration.value>0);
    previous=pts.value;received++;CFArrayAppendValue(frames,image);
}
static void readExact(FILE *f,void *p,size_t n){assert(fread(p,1,n,f)==n);}
static uint32_t word(FILE *f){uint32_t n;readExact(f,&n,4);return n;}
int main(int argc,char **argv) {@autoreleasepool {
    assert(argc==3);FILE *file=fopen(argv[1],"rb");assert(file);
    uint32_t codec=word(file),count=word(file);assert(count>1&&count<64);
    const uint8_t *sets[64];size_t sizes[64];
    for(unsigned i=0;i<count;i++){sizes[i]=word(file);uint8_t *p=malloc(sizes[i]);readExact(file,p,sizes[i]);sets[i]=p;}
    Video *format=NULL;assert(!formatFromSets(codec,count,sets,sizes,4,&format));
    for(unsigned i=0;i<count;i++)free((void *)sets[i]);
    Dimensions dimensions=CMVideoFormatDescriptionGetDimensions(format);assert(dimensions.width==64&&dimensions.height==48);
    const uint8_t *parameter;size_t parameterSize,parameterCount;int length;
    assert(!getParameter(format,0,&parameter,&parameterSize,&parameterCount,&length)&&parameter&&parameterSize&&parameterCount==count&&length==4);
    uint32_t pf=0;for(int i=0;i<4;i++)pf=pf*256+(uint8_t)argv[2][i];
    CFNumberRef pixel=CFNumberCreate(NULL,kCFNumberSInt32Type,&pf);
    const void *key=CFSTR("PixelFormatType"),*value=pixel;
    CFDictionaryRef attrs=CFDictionaryCreate(NULL,&key,&value,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);CFRelease(pixel);
    frames=CFArrayCreateMutable(NULL,0,&kCFTypeArrayCallBacks);Callback callback={decoded,frames};Video *session=(void *)1;
    assert(VTDecompressionSessionCreate(NULL,NULL,NULL,attrs,&callback,&session)==-12902&&!session);
    assert(!VTDecompressionSessionCreate(NULL,format,NULL,attrs,&callback,&session));CFRelease(attrs);
    CFTypeRef hardware=(void *)1;assert(!VTSessionCopyProperty(session,CFSTR("UsingHardwareAcceleratedVideoDecoder"),NULL,&hardware));
    assert(hardware&&!CFBooleanGetValue(hardware));CFRelease(hardware);
    hardware=(void *)1;assert(VTSessionCopyProperty(session,CFSTR("unsupported"),NULL,&hardware)==-12900&&!hardware);
    assert(VTSessionSetProperty(session,CFSTR("UsingHardwareAcceleratedVideoDecoder"),kCFBooleanTrue)==-12901);
    count=word(file);scale=word(file);previous=-1;
    for(unsigned i=0;i<count;i++) {
        int64_t pts,duration;readExact(file,&pts,8);readExact(file,&duration,8);size_t size=word(file);
        void *bytes=malloc(size);readExact(file,bytes,size);Video *block=NULL,*sample=NULL;
        assert(!CMBlockBufferCreateWithMemoryBlock(NULL,bytes,size,kCFAllocatorNull,NULL,0,size,0,&block));
        memset(bytes,0,size);free(bytes); // BlockBuffer owns its copy, not the caller's storage.
        Video *contiguous=NULL;assert(!CMBlockBufferCreateContiguous(NULL,block,NULL,NULL,0,size,2,&contiguous));CFRelease(block);block=contiguous;
        Timing timing={CMTimeMake(duration,scale),CMTimeMake(pts,scale),{0}};
        assert(!CMSampleBufferCreate(NULL,block,true,NULL,NULL,format,1,1,&timing,1,&size,&sample));CFRelease(block);
        assert(CMSampleBufferDataIsReady(sample)&&CMSampleBufferIsValid(sample));
        assert(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))==(double)pts/scale);
        assert(CFArrayGetCount(CMSampleBufferGetSampleAttachmentsArray(sample,true))==1);
        assert(!VTDecompressionSessionDecodeFrame(session,sample,9,sample,NULL));CFRelease(sample);
    }
    fclose(file);
    uint8_t malformed[]={0,0,0,20,1};Video *badBlock=NULL,*badSample=NULL;
    assert(!CMBlockBufferCreateWithMemoryBlock(NULL,malformed,sizeof(malformed),kCFAllocatorNull,NULL,0,sizeof(malformed),0,&badBlock));
    Timing badTiming={CMTimeMake(1,scale),CMTimeMake(999,scale),{0}};
    assert(!CMSampleBufferCreate(NULL,badBlock,true,NULL,NULL,format,1,1,&badTiming,0,NULL,&badSample));CFRelease(badBlock);
    assert(VTDecompressionSessionDecodeFrame(session,badSample,9,badSample,NULL)==-12909);CFRelease(badSample);
    assert(!VTDecompressionSessionFinishDelayedFrames(session));assert(!VTDecompressionSessionWaitForAsynchronousFrames(session));
    assert(received==(int)count);CFRelease(format);VTDecompressionSessionInvalidate(session);CFRelease(session);
    // The callback's retained frames must outlive all compressed data and the decoder.
    Method display=class_getClassMethod(objc_getClass("NSDisplay"),sel_registerName("currentDisplay"));assert(display);
    method_setImplementation(display,(IMP)fakeDisplay);setprogname("tracka-render-check");
    id device=MTLCreateSystemDefaultDevice();assert(device);Video *cache=NULL;assert(!CVMetalTextureCacheCreate(NULL,NULL,device,NULL,&cache));
    uint64_t sums[2]={0};
    for(int index=0;index<2;index++) {
        Video *frame=(Video *)CFArrayGetValueAtIndex(frames,index?count-1:0);assert(CVPixelBufferGetWidth(frame)==64&&CVPixelBufferGetHeight(frame)==48);
        BOOL bgra=pf=='BGRA',planar=pf=='y420'||pf=='f420';
        assert(!CVPixelBufferLockBaseAddress(frame,1));
        for(size_t plane=0;plane<(bgra?1:planar?3:2);plane++) {
            size_t w=bgra?64:CVPixelBufferGetWidthOfPlane(frame,plane),h=bgra?48:CVPixelBufferGetHeightOfPlane(frame,plane);
            size_t stride=bgra?CVPixelBufferGetBytesPerRow(frame):CVPixelBufferGetBytesPerRowOfPlane(frame,plane);
            uint8_t *data=bgra?CVPixelBufferGetBaseAddress(frame):CVPixelBufferGetBaseAddressOfPlane(frame,plane);
            Video *texture=NULL;NSUInteger fmt=bgra?80:!planar&&plane?30:10;
            assert(!CVMetalTextureCacheCreateTextureFromImage(NULL,cache,frame,NULL,fmt,w,h,plane,&texture));
            uint8_t *copy=calloc(h,stride);[CVMetalTextureGetTexture(texture) getBytes:copy bytesPerRow:stride fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
            size_t rowBytes=w*(bgra?4:!planar&&plane?2:1);
            for(size_t y=0;y<h;y++){assert(!memcmp(copy+y*stride,data+y*stride,rowBytes));for(size_t x=0;x<rowBytes;x++)sums[index]+=data[y*stride+x]*(1+x+y*w);}
            free(copy);CFRelease(texture);
        }
        assert(!CVPixelBufferUnlockBaseAddress(frame,1));
    }
    assert(sums[0]&&sums[1]&&sums[0]!=sums[1]);CFRelease(cache);[device release];CFRelease(frames);
    // Truncated length-prefixed packets fail before entering FFmpeg.
    uint8_t bad[]={0,0,0,20,1};Video *block=NULL;
    assert(CMBlockBufferCreateWithMemoryBlock(NULL,bad,sizeof(bad),kCFAllocatorNull,NULL,4,2,0,&block)&&!block);
    printf("PASS %c%c%c%c/%s: %u reordered frames, timestamps, retained pixels and Metal texture readback\n",codec>>24,(codec>>16)&255,(codec>>8)&255,codec&255,argv[2],count);
    return 0;
}}
