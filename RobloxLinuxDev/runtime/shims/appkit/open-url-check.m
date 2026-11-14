// A link handed to the launcher must reach the application's delegate as one open-URL event
// when Cocotron opens the launch documents (-[NSApplication openFiles], which finishLaunching
// calls between the two launch notifications), exactly once, and only if it is a client link.
// Run by check.sh with ROBLOX_MAC_OPEN_URL set per case; prints what the delegate received.
//
// The application object is allocated but never initialised: -init connects to a display,
// and a guest shell has none. -openFiles and the delegate need no display.
#import <Foundation/NSObject.h>
#import <Foundation/NSArray.h>
#import <Foundation/NSString.h>
#import <Foundation/NSURL.h>
#import <Foundation/NSNotification.h>
#import <Foundation/NSAutoreleasePool.h>
#include <stdio.h>

// Declared here: the SDK's umbrella headers do not compile against the port's sysroot.
@interface NSApplication : NSObject
- (void)setDelegate:(id)delegate;
- (BOOL)openFiles;
@end

static NSMutableArray *events;

@interface OpenURLCheckDelegate : NSObject
@end
@implementation OpenURLCheckDelegate
- (void)application:(NSApplication *)application openURLs:(NSArray *)urls {
    for (NSURL *url in urls) [events addObject:[url absoluteString]];
}
@end

int main(void) {
    NSAutoreleasePool *pool = [NSAutoreleasePool new];
    events = [NSMutableArray new];
    NSApplication *application = [NSApplication alloc];
    [application setDelegate:[OpenURLCheckDelegate new]];
    [events addObject:@"first"];
    [application openFiles];
    [events addObject:@"second"]; // a later call must not deliver the link again
    [application openFiles];
    printf("OPEN-URL %s\n", [[events componentsJoinedByString:@" "] UTF8String]);
    [pool release];
    return 0;
}
