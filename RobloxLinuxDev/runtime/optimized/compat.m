// Native x86 fixes absent from runtime/shims. Loaded by the parent launcher.
#include <Foundation/NSObject.h>
#include <Foundation/NSArray.h>
#include <Foundation/NSString.h>
#include <Foundation/NSError.h>
#include <Foundation/NSURL.h>
#include <Foundation/NSSet.h>
#include <objc/runtime.h>
#include <Block.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dlfcn.h>

typedef struct { uint32_t platform, version; } TrackABuildVersion;
typedef struct { unsigned major, minor, patch; } TrackASystemVersion;
bool _availability_version_check(uint32_t count, const TrackABuildVersion *versions);

@interface NSEvent : NSObject
- (unsigned long)type;
@end
@interface NSEvent (TrackALocalMonitors)
+ (id)addLocalMonitorForEventsMatchingMask:(unsigned long)mask handler:(id(^)(id))handler;
+ (void)removeMonitor:(id)token;
@end
@interface MTLDeviceInternal : NSObject
- (signed char)supportsFeatureSet:(unsigned long)feature;
@end
@interface MTLDeviceInternal (TrackAFeatureQueries)
- (signed char)supportsFamily:(unsigned long)family;
- (unsigned long)argumentBuffersSupport;
@end
@interface AVCaptureDevice : NSObject @end
@interface AVCaptureDevice (TrackACapturePermission)
+ (long)authorizationStatusForMediaType:(id)type;
+ (void)requestAccessForMediaType:(id)type completionHandler:(void(^)(BOOL))completion;
@end
@interface GCController : NSObject @end
@interface GCController (TrackANoControllers)
+ (NSArray *)controllers;
@end
@interface UNUserNotificationCenter : NSObject @end
@interface UNUserNotificationCenter (TrackANoNotifications)
+ (id)currentNotificationCenter;
- (void)setDelegate:(id)value;
- (id)delegate;
- (void)getNotificationSettingsWithCompletionHandler:(void(^)(id))completion;
- (void)requestAuthorizationWithOptions:(unsigned long)options completionHandler:(void(^)(_Bool,id))completion;
- (void)setNotificationCategories:(id)categories;
- (void)addNotificationRequest:(id)request withCompletionHandler:(void(^)(NSError *))completion;
@end
@interface UNMutableNotificationContent : NSObject @end
@interface UNMutableNotificationContent (TrackANoNotifications)
- (void)setTitle:(id)value;
- (void)setBody:(id)value;
- (void)setAttachments:(id)value;
- (void)setCategoryIdentifier:(id)value;
@end
@interface UNNotificationAttachment : NSObject @end
@interface UNNotificationAttachment (TrackANoNotifications)
+ (id)attachmentWithIdentifier:(id)identifier URL:(id)url options:(id)options error:(NSError **)error;
@end
@interface UNNotificationAction : NSObject @end
@interface UNNotificationAction (TrackANoNotifications)
+ (id)actionWithIdentifier:(id)identifier title:(id)title options:(unsigned long)options;
@end
@interface UNNotificationCategory : NSObject @end
@interface UNNotificationCategory (TrackANoNotifications)
+ (id)categoryWithIdentifier:(id)identifier actions:(id)actions intentIdentifiers:(id)intents options:(unsigned long)options;
@end
@interface UNNotificationRequest : NSObject @end
@interface UNNotificationRequest (TrackANoNotifications)
+ (id)requestWithIdentifier:(id)identifier content:(id)content trigger:(id)trigger;
@end
@interface UNNotificationSettings : NSObject @end
@interface UNNotificationSettings (TrackANoNotifications)
- (long)authorizationStatus;
- (long)alertSetting;
- (long)badgeSetting;
- (long)soundSetting;
@end

#ifndef TRACKA_COMPAT_CHECK

static pthread_once_t versionOnce = PTHREAD_ONCE_INIT;
static uint32_t systemVersion;
static bool versionKnown;
static void loadSystemVersion(void) {
    int (*get)(TrackASystemVersion *) = dlsym(RTLD_DEFAULT, "os_system_version_get_current_version");
    TrackASystemVersion v = {0};
    if (get && !get(&v) && v.major && v.major <= 65535 && v.minor <= 255 && v.patch <= 255) {
        systemVersion = v.major << 16 | v.minor << 8 | v.patch;
        versionKnown = true;
    }
}
// compiler-rt's @available ABI: packed major/minor/patch, one entry per
// platform. Use Darling's cached OS version, not the Linux kernel version.
// Platforms absent from the request use @available's wildcard clause.
bool _availability_version_check(uint32_t count, const TrackABuildVersion *versions) {
    if (count && !versions) return false;
    pthread_once(&versionOnce, loadSystemVersion);
    for (uint32_t i = 0; i < count; ++i)
        if (versions[i].platform == 1 /* macOS */ &&
            (!versionKnown || systemVersion < versions[i].version)) return false;
    return true;
}

@interface TrackALocalMonitor : NSObject {
@public
    unsigned long mask;
    id (^handler)(id);
}
@end
@implementation TrackALocalMonitor
- (void)dealloc { Block_release(handler); [super dealloc]; }
@end

static NSMutableArray *monitors;
static void (*originalSendEvent)(id,SEL,id);
static int pointerTrace(void) {static int enabled=-1;if(enabled<0)enabled=getenv("ROBLOX_MAC_POINTER_TRACE") && !strcmp(getenv("ROBLOX_MAC_POINTER_TRACE"),"1");return enabled;}
static void monitoredSendEvent(id self,SEL selector,id event) {
    NSArray *snapshot;
    @synchronized([TrackALocalMonitor class]) { snapshot=[monitors copy]; }
    @try {
        // Handlers can replace/swallow events or remove themselves. Never hold
        // the collection lock across user code; the snapshot owns each block.
        for(TrackALocalMonitor *monitor in snapshot) {
            if(!event)break;
            unsigned long type=[event type];
            if(type<64 && (monitor->mask&(1UL<<type))) {
                event=monitor->handler(event);
                if(pointerTrace() && (type==3 || type==4 || type==12))
                    fprintf(stderr,"POINTER monitor in=%lu out=%lu mask=%lu\n",type,event?[event type]:0,monitor->mask);
            }
        }
        if(event)originalSendEvent(self,selector,event);
    } @finally { [snapshot release]; }
}

@implementation NSEvent (TrackALocalMonitors)
+ (id)addLocalMonitorForEventsMatchingMask:(unsigned long)mask handler:(id(^)(id))handler {
    if(!handler)return nil;
    TrackALocalMonitor *token=[[TrackALocalMonitor alloc] init];
    token->mask=mask;
    token->handler=Block_copy(handler);
    @synchronized([TrackALocalMonitor class]) {
        if(!monitors) {
            Method method=class_getInstanceMethod(objc_getClass("NSApplication"),sel_registerName("sendEvent:"));
            if(!method) { [token release]; return nil; }
            monitors=[[NSMutableArray alloc] init];
            originalSendEvent=(void *)method_getImplementation(method);
            method_setImplementation(method,(IMP)monitoredSendEvent);
        }
        [monitors addObject:token];
    }
    return [token autorelease];
}
+ (void)removeMonitor:(id)token {
    // Release outside the lock: a captured object's dealloc can register/remove.
    [token retain];
    @try {
        @synchronized([TrackALocalMonitor class]) { [monitors removeObjectIdenticalTo:token]; }
    } @finally { [token release]; }
}
@end

@implementation MTLDeviceInternal (TrackAFeatureQueries)
- (signed char)supportsFamily:(unsigned long)family {
    // Reuse shared metal-gaps' baseline; don't advertise additional API tiers.
    return family==2001 && [self supportsFeatureSet:10000];
}
- (unsigned long)argumentBuffersSupport { return 0; } // Tier 1, never Tier 2/bindless.
@end

@implementation AVCaptureDevice (TrackACapturePermission)
// Audio capture uses CoreAudio's native Pulse backend, like playback. Linux
// enforces access when opening the stream; there is no macOS TCC prompt here.
// Camera capture remains unsupported. AVMediaTypeAudio's value is "soun".
+ (long)authorizationStatusForMediaType:(id)type { return [type isEqual:@"soun"] ? 3 : 2; }
+ (void)requestAccessForMediaType:(id)type completionHandler:(void(^)(BOOL))completion {
    // ponytail: immediate result; dispatch asynchronously if a host permission prompt is added.
    if(completion)completion([self authorizationStatusForMediaType:type]==3);
}
@end
@implementation GCController (TrackANoControllers)
+ (NSArray *)controllers { return [NSArray array]; }
@end

static id notificationCenter,notificationDelegate;
static void makeNotificationCenter(void) { notificationCenter=[[UNUserNotificationCenter alloc] init]; }
@implementation UNUserNotificationCenter (TrackANoNotifications)
+ (id)currentNotificationCenter {
    static pthread_once_t once=PTHREAD_ONCE_INIT;
    pthread_once(&once,makeNotificationCenter);
    return notificationCenter;
}
- (void)setDelegate:(id)value { objc_storeWeak(&notificationDelegate,value); }
- (id)delegate { return objc_loadWeak(&notificationDelegate); }
// ponytail: synchronous denial callbacks; dispatch asynchronously when a backend exists.
- (void)getNotificationSettingsWithCompletionHandler:(void(^)(id))completion {
    if(!completion)return;
    id settings=[[UNNotificationSettings alloc] init];
    @try { completion(settings); } @finally { [settings release]; }
}
- (void)requestAuthorizationWithOptions:(unsigned long)options completionHandler:(void(^)(_Bool,id))completion {
    if(completion)completion(0,nil);
}
- (void)setNotificationCategories:(id)categories {}
- (void)addNotificationRequest:(id)request withCompletionHandler:(void(^)(NSError *))completion {
    if(completion)completion([NSError errorWithDomain:@"UNErrorDomain" code:1 userInfo:nil]); // NotificationsNotAllowed
}
@end
// Roblox constructs notifications even after permission is denied. Supply real
// method signatures instead of Darling's v@: forwarding stub. ponytail: these
// objects are discarded at submission; store their fields when adding a backend.
@implementation UNMutableNotificationContent (TrackANoNotifications)
- (void)setTitle:(id)value {}
- (void)setBody:(id)value {}
- (void)setAttachments:(id)value {}
- (void)setCategoryIdentifier:(id)value {}
@end
@implementation UNNotificationAttachment (TrackANoNotifications)
+ (id)attachmentWithIdentifier:(id)identifier URL:(id)url options:(id)options error:(NSError **)error {
    // No notification attachment store exists. Do not move/delete the source file.
    if(error)*error=[NSError errorWithDomain:@"UNErrorDomain" code:101 userInfo:nil]; // AttachmentUnrecognizedType
    return nil;
}
@end
@implementation UNNotificationAction (TrackANoNotifications)
+ (id)actionWithIdentifier:(id)identifier title:(id)title options:(unsigned long)options {
    return [[[self alloc] init] autorelease];
}
@end
@implementation UNNotificationCategory (TrackANoNotifications)
+ (id)categoryWithIdentifier:(id)identifier actions:(id)actions intentIdentifiers:(id)intents options:(unsigned long)options {
    return [[[self alloc] init] autorelease];
}
@end
@implementation UNNotificationRequest (TrackANoNotifications)
+ (id)requestWithIdentifier:(id)identifier content:(id)content trigger:(id)trigger {
    return [[[self alloc] init] autorelease];
}
@end
@implementation UNNotificationSettings (TrackANoNotifications)
- (long)authorizationStatus { return 1; } // Denied.
- (long)alertSetting { return 0; } // Not supported.
- (long)badgeSetting { return 0; }
- (long)soundSetting { return 0; }
@end

#else
// Built separately and injected with the actual dylib; no display or real input.
#include <Foundation/NSAutoreleasePool.h>
#include <Foundation/NSUserDefaults.h>
#include <assert.h>
#include <string.h>

@interface TrackACheckEvent : NSObject { @public unsigned long eventType; }
- (unsigned long)type;
@end
@implementation TrackACheckEvent
- (unsigned long)type { return eventType; }
@end
static id delivered;
static unsigned deliveries;
static void recordEvent(id self,SEL selector,id event) { delivered=event; ++deliveries; }
static void sendEvent(id event) {
    SEL selector=sel_registerName("sendEvent:");
    ((void(*)(id,SEL,id))method_getImplementation(class_getInstanceMethod(objc_getClass("NSApplication"),selector)))(nil,selector,event);
}
static id addCopiedMonitor(int *calls) {
    int captured=7;
    return [NSEvent addLocalMonitorForEventsMatchingMask:1UL<<10 handler:^id(id event) {
        *calls+=captured; return event;
    }];
}
static signed char baseline;
static unsigned featureQueries;
static signed char featureSet(id self,SEL selector,unsigned long feature) {
    ++featureQueries; assert(feature==10000); return baseline;
}
static void checkHost(void) {
    TrackASystemVersion version = {0};
    int (*getVersion)(TrackASystemVersion *) = dlsym(RTLD_DEFAULT, "os_system_version_get_current_version");
    assert(getVersion && !getVersion(&version) && version.major);
    uint32_t current = version.major << 16 | version.minor << 8 | version.patch;
    TrackABuildVersion requested[] = {{2, UINT32_MAX}, {1, current}};
    printf("PASS native availability version %u.%u.%u\n", version.major, version.minor, version.patch);
    assert(_availability_version_check(0, NULL));
    assert(!_availability_version_check(1, NULL));
    assert(_availability_version_check(1, requested)); // iOS-only clause on macOS
    assert(_availability_version_check(2, requested));
    requested[1].version = current + 1;
    assert(!_availability_version_check(2, requested));
    requested[1].version = current - 1;
    assert(_availability_version_check(2, requested));
    Method dispatch=class_getInstanceMethod(objc_getClass("NSApplication"),sel_registerName("sendEvent:"));
    assert(dispatch);
    method_setImplementation(dispatch,(IMP)recordEvent);
    TrackACheckEvent *key=[TrackACheckEvent new],*replacement=[TrackACheckEvent new];
    key->eventType=10; replacement->eventType=11;
    int calls=0;
    NSAutoreleasePool *pool=[NSAutoreleasePool new];
    id copied=addCopiedMonitor(&calls);
    assert(copied); [pool drain]; // Registry must keep the token AND its copied block alive.
    sendEvent(key); assert(calls==7 && delivered==key && deliveries==1);
    sendEvent(replacement); assert(calls==7 && delivered==replacement);
    __block id token=nil;
    token=[NSEvent addLocalMonitorForEventsMatchingMask:1UL<<10 handler:^id(id event) {
        [NSEvent removeMonitor:token]; return replacement;
    }];
    __block unsigned swallowed=0;
    id swallow=[NSEvent addLocalMonitorForEventsMatchingMask:1UL<<11 handler:^id(id event) {
        ++swallowed; return nil;
    }];
    unsigned before=deliveries;
    sendEvent(key); assert(calls==14 && swallowed==1 && deliveries==before);
    sendEvent(key); assert(calls==21 && swallowed==1 && delivered==key && deliveries==before+1);
    [NSEvent removeMonitor:swallow]; [NSEvent removeMonitor:copied];
    [NSEvent removeMonitor:nil];
    assert(![NSEvent addLocalMonitorForEventsMatchingMask:~0UL handler:nil]);
    int *counter=&calls;
    id all=[NSEvent addLocalMonitorForEventsMatchingMask:~0UL handler:^id(id event) { ++*counter; return event; }];
    key->eventType=63; sendEvent(key); assert(calls==22);
    key->eventType=64; sendEvent(key); assert(calls==22 && delivered==key);
    [NSEvent removeMonitor:all]; [key release]; [replacement release];

    Class metal=objc_getClass("MTLDeviceInternal");
    Method feature=class_getInstanceMethod(metal,@selector(supportsFeatureSet:));
    assert(feature);
    // Allocate no Indium/Vulkan device; only these scalar query methods execute.
    id device=class_createInstance(metal,0);
    assert([device supportsFeatureSet:10000] && ![device supportsFeatureSet:10001]);
    IMP previous=method_setImplementation(feature,(IMP)featureSet);
    baseline=1; assert([device supportsFamily:2001]);
    baseline=0; assert(![device supportsFamily:2001]);
    assert(![device supportsFamily:2002] && ![device supportsFamily:1001] && ![device supportsFamily:~0UL]);
    assert(featureQueries==2 && [device argumentBuffersSupport]==0);
    method_setImplementation(feature,previous); object_dispose(device);
    assert([AVCaptureDevice authorizationStatusForMediaType:@"vide"]==2);
    assert([AVCaptureDevice authorizationStatusForMediaType:@"soun"]==3);
    assert([AVCaptureDevice authorizationStatusForMediaType:nil]==2);
    assert([AVCaptureDevice authorizationStatusForMediaType:@"unknown"]==2);
    __block unsigned captureCallbacks=0;
    [AVCaptureDevice requestAccessForMediaType:@"soun" completionHandler:^(BOOL granted) {
        ++captureCallbacks; assert(granted);
    }];
    [AVCaptureDevice requestAccessForMediaType:@"vide" completionHandler:^(BOOL granted) {
        ++captureCallbacks; assert(!granted);
    }];
    [AVCaptureDevice requestAccessForMediaType:nil completionHandler:^(BOOL granted) {
        ++captureCallbacks; assert(!granted);
    }];
    [AVCaptureDevice requestAccessForMediaType:@"soun" completionHandler:nil];
    assert(captureCallbacks==3);
    puts("PASS native microphone permission query/request; unsupported capture stays denied");
    assert([[GCController controllers] count]==0);
    UNUserNotificationCenter *center=[UNUserNotificationCenter currentNotificationCenter];
    assert(center && center==[UNUserNotificationCenter currentNotificationCenter]);
    id delegate=[NSObject new]; [center setDelegate:delegate];
    @autoreleasepool { assert([center delegate]==delegate); }
    [delegate release]; assert(![center delegate]);
    __block unsigned callbacks=0;
    [center getNotificationSettingsWithCompletionHandler:^(id settings) {
        ++callbacks; assert(settings && [settings authorizationStatus]==1);
        assert([settings alertSetting]==0 && [settings badgeSetting]==0 && [settings soundSetting]==0);
    }];
    [center requestAuthorizationWithOptions:~0UL completionHandler:^(_Bool granted,id error) {
        ++callbacks; assert(!granted && !error);
    }];
    [center getNotificationSettingsWithCompletionHandler:nil];
    [center requestAuthorizationWithOptions:0 completionHandler:nil];
    assert(callbacks==2);
    // Reproduce the client's complete post-denial construction/submission path,
    // including the attachment factory that crashed at 2026-09-09 13:06.
    UNMutableNotificationContent *content=[UNMutableNotificationContent new];
    [content setTitle:@"Roblox"]; [content setBody:@"Local compatibility check"];
    NSError *attachmentError=nil;
    assert(![UNNotificationAttachment attachmentWithIdentifier:@"image" URL:[NSURL fileURLWithPath:@"/no-such-notification.png"] options:nil error:&attachmentError]);
    assert(attachmentError && [[attachmentError domain] isEqual:@"UNErrorDomain"]);
    assert(![UNNotificationAttachment attachmentWithIdentifier:@"image" URL:nil options:nil error:NULL]);
    [content setAttachments:[NSArray array]];
    id action=[UNNotificationAction actionWithIdentifier:@"open" title:@"Open" options:1];
    assert(action);
    id category=[UNNotificationCategory categoryWithIdentifier:@"check" actions:[NSArray arrayWithObject:action] intentIdentifiers:[NSArray array] options:0];
    assert(category); [center setNotificationCategories:[NSSet setWithObject:category]];
    [content setCategoryIdentifier:@"check"];
    id request=[UNNotificationRequest requestWithIdentifier:@"check" content:content trigger:nil];
    assert(request);
    [center addNotificationRequest:request withCompletionHandler:^(NSError *error) {
        ++callbacks; assert(error && [error code]==1 && [[error domain] isEqual:@"UNErrorDomain"]);
    }];
    [center addNotificationRequest:request withCompletionHandler:nil];
    assert(callbacks==3); [content release];
    puts("PASS post-denial notification attachment/actions/category/request and error callback");
    puts("PASS OS availability boundaries, local monitors, conservative Metal queries, unsupported frameworks");
}
int main(int argc,char **argv) {
    @autoreleasepool {
        assert(argc==2);
        if(!strcmp(argv[1],"host")) { checkHost(); return 0; }
        // The x86 client uses ordinary setters + synchronize. Preserve the
        // working Foundation path; no replacement store or timer is necessary.
        NSUserDefaults *defaults=[[NSUserDefaults alloc] initWithSuiteName:@"org.tracka.compat.target"];
        NSUserDefaults *other=[[NSUserDefaults alloc] initWithSuiteName:@"org.tracka.compat.other"];
        if(!strcmp(argv[1],"write")) {
            [other setInteger:42 forKey:@"sentinel"]; assert([other synchronize]);
            [defaults setBool:YES forKey:@"enabled"];
            [defaults setObject:@"saved" forKey:@"text"]; assert([defaults synchronize]);
        } else if(!strcmp(argv[1],"read") || !strcmp(argv[1],"remove")) {
            assert([defaults boolForKey:@"enabled"] && [[defaults objectForKey:@"text"] isEqual:@"saved"]);
            if(!strcmp(argv[1],"remove")) {
                [defaults removeObjectForKey:@"enabled"]; [defaults removeObjectForKey:@"text"];
                assert([defaults synchronize]);
            }
        } else {
            assert(!strcmp(argv[1],"empty"));
            assert(![defaults objectForKey:@"enabled"] && ![defaults objectForKey:@"text"]);
        }
        assert([other integerForKey:@"sentinel"]==42);
        printf("PASS native settings %s\n",argv[1]);
        [defaults release]; [other release];
    }
    return 0;
}
#endif
