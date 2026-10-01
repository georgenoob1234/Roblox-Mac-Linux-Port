// Methods Roblox calls that Darling's AppKit does not implement, added as categories so they
// attach to the real classes at load time.
//
// The backing-store conversions are the Retina API: on a 1x display every one of them is the
// identity, which is exactly what we want until someone runs this on a HiDPI screen.
// ponytail: hardcoded 1.0 scale, read the real scale off the SDL3/X11 output when M5 lands.

@interface NSObject { void *isa; }
+ (id)alloc; - (id)init;
@end

typedef struct { double width, height; } NSSize;
typedef struct { double x, y; } NSPoint;
typedef struct { NSPoint origin; NSSize size; } NSRect;
typedef struct { unsigned long location,length; } NSRange;

@interface NSTextView : NSObject @end
@implementation NSTextView (RbxTextComposition)
// Darling's editor has no marked-text/preedit state. Report that honestly;
// Roblox's NSTextView subclass queries it when any textbox gains focus.
- (signed char)hasMarkedText { return 0; }
- (NSRange)markedRange { return (NSRange){0x7fffffffffffffffUL,0}; }
- (void)unmarkText {}
@end

@interface NSView : NSObject - (NSRect)visibleRect; @end
extern Class objc_getClass(const char *);
extern void *class_getInstanceMethod(Class,SEL);
extern void method_exchangeImplementations(void *,void *);
extern void *method_setImplementation(void *,void *);
extern void *class_getInstanceVariable(Class,const char *);
extern id object_getIvar(id,void *);
extern long ivar_getOffset(void *);
@interface NSObject (LayerContextRoot)
- (void)setLayer:(id)layer;
@end

@implementation NSView (RbxReuseLayerContext)
+ (void)load {
    Class cls=objc_getClass("NSView");
    method_exchangeImplementations(class_getInstanceMethod(cls,@selector(_createLayerContextIfNeeded)),class_getInstanceMethod(cls,@selector(rbxCreateLayerContextIfNeeded)));
    method_exchangeImplementations(class_getInstanceMethod(cls,@selector(setLayer:)),class_getInstanceMethod(cls,@selector(rbxSetLayer:)));
}
- (void)rbxSetLayer:(id)layer {
    [self rbxSetLayer:layer];
    // Replacing a root view's backing layer must also replace the compositor root.
    id context=object_getIvar(self,class_getInstanceVariable(objc_getClass("NSView"),"_layerContext"));
    if(context)[context setLayer:layer];
    // setLayer leaves cached view transforms valid, so Darling never configures
    // the replacement layer's bounds/anchor/position. Reuse its geometry setup.
    *(signed char *)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("NSView"),"_validTransforms")))=0;
    [self visibleRect];
}
- (void)rbxCreateLayerContextIfNeeded {
    // setLayer followed by setWantsLayer calls this twice. Recreating the GL
    // context leaves CAMetalLayer's texture name pointing into the old context;
    // that name then collides with (and deletes) a new drawable's shared texture.
    if(!object_getIvar(self,class_getInstanceVariable(objc_getClass("NSView"),"_layerContext")))
        [self rbxCreateLayerContextIfNeeded];
}
@end

@implementation NSView (RbxBacking)
- (NSSize)convertSizeToBacking:(NSSize)s   { return s; }
- (NSSize)convertSizeFromBacking:(NSSize)s { return s; }
- (NSPoint)convertPointToBacking:(NSPoint)p   { return p; }
- (NSPoint)convertPointFromBacking:(NSPoint)p { return p; }
- (NSRect)convertRectToBacking:(NSRect)r   { return r; }
- (NSRect)convertRectFromBacking:(NSRect)r { return r; }
- (NSRect)backingAlignedRect:(NSRect)r options:(unsigned long)o { (void)o; return r; }

// Trackpad/touch-bar input. We have neither; "no touch types accepted" is the honest answer.
- (void)setAllowedTouchTypes:(unsigned long)t { (void)t; }
- (unsigned long)allowedTouchTypes { return 0; }
@end

@interface NSEvent_mouse : NSObject
- (double)deltaX; - (double)deltaY;
@end
@implementation NSEvent_mouse (RbxButtonNumbers)
- (double)scrollingDeltaX { return [self deltaX]; }
- (double)scrollingDeltaY { return [self deltaY]; }
- (signed char)hasPreciseScrollingDeltas { return 0; }
- (signed char)isDirectionInvertedFromDevice { return 0; }
- (unsigned long)phase { return 0; }
- (unsigned long)momentumPhase { return 0; }
- (void)_setButtonNumber:(long)number {
    // Only X11Display calls this private setter: X11 left/middle/right are
    // 1/2/3; Cocoa left/right/middle are 0/1/2. X11 4-7 are scroll wheels.
    long cocoa=number==1 ? 0 : number==3 ? 1 : number==2 ? 2 : number>=8 ? number-5 : 0;
    *(long *)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("NSEvent_mouse"),"_buttonNumber")))=cocoa;
}
@end

@interface NSWindow : NSObject
- (id)platformWindow;
- (NSPoint)convertBaseToScreen:(NSPoint)point;
- (NSPoint)convertScreenToBase:(NSPoint)point;
@end
@interface NSObject (LayerWindow)
- (id)createSubWindowWithFrame:(NSRect)frame;
- (id)delegate; - (NSRect)frame;
- (id)windowForID:(unsigned long)number;
- (signed char)acceptsMouseMovedEvents;
- (signed char)platformWindowSetCursorEvent:(id)window;
- (unsigned long)windowHandle;
- (void)syncDelegateProperties;
- (id)copy; - (id)autorelease; - (unsigned long)count; - (id)objectAtIndex:(unsigned long)i;
- (unsigned long)options; - (signed char)_mouseInside; - (id)owner;
- (signed char)respondsToSelector:(SEL)selector; - (void)mouseMoved:(id)event;
- (id)nextResponder; - (void)otherMouseDown:(id)event; - (void)otherMouseUp:(id)event;
- (NSPoint)mouseLocationOutsideOfEventStream;
- (NSPoint)transformPoint:(NSPoint)point;
- (void)setLastKnownCursorPosition:(NSPoint)point;
- (id)keyWindow; - (id)mainWindow; + (id)sharedApplication; - (id)platformWindow;
- (unsigned long)modifierFlagsForState:(unsigned)state;
- (void)postEvent:(id)event atStart:(signed char)start;
- (long)windowNumber;
- (id)lastObject; - (void)removeLastObject; - (void)removeObjectAtIndex:(unsigned long)i;
- (unsigned long)type; - (unsigned long)modifierFlags; - (id)window;
- (double)deltaX; - (double)deltaY;
- (void)_setButtonNumber:(long)number; - (long)buttonNumber;
- (NSPoint)locationInWindow; - (id)hitTest:(NSPoint)point; - (id)class;
@end
@interface NSEvent : NSObject
+ (id)mouseEventWithType:(unsigned long)type location:(NSPoint)point modifierFlags:(unsigned long)flags window:(id)window clickCount:(long)count deltaX:(double)x deltaY:(double)y;
+ (unsigned long)modifierFlags;
+ (id)keyEventWithType:(unsigned long)type location:(NSPoint)point modifierFlags:(unsigned long)flags timestamp:(double)time windowNumber:(long)number context:(id)context characters:(id)text charactersIgnoringModifiers:(id)plain isARepeat:(signed char)repeat keyCode:(unsigned short)code;
@end
// Tracking areas work even when ordinary mouseMoved delivery is disabled.
// X11Display's early return skips these callbacks in that case.
typedef struct { int type; unsigned long serial; int send; void *display; unsigned long window; } XAnyEvent;
typedef struct { XAnyEvent any; unsigned long root,subwindow,time; int x,y,xroot,yroot; unsigned state,button; int sameScreen; } XButtonEvent;
extern void *dlopen(const char *,int),*dlsym(void *,const char *);
// XKeyEvent has the same layout as XButtonEvent (button is keycode).
static XButtonEvent heldKeys[256];
static unsigned heldModifiers[256];
static unsigned short modifierCode(unsigned long symbol,unsigned *mask) {
    switch(symbol) {
    case 0xffe1:*mask=1;return 56; case 0xffe2:*mask=1;return 60;
    case 0xffe3:*mask=4;return 59; case 0xffe4:*mask=4;return 62;
    case 0xffe9:*mask=8;return 58; case 0xffea:*mask=8;return 61;
    case 0xffeb:*mask=64;return 55;case 0xffec:*mask=64;return 54;
    case 0xffe5:*mask=2;return 57;
    default:*mask=0;return 0;
    }
}
static void (*originalPostXEvent)(id,SEL,XAnyEvent *);
typedef struct {int type;unsigned long serial;int send;void *display;int extension,evtype;unsigned cookie;void *data;} RawCookie;
typedef struct {int type;unsigned long serial;int send;void *display;int extension,evtype;unsigned long time;int device,source,detail,flags;struct {int length;unsigned char *mask;double *values;} valuators;double *raw;} RawMotion;
extern void NSLog(id,...);
static int rawOpcode,rawActive,capturePinned;
static void *captureConnection;
static id captureWindow,captureOwner;
static unsigned long rightPressWindow;
static NSPoint rightPressPoint,capturePoint;
extern void rbxLockedCursor(id,void *,NSPoint,int),rbxMoveLockedCursor(NSPoint),rbxInstallLockedCursor(void);
static unsigned rawButtons;
static int (*selectRaw)(void *,unsigned long,void *,int);
struct InputElfCalls {void *(*open)(const char *);int (*close)(void *);void *(*symbol)(void *,const char *);};
extern struct InputElfCalls *_elfcalls;
static void selectRawMotion(void *display,int enabled) {
    static void *xlib;if(!xlib)xlib=dlopen("/usr/lib/native/libX11.dylib",1);
    static int (*version)(void *,int *,int *);
    if(!selectRaw) {
        void *xi=_elfcalls ? _elfcalls->open("libXi.so.6") : 0;
        version=xi ? _elfcalls->symbol(xi,"XIQueryVersion") : 0;
        selectRaw=xi ? _elfcalls->symbol(xi,"XISelectEvents") : 0;
    }
    rawActive=0;
    if(!selectRaw || !version)return;
    // Version negotiation belongs to the receiving connection. XI 2.1 keeps
    // raw motion flowing during grabs (including GTK's implicit button grab).
    if(enabled) {
        int event,error,major=2,minor=1;
        int (*query)(void *,const char *,int *,int *,int *)=dlsym(xlib,"XQueryExtension");
        if(!query || !query(display,"XInputExtension",&rawOpcode,&event,&error) || version(display,&major,&minor))return;
    }
    unsigned char bits[3]={0,0,enabled ? 2 : 0}; // XI_RawMotion (17), master pointer only.
    struct {int device,length;unsigned char *mask;} mask={1,3,bits};
    unsigned long (*root)(void *)=dlsym(xlib,"XDefaultRootWindow");
    rawActive=!selectRaw(display,root(display),&mask,1) && enabled;
}
@interface NSDisplay:NSObject
- (NSPoint)rbxCapturePointForWindow:(id)window;
@end
extern unsigned long NSEventMaskFromType(unsigned long);
@implementation NSDisplay (RbxRawMotion)
- (NSPoint)rbxCapturePointForWindow:(id)window {
    if((rawButtons&(1u<<10)) && rightPressWindow==[window windowHandle])return rightPressPoint;
    return [window transformPoint:[window mouseLocationOutsideOfEventStream]];
}
- (void)discardEventsMatchingMask:(unsigned long)mask beforeEvent:(id)event {
    id queue=object_getIvar(self,class_getInstanceVariable(objc_getClass("NSDisplay"),"_eventQueue"));
    long i=[queue count];
    while(--i>=0 && [queue objectAtIndex:i]!=event) {}
    // Test each queued event, not the boundary event. Darling's predicate
    // removed mouse-down and key events whenever a newer drag was posted.
    while(--i>=0)
        if(NSEventMaskFromType([[queue objectAtIndex:i] type]) & mask)
            [queue removeObjectAtIndex:i];
}
- (void)rbxRawMotion:(RawMotion *)motion window:(id)window {
    if(!window || !motion->raw || !motion->valuators.mask || motion->valuators.length<1)return;
    unsigned bits=motion->valuators.mask[0],at=0;
    double x=(bits&1)?motion->raw[at++]:0,y=(bits&2)?motion->raw[at]:0;
    if(!x && !y)return;
    unsigned long type=(rawButtons&(1u<<8))?6:(rawButtons&(1u<<10))?7:5;
    id delegate=[window delegate];unsigned long flags=[NSEvent modifierFlags];
    id queue=object_getIvar(self,class_getInstanceVariable(objc_getClass("NSDisplay"),"_eventQueue"));
    id last=[queue lastObject];
    // Preserve every physical count without making Roblox process a growing
    // queue at the mouse polling rate. Never merge across other input events.
    if(last && [last type]==type && [last window]==delegate && [last modifierFlags]==flags) {
        x += [last deltaX];y += [last deltaY];[queue removeLastObject];
    }
    NSPoint location=capturePinned && window==captureWindow?[window transformPoint:capturePoint]:[window mouseLocationOutsideOfEventStream];
    id event=[NSEvent mouseEventWithType:type location:location
        modifierFlags:flags window:delegate clickCount:1 deltaX:x deltaY:y];
    [event _setButtonNumber:type==7 ? 3 : 1]; // setter converts X11 indices to Cocoa
    [self postEvent:event atStart:0];
}
@end
static signed char *cursorGrabbed(id display) {
    return (signed char *)display+ivar_getOffset(class_getInstanceVariable(objc_getClass("X11Display"),"_cursorGrabbed"));
}
static void *windowConnection(id window) {
    return *(void **)((char *)window+ivar_getOffset(class_getInstanceVariable(objc_getClass("X11Window"),"_display")));
}
static void warpInWindow(id display,id window,NSPoint point) {
    static int (*warp)(void *,unsigned long,unsigned long,int,int,unsigned,unsigned,int,int);
    if(!warp)warp=dlsym(dlopen("/usr/lib/native/libX11.dylib",1),"XWarpPointer");
    void *xdisplay=windowConnection(window);
    warp(xdisplay,0,[window windowHandle],0,0,0,0,(int)point.x,(int)point.y);
}
// A visible cursor prevents Xwayland's warp emulation from locking. Use the
// existing game window for confinement and a blank active-grab cursor; no child
// input windows and no changes to NSCursor's hide count or the guest cursor.
int rbxGrabPointer(id window,int pin) {
    void *display=windowConnection(window);
    unsigned long handle=[window windowHandle],cursor=0;
    static void *lib;if(!lib)lib=dlopen("/usr/lib/native/libX11.dylib",1);
    if(pin) {
        unsigned long (*bitmap)(void *,unsigned long,const char *,unsigned,unsigned)=dlsym(lib,"XCreateBitmapFromData");
        struct Color {unsigned long pixel;unsigned short red,green,blue;char flags,pad;} color={0};
        unsigned long (*create)(void *,unsigned long,unsigned long,struct Color *,struct Color *,unsigned,unsigned)=dlsym(lib,"XCreatePixmapCursor");
        int (*freePixmap)(void *,unsigned long)=dlsym(lib,"XFreePixmap");
        unsigned long pixmap=bitmap(display,handle,"",1,1);
        if(pixmap){cursor=create(display,pixmap,pixmap,&color,&color,0,0);freePixmap(display,pixmap);}
        if(!cursor)return 1;
    }
    int (*grab)(void *,unsigned long,int,unsigned,int,int,unsigned long,unsigned long,unsigned long)=dlsym(lib,"XGrabPointer");
    int result=grab(display,handle,0,(1u<<2)|(1u<<3)|(1u<<6),1,1,pin?handle:0,cursor,0);
    // The server retains the cursor while the grab uses it.
    if(cursor){int (*release)(void *,unsigned long)=dlsym(lib,"XFreeCursor");release(display,cursor);}
    int (*flush)(void *)=dlsym(lib,"XFlush");flush(display);
    return result;
}
static void updatePointerLock(void) {
    if(!captureConnection || !captureWindow)return;
    int pin=rawActive; // CGAssociate(false) covers right-drag, Shift Lock and first-person.
    if(pin==capturePinned)return;
    // Submit the position hint before releasing Xwayland's lock.
    if(capturePinned || pin)warpInWindow(captureOwner,captureWindow,capturePoint);
    rbxLockedCursor(captureWindow,captureConnection,capturePoint,pin);
    if(!rbxGrabPointer(captureWindow,pin)) {
        capturePinned=pin;
        if(pin)warpInWindow(captureOwner,captureWindow,capturePoint);
        NSLog(@"roblox-mac: pointer lock %s at %.0f,%.0f",pin?"enabled":"released",capturePoint.x,capturePoint.y);
        [captureWindow setLastKnownCursorPosition:[captureWindow transformPoint:capturePoint]];
    } else rbxLockedCursor(captureWindow,captureConnection,capturePoint,0);
}
static void warpMouseWithoutMotion(id self,SEL selector,NSPoint point) {
    // Raw input controls the camera; guest recenter requests must not move the
    // visible pointer or its logical position while it is pinned.
    if(captureConnection && rawActive && (rawButtons&(1u<<10)))return;
    void *xdisplay=*(void **)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("X11Display"),"_display")));
    static void *xlib; if(!xlib)xlib=dlopen("/usr/lib/native/libX11.dylib",1);
    unsigned long (*rootWindow)(void *)=dlsym(xlib,"XDefaultRootWindow");
    int (*translate)(void *,unsigned long,unsigned long,int,int,int *,int *,unsigned long *)=dlsym(xlib,"XTranslateCoordinates");
    int (*warp)(void *,unsigned long,unsigned long,int,int,unsigned,unsigned,int,int)=dlsym(xlib,"XWarpPointer");
    id app=[objc_getClass("NSApplication") sharedApplication],nswin=[app keyWindow];if(!nswin)nswin=[app mainWindow];
    id window=[nswin platformWindow];unsigned long root=rootWindow(xdisplay),child;int x,y;
    // CGWarpMouseCursorPosition does not represent physical movement. Update the
    // relative anchor before X11 delivers its synthetic MotionNotify.
    if(window && (*cursorGrabbed(self) || captureConnection) && translate(xdisplay,root,[window windowHandle],(int)point.x,(int)point.y,&x,&y,&child)) {
        [window setLastKnownCursorPosition:[window transformPoint:(NSPoint){x,y}]];
        // Roblox recenters through CGWarp for Shift Lock. Honor that anchor
        // while keeping relative capture active until CGAssociate(true).
        if(capturePinned && window==captureWindow && (capturePoint.x!=x || capturePoint.y!=y)) {
            capturePoint=(NSPoint){x,y};rbxMoveLockedCursor(capturePoint);
        }
    }
    warp(xdisplay,0,root,0,0,0,0,(int)point.x,(int)point.y);
}
static void grabMouseInWindow(id self,SEL selector,signed char grab) {
    signed char *active=cursorGrabbed(self);if(!!captureConnection==!!grab)return;
    void *xdisplay=*(void **)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("X11Display"),"_display")));
    static void *xlib;if(!xlib)xlib=dlopen("/usr/lib/native/libX11.dylib",1);
    if(!grab) {
        xdisplay=captureConnection;
        rbxLockedCursor(captureWindow,xdisplay,capturePoint,0);
        selectRawMotion(xdisplay,0);rawButtons=0;capturePinned=0;
        if(captureOwner)*cursorGrabbed(captureOwner)=0;
        captureConnection=0;captureWindow=0;captureOwner=0;
        int (*ungrab)(void *,unsigned long)=dlsym(xlib,"XUngrabPointer");ungrab(xdisplay,0);
        int (*flush)(void *)=dlsym(xlib,"XFlush");flush(xdisplay);*active=0;return;
    }
    id app=[objc_getClass("NSApplication") sharedApplication],nswin=[app keyWindow];if(!nswin)nswin=[app mainWindow];
    id window=[nswin platformWindow];if(!window)return;
    // CGAssociateMouseAndMouseCursorPosition may run on a worker NSDisplay.
    // Its private X connection is not polled by the window's event loop.
    xdisplay=windowConnection(window);
    NSPoint local=[self rbxCapturePointForWindow:window];
    if(rbxGrabPointer(window,0)!=0)return;
    selectRawMotion(xdisplay,1);
    captureConnection=xdisplay;captureWindow=window;captureOwner=self;capturePoint=local;*active=1;
    updatePointerLock();
    [window setLastKnownCursorPosition:[window transformPoint:local]];
    // Keep the existing window as the only input target. Extra confinement
    // windows interfere with GTK/Xwayland button delivery.
    warpInWindow(self,window,local);
    int (*flush)(void *)=dlsym(xlib,"XFlush");flush(xdisplay);
    NSLog(@"roblox-mac: pointer captured, raw input %s, window connection %s",rawActive?"enabled":"unavailable",
        xdisplay==*(void **)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("X11Display"),"_display")))?"local":"shared");
}
extern char *getenv(const char *);
static int inputTraceEnabled(void) {
    static int enabled=-1;if(enabled<0)enabled=getenv("ROBLOX_MAC_INPUT_TRACE")!=0;return enabled;
}
static void (*originalWindowSendEvent)(id,SEL,id);
extern id objc_getAssociatedObject(id,const void *);
extern void objc_setAssociatedObject(id,const void *,id,unsigned long);
@interface NSValue:NSObject
+ (id)valueWithPoint:(NSPoint)point; - (NSPoint)pointValue;
@end
static char buttonLocationKeys[3];
static void dispatchWindowEvent(id self,SEL selector,id event) {
    unsigned long type=[event type];
    int button=(type==1 || type==2 || type==6)?0:(type==3 || type==4 || type==7)?1:-1;
    NSPoint *slot=(NSPoint *)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("NSWindow"),"_mouseDownLocationInWindow")));
    NSPoint previous=*slot;
    if(button>=0) {
        // Cocotron shares one down location between buttons. A left release
        // erased the right target, dropping rightMouseUp and leaving capture on.
        id saved=objc_getAssociatedObject(self,&buttonLocationKeys[button]);
        *slot=saved?[saved pointValue]:(NSPoint){__builtin_nan(""),__builtin_nan("")};
    }
    if(inputTraceEnabled() && ((type>=1 && type<=7) || type==25 || type==26)) {
        NSPoint p=[event locationInWindow];
        NSPoint down=*(NSPoint *)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("NSWindow"),"_mouseDownLocationInWindow")));
        id background=object_getIvar(self,class_getInstanceVariable(objc_getClass("NSWindow"),"_backgroundView"));
        id hit=[background hitTest:(type==2 || type==4 || type==26)?down:p];
        NSLog(@"INPUT window type=%lu button=%ld flags=%lu position=%.0f,%.0f down=%.0f,%.0f receiver=%@",type,[event buttonNumber],[event modifierFlags],p.x,p.y,down.x,down.y,[hit class]);
    }
    if(type==25 || type==26) {
        // Cocotron's sendEvent: has no NSOtherMouse* cases, so middle-button
        // presses arrived here and fell into its unimplemented default: every
        // middle click was dropped. Dispatch it like left/right, off its own
        // down location, and walk the responder chain because NSResponder has
        // no otherMouse default to forward for us.
        id saved=objc_getAssociatedObject(self,&buttonLocationKeys[2]);
        NSPoint down=type==25 ? [event locationInWindow]
            : saved ? [saved pointValue] : (NSPoint){__builtin_nan(""),__builtin_nan("")};
        objc_setAssociatedObject(self,&buttonLocationKeys[2],type==25?[NSValue valueWithPoint:down]:0,1);
        id background=object_getIvar(self,class_getInstanceVariable(objc_getClass("NSWindow"),"_backgroundView"));
        for(id responder=[background hitTest:down];responder;responder=[responder nextResponder])
            if([responder respondsToSelector:type==25?@selector(otherMouseDown:):@selector(otherMouseUp:)]) {
                if(type==25)[responder otherMouseDown:event]; else [responder otherMouseUp:event];
                break;
            }
        return;
    }
    @try { originalWindowSendEvent(self,selector,event); }
    @finally {
        if(button>=0) {
            if(type<=4)objc_setAssociatedObject(self,&buttonLocationKeys[button],[NSValue valueWithPoint:*slot],1);
            *slot=previous;
        }
    }
}
void rbxUpdateWindowTracking(id window,id delegate) {
    if(delegate && ![delegate acceptsMouseMovedEvents]) {
            [delegate platformWindowSetCursorEvent:window];
            // NSWindow also incorrectly gates tracking-area mouseMoved on the
            // ordinary-window flag. Its callback above updates active/inside state.
            id areas=[[object_getIvar(delegate,class_getInstanceVariable(objc_getClass("NSWindow"),"_trackingAreas")) copy] autorelease];
            id motion=[NSEvent mouseEventWithType:5 location:[delegate mouseLocationOutsideOfEventStream]
                modifierFlags:[NSEvent modifierFlags] window:delegate clickCount:0 deltaX:0 deltaY:0];
            for(unsigned long i=0;i<[areas count];i++) {
                id area=[areas objectAtIndex:i],owner=[area owner];
                if(([area options]&2) && [area _mouseInside] && [owner respondsToSelector:@selector(mouseMoved:)])
                    [owner mouseMoved:motion];
            }
        }
}
static void postXEventWithTracking(id self,SEL selector,XAnyEvent *event) {
    if(inputTraceEnabled() && (event->type==4 || event->type==5)) {
        XButtonEvent *button=(XButtonEvent *)event;
        if(button->button<=3)NSLog(@"INPUT native type=%d button=%u position=%d,%d knownWindow=%d captured=%d",event->type,button->button,button->x,button->y,[self windowForID:event->window]!=0,captureConnection!=0);
    }
    if(event->type==35) {
        RawCookie *cookie=(RawCookie *)event;
        if(cookie->extension==rawOpcode && cookie->evtype==17) {
            static int (*get)(void *,RawCookie *);
            static void (*freeData)(void *,RawCookie *);
            if(!get) {void *lib=dlopen("/usr/lib/native/libX11.dylib",1);get=dlsym(lib,"XGetEventData");freeData=dlsym(lib,"XFreeEventData");}
            if(get(event->display,cookie)) {
                if(rawActive && event->display==captureConnection) {
                    static int reported;if(!reported){NSLog(@"roblox-mac: raw mouse motion received by window event loop");reported=1;}
                    [self rbxRawMotion:cookie->data window:captureWindow];
                }
                freeData(event->display,cookie);
            }
            return;
        }
    }
    if(event->type==4 || event->type==5) {
        XButtonEvent *button=(XButtonEvent *)event;
        if(capturePinned && event->display==captureConnection && event->window==rightPressWindow) {
            // Xwayland's virtual pointer can move between the raw packet and
            // our recenter request. Button coordinates stay at the lock anchor.
            button->x=(int)capturePoint.x;button->y=(int)capturePoint.y;
        }
        if(event->type==4 && button->button==3) {
            rightPressWindow=event->window;rightPressPoint=(NSPoint){button->x,button->y};
            // Roblox can keep CGAssociate(false) active for the whole game.
            // Each right-button press starts a new anchor within that capture.
            id window=[self windowForID:event->window];
            if(window)[window setLastKnownCursorPosition:[window transformPoint:rightPressPoint]];
        }
        if(button->button>=1 && button->button<=3) {
            unsigned bit=1u<<(button->button+7);
            if(event->type==4)rawButtons|=bit;else rawButtons&=~bit;
            updatePointerLock();
        }
    }
    if(event->type==10 && captureConnection==event->display && captureWindow && event->window==[captureWindow windowHandle]) {
        // Ignore the focus notifications generated by the grab itself.
        int mode=*(int *)((char *)event+sizeof(XAnyEvent));
        if(mode==0)grabMouseInWindow(self,@selector(grabMouse:),0);
    }
    if(event->type==6 && (*cursorGrabbed(self) || (captureConnection && event->display==captureConnection))) {
        XButtonEvent *motion=(XButtonEvent *)event;
        id window=[self windowForID:event->window],delegate=[window delegate];if(!delegate)return;
        NSPoint anchor=capturePinned?[window transformPoint:capturePoint]:[window mouseLocationOutsideOfEventStream];
        NSPoint point=[window transformPoint:(NSPoint){motion->x,motion->y}];
        if(point.x==anchor.x && point.y==anchor.y)return; // synthetic recenter event
        if(rawActive) {
            // Capture belongs to Roblox, including Shift Lock. Keep its anchor
            // fixed until the client reassociates the mouse; raw packets carry
            // all physical camera movement.
            if(capturePinned)warpInWindow(self,window,[window transformPoint:anchor]);
            else [window setLastKnownCursorPosition:point];
            return;
        }
        unsigned long type=(motion->state&(1u<<8))?6:(motion->state&(1u<<10))?7:5;
        id translated=[NSEvent mouseEventWithType:type location:anchor modifierFlags:[self modifierFlagsForState:motion->state]
            window:delegate clickCount:1 deltaX:point.x-anchor.x deltaY:anchor.y-point.y];
        [translated _setButtonNumber:type==7 ? 3 : 1];
        warpInWindow(self,window,[window transformPoint:anchor]);
        [self postEvent:translated atStart:0];
        return;
    }
    if(event->type==2 || event->type==3) {
        XButtonEvent *key=(XButtonEvent *)event;
        static unsigned long (*lookup)(XButtonEvent *,int);
        if(!lookup)lookup=dlsym(dlopen("/usr/lib/native/libX11.dylib",1),"XLookupKeysym");
        unsigned mask=0;unsigned short code=modifierCode(lookup(key,0),&mask);
        if(key->button<256) {
            heldKeys[key->button]=*key;
            heldKeys[key->button].any.type=event->type==2 ? 2 : 0;
            heldModifiers[key->button]=event->type==2 ? mask : 0;
        }
        if(mask) {
            // X11 state describes BEFORE the transition. Cocoa flagsChanged
            // describes AFTER it; releasing one Ctrl must preserve the other.
            unsigned state=key->state;
            if(mask==2) {if(event->type==2)state^=mask;else return;}
            else {
                state&=~mask;
                for(unsigned i=0;i<256;i++)state|=heldModifiers[i]&mask;
            }
            id delegate=[[self windowForID:event->window] delegate];
            if(delegate)[self postEvent:[NSEvent keyEventWithType:12 location:(NSPoint){0,0}
                modifierFlags:[self modifierFlagsForState:state] timestamp:0 windowNumber:[delegate windowNumber]
                context:0 characters:@"" charactersIgnoringModifiers:@"" isARepeat:0 keyCode:code] atStart:0];
            return;
        }
    }
    if(event->type==10) { // FocusOut: GTK/compositor may consume the eventual release.
        for(unsigned i=0;i<256;i++)if(heldKeys[i].any.type && heldKeys[i].any.window==event->window) {
            XButtonEvent release=heldKeys[i];release.any.type=3;release.state=0;
            postXEventWithTracking(self,selector,&release.any);
        }
    }

    if(event->type==4 || event->type==5) {
        XButtonEvent *button=(XButtonEvent *)event;
        if(button->button>=4 && button->button<=7) {
            if(event->type==5) {
                id window=[self windowForID:event->window],delegate=[window delegate];
                if(delegate)[self postEvent:[NSEvent mouseEventWithType:22
                    location:[window transformPoint:(NSPoint){button->x,button->y}]
                    modifierFlags:[self modifierFlagsForState:button->state] window:delegate clickCount:0
                    deltaX:button->button==6 ? 1 : button->button==7 ? -1 : 0
                    deltaY:button->button==4 ? 1 : button->button==5 ? -1 : 0] atStart:0];
            }
            return; // Wheel detents are scrolls, not mouse-button clicks.
        }
    }
    originalPostXEvent(self,selector,event);
    if(event->type==6) { // MotionNotify; original already updates polled position.
        id window=[self windowForID:event->window],delegate=[window delegate];
        rbxUpdateWindowTracking(window,delegate);
    }
}
extern void *dlopen(const char *,int),*dlsym(void *,const char *);
static void syncTrackingEvents(id self,SEL selector) {
    static int (*selectInput)(void *,unsigned long,long);
    if(!selectInput)selectInput=dlsym(dlopen("/usr/lib/native/libX11.dylib",1),"XSelectInput");
    void *display=*(void **)((char *)self+ivar_getOffset(class_getInstanceVariable(objc_getClass("X11Window"),"_display")));
    // Same X11Window event mask, plus PointerMotionMask for tracking areas.
    long mask=(1L<<0)|(1L<<1)|(1L<<2)|(1L<<3)|(1L<<4)|(1L<<5)|(1L<<6)|
              (1L<<13)|(1L<<15)|(1L<<16)|(1L<<17)|(1L<<20)|(1L<<21);
    selectInput(display,[self windowHandle],mask);
}
extern void rbxInstallCursor(void);
static void installTrackingEvents(void) {
    if(originalPostXEvent)return;
    void *method=class_getInstanceMethod(objc_getClass("X11Display"),@selector(postXEvent:));
    if(!method)return;
    rbxInstallCursor();rbxInstallLockedCursor();
    originalWindowSendEvent=method_setImplementation(class_getInstanceMethod(objc_getClass("NSWindow"),@selector(sendEvent:)),(void *)dispatchWindowEvent);
    originalPostXEvent=method_setImplementation(method,(void *)postXEventWithTracking);
    method_setImplementation(class_getInstanceMethod(objc_getClass("X11Display"),@selector(grabMouse:)),(void *)grabMouseInWindow);
    method_setImplementation(class_getInstanceMethod(objc_getClass("X11Display"),@selector(warpMouse:)),(void *)warpMouseWithoutMotion);
    method_setImplementation(class_getInstanceMethod(objc_getClass("X11Window"),@selector(syncDelegateProperties)),(void *)syncTrackingEvents);
}
static NSRect (*originalSubwindowFrame)(id,SEL,NSRect);
static NSRect subwindowFrame(id self,SEL selector,NSRect frame) {
    NSRect native=originalSubwindowFrame(self,selector,frame);
    id parent=object_getIvar(self,class_getInstanceVariable(objc_getClass("X11SubWindow"),"_parent"));
    id window=[parent delegate];
    // NSWindow sets its new frame before resizing views, but updates X11Window
    // afterward. Use the authoritative window height for bottom-to-top conversion.
    if(window)native.origin.y += [window frame].size.height-[parent frame].size.height;
    return native;
}
@implementation NSWindow (RbxLayerWindow)
- (id)_createSubWindowWithFrame:(NSRect)frame {
    // Layers can be installed before orderFront. The getter creates the native
    // parent; Darling's direct _platformWindow access silently returns nil.
    id platform=[self platformWindow];
    // X11.backend is loaded lazily by platformWindow, after shim +load runs.
    if(!originalSubwindowFrame) {
        void *method=class_getInstanceMethod(objc_getClass("X11SubWindow"),@selector(convertFrame:));
        if(method)originalSubwindowFrame=method_setImplementation(method,(void *)subwindowFrame);
        installTrackingEvents();
        [platform syncDelegateProperties];
    }
    return [platform createSubWindowWithFrame:frame];
}
@end
@implementation NSWindow (RbxScreenRects)
- (NSRect)convertRectFromScreen:(NSRect)rect {
    rect.origin = [self convertScreenToBase:rect.origin]; return rect;
}
- (NSRect)convertRectToScreen:(NSRect)rect {
    rect.origin = [self convertBaseToScreen:rect.origin]; return rect;
}
@end

// QuartzCore: Darling's CALayer has no HiDPI contents scaling either.
@interface CALayer : NSObject @end

@implementation CALayer (RbxBacking)
- (void)setContentsScale:(double)s { (void)s; }
- (double)contentsScale { return 1.0; }
@end

// AppKit asks -load again for each thread's NSDisplay. Darling reports NO for an already
// loaded bundle, so the second display sees an empty backend list and raises an exception.
// Loading an already loaded bundle is success; preserve genuine failures from the loader.
@interface NSBundle : NSObject
- (signed char)isLoaded;
- (signed char)loadAndReturnError:(id *)error;
@end
@implementation NSBundle (RbxIdempotentLoad)
- (signed char)load {
    signed char loaded=[self isLoaded] || [self loadAndReturnError:0];
    if(loaded)installTrackingEvents();
    return loaded;
}
@end

// Startup sets a legacy WebKit plug-in preference before showing the main window.
// Darling has the class but neither method. No plug-in engine exists here.
@interface WebPreferences : NSObject
+ (id)standardPreferences;
- (void)setPlugInsEnabled:(signed char)enabled;
@end
@implementation WebPreferences (RbxLegacyPlugins)
+ (id)standardPreferences {
    static id preferences;
    @synchronized(self) { if (!preferences) preferences = [[self alloc] init]; }
    return preferences;
}
- (void)setPlugInsEnabled:(signed char)enabled { (void)enabled; }
@end

// No camera capture backend is implemented in this runtime. Enumerate no capture devices.
@interface NSArray : NSObject + (id)array; @end
@interface AVCaptureDevice : NSObject @end
@implementation AVCaptureDevice (RbxNoCaptureDevices)
+ (id)devicesWithMediaType:(id)type { (void)type; return [NSArray array]; }
+ (id)defaultDeviceWithMediaType:(id)type { (void)type; return 0; }
@end

// Darling's currentAppleEvent stub returns an unspecified value. This launcher starts
// the executable directly and does not dispatch AppleEvents, so no event is current.
@interface NSAppleEventManager : NSObject @end
@implementation NSAppleEventManager (RbxDirectLaunch)
- (id)currentAppleEvent { return 0; }
@end

@interface NSProcessInfo : NSObject @end
@implementation NSProcessInfo (RbxThermalState)
// ponytail: no macOS thermal-pressure provider; add Linux sensor reporting later.
- (long)thermalState { return 0; } // NSProcessInfoThermalStateNominal
@end

extern void *dlsym(void *, const char *);
int CGAssociateMouseAndMouseCursorPosition(unsigned connected) {
    // Apple's true restores normal movement; Darling forwards true to grabMouse.
    int (*original)(unsigned)=dlsym((void *)-1L,"CGAssociateMouseAndMouseCursorPosition");
    return original ? original(!connected) : 1002;
}
extern void CFRelease(const void *);
int IOServiceAddMatchingNotification(void *port, const char *type, void *matching,
                                    void *callback, void *refcon, unsigned *iterator) {
    if (!port) {
        if (iterator) *iterator = 0;
        if (matching) CFRelease(matching); // this API consumes the matching dictionary
        return (int)0xe00002c2; // kIOReturnBadArgument, not a successful subscription
    }
    int (*original)(void *,const char *,void *,void *,void *,unsigned *) =
        dlsym((void *)-1L, "IOServiceAddMatchingNotification");
    return original(port,type,matching,callback,refcon,iterator);
}
void IONotificationPortDestroy(void *port) {
    if (port) ((void (*)(void *))dlsym((void *)-1L,"IONotificationPortDestroy"))(port);
}
unsigned IONotificationPortGetMachPort(void *port) {
    return port ? ((unsigned (*)(void *))dlsym((void *)-1L,"IONotificationPortGetMachPort"))(port) : 0;
}
