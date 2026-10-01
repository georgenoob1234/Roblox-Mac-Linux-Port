// Xwayland needs an invisible system cursor for relative locking. Paint its
// image as a non-interactive sublayer of the existing game surface instead.
#import <Foundation/NSArray.h>
#import <Foundation/NSThread.h>
#import <Foundation/NSGeometry.h>
#import <CoreGraphics/CGBitmapContext.h>
#import <CoreGraphics/CGImage.h>
#import <CoreGraphics/CGColorSpace.h>
#import <QuartzCore/CATransaction.h>
@interface CALayer:NSObject
- (void)removeFromSuperlayer; - (void)setContents:(id)image; - (void)setFrame:(CGRect)frame;
- (void)addSublayer:(id)layer;
@end
@interface NSView:NSObject
- (CALayer *)layer; - (NSArray *)subviews; - (NSPoint)convertPoint:(NSPoint)point fromView:(id)view;
@end
@interface NSImage:NSObject
- (NSSize)size; - (void)drawInRect:(NSRect)rect fromRect:(NSRect)source operation:(NSUInteger)operation fraction:(CGFloat)fraction;
@end
@interface NSCursor:NSObject
+ (id)currentCursor; - (NSImage *)image; - (NSPoint)hotSpot;
@end
@interface NSGraphicsContext:NSObject
+ (void)saveGraphicsState; + (void)restoreGraphicsState; + (void)setCurrentContext:(id)context;
+ (id)graphicsContextWithGraphicsPort:(void *)port flipped:(BOOL)flipped;
@end
@interface NSObject (RbxContentView) - (NSView *)contentView; @end
#import <objc/runtime.h>
#import <dispatch/dispatch.h>
#include <dlfcn.h>

@interface NSObject (RbxNativeCursor)
- (void *)display; - (id)delegate; - (NSPoint)transformPoint:(NSPoint)point;
@end
static CALayer *cursorLayer;
static id lockedWindow;
static NSPoint lockedPoint,hotspot;
static CGImageRef cursorImage;
static NSCursor *lastCursor;
static int cursorOverlayVisible;
int rbxWaylandSoftwareCursorVisible(void) {
    return __atomic_load_n(&cursorOverlayVisible,__ATOMIC_ACQUIRE);
}
extern CGImageRef rbxWaylandCursorImage(NSPoint*);
static void (*originalSetCursor)(id,SEL,id);
struct CursorElfCalls {void *(*open)(const char *);int (*close)(void *);void *(*symbol)(void *,const char *);};
extern struct CursorElfCalls *_elfcalls;

static NSView *layerView(NSView *view) {
    if([view layer])return view;
    for(NSView *child in [view subviews]){NSView *found=layerView(child);if(found)return found;}
    return nil;
}
static void updateOverlay(void) {
    [cursorLayer removeFromSuperlayer]; // also retires its old GL texture
    if(!lockedWindow || !cursorImage) {
        __atomic_store_n(&cursorOverlayVisible,0,__ATOMIC_RELEASE);return;
    }
    NSView *view=layerView([[lockedWindow delegate] contentView]);
    if(!view) {
        __atomic_store_n(&cursorOverlayVisible,0,__ATOMIC_RELEASE);return;
    }
    if(!cursorLayer)cursorLayer=[[CALayer alloc] init];
    NSPoint point=[view convertPoint:[lockedWindow transformPoint:lockedPoint] fromView:nil];
    CGFloat width=CGImageGetWidth(cursorImage),height=CGImageGetHeight(cursorImage);
    [CATransaction begin];[CATransaction setDisableActions:YES];
    [cursorLayer setContents:(id)cursorImage];
    [cursorLayer setFrame:CGRectMake(point.x-hotspot.x,point.y+hotspot.y-height,width,height)];
    [[view layer] addSublayer:cursorLayer];
    [CATransaction commit];
    __atomic_store_n(&cursorOverlayVisible,1,__ATOMIC_RELEASE);
}
static CGImageRef imageFromCursor(NSCursor *cursor,NSPoint *hot) {
    NSImage *image=[cursor image];
    if(!image)return rbxWaylandCursorImage(hot);
    NSSize size=[image size];
    if(size.width<1 || size.height<1 || size.width>1024 || size.height>1024)return NULL;
    CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
    CGContextRef context=CGBitmapContextCreate(NULL,size.width,size.height,8,0,color,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(color);if(!context)return NULL;
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithGraphicsPort:context flipped:NO]];
    [image drawInRect:NSMakeRect(0,0,size.width,size.height) fromRect:NSZeroRect operation:1 fraction:1];
    [NSGraphicsContext restoreGraphicsState];
    CGImageRef result=CGBitmapContextCreateImage(context);CGContextRelease(context);
    *hot=[cursor hotSpot];return result;
}
// Called before the active grab hides the system cursor; covers named cursors
// (whose NSCursor.image is nil) without substituting a different arrow.
static CGImageRef systemCursor(void *display,NSPoint *hot) {
    struct Image {short x,y;unsigned short width,height,xhot,yhot;unsigned long serial,*pixels,atom;const char *name;};
    static struct Image *(*get)(void *);
    if(!get && _elfcalls){void *lib=_elfcalls->open("libXfixes.so.3");if(lib)get=_elfcalls->symbol(lib,"XFixesGetCursorImage");}
    if(!get)return NULL;
    struct Image *image=get(display);if(!image)return NULL;
    CGImageRef result=NULL;
    if(image->width && image->height && image->width<=1024 && image->height<=1024) {
        CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
        CGContextRef context=CGBitmapContextCreate(NULL,image->width,image->height,8,image->width*4,color,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(color);
        if(context){uint32_t *pixels=CGBitmapContextGetData(context);for(unsigned i=0;i<image->width*image->height;i++)pixels[i]=(uint32_t)image->pixels[i];
            result=CGBitmapContextCreateImage(context);CGContextRelease(context);*hot=NSMakePoint(image->xhot,image->yhot);}
    }
    int (*release)(void *)=dlsym(dlopen("/usr/lib/native/libX11.dylib",1),"XFree");release(image);return result;
}
void rbxLockedCursor(id window,void *display,NSPoint point,int visible) {
    // Snapshot native pixels before dispatching to the UI thread: the grab can
    // already be blank by the time the main queue processes this update.
    NSPoint hot=NSZeroPoint;CGImageRef image=visible && display?systemCursor(display,&hot):NULL;
    void (^apply)(void)=^{
        [lockedWindow release];lockedWindow=visible?[window retain]:nil;lockedPoint=point;
        if(image){if(cursorImage)CGImageRelease(cursorImage);cursorImage=CGImageRetain(image);hotspot=hot;}
        if(visible){[lastCursor release];lastCursor=[[NSCursor currentCursor] retain];NSPoint customHot;CGImageRef custom=imageFromCursor(lastCursor,&customHot);
            if(custom){if(cursorImage)CGImageRelease(cursorImage);cursorImage=custom;hotspot=customHot;}}
        updateOverlay();if(image)CGImageRelease(image);
    };
    if([NSThread isMainThread])apply();else dispatch_async(dispatch_get_main_queue(),apply);
}
void rbxMoveLockedCursor(NSPoint point) {
    void (^apply)(void)=^{if(lockedPoint.x==point.x && lockedPoint.y==point.y)return;lockedPoint=point;if(lockedWindow)updateOverlay();};
    if([NSThread isMainThread])apply();else dispatch_async(dispatch_get_main_queue(),apply);
}
void rbxRefreshLockedCursor(void) {
    // Roblox changes the Shift Lock cursor from a worker. Marshal the specific
    // cursor object to the UI thread rather than dropping that change.
    NSCursor *current=[NSCursor currentCursor];
    void (^apply)(void)=^{
        if(!lockedWindow || current==lastCursor)return;
        [lastCursor release];lastCursor=[current retain];
        NSPoint hot;CGImageRef image=imageFromCursor(current,&hot);if(!image)return;
        if(cursorImage)CGImageRelease(cursorImage);cursorImage=image;hotspot=hot;updateOverlay();
    };
    if([NSThread isMainThread])apply();else dispatch_async(dispatch_get_main_queue(),apply);
}
static void setCursor(id self,SEL selector,id cursor) {
    originalSetCursor(self,selector,cursor);rbxRefreshLockedCursor();
}
void rbxInstallLockedCursor(void) {
    Method method=class_getInstanceMethod(objc_getClass("X11Display"),@selector(setCursor:));
    if(method)originalSetCursor=(void *)method_setImplementation(method,(IMP)setCursor);
}
