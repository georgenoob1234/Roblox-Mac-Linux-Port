// Diagnostic only: no view text, URLs, cookies or credential values are logged.
extern int fprintf(void *,const char *,...);extern void *__stderrp;
extern char *getenv(const char *);extern const char *object_getClassName(id);
extern void *class_getInstanceMethod(Class,SEL);extern void method_exchangeImplementations(void *,void *);
extern const char *sel_getName(SEL);
extern long long dispatch_time(long long,long long);
extern void dispatch_after_f(long long,void *,void *,void (*)(void *));extern char _dispatch_main_q;
typedef struct {double x,y;} Point;typedef struct {double width,height;} Size;typedef struct {Point origin;Size size;} Rect;
@interface NSObject
+ (Class)class;
- (void)doesNotRecognizeSelector:(SEL)s;
@end
@interface NSArray:NSObject - (unsigned long)count;- (id)objectAtIndex:(unsigned long)i;@end
@interface NSView:NSObject
- (Rect)frame;- (Rect)bounds;- (NSArray *)subviews;- (signed char)isHidden;- (id)layer;
- (Point)convertPoint:(Point)p fromView:(id)view;
- (NSArray *)trackingAreas;
@end
@interface NSWindow:NSObject - (id)contentView;- (id)_backgroundView;- (void)sendEvent:(id)event;- (Point)mouseLocationOutsideOfEventStream;- (id)firstResponder;@end
@interface NSEvent:NSObject - (unsigned long)type;- (Point)locationInWindow;- (long)buttonNumber;@end
@interface NSView (HitTest) - (id)hitTest:(Point)point;@end
@interface NSApplication:NSObject + (id)sharedApplication;- (NSArray *)windows;@end
static void view(id v,unsigned depth) {
 if(!v || depth>12)return;
 Rect f=[v frame],b=[v bounds];
 fprintf(__stderrp,"UI depth=%u class=%s frame=%.0f,%.0f %.0fx%.0f bounds=%.0fx%.0f hidden=%d layer=%s\n",depth,object_getClassName(v),f.origin.x,f.origin.y,f.size.width,f.size.height,b.size.width,b.size.height,[v isHidden],object_getClassName([v layer]));
 NSArray *children=[v subviews];for(unsigned long i=0;i<[children count] && i<50;i++)view([children objectAtIndex:i],depth+1);
}
static void dump(void *unused) {
 fprintf(__stderrp,"UI MAIN QUEUE ALIVE\n");NSArray *windows=[[NSApplication sharedApplication] windows];
 for(unsigned long i=0;i<[windows count];i++)view([[windows objectAtIndex:i] contentView],0);
}
@implementation NSObject (UITrace)
+ (void)load {
 if(!getenv("RBX_UI_TRACE"))return;
 method_exchangeImplementations(class_getInstanceMethod([NSObject class],@selector(doesNotRecognizeSelector:)),class_getInstanceMethod([NSObject class],@selector(rbxUnknown:)));
 dispatch_after_f(dispatch_time(0,15000000000LL),&_dispatch_main_q,0,dump);
 dispatch_after_f(dispatch_time(0,60000000000LL),&_dispatch_main_q,0,dump);
}
- (void)rbxUnknown:(SEL)s {
 fprintf(__stderrp,"UI MISSING METHOD %s %s\n",object_getClassName(self),sel_getName(s));[self rbxUnknown:s];
}
@end

@implementation NSWindow (MouseTrace)
+ (void)load {
 if(getenv("RBX_UI_TRACE"))method_exchangeImplementations(class_getInstanceMethod([NSWindow class],@selector(sendEvent:)),class_getInstanceMethod([NSWindow class],@selector(rbxMouseEvent:)));
}
- (void)rbxMouseEvent:(id)event {
 unsigned long type=[event type];
 if(type==1 || type==2){Point p=[event locationInWindow],m=[self mouseLocationOutsideOfEventStream];id hit=[[self _backgroundView] hitTest:p];Point v=[hit convertPoint:p fromView:0];fprintf(__stderrp,"UI MOUSE type=%lu button=%ld location=%.0f,%.0f polled=%.0f,%.0f view=%.0f,%.0f hit=%s responder=%s\n",type,[event buttonNumber],p.x,p.y,m.x,m.y,v.x,v.y,object_getClassName(hit),object_getClassName([self firstResponder]));}
 [self rbxMouseEvent:event];
}
@end

// Opt-in account-dialog construction test with a local, non-authenticated page.
extern Class objc_getClass(const char *);
@interface NSString:NSObject + (id)stringWithUTF8String:(const char *)s; @end
@interface NSObject (DialogProbe)
+ (id)alloc; - (id)initWithFrame:(Rect)r inFullscreen:(signed char)f withTitle:(id)t withDelegate:(id)d;
- (void)addSubview:(id)view; - (void)loadUrl:(id)url; - (void)removeFromSuperview;
@end
extern int strncmp(const char *,const char *,unsigned long);
static id probeDialog;
static void closeDialogProbe(void *unused) {
 [probeDialog removeFromSuperview];fprintf(__stderrp,"DIALOG PROBE closed without exception\n");
}
static void dialogProbe(void *unused) {
 const char *url=getenv("RBX_LOCAL_DIALOG_PROBE");if(!url || strncmp(url,"http://127.0.0.1:",17-1))return;
 NSArray *windows=[[NSApplication sharedApplication] windows];if(![windows count])return;
 id content=[[windows objectAtIndex:0] contentView];
 id view=[[objc_getClass("EmbeddedWebView") alloc] initWithFrame:[content bounds] inFullscreen:0 withTitle:@"Local embedded dialog test" withDelegate:0];
 [content addSubview:view];probeDialog=view;dispatch_after_f(dispatch_time(0,12000000000LL),&_dispatch_main_q,0,closeDialogProbe);[view loadUrl:[NSString stringWithUTF8String:url]];
 fprintf(__stderrp,"DIALOG PROBE constructed and load requested\n");
}
@interface RbxLocalDialogProbe:NSObject @end
@implementation RbxLocalDialogProbe
+ (void)load {if(getenv("RBX_LOCAL_DIALOG_PROBE"))dispatch_after_f(dispatch_time(0,8000000000LL),&_dispatch_main_q,0,dialogProbe);}
@end
