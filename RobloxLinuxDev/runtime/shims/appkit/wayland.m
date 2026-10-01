// Native Wayland backend for Cocotron. GTK/SDL run in the ELF helper on this
// same UI thread; Metal's GPU-shared GL compositor presents directly to EGL.
#import <Foundation/NSObject.h>
#import <Foundation/NSArray.h>
#import <Foundation/NSDictionary.h>
#import <Foundation/NSData.h>
#import <Foundation/NSString.h>
#import <Foundation/NSValue.h>
#import <Foundation/NSGeometry.h>
#import <Foundation/NSDate.h>
#import <Foundation/NSBundle.h>
#import <Foundation/NSException.h>
#import <Foundation/NSThread.h>
#import <Foundation/NSTimer.h>
#import <CoreGraphics/CGBitmapContext.h>
#import <CoreGraphics/CGColorSpace.h>
#import <OpenGL/CGLTypes.h>
typedef NSUInteger NSEventType,NSEventModifierFlags;
enum {NSLeftMouseDown=1,NSLeftMouseUp=2,NSRightMouseDown=3,NSRightMouseUp=4,NSMouseMoved=5,NSLeftMouseDragged=6,NSRightMouseDragged=7,NSKeyDown=10,NSKeyUp=11,NSFlagsChanged=12,NSScrollWheel=22,NSOtherMouseDown=25,NSOtherMouseUp=26};
enum {NSWindowStyleMaskFullScreen=1<<14,NSCompositeCopy=1};
#define NSWindowServerCommunicationException @"NSWindowServerCommunicationException"
@class NSWindow,NSColor,NSEvent,CGWindow;
@interface NSPasteboard:NSObject @end
@interface NSApplication:NSObject
+ (id)sharedApplication;
- (void)terminate:(id)sender;
- (void)sendEvent:(NSEvent*)event;
@end
@interface NSDisplay:NSObject
+ (id)currentDisplay;
- (void)postEvent:(NSEvent*)event atStart:(BOOL)first;
@end
@interface CGWindow:NSObject @end
@interface CGSubWindow:NSObject @end
@interface NSWindow:NSObject
- (NSRect)frame; - (NSUInteger)styleMask; - (NSInteger)windowNumber;
- (BOOL)platformWindowSetCursorEvent:(CGWindow*)window;
- (void)platformWindowActivated:(id)window displayIfNeeded:(BOOL)display;
- (void)platformWindowDeactivated:(id)window checkForAppDeactivation:(BOOL)check;
- (void)platformWindow:(id)window frameChanged:(NSRect)frame didSize:(BOOL)size;
- (void)performClose:(id)sender;
- (id)firstResponder;
@end
@interface NSObject (RbxTextInput) - (void)insertText:(id)text; @end
@interface NSScreen:NSObject
- (id)initWithFrame:(NSRect)frame visibleFrame:(NSRect)visible;
@end
@interface NSImage:NSObject
- (NSSize)size;
- (void)drawInRect:(NSRect)rect fromRect:(NSRect)src operation:(NSUInteger)op fraction:(CGFloat)fraction;
@end
@interface NSGraphicsContext:NSObject
+ (void)saveGraphicsState; + (void)restoreGraphicsState; + (void)setCurrentContext:(id)context;
+ (id)graphicsContextWithGraphicsPort:(void*)port flipped:(BOOL)flipped;
@end
@interface NSEvent:NSObject
- (NSUInteger)type; - (id)window; - (NSUInteger)modifierFlags; - (double)deltaX; - (double)deltaY;
+ (id)keyEventWithType:(NSUInteger)type location:(NSPoint)p modifierFlags:(NSUInteger)flags timestamp:(double)time windowNumber:(NSInteger)number context:(id)context characters:(NSString*)text charactersIgnoringModifiers:(NSString*)plain isARepeat:(BOOL)repeat keyCode:(unsigned short)code;
@end
typedef void UCKeyboardLayout;
extern int CGLRegisterNativeDisplay(void*);
#import <objc/runtime.h>
#import <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../browser/wayland-api.h"
#include "../../browser/keyboard-map.h"

struct WaylandElfCalls {void *(*open)(const char*);int (*close)(void*);void *(*symbol)(void*,const char*);char *(*error)(void);};
extern struct WaylandElfCalls *_elfcalls;
static const struct RbxWaylandAPI *api;
static void (*hide_vulkan)(void);
static BOOL enabled(void){const char *v=getenv("ROBLOX_MAC_WAYLAND");return v && !strcmp(v,"1");}
CGImageRef rbxWaylandCursorImage(NSPoint *hot) {
    if(!enabled() || !api)return NULL;
    int w,h,pitch,x,y;const void *pixels=api->cursor_image(&w,&h,&pitch,&x,&y);
    if(!pixels || w<1 || h<1)return NULL;
    CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
    CGContextRef context=CGBitmapContextCreate((void*)pixels,w,h,8,pitch,color,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);CGColorSpaceRelease(color);
    if(!context)return NULL;
    CGImageRef image=CGBitmapContextCreateImage(context);CGContextRelease(context);*hot=NSMakePoint(x,y);return image;
}
int rbxWaylandCanPresent(void){return !enabled() || !api || api->visible();}
static void initializeNative(void) {
    if(api)return;
    const char *path=getenv("ROBLOX_MAC_WAYLAND_HELPER");
    void *library=path?_elfcalls->open(path):NULL;
    if(!library) {
        const char *reason=path?_elfcalls->error():"ROBLOX_MAC_WAYLAND_HELPER is not set";
        [NSException raise:NSWindowServerCommunicationException format:@"Native UI helper load failed: %s",reason?:"unknown loader error"];
    }
    if(library)hide_vulkan=_elfcalls->symbol(library,"rbx_wayland_vulkan_hide");
    const struct RbxWaylandAPI *(*get)(void)=library?_elfcalls->symbol(library,"rbx_wayland_api"):NULL;
    if(!get)[NSException raise:NSWindowServerCommunicationException format:@"Native UI helper entry point missing: %s",_elfcalls->error()?:"unknown symbol error"];
    if(!get || !(api=get()))[NSException raise:NSWindowServerCommunicationException format:@"Native GTK/Wayland initialization failed"];
    if(api->user_agent())setenv("ROBLOX_MAC_WEBKIT_USER_AGENT",api->user_agent(),1);
    fprintf(stderr,"WAYLAND registering EGL display\n");
    if(CGLRegisterNativeDisplay(api->display()))[NSException raise:NSWindowServerCommunicationException format:@"Wayland EGL display initialization failed"];
    fprintf(stderr,"WAYLAND EGL display registered\n");
}
void rbxWaylandHideVulkan(void) { if(hide_vulkan)hide_vulkan(); }
@interface RbxWaylandWindow:CGWindow {
@public NSWindow *owner;NSRect frame;void *native;id bitmap;NSUInteger style;BOOL mapped;
}
- (id)initWithDelegate:(NSWindow*)delegate;
- (NSPoint)transformPoint:(NSPoint)point;
@end
static RbxWaylandWindow *gameWindow;
static void windowAction(RbxWaylandWindow *window,int op,double x,double y,const char *text) {
    if(window==gameWindow)api->action(window->native,op,x,y,text);
}
static void (*originalThemeDraw)(id,SEL,NSRect);
static void drawThemeFrame(id view,SEL selector,NSRect rect) {
    // GTK owns the window decoration; Metal/WebKit cover its content. Painting
    // Cocotron's background twice at 4K only fills an unpresented CPU bitmap.
    if(gameWindow && [view window]==gameWindow->owner)return;
    originalThemeDraw(view,selector,rect);
}
static NSPoint pointer,anchor,rightPressPoint;
static unsigned modifiers,buttons;
static BOOL locked,focused,captureWanted,captureSuspended;
static BOOL cursorVisible=YES;
static unsigned cursorTrackingDirty,cursorVisibilityRequest;
static unsigned captureRequest;
static NSPoint pendingWarp;
static BOOL warpQueued;
static BOOL pendingCapture,captureQueued;
static unsigned pendingCaptureRequest;
static BOOL pointerTrace(void){static int value=-1;if(value<0)value=getenv("ROBLOX_MAC_POINTER_TRACE") && !strcmp(getenv("ROBLOX_MAC_POINTER_TRACE"),"1");return value;}
extern void rbxLockedCursor(id,void*,NSPoint,int);
extern void rbxMoveLockedCursor(NSPoint);
extern void rbxRefreshLockedCursor(void);
static void markCursorTrackingDirty(void) {
    __atomic_store_n(&cursorTrackingDirty,1,__ATOMIC_RELEASE);
}
static BOOL takeCursorTrackingRefresh(void) {
    return gameWindow && focused && __atomic_exchange_n(&cursorTrackingDirty,0,__ATOMIC_ACQ_REL);
}
static void setCursorVisibility(BOOL value) {
    cursorVisible=value;
    if(api)api->action(NULL,RBX_WL_CURSOR_VISIBLE,value,0,NULL);
    // Pointer capture and cursor visibility are independent. A game may keep
    // raw camera input while hiding the OS cursor and drawing its own reticle.
    if(gameWindow)rbxLockedCursor(gameWindow,NULL,anchor,locked && value);
    if(pointerTrace())fprintf(stderr,"CURSOR visible=%d locked=%d\n",value,locked);
}
static void applyCursorVisibility(unsigned request,BOOL value) {
    if(request==__atomic_load_n(&cursorVisibilityRequest,__ATOMIC_SEQ_CST))setCursorVisibility(value);
}
static NSPoint warpScreenPoint(NSPoint point,double screenHeight,double windowHeight) {
    // CGWarp uses top-left display coordinates; the guest Cocoa window has
    // bottom-left frame origin zero. Convert origins, do not flip the local y.
    point.y-=screenHeight-windowHeight;
    return point;
}
static void warpPointer(NSPoint next) {
    // Engine warps change the lock anchor even while RMB remains held (for
    // example, enabling Shift Lock). Physical motion never changes this anchor.
    if(locked || captureSuspended){
        if(anchor.x!=next.x || anchor.y!=next.y)markCursorTrackingDirty();
        anchor=next;rbxMoveLockedCursor(anchor);
    }
    else {pointer=next;if(focused)api->action(gameWindow->native,RBX_WL_WARP,next.x,next.y,NULL);}
}
static void applyCapture(BOOL value) {
    if(!captureWanted)captureSuspended=NO;
    value=value && focused && gameWindow;
    if(pointerTrace())fprintf(stderr,"POINTER appkit apply=%d focused=%d locked=%d buttons=%u\n",value,focused,locked,buttons);
    if(gameWindow && api) {
        // Commit the engine's anchor before unlocking: Wayland otherwise
        // restores the compositor's old position (not the Shift Lock center).
        // Never relocate the desktop pointer when merely leaving a workspace.
        if(locked && !value && focused && !captureWanted) {
            pointer=anchor;
            api->action(gameWindow->native,RBX_WL_WARP,anchor.x,anchor.y,NULL);
        }
        // Always forward release: SDL can already differ after host focus/browser
        // handling. Never let a cached Cocoa flag suppress an engine unlock.
        api->action(gameWindow->native,RBX_WL_LOCK,value,0,NULL);
        if(locked!=value) {
            markCursorTrackingDirty();
            if(value && !captureSuspended)anchor=(buttons & (1u<<3))?rightPressPoint:pointer;
            rbxLockedCursor(gameWindow,NULL,anchor,value && cursorVisible);
        }
    }
    locked=value;
    if(pointerTrace())fprintf(stderr,"POINTER anchor x=%.1f y=%.1f locked=%d suspended=%d\n",anchor.x,anchor.y,locked,captureSuspended);
    if(value)captureSuspended=NO;
}
static void captureFocus(BOOL value) {
    // Keep the engine warp anchor across suspension, not the host cursor
    // position which can change freely on another workspace.
    if(!value && locked)captureSuspended=YES;
    focused=value;applyCapture(value && captureWanted);
    if(value)markCursorTrackingDirty();
    if(!value){buttons=0;modifiers=0;}
}
static void cancelCapture(void) {
    __atomic_add_fetch(&captureRequest,1,__ATOMIC_SEQ_CST);
    captureWanted=NO;captureFocus(NO);
}
@interface RbxWaylandSubwindow:CGSubWindow @end
@implementation RbxWaylandSubwindow
- (void*)nativeWindow{return api->surface(gameWindow->native);}
- (void)show{}
- (void)hide{}
- (void)setFrame:(CGRect)rect{}
@end
@implementation RbxWaylandWindow
- (id)initWithDelegate:(NSWindow*)delegate {
    if(!(self=[super init]))return nil;
    owner=delegate;frame=[owner frame];frame.origin=NSZeroPoint;style=[owner styleMask];
    // The backend has one game surface; Cocoa helper windows must not claim it.
    if(gameWindow)return self;
    initializeNative();native=api->create(frame.size.width,frame.size.height);
    if(!native)[NSException raise:NSWindowServerCommunicationException format:@"Native Wayland surface creation failed"];
    fprintf(stderr,"WAYLAND window create self=%p owner=%p style=%lu size=%.0fx%.0f\n",self,owner,(unsigned long)style,frame.size.width,frame.size.height);gameWindow=self;return self;
}
- (void)setDelegate:(id)delegate{owner=delegate;}
- (id)delegate{return owner;}
- (NSUInteger)styleMask{return style;}
- (void)setStyleMask:(NSUInteger)mask{style=mask;windowAction(self,RBX_WL_FULLSCREEN,(mask&NSWindowStyleMaskFullScreen)!=0,0,NULL);}
- (void)setFrame:(CGRect)value{frame=value;frame.origin=NSZeroPoint;windowAction(self,RBX_WL_RESIZE_WINDOW,value.size.width,value.size.height,NULL);}
- (void)setTitle:(NSString*)title{windowAction(self,RBX_WL_TITLE,0,0,[title UTF8String]);}
- (void)setLevel:(int)value{}
- (void)setOpaque:(BOOL)value{}
- (void)setAlphaValue:(CGFloat)value{}
- (void)setHasShadow:(BOOL)value{}
- (void)syncDelegateProperties{}
- (void)invalidate{if(gameWindow==self){cancelCapture();gameWindow=nil;}[bitmap release];bitmap=nil;owner=nil;}
- (void)dealloc{[self invalidate];[super dealloc];}
- (NSUInteger)windowHandle{return (NSUInteger)native;}
- (NSRect)frame{return frame;}
- (void)showWindowWithoutActivation{fprintf(stderr,"WAYLAND window show self=%p active=%p mapped=%d\n",self,gameWindow,mapped);mapped=YES;windowAction(self,RBX_WL_SHOW,0,0,NULL);}
- (void)showWindowForAppActivation:(NSRect)value{[self showWindowWithoutActivation];}
- (void)hideWindowForAppDeactivation:(NSRect)value{[self hideWindow];}
- (void)hideWindow{fprintf(stderr,"WAYLAND window hide self=%p active=%p mapped=%d caller=%p\n",self,gameWindow,mapped,__builtin_return_address(0));if(gameWindow==self)cancelCapture();mapped=NO;windowAction(self,RBX_WL_HIDE,0,0,NULL);}
- (void)placeAboveWindow:(NSInteger)other{[self showWindowWithoutActivation];}
- (void)placeBelowWindow:(NSInteger)other{[self showWindowWithoutActivation];}
- (void)makeKey{windowAction(self,RBX_WL_SHOW,0,0,NULL);}
- (void)makeMain{}
- (void)captureEvents{}
- (void)miniaturize{windowAction(self,RBX_WL_MINIMIZE,0,0,NULL);}
- (void)deminiaturize{[self showWindowWithoutActivation];}
- (BOOL)isMiniaturized{return NO;}
- (void)flushBuffer{}
- (void)flashWindow{}
- (void)addEntriesToDeviceDictionary:(NSDictionary*)entries{}
- (NSPoint)transformPoint:(NSPoint)p{return NSMakePoint(p.x,frame.size.height-p.y);}
- (NSPoint)mouseLocationOutsideOfEventStream{return [self transformPoint:locked?anchor:pointer];}
- (CGSubWindow*)createSubWindowWithFrame:(CGRect)value{return [[[RbxWaylandSubwindow alloc] init] autorelease];}
- (CGLContextObj)cglContext{return NULL;}
- (id)cgContext {
    if(!bitmap) {
        CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
        bitmap=(id)CGBitmapContextCreate(NULL,MAX(1,frame.size.width),MAX(1,frame.size.height),8,0,color,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);
        CGColorSpaceRelease(color);
    }
    return bitmap;
}
@end

// Text is the clipboard format used by Roblox's edit fields. Other formats
// remain local to their named pasteboard instead of clobbering desktop text.
@interface RbxWaylandPasteboard:NSPasteboard {NSString *boardName;NSMutableDictionary *values;NSInteger changes;}
- (id)initWithName:(NSString*)name;
@end
@implementation RbxWaylandPasteboard
- (id)initWithName:(NSString*)name{if((self=[super init])){boardName=[name copy];values=[NSMutableDictionary new];}return self;}
- (void)dealloc{[boardName release];[values release];[super dealloc];}
- (NSString*)name{return boardName;}
- (NSInteger)changeCount{return changes;}
- (BOOL)general{return [boardName isEqual:@"NSGeneralPboard"] || [boardName isEqual:@"Apple CFPasteboard general"];}
- (BOOL)textType:(NSString*)type{return [type isEqual:@"NSStringPboardType"] || [type isEqual:@"public.utf8-plain-text"];}
- (NSInteger)clearContents{[values removeAllObjects];if([self general] && api){void (^clear)(void)=^{api->clipboard("");};if([NSThread isMainThread])clear();else dispatch_sync(dispatch_get_main_queue(),clear);}return ++changes;}
- (NSInteger)declareTypes:(NSArray*)types owner:(id)owner{return [self clearContents];}
- (NSInteger)addTypes:(NSArray*)types owner:(id)owner{return ++changes;}
- (NSArray*)types{return [self general]?@[@"NSStringPboardType",@"public.utf8-plain-text"]:[values allKeys];}
- (NSData*)dataForType:(NSString*)type {
    if(![self general] || ![self textType:type] || !api)return [values objectForKey:type];
    __block NSData *result=nil;
    void (^read)(void)=^{const char *text=api->clipboard(NULL);if(text)result=[[[NSString stringWithUTF8String:text] dataUsingEncoding:[type isEqual:@"NSStringPboardType"]?NSUnicodeStringEncoding:NSUTF8StringEncoding] retain];};
    if([NSThread isMainThread])read();else dispatch_sync(dispatch_get_main_queue(),read);
    return [result autorelease];
}
- (BOOL)setData:(NSData*)data forType:(NSString*)type {
    if(!data || !type)return NO;
    if([self general] && [self textType:type] && api) {
        NSString *text=[[[NSString alloc] initWithData:data encoding:[type isEqual:@"NSStringPboardType"]?NSUnicodeStringEncoding:NSUTF8StringEncoding] autorelease];if(!text)return NO;
        __block BOOL ok=NO;void (^write)(void)=^{ok=api->clipboard([text UTF8String])!=NULL;};
        if([NSThread isMainThread])write();else dispatch_sync(dispatch_get_main_queue(),write);if(!ok)return NO;
    }
    [values setObject:data forKey:type];changes++;return YES;
}
@end

// Roblox's modern 64px cursor textures place their hotspot at the center.
// Keep game-provided NSCursor images and all other system shapes independent.
static const char *robloxCursorTexture(const char *name) {
    if(!name)return NULL;
    if(!strcmp(name,"arrowCursor"))return "ArrowFarCursor.png";
    if(!strcmp(name,"pointingHandCursor"))return "ArrowCursor.png";
    if(!strcmp(name,"IBeamCursor"))return "IBeamCursor.png";
    return NULL;
}
@interface RbxWaylandCursor:NSObject { @public NSImage *image;NSPoint hot;NSString *name; } @end
@implementation RbxWaylandCursor
- (void)dealloc{[image release];[name release];[super dealloc];}
@end
@interface RbxWaylandDisplay:NSDisplay
- (void)pump:(id)timer;
@end
@interface NSEvent (RbxWaylandPrivate)
+ (NSEvent*)mouseEventWithType:(NSEventType)type location:(NSPoint)point modifierFlags:(NSEventModifierFlags)flags window:(NSWindow*)window clickCount:(NSInteger)count deltaX:(CGFloat)dx deltaY:(CGFloat)dy;
- (void)_setButtonNumber:(NSInteger)button;
@end
static struct RbxWaylandEvent pendingFocusEvent;
static BOOL hasPendingFocus;
static int pollInput(struct RbxWaylandEvent *event,BOOL queueBusy) {
    // Cocoa must dispatch earlier input before a backend focus transition.
    if(hasPendingFocus) {
        if(queueBusy)return 0;
        *event=pendingFocusEvent;hasPendingFocus=NO;return 1;
    }
    if(!api->poll(event))return 0;
    if(queueBusy && (event->type==RBX_WL_BLUR || event->type==RBX_WL_FOCUS)) {
        pendingFocusEvent=*event;hasPendingFocus=YES;return 0;
    }
    return 1;
}
@implementation RbxWaylandDisplay
+ (void)initialize {
    if(self!=[RbxWaylandDisplay class])return;
    // Reuse the backend's platform-independent Fontconfig/color methods. Loading
    // the bundle does not open X; its X11Display initializer is never invoked.
        NSBundle *bundle=[NSBundle bundleForClass:[NSDisplay class]];
        for(NSString *path in [bundle pathsForResourcesOfType:@"backend" inDirectory:@"Backends"])[[NSBundle bundleWithPath:path] load];
        Class source=objc_getClass("X11Display"),target=[RbxWaylandDisplay class];
        for(NSString *name in @[@"allFontFamilyNames",@"fontTypefacesForFamilyName:",@"substituteFamilyName:",@"colorWithName:"]) {
            SEL selector=NSSelectorFromString(name);Method method=class_getInstanceMethod(source,selector);
            if(method)class_addMethod(target,selector,method_getImplementation(method),method_getTypeEncoding(method));
        }
}

- (id)init {
    if(!(self=[super init]))return nil;
    if([NSThread isMainThread]) {
        initializeNative();
        [NSTimer scheduledTimerWithTimeInterval:0.002 target:self selector:@selector(pump:) userInfo:nil repeats:YES];
    }
    return self;
}
- (void*)display{return api?api->display():NULL;}
- (CGWindow*)newWindowWithDelegate:(NSWindow*)delegate{return [[RbxWaylandWindow alloc] initWithDelegate:delegate];}
- (NSArray*)screens {
    int w=1920,h=1080;double hz=60;if(api)api->screen(&w,&h,&hz);
    NSRect rect=NSMakeRect(0,0,w,h);NSScreen *screen=[[[NSScreen alloc] initWithFrame:rect visibleFrame:rect] autorelease];
    return @[screen];
}
- (NSDictionary*)currentModeForScreen:(int)index {int w=1920,h=1080;double hz=60;if(api)api->screen(&w,&h,&hz);return @{@"Width":@(w),@"Height":@(h),@"Depth":@32,@"RefreshRate":@(hz)};}
- (NSArray*)modesForScreen:(int)index{return @[[self currentModeForScreen:index]];}
- (BOOL)setMode:(NSDictionary*)mode forScreen:(int)index{return NO;}
- (CGRect)insetRect:(CGRect)rect forNativeWindowBorderWithStyle:(NSUInteger)style{return rect;}
- (CGRect)outsetRect:(CGRect)rect forNativeWindowBorderWithStyle:(NSUInteger)style{return rect;}
- (NSPoint)mouseLocation{return gameWindow?[gameWindow mouseLocationOutsideOfEventStream]:NSZeroPoint;}
- (NSUInteger)currentModifierFlags{return modifiers;}
- (NSArray*)orderedWindowNumbers{return gameWindow?@[@([gameWindow->owner windowNumber])]:@[];}
- (NSTimeInterval)textCaretBlinkInterval{return 0.5;}
- (CGFloat)scrollerWidth{return 15;}
- (CGFloat)doubleClickInterval{return 0.5;}
- (id)draggingManager{return nil;}
- (void)beep{}
- (void)_addSystemColor:(NSColor*)color forName:(NSString*)name{}
- (int)keyboardLayoutId{return 0;}
- (void)keyboardLayoutName:(NSString**)name fullName:(NSString**)full{if(name)*name=@"Wayland";if(full)*full=@"Wayland keyboard";}
- (UCKeyboardLayout*)keyboardLayout:(uint32_t*)size{if(size)*size=0;return NULL;}
- (NSPasteboard*)pasteboardWithName:(NSString*)name {
    static NSMutableDictionary *boards;
    @synchronized([RbxWaylandPasteboard class]){if(!boards)boards=[NSMutableDictionary new];id board=[boards objectForKey:name];if(!board){board=[[[RbxWaylandPasteboard alloc] initWithName:name] autorelease];[boards setObject:board forKey:name];}return board;}
}
- (void)rbxSetCursorVisible:(BOOL)value {
    unsigned request=__atomic_add_fetch(&cursorVisibilityRequest,1,__ATOMIC_SEQ_CST);
    void (^apply)(void)=^{applyCursorVisibility(request,value);};
    if([NSThread isMainThread])apply();else dispatch_async(dispatch_get_main_queue(),apply);
}
- (void)hideCursor{[self rbxSetCursorVisible:NO];}
- (void)unhideCursor{[self rbxSetCursorVisible:YES];}
- (id)cursorWithName:(NSString*)name {
    RbxWaylandCursor *c=[RbxWaylandCursor new];c->name=[name copy];
    const char *texture=robloxCursorTexture([name UTF8String]);
    if(texture) {
        NSString *relative=[@"content/textures/Cursors/KeyboardMouse" stringByAppendingPathComponent:[NSString stringWithUTF8String:texture]];
        NSString *path=[[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:relative];
        c->image=[[NSImage alloc] initWithContentsOfFile:path];
        if(c->image){NSSize size=[c->image size];c->hot=NSMakePoint(size.width/2,size.height/2);}
    }
    return [c autorelease];
}
- (id)cursorWithImage:(NSImage*)image hotSpot:(NSPoint)hot {RbxWaylandCursor *c=[RbxWaylandCursor new];c->image=[image retain];c->hot=hot;return [c autorelease];}
- (void)setCursor:(RbxWaylandCursor*)cursor {
    if(!cursor)return;
    if(![NSThread isMainThread]){dispatch_async(dispatch_get_main_queue(),^{[self setCursor:cursor];});return;}
    if(!api)return;
    static RbxWaylandCursor *applied;
    if(applied==cursor)return; // Tracking can reapply the same cursor every motion.
    if(pointerTrace())fprintf(stderr,"CURSOR set image=%d name=%s\n",cursor->image!=nil,cursor->name?[cursor->name UTF8String]:"custom");
    if(cursor->image) {
        NSSize size=[cursor->image size];if(size.width<1 || size.height<1 || size.width>1024 || size.height>1024)return;
        CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();CGContextRef context=CGBitmapContextCreate(NULL,size.width,size.height,8,0,color,kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little);CGColorSpaceRelease(color);
        if(!context)return;
        [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithGraphicsPort:context flipped:NO]];
        [cursor->image drawInRect:NSMakeRect(0,0,size.width,size.height) fromRect:NSZeroRect operation:NSCompositeCopy fraction:1];
        [NSGraphicsContext restoreGraphicsState];
        if(pointerTrace())fprintf(stderr,"CURSOR bitmap width=%.0f height=%.0f hot=%.1f,%.1f\n",size.width,size.height,cursor->hot.x,cursor->hot.y);
        api->cursor(CGBitmapContextGetData(context),size.width,size.height,CGBitmapContextGetBytesPerRow(context),cursor->hot.x,cursor->hot.y,NULL);CGContextRelease(context);
    } else api->cursor(NULL,0,0,0,0,0,[cursor->name UTF8String]);
    [cursor retain];[applied release];applied=cursor;
    rbxRefreshLockedCursor();
}
- (void)warpMouse:(NSPoint)p {
    if(![NSThread isMainThread]){
        @synchronized([NSObject class]) {
            pendingWarp=p;if(warpQueued)return;warpQueued=YES;
        }
        dispatch_async(dispatch_get_main_queue(),^{
            NSPoint next;@synchronized([NSObject class]){next=pendingWarp;warpQueued=NO;}
            [self warpMouse:next];
        });return;
    }
    if(!gameWindow)return;
    int width,height;double hz;api->screen(&width,&height,&hz);
    NSPoint next=warpScreenPoint(p,height,gameWindow->frame.size.height);
    if(pointerTrace())fprintf(stderr,"POINTER warp x=%.1f y=%.1f locked=%d buttons=%u\n",next.x,next.y,locked,buttons);
    warpPointer(next);
}
- (void)grabMouse:(BOOL)value {
    unsigned request=__atomic_add_fetch(&captureRequest,1,__ATOMIC_SEQ_CST);
    if(pointerTrace())fprintf(stderr,"POINTER appkit request=%u value=%d main=%d\n",request,value,[NSThread isMainThread]);
    if([NSThread isMainThread]){
        if(request==__atomic_load_n(&captureRequest,__ATOMIC_SEQ_CST) && captureWanted!=value){captureWanted=value;applyCapture(value);}
        return;
    }
    @synchronized([NSObject class]) {
        pendingCapture=value;pendingCaptureRequest=request;
        if(captureQueued)return;captureQueued=YES;
    }
    dispatch_async(dispatch_get_main_queue(),^{
        BOOL next;unsigned latest;
        @synchronized([NSObject class]){next=pendingCapture;latest=pendingCaptureRequest;captureQueued=NO;}
        if(latest==__atomic_load_n(&captureRequest,__ATOMIC_SEQ_CST) && captureWanted!=next){captureWanted=next;applyCapture(next);}
        else if(pointerTrace())fprintf(stderr,"POINTER appkit stale request=%u ignored\n",latest);
    });
}
- (void)pump:(id)timer {
    static BOOL pumping;
    if(!api || ![NSThread isMainThread] || pumping)return;
    pumping=YES;
    @try {
    id queue=object_getIvar(self,class_getInstanceVariable([NSDisplay class],"_eventQueue"));
    struct RbxWaylandEvent e;
    for(unsigned i=0;i<512 && pollInput(&e,[queue count]!=0);i++) {
        if(!gameWindow)continue;NSWindow *window=gameWindow->owner;modifiers=e.modifiers;
        if(e.type==RBX_WL_RESIZE) {
            NSRect frame=NSMakeRect(0,0,e.x,e.y);gameWindow->frame=frame;
            [gameWindow->bitmap release];gameWindow->bitmap=nil;
            [window platformWindow:gameWindow frameChanged:frame didSize:YES];continue;
        }
        if(e.type==RBX_WL_CLOSE){cancelCapture();if(pointerTrace())fprintf(stderr,"POINTER appkit close\n");[[NSApplication sharedApplication] terminate:nil];continue;}
        if(e.type==RBX_WL_FOCUS){captureFocus(YES);if(pointerTrace())fprintf(stderr,"POINTER appkit focus\n");[window platformWindowActivated:gameWindow displayIfNeeded:YES];continue;}
        if(e.type==RBX_WL_BLUR){
            // A release on another workspace never reaches this surface. End
            // held buttons through normal window dispatch before deactivation; a
            // queued release could otherwise run after focus has already returned.
            NSPoint position=[gameWindow transformPoint:locked?anchor:pointer];
            unsigned held=buttons;captureFocus(NO);
            for(unsigned button=1;button<32;button++)if(held&(1u<<button)) {
                NSEventType type=button==1?NSLeftMouseUp:button==3?NSRightMouseUp:NSOtherMouseUp;
                NSEvent *up=[NSEvent mouseEventWithType:type location:position modifierFlags:0 window:window clickCount:1 deltaX:0 deltaY:0];
                [up _setButtonNumber:button];[[NSApplication sharedApplication] sendEvent:up];
            }
            if(pointerTrace())fprintf(stderr,"POINTER appkit blur\n");
            [window platformWindowDeactivated:gameWindow checkForAppDeactivation:YES];return;
        }
        if(e.type==RBX_WL_TEXT) {
            id responder=[window firstResponder];
            if([responder respondsToSelector:@selector(insertText:)])[responder insertText:[NSString stringWithUTF8String:e.text]];
            continue;
        }
        if(e.type==RBX_WL_KEY_DOWN || e.type==RBX_WL_KEY_UP) {
            unsigned short key=macKey(e.key);if(key==0xffff)continue;
            if((e.key>=83 && e.key<=99) || e.key==103)modifiers|=1u<<21; // NSNumericPadKeyMask
            if(pointerTrace() && e.key>=224)fprintf(stderr,"POINTER modifier key=%u down=%d flags=%u\n",e.key,e.type==RBX_WL_KEY_DOWN,modifiers);
            NSString *text=[NSString stringWithUTF8String:e.text];
            NSEventType type=e.key>=224?NSFlagsChanged:e.type==RBX_WL_KEY_DOWN?NSKeyDown:NSKeyUp;
            [self postEvent:[NSEvent keyEventWithType:type location:[self mouseLocation] modifierFlags:modifiers timestamp:[NSDate timeIntervalSinceReferenceDate] windowNumber:[window windowNumber] context:nil characters:text charactersIgnoringModifiers:text isARepeat:e.repeat keyCode:key] atStart:NO];continue;
        }
        pointer=NSMakePoint(e.x,e.y);NSPoint position=[gameWindow transformPoint:locked?anchor:pointer];
        if(e.type==RBX_WL_MOTION && pointerTrace())fprintf(stderr,"POINTER motion dx=%.1f dy=%.1f locked=%d buttons=%u\n",e.dx,e.dy,locked,buttons);
        NSEventType type=NSMouseMoved;unsigned button=e.button;
        if(e.type==RBX_WL_DOWN || e.type==RBX_WL_UP) {
            if(pointerTrace())fprintf(stderr,"POINTER appkit button=%u down=%d locked=%d focused=%d\n",button,e.type==RBX_WL_DOWN,locked,focused);
            if(e.type==RBX_WL_DOWN && button==3)rightPressPoint=pointer;
            unsigned bit=button<32?1u<<button:0;
            if(e.type==RBX_WL_DOWN)buttons|=bit;else buttons&=~bit;
            type=button==1?(e.type==RBX_WL_DOWN?NSLeftMouseDown:NSLeftMouseUp):button==3?(e.type==RBX_WL_DOWN?NSRightMouseDown:NSRightMouseUp):(e.type==RBX_WL_DOWN?NSOtherMouseDown:NSOtherMouseUp);
        } else if(e.type==RBX_WL_SCROLL) {
            type=NSScrollWheel;
            if(pointerTrace())fprintf(stderr,"POINTER appkit wheel dx=%.2f dy=%.2f at=%.0f,%.0f locked=%d\n",e.dx,e.dy,position.x,position.y,locked);
        }
        else if(e.type==RBX_WL_MOTION){type=(buttons&(1<<1))?NSLeftMouseDragged:(buttons&(1<<3))?NSRightMouseDragged:NSMouseMoved;button=type==NSRightMouseDragged?3:1;}
        else continue;
        // Match Cocoa's physical deltas while flipping only the position origin.
        // Merge adjacent motion counts, never across a button/key/scroll event.
        NSEvent *last=[queue lastObject];
        if(e.type==RBX_WL_MOTION && [last type]==type && [last window]==window && [last modifierFlags]==modifiers) {
            e.dx+=[last deltaX];e.dy+=[last deltaY];[queue removeLastObject];
        }
        NSEvent *event=[NSEvent mouseEventWithType:type location:position modifierFlags:modifiers window:window clickCount:e.clicks deltaX:e.dx deltaY:e.dy];
        [event _setButtonNumber:button];[self postEvent:event atStart:NO];
        // Mouse events alone do not run AppKit cursor rectangles/tracking.
        // Defer the refresh until this pump drains its batch: raw capture can
        // deliver hundreds of motions per tick, and tracking only needs the
        // final position.
        if(e.type==RBX_WL_MOTION) {
            markCursorTrackingDirty();
        }
    }
    if(takeCursorTrackingRefresh()) {
        if(pointerTrace())fprintf(stderr,"CURSOR stationary refresh\n");
        [gameWindow->owner platformWindowSetCursorEvent:gameWindow];
    }
    } @finally {pumping=NO;}
}
@end
static void (*originalInvalidateTracking)(id,SEL);
static void invalidateTracking(id window,SEL selector) {
    originalInvalidateTracking(window,selector);
    markCursorTrackingDirty();
}
@implementation NSDisplay (RbxWaylandSelection)
+ (void)load {
    if(!enabled())return;
    Method theme=class_getInstanceMethod(objc_getClass("NSThemeFrame"),sel_registerName("drawRect:"));
    if(theme)originalThemeDraw=(void*)method_setImplementation(theme,(IMP)drawThemeFrame);
    Method tracking=class_getInstanceMethod([NSWindow class],sel_registerName("_invalidateTrackingAreas"));
    if(tracking)originalInvalidateTracking=(void*)method_setImplementation(tracking,(IMP)invalidateTracking);
    method_exchangeImplementations(class_getInstanceMethod(self,@selector(init)),class_getInstanceMethod(self,@selector(rbxWaylandInit)));

}
- (id)rbxWaylandInit {
    if([self class]==[NSDisplay class]){[self release];return [[RbxWaylandDisplay alloc] init];}
    return [self rbxWaylandInit];
}
@end
