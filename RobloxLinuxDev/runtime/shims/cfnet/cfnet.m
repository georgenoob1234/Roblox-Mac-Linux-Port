// The existing exception workaround remains; cookie parsing/storage uses native libsoup.
#import <Foundation/NSObject.h>
#import <Foundation/NSException.h>
#import <Foundation/NSArray.h>
#import <Foundation/NSDictionary.h>
#import <Foundation/NSString.h>
#import <Foundation/NSData.h>
#import <Foundation/NSDate.h>
#import <Foundation/NSValue.h>
#import <Foundation/NSURL.h>
#import <Foundation/NSJSONSerialization.h>
#import <Foundation/NSHTTPCookie.h>
#import <Foundation/NSHTTPCookieStorage.h>
#import <dispatch/dispatch.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

@implementation NSException (RobloxMac)
- (void)_installStackTraceKeyIfNeeded { }
@end
struct ElfCalls {void *(*dlopen)(const char *);int (*dlclose)(void *);void *(*dlsym)(void *,const char *);};
extern struct ElfCalls *_elfcalls;
static char *(*nativeCall)(const char *);
static void (*nativeFree)(void *);
static void loadCookies(void *unused) {
    const char *path=getenv("ROBLOX_MAC_COOKIE_HELPER");
    if(!_elfcalls || !path)return;
    void *library=_elfcalls->dlopen(path);if(!library)return;
    nativeCall=_elfcalls->dlsym(library,"rbx_cookies_call");nativeFree=_elfcalls->dlsym(library,"rbx_cookies_free");
}
static NSArray *call(NSMutableDictionary *request) {
    static dispatch_once_t once;dispatch_once_f(&once,NULL,loadCookies);
    if(!nativeCall || !nativeFree){fprintf(stderr,"roblox-mac: native cookie helper unavailable\n");return @[];}
    const char *data=getenv("ROBLOX_MAC_DATA");
    if(data)request[@"directory"]=[[NSString stringWithUTF8String:data] stringByAppendingString:@"/cookies"];
    NSData *json=[NSJSONSerialization dataWithJSONObject:request options:0 error:NULL];
    NSString *text=[[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] autorelease];
    char *response=nativeCall([text UTF8String]);if(!response)return @[];
    NSData *bytes=[NSData dataWithBytes:response length:strlen(response)];nativeFree(response);
    NSDictionary *reply=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
    if(reply[@"error"])fprintf(stderr,"roblox-mac: cookie storage operation failed\n");
    return reply[@"cookies"]?:@[];
}
static NSArray *decoded(NSArray *values) {
    NSMutableArray *cookies=[NSMutableArray array];
    @synchronized([NSHTTPCookie class]) {
        for(NSDictionary *value in values) {
            NSMutableDictionary *p=[[value mutableCopy] autorelease];
            if(p[@"Expires"])p[@"Expires"]=[NSDate dateWithTimeIntervalSince1970:[p[@"Expires"] doubleValue]];
            NSHTTPCookie *cookie=[NSHTTPCookie cookieWithProperties:p];if(cookie)[cookies addObject:cookie];
        }
    }
    return cookies;
}
static NSArray *encoded(NSArray *cookies) {
    NSMutableArray *values=[NSMutableArray array];
    for(NSHTTPCookie *c in cookies) {
        NSDictionary *p=[c properties];
        if(![c name] || ![c value] || ![c domain] || ![c path])continue;
        NSMutableDictionary *v=[NSMutableDictionary dictionaryWithDictionary:@{@"Name":[c name],@"Value":[c value],@"Domain":[c domain],@"Path":[c path],@"Secure":@([p[@"Secure"] boolValue]),@"HTTPOnly":@([p[@"HTTPOnly"] boolValue]),@"Discard":@([p[@"Discard"] boolValue])}];
        if([c expiresDate])v[@"Expires"]=@([[c expiresDate] timeIntervalSince1970]);
        if(p[@"Max-Age"])v[@"Max-Age"]=@([p[@"Max-Age"] longLongValue]);
        if(p[@"SameSite"])v[@"SameSite"]=@([p[@"SameSite"] intValue]);
        [values addObject:v];
    }
    return values;
}
@implementation NSHTTPCookie (RobloxMac)
+ (NSArray *)cookiesWithResponseHeaderFields:(NSDictionary *)fields forURL:(NSURL *)url {
    if(!fields || !url)return @[];
    return decoded(call([NSMutableDictionary dictionaryWithDictionary:@{@"op":@"parse",@"headers":fields,@"url":[url absoluteString]}]));
}
// Darling calls CFBooleanGetValue even when these optional properties are absent.
- (BOOL)isSecure {return [[[self properties] objectForKey:@"Secure"] boolValue];}
- (BOOL)isHTTPOnly {return [[[self properties] objectForKey:@"HTTPOnly"] boolValue];}
- (BOOL)isSessionOnly {return ![self expiresDate] || [[[self properties] objectForKey:@"Discard"] boolValue];}
@end
@interface RbxPersistentCookieStorage:NSHTTPCookieStorage @end
@implementation NSHTTPCookieStorage (RobloxMac)
+ (NSHTTPCookieStorage *)sharedHTTPCookieStorage {
    static id store;static dispatch_once_t once;
    dispatch_once(&once,^{store=[RbxPersistentCookieStorage new];});return store;
}
@end
@implementation RbxPersistentCookieStorage
- (NSArray *)cookies {return decoded(call([NSMutableDictionary dictionaryWithObject:@"get" forKey:@"op"]));}
- (NSArray *)cookiesForURL:(NSURL *)url {
    if(!url)return @[];
    return decoded(call([NSMutableDictionary dictionaryWithDictionary:@{@"op":@"get",@"url":[url absoluteString]}]));
}
- (void)setCookies:(NSArray *)cookies forURL:(NSURL *)url mainDocumentURL:(NSURL *)main {
    NSMutableDictionary *r=[NSMutableDictionary dictionaryWithDictionary:@{@"op":@"set",@"cookies":encoded(cookies),@"policy":@([self cookieAcceptPolicy])}];
    if(url)r[@"url"]=[url absoluteString];if(main)r[@"first"]=[main absoluteString];call(r);
}
- (void)setCookie:(NSHTTPCookie *)cookie {if(cookie)[self setCookies:@[cookie] forURL:nil mainDocumentURL:nil];}
- (void)deleteCookie:(NSHTTPCookie *)cookie {
    if(cookie)call([NSMutableDictionary dictionaryWithDictionary:@{@"op":@"delete",@"cookies":encoded(@[cookie])}]);
}
- (NSArray *)sortedCookiesUsingDescriptors:(NSArray *)order {return [[self cookies] sortedArrayUsingDescriptors:order];}
@end
