#import <Foundation/NSObject.h>
#import <Foundation/NSArray.h>
#import <Foundation/NSDictionary.h>
#import <Foundation/NSData.h>
#import <Foundation/NSString.h>
#import <Foundation/NSGeometry.h>
#import <Foundation/NSThread.h>
#import <Foundation/NSTimer.h>
#import <Foundation/NSURL.h>
#import <Foundation/NSValue.h>
#import <Foundation/NSJSONSerialization.h>
#import <Foundation/NSAutoreleasePool.h>
#import <objc/runtime.h>
#import <dispatch/dispatch.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

@class NSWindow;
@interface NSView:NSObject
- (id)initWithFrame:(NSRect)frame; - (void)viewDidMoveToWindow; - (NSWindow *)window; - (void)removeFromSuperview;
- (id)superview; - (NSRect)bounds; - (void)setFrame:(NSRect)frame; - (void)setAutoresizingMask:(unsigned long)mask;
@end
@interface NSWindow:NSObject
- (NSRect)frame; - (BOOL)isVisible; - (void)setTitle:(NSString *)title;
@end
@interface NSPanel:NSWindow @end
@interface NSApplication:NSObject - (NSArray *)windows; - (id)delegate; @end
extern NSApplication *NSApp;
@interface NSWorkspace:NSObject @end
@interface NSImage:NSObject @end
@interface NSButton:NSView
- (void)setTitle:(NSString *)title; - (void)setImage:(NSImage *)image;
- (void)setTarget:(id)target; - (void)setAction:(SEL)action; - (void)setBordered:(BOOL)bordered;
@end
@interface NSWindow (NativeHost)
- (id)platformWindow; - (void)setMenu:(id)menu;
+ (BOOL)hasMainMenuForStyleMask:(unsigned long)style;
@end
@interface NSObject (NativeHandle) - (unsigned long)windowHandle; - (id)getView; - (void)rbxCloseHostedView; @end
static void nativeBrowserLayout(id self,SEL selector) {
    // The visible browser and its toolbar are laid out by GtkStack/GtkHeaderBar.
    // Keep the guest content view sized for callbacks, without Cocoa Auto Layout.
    id view=[self getView];[view setFrame:[self bounds]];[view setAutoresizingMask:18];
}
static void (*removeEmbeddedView)(id,SEL);
static void closeEmbeddedView(id self,SEL selector) {
    // Cocotron calls removeFromSuperview even during the first addSubview.
    // The client override tears down its script handlers, so only run it when attached.
    if(![self superview])return;
    removeEmbeddedView(self,selector);
}
static int connection=-1;
static NSMutableData *received;
static NSMutableArray *outgoing;
static NSWindow *gameWindow;
static NSMutableDictionary *views,*callbacks;
static long nextView,nextRequest;
static void receiveMessage(NSDictionary *message);
static void sendMessage(NSDictionary *message) {
    if(!getenv("ROBLOX_MAC_UI_SOCKET"))return;
    if(![NSThread isMainThread]){dispatch_async(dispatch_get_main_queue(),^{sendMessage(message);});return;}
    NSData *json=[NSJSONSerialization dataWithJSONObject:message options:0 error:NULL];
    if(!json || [json length]>1024*1024)return;
    NSMutableData *line=[json mutableCopy];[line appendBytes:"\n" length:1];
    [outgoing addObject:line];[line release];
}
@interface RbxBrowserBridge:NSObject + (void)tick:(id)timer; @end
@implementation RbxBrowserBridge
+ (void)load {
    if(!getenv("ROBLOX_MAC_UI_SOCKET"))return;
    dispatch_async(dispatch_get_main_queue(),^{
        received=[NSMutableData new];outgoing=[NSMutableArray new];views=[NSMutableDictionary new];callbacks=[NSMutableDictionary new];
        [NSTimer scheduledTimerWithTimeInterval:0.02 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    });
}
+ (void)tick:(id)timer {
    static BOOL adapted=NO;
    if(!adapted){Method method=class_getInstanceMethod(objc_getClass("EmbeddedWebView"),sel_registerName("setupConstraints"));if(method){method_setImplementation(method,(IMP)nativeBrowserLayout);
        Method remove=class_getInstanceMethod(objc_getClass("EmbeddedWebView"),@selector(removeFromSuperview));
        if(remove)removeEmbeddedView=(void *)method_setImplementation(remove,(IMP)closeEmbeddedView);adapted=YES;}}
    if(connection<0) {
        const char *path=getenv("ROBLOX_MAC_UI_SOCKET");struct sockaddr_un address={0};
        if(!path || strlen(path)>=sizeof(address.sun_path))return;
        address.sun_family=AF_UNIX;strcpy(address.sun_path,path);address.sun_len=sizeof(address);
        int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return;
        if(connect(fd,(struct sockaddr*)&address,sizeof(address))){close(fd);return;}
        int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);connection=fd;
    }
    if(!gameWindow) {
        for(NSWindow *w in [NSApp windows]) {
            if([w isVisible] && ![w isKindOfClass:[NSPanel class]] && [w frame].size.width>400 && [w frame].size.height>300) {
                gameWindow=[w retain];[w setMenu:nil];
                sendMessage(@{@"op":@"attach",@"window":@([[w platformWindow] windowHandle])});break;
            }
        }
    }
    while([outgoing count]) {
        NSMutableData *line=[outgoing objectAtIndex:0];ssize_t n=write(connection,[line bytes],[line length]);
        if(n<0 && (errno==EAGAIN || errno==EINTR))break;
        if(n<=0){close(connection);connection=-1;return;}
        [line replaceBytesInRange:NSMakeRange(0,n) withBytes:NULL length:0];
        if(![line length])[outgoing removeObjectAtIndex:0];
    }
    char buffer[8192];ssize_t n;
    while((n=read(connection,buffer,sizeof(buffer)))>0)[received appendBytes:buffer length:n];
    if(n==0 || [received length]>1024*1024){close(connection);connection=-1;[received setLength:0];return;}
    for(;;) {
        const char *bytes=[received bytes];const char *end=memchr(bytes,'\n',[received length]);if(!end)break;
        NSUInteger length=end-bytes;NSData *line=[NSData dataWithBytes:bytes length:length];
        [received replaceBytesInRange:NSMakeRange(0,length+1) withBytes:NULL length:0];
        id message=[NSJSONSerialization JSONObjectWithData:line options:0 error:NULL];
        if([message isKindOfClass:[NSDictionary class]])receiveMessage(message);
    }
}
@end

@implementation NSWindow (RbxGtkChrome)
+ (void)load {
    if(getenv("ROBLOX_MAC_UI_SOCKET"))method_exchangeImplementations(class_getClassMethod(self,@selector(hasMainMenuForStyleMask:)),class_getClassMethod(self,@selector(rbxHasMainMenuForStyleMask:)));
}
+ (BOOL)rbxHasMainMenuForStyleMask:(unsigned long)style {return NO;}
@end

@interface NSTextField:NSView
- (void)setStringValue:(NSString *)value; - (void)setEditable:(BOOL)value;
- (void)setSelectable:(BOOL)value; - (void)setBezeled:(BOOL)value;
- (void)setDrawsBackground:(BOOL)value; - (void)sizeToFit;
@end
@implementation NSTextField (RbxConvenienceLabels)
+ (id)labelWithString:(NSString *)string {
    NSTextField *label=[[[self alloc] initWithFrame:NSZeroRect] autorelease];
    [label setStringValue:string];[label setEditable:NO];[label setSelectable:NO];
    [label setBezeled:NO];[label setDrawsBackground:NO];[label sizeToFit];return label;
}
@end

#include "../../browser/client-url.h"
@interface NSObject (RbxApplicationURLs)
- (void)application:(id)application openURLs:(NSArray*)urls;
@end
static BOOL deliverClientURL(id value) {
    if(![value isKindOfClass:[NSString class]] || !rbx_client_url([value UTF8String]))return NO;
    NSURL *url=[NSURL URLWithString:value];if(!url)return NO;
    if(![NSThread isMainThread]){dispatch_async(dispatch_get_main_queue(),^{deliverClientURL(value);});return YES;}
    id delegate=[NSApp delegate];
    if(![delegate respondsToSelector:@selector(application:openURLs:)])return NO;
    [delegate application:NSApp openURLs:@[url]];return YES;
}
@interface MacWorkspace:NSWorkspace @end
@implementation MacWorkspace (RbxEmbeddedLinks)
- (BOOL)openURL:(NSURL *)url {
    if(deliverClientURL([url absoluteString]))return YES;
    NSString *scheme=[[url scheme] lowercaseString];
    if(!getenv("ROBLOX_MAC_UI_SOCKET") || !([scheme isEqual:@"https"] || [scheme isEqual:@"http"]) || ![[url host] length])return NO;
    sendMessage(@{@"op":@"load",@"view":@0,@"url":[url absoluteString]});return YES;
}
@end

@implementation NSButton (RbxConvenienceButtons)
+ (id)buttonWithImage:(NSImage *)image target:(id)target action:(SEL)action {
    NSButton *button=[[[self alloc] initWithFrame:NSMakeRect(0,0,28,28)] autorelease];
    [button setTitle:@""];[button setImage:image];[button setTarget:target];[button setAction:action];[button setBordered:NO];return button;
}
+ (id)buttonWithTitle:(NSString *)title target:(id)target action:(SEL)action {
    NSButton *button=[[[self alloc] initWithFrame:NSMakeRect(0,0,100,28)] autorelease];
    [button setTitle:title];[button setTarget:target];[button setAction:action];return button;
}
@end

#include "webview.inc"

@implementation NSButton (RbxBrowserButtonTint)
// The hosted browser toolbar is drawn by GTK, which applies its theme tint.
- (void)setContentTintColor:(id)color {if(color)stateFor(self)[@"contentTintColor"]=color;else [stateFor(self) removeObjectForKey:@"contentTintColor"];}
- (id)contentTintColor {return stateFor(self)[@"contentTintColor"];}
@end

@interface NSImage (RbxDrawingAPI)
- (id)initWithSize:(NSSize)size; - (void)setFlipped:(BOOL)flipped;
- (void)lockFocus; - (void)unlockFocus;
@end
@implementation NSImage (RbxDrawingHandler)
+ (id)imageWithSize:(NSSize)size flipped:(BOOL)flipped drawingHandler:(BOOL (^)(NSRect))draw {
    NSImage *image=[[[self alloc] initWithSize:size] autorelease];[image setFlipped:flipped];
    [image lockFocus];@try {draw(NSMakeRect(0,0,size.width,size.height));}@finally{[image unlockFocus];}return image;
}
@end
@interface NSAnimationContext:NSObject @end
@implementation NSAnimationContext (RbxAnimationGroups)
+ (void)runAnimationGroup:(void (^)(id))changes completionHandler:(void (^)(void))completion {
    // Darling's animator applies properties immediately; still execute the completion.
    id context=[[[self alloc] init] autorelease];if(changes)changes(context);if(completion)completion();
}
- (void)setDuration:(double)duration {stateFor(self)[@"duration"]=@(duration);}
- (double)duration {return [stateFor(self)[@"duration"] doubleValue];}
- (void)setTimingFunction:(id)function {stateFor(self)[@"timing"]=function;}
@end

#import <CoreFoundation/CFURL.h>
#import <Foundation/NSFileManager.h>
@implementation NSURL (RbxRelativeFileURL)
+ (id)fileURLWithPath:(NSString *)path isDirectory:(BOOL)directory relativeToURL:(NSURL *)base {
    return [(id)CFURLCreateWithFileSystemPathRelativeToBase(NULL,(CFStringRef)path,kCFURLPOSIXPathStyle,directory,(CFURLRef)base) autorelease];
}
+ (id)fileURLWithPath:(NSString *)path relativeToURL:(NSURL *)base {
    NSURL *url=[self fileURLWithPath:path isDirectory:NO relativeToURL:base];
    BOOL directory=NO;[[NSFileManager defaultManager] fileExistsAtPath:[url path] isDirectory:&directory];
    return directory?[self fileURLWithPath:path isDirectory:YES relativeToURL:base]:url;
}
@end
