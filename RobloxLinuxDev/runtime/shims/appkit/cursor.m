// Replace Cocotron's overlapping, out-of-bounds cursor row copies.
typedef struct { double x,y; } Point;
typedef struct { double width,height; } Size;
typedef struct { Point origin; Size size; } Rect;
typedef struct { unsigned version,size,width,height,xhot,yhot,delay; unsigned *pixels; } CursorImage;
@interface NSObject @end
@interface NSDisplay:NSObject + (id)currentDisplay; - (void *)display; @end
@interface NSImage:NSObject
- (Size)size; - (void)drawInRect:(Rect)rect fromRect:(Rect)src operation:(unsigned long)op fraction:(double)alpha;
@end
@interface NSGraphicsContext:NSObject
+ (id)graphicsContextWithGraphicsPort:(void *)context flipped:(signed char)flipped;
+ (void)saveGraphicsState; + (void)restoreGraphicsState; + (void)setCurrentContext:(id)context;
@end
@interface NSObject (CursorFallback) - (id)initWithName:(const char *)name; @end
extern void *CGColorSpaceCreateDeviceRGB(void),*CGBitmapContextCreate(void *,unsigned long,unsigned long,unsigned long,unsigned long,void *,unsigned);
extern void CGColorSpaceRelease(void *),CGContextRelease(void *);
extern void *CGBitmapContextGetData(void *);
extern unsigned long CGBitmapContextGetBytesPerRow(void *);
extern void *dlopen(const char *,int),*dlsym(void *,const char *),*memcpy(void *,const void *,unsigned long);
extern Class objc_getClass(const char *);
extern void *class_getInstanceMethod(Class,SEL),*method_setImplementation(void *,void *),*class_getInstanceVariable(Class,const char *);
extern id object_getIvar(id,void *);
extern long ivar_getOffset(void *);

@interface NSBitmapImageRep:NSObject
- (long)bitsPerSample; - (long)samplesPerPixel; - (signed char)isPlanar; - (signed char)hasAlpha;
- (long)pixelsWide; - (long)pixelsHigh; - (long)bytesPerRow;
- (unsigned long)bitmapFormat; - (unsigned char *)bitmapData; - (void *)CGColorSpace;
@end
extern void *CFDataCreateMutable(void *,long);
extern void CFDataSetLength(void *,long),CFRelease(const void *);
extern unsigned char *CFDataGetMutableBytePtr(void *);
extern void *CGDataProviderCreateWithCFData(void *);
extern void CGDataProviderRelease(void *);
extern void *CGImageCreate(unsigned long,unsigned long,unsigned long,unsigned long,unsigned long,void *,unsigned,void *,const double *,signed char,int);
static void *(*originalBitmapImage)(id,SEL);
static void *bitmapImage(id self,SEL selector) {
    unsigned long format=[self bitmapFormat];
    // Encoded/CGImage-backed reps already carry their own format. Normalize raw
    // 8-bit RGBA/ARGB reps here, where all their draw/CGImage callers converge.
    if(object_getIvar(self,class_getInstanceVariable(objc_getClass("NSBitmapImageRep"),"_cgImage")) ||
       [self bitsPerSample]!=8 || [self samplesPerPixel]!=4 || ![self hasAlpha] || [self isPlanar] || (format&~3UL))
        return originalBitmapImage(self,selector);
    long width=[self pixelsWide],height=[self pixelsHigh],stride=[self bytesPerRow];
    if(width<1 || height<1 || width>0x7fffffffffffffffL/4/height || stride<width*4 || stride>0x7fffffffffffffffL/height)return 0;
    const unsigned char *source=[self bitmapData];
    if(!source)return 0;
    void *data=CFDataCreateMutable(0,width*height*4);
    if(!data)return 0;
    CFDataSetLength(data,width*height*4);
    unsigned char *pixels=CFDataGetMutableBytePtr(data);
    for(long y=0;y<height;y++)for(long x=0;x<width;x++) {
        const unsigned char *p=source+y*stride+x*4;
        unsigned a=p[(format&1)?0:3],r=p[(format&1)?1:0],g=p[(format&1)?2:1],b=p[(format&1)?3:2];
        if(format&2){r=(r*a+127)/255;g=(g*a+127)/255;b=(b*a+127)/255;}
        unsigned char *out=pixels+(y*width+x)*4;
        out[0]=b;out[1]=g;out[2]=r;out[3]=a;
    }
    void *provider=CGDataProviderCreateWithCFData(data);
    void *image=provider ? CGImageCreate(width,height,8,32,width*4,[self CGColorSpace],0x2002,provider,0,0,0) : 0;
    if(provider)CGDataProviderRelease(provider);
    CFRelease(data);
    return image; // createCGImageIfNeeded returns a retained image
}
@implementation NSBitmapImageRep (RbxBitmapChannels)
+ (void)load {
    originalBitmapImage=method_setImplementation(class_getInstanceMethod(objc_getClass("NSBitmapImageRep"),@selector(createCGImageIfNeeded)),(void *)bitmapImage);
}
@end

void rbxCopyCursorRows(unsigned *pixels,const void *data,unsigned long width,unsigned long height,unsigned long stride) {
    for(unsigned long row=0;row<height;row++)
        memcpy(pixels+row*width,(const char *)data+row*stride,width*sizeof(*pixels));
}
static id imageCursor(id self,SEL selector,NSImage *image,Point hotspot) {
    Size size=[image size];
    if(!(size.width>=1 && size.width<=1024 && size.height>=1 && size.height<=1024))return [self initWithName:"left_ptr"];
    static void *library;
    if(!library)library=dlopen("/usr/lib/native/libXcursor.dylib",1);
    CursorImage *(*create)(int,int)=dlsym(library,"XcursorImageCreate");
    void (*destroy)(CursorImage *)=dlsym(library,"XcursorImageDestroy");
    unsigned long (*load)(void *,const CursorImage *)=dlsym(library,"XcursorImageLoadCursor");
    if(!create || !destroy || !load)return [self initWithName:"left_ptr"];
    unsigned long width=size.width,height=size.height;
    CursorImage *cursor=create(width,height);
    if(!cursor)return [self initWithName:"left_ptr"];
    cursor->xhot=hotspot.x>=0 && hotspot.x<width ? hotspot.x : 0;
    cursor->yhot=hotspot.y>=0 && hotspot.y<height ? hotspot.y : 0;
    void *color=CGColorSpaceCreateDeviceRGB();
    void *context=CGBitmapContextCreate(0,width,height,8,0,color,0x2002); // premultiplied BGRA, x86_64
    CGColorSpaceRelease(color);
    if(!context){destroy(cursor);return [self initWithName:"left_ptr"];}
    [NSGraphicsContext saveGraphicsState];
    @try {
        [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithGraphicsPort:context flipped:0]];
        [image drawInRect:(Rect){{0,0},{width,height}} fromRect:(Rect){{0,0},{0,0}} operation:1 fraction:1];
        rbxCopyCursorRows(cursor->pixels,CGBitmapContextGetData(context),width,height,CGBitmapContextGetBytesPerRow(context));
        unsigned long handle=load([[NSDisplay currentDisplay] display],cursor);
        *(unsigned long *)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("X11Cursor"),"_cursor")))=handle;
        if(!handle)return [self initWithName:"left_ptr"];
    } @finally {
        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(context);
        destroy(cursor);
    }
    return self;
}
void rbxInstallCursor(void) {
    void *method=class_getInstanceMethod(objc_getClass("X11Cursor"),@selector(initWithImage:hotPoint:));
    if(method)method_setImplementation(method,(void *)imageCursor);
}
