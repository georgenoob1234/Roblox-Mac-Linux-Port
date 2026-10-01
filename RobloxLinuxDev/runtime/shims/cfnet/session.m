// Completion-handler data tasks over Darling's existing CFURLConnection transport.
// .738 uses sharedSession/dataTaskWithRequest:completionHandler:/resume. Do not
// route through __NSCFURLSession: its configuration and task classes are empty.
#import <Foundation/NSURLSession.h>
#import <Foundation/NSURLConnection.h>
#import <Foundation/NSURLResponse.h>
#import <Foundation/NSURLCache.h>
#import <Foundation/NSURL.h>
#import <Foundation/NSURLError.h>
#import <Foundation/NSException.h>
#import <Foundation/NSOperation.h>
#import <Foundation/NSRunLoop.h>
#import <Foundation/NSData.h>
#import <Foundation/NSArray.h>
#import <Foundation/NSDictionary.h>
#import <Foundation/NSError.h>
#import <Foundation/NSString.h>
#import <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>

@interface RbxSessionConfiguration:NSURLSessionConfiguration @end
@implementation RbxSessionConfiguration
- (void)dealloc {
    self.connectionProxyDictionary=nil;self.HTTPAdditionalHeaders=nil;
    self.HTTPCookieStorage=nil;self.URLCredentialStorage=nil;self.URLCache=nil;self.protocolClasses=nil;
    [super dealloc];
}
@end

@implementation NSURLSessionConfiguration (RbxDataConfiguration)
+ (NSURLSessionConfiguration *)defaultSessionConfiguration {
    NSURLSessionConfiguration *c=[[[RbxSessionConfiguration alloc] init] autorelease];
    c.timeoutIntervalForRequest=60; c.timeoutIntervalForResource=604800;
    c.allowsCellularAccess=YES; c.HTTPShouldSetCookies=YES;
    c.HTTPCookieAcceptPolicy=NSHTTPCookieAcceptPolicyOnlyFromMainDocumentDomain;
    c.HTTPMaximumConnectionsPerHost=6;
    c.HTTPCookieStorage=[NSHTTPCookieStorage sharedHTTPCookieStorage];
    // NSURLConnection retains its own native authentication handling. Darling's
    // NSURLCredentialStorage factory is absent; no substitute credential store.
    c.URLCache=[NSURLCache sharedURLCache];
    return c;
}
- (id)copyWithZone:(NSZone *)zone {
    NSURLSessionConfiguration *c=[[[self class] allocWithZone:zone] init];
#define COPY(p) c.p=self.p
    COPY(requestCachePolicy); COPY(timeoutIntervalForRequest); COPY(timeoutIntervalForResource);
    COPY(networkServiceType); COPY(allowsCellularAccess); COPY(discretionary);
    COPY(sessionSendsLaunchEvents); COPY(connectionProxyDictionary);
    COPY(TLSMinimumSupportedProtocol); COPY(TLSMaximumSupportedProtocol);
    COPY(HTTPShouldUsePipelining); COPY(HTTPShouldSetCookies); COPY(HTTPCookieAcceptPolicy);
    COPY(HTTPAdditionalHeaders); COPY(HTTPMaximumConnectionsPerHost); COPY(HTTPCookieStorage);
    COPY(URLCredentialStorage); COPY(URLCache); COPY(protocolClasses);
#undef COPY
    return c;
}
@end

@class RbxSessionDataTask;
@interface RbxDataSession:NSURLSession {
    NSURLSessionConfiguration *_config;
    NSOperationQueue *_queue;
    NSMutableArray *_tasks;
    NSUInteger _nextID;
    BOOL _invalidated;
    NSString *_description;
}
- (id)initWithConfiguration:(NSURLSessionConfiguration *)config queue:(NSOperationQueue *)queue;
- (void)completed:(RbxSessionDataTask *)task;
@end
@interface RbxSessionDataTask:NSURLSessionDataTask <NSURLConnectionDataDelegate> {
    RbxDataSession *_session;
    NSURLRequest *_request,*_current;
    NSURLConnection *_connection;
    NSMutableData *_data;
    NSURLResponse *_response;
    NSError *_error;
    void (^_completion)(NSData *,NSURLResponse *,NSError *);
    NSURLSessionTaskState _state;
    NSUInteger _identifier;
    NSString *_description;
    dispatch_source_t _timer;
    double _lastActivity,_started,_resourceTimeout;
}
- (id)initWithSession:(RbxDataSession *)session request:(NSURLRequest *)request
          identifier:(NSUInteger)identifier resourceTimeout:(double)resourceTimeout completion:(void (^)(NSData *,NSURLResponse *,NSError *))completion;
- (void)finish:(NSError *)error;
- (void)armTimeout;
@end

@implementation NSURLSession (RbxDataSessions)
// The original +initialize mutates NSURLSession's superclass to an unrelated
// class with larger instance storage. Our concrete subclass owns its own state.
+ (void)initialize {}
+ (NSURLSession *)sharedSession {
    static NSURLSession *session;static dispatch_once_t once;
    dispatch_once(&once,^{session=[[RbxDataSession alloc]
        initWithConfiguration:[NSURLSessionConfiguration defaultSessionConfiguration] queue:nil];});
    return session;
}
+ (NSURLSession *)sessionWithConfiguration:(NSURLSessionConfiguration *)config {
    return [self sessionWithConfiguration:config delegate:nil delegateQueue:nil];
}
+ (NSURLSession *)sessionWithConfiguration:(NSURLSessionConfiguration *)config
                                  delegate:(id<NSURLSessionDelegate>)delegate delegateQueue:(NSOperationQueue *)queue {
    if(delegate)[NSException raise:NSInvalidArgumentException format:@"Delegate-based URL sessions are not supported by the data-task bridge"];
    return [[[RbxDataSession alloc] initWithConfiguration:config queue:queue] autorelease];
}
@end

@implementation RbxDataSession
- (id)initWithConfiguration:(NSURLSessionConfiguration *)config queue:(NSOperationQueue *)queue {
    if(!config){[self release];[NSException raise:NSInvalidArgumentException format:@"URL session configuration is required"];return nil;}
    if([config.connectionProxyDictionary count] || [config.protocolClasses count] ||
       config.TLSMinimumSupportedProtocol || config.TLSMaximumSupportedProtocol || config.URLCredentialStorage ||
       config.HTTPCookieStorage!=[NSHTTPCookieStorage sharedHTTPCookieStorage] || config.URLCache!=[NSURLCache sharedURLCache] ||
       config.HTTPMaximumConnectionsPerHost!=6) {
        [self release];[NSException raise:NSInvalidArgumentException format:@"Custom transport configuration is not supported by the data-task bridge"];return nil;
    }
    if(!isfinite(config.timeoutIntervalForRequest) || config.timeoutIntervalForRequest<=0 ||
       !isfinite(config.timeoutIntervalForResource) || config.timeoutIntervalForResource<=0){
        [self release];[NSException raise:NSInvalidArgumentException format:@"URL session timeouts must be finite and positive"];return nil;
    }
    self=[super init];if(self){
        _config=[config copy];_tasks=[NSMutableArray new];
        _queue=queue?[queue retain]:[NSOperationQueue new];
        if(!queue)[_queue setMaxConcurrentOperationCount:1];
    }return self;
}
- (NSURLSessionConfiguration *)configuration {return [[_config copy] autorelease];}
- (NSOperationQueue *)delegateQueue {return _queue;}
- (id)delegate {return nil;}
- (NSString *)sessionDescription {@synchronized(self){return [[_description retain] autorelease];}}
- (void)setSessionDescription:(NSString *)value {@synchronized(self){id old=_description;_description=[value copy];[old release];}}
- (NSURLSessionDataTask *)dataTaskWithRequest:(NSURLRequest *)request completionHandler:(void (^)(NSData *,NSURLResponse *,NSError *))completion {
    if(!request)[NSException raise:NSInvalidArgumentException format:@"URL request is required"];
    NSMutableURLRequest *r=[[request mutableCopy] autorelease];
    // Preserve explicit request values. Session defaults supply unchanged defaults.
    if([r timeoutInterval]==60)[r setTimeoutInterval:_config.timeoutIntervalForRequest];
    if([r cachePolicy]==NSURLRequestUseProtocolCachePolicy)[r setCachePolicy:_config.requestCachePolicy];
    if(!_config.HTTPShouldSetCookies)[r setHTTPShouldHandleCookies:NO];
    if(_config.HTTPShouldUsePipelining)[r setHTTPShouldUsePipelining:YES];
    for(NSString *key in _config.HTTPAdditionalHeaders)
        if(![r valueForHTTPHeaderField:key])[r setValue:[_config.HTTPAdditionalHeaders objectForKey:key] forHTTPHeaderField:key];
    @synchronized(self){
        if(_invalidated)[NSException raise:NSGenericException format:@"The URL session has been invalidated"];
        RbxSessionDataTask *task=[[[RbxSessionDataTask alloc] initWithSession:self request:r
            identifier:++_nextID resourceTimeout:_config.timeoutIntervalForResource completion:completion] autorelease];
        [_tasks addObject:task];return task;
    }
}
- (NSURLSessionDataTask *)dataTaskWithURL:(NSURL *)url completionHandler:(void (^)(NSData *,NSURLResponse *,NSError *))completion {
    return [self dataTaskWithRequest:[NSURLRequest requestWithURL:url] completionHandler:completion];
}
- (NSURLSessionDataTask *)dataTaskWithRequest:(NSURLRequest *)request {
    return [self dataTaskWithRequest:request completionHandler:nil];
}
- (NSURLSessionDataTask *)dataTaskWithURL:(NSURL *)url {
    return [self dataTaskWithURL:url completionHandler:nil];
}
- (void)completed:(RbxSessionDataTask *)task {@synchronized(self){[_tasks removeObjectIdenticalTo:task];}}
- (void)invalidateAndCancel {
    if(self==[NSURLSession sharedSession])return;
    NSArray *tasks;@synchronized(self){_invalidated=YES;tasks=[_tasks copy];}
    for(RbxSessionDataTask *task in tasks)[task cancel];[tasks release];
}
- (void)finishTasksAndInvalidate {
    if(self==[NSURLSession sharedSession])return;
    @synchronized(self){_invalidated=YES;}
}
- (void)getTasksWithCompletionHandler:(void (^)(NSArray *,NSArray *,NSArray *))completion {
    NSArray *tasks;@synchronized(self){tasks=[_tasks copy];}
    [_queue addOperationWithBlock:^{completion(tasks,@[],@[]);}];[tasks release];
}
- (void)dealloc {[_config release];[_queue release];[_tasks release];[_description release];[super dealloc];}
@end

@implementation RbxSessionDataTask
static double monotonicSeconds(void){
    static mach_timebase_info_data_t scale;static dispatch_once_t once;
    dispatch_once(&once,^{mach_timebase_info(&scale);});
    if(!scale.numer || !scale.denom)[NSException raise:NSInternalInconsistencyException format:@"Mach timebase unavailable"];
    return mach_absolute_time()*(double)scale.numer/scale.denom/1e9;
}
- (id)initWithSession:(RbxDataSession *)session request:(NSURLRequest *)request
          identifier:(NSUInteger)identifier resourceTimeout:(double)resourceTimeout completion:(void (^)(NSData *,NSURLResponse *,NSError *))completion {
    self=[super init];if(self){
        _session=[session retain];_request=[request copy];_current=[request copy];
        _identifier=identifier;_completion=[completion copy];_state=NSURLSessionTaskStateSuspended;
        _data=[NSMutableData new];
        _resourceTimeout=resourceTimeout;
    }return self;
}
- (NSUInteger)taskIdentifier {return _identifier;}
- (NSURLRequest *)originalRequest {return _request;}
- (NSURLRequest *)currentRequest {@synchronized(self){return [[_current retain] autorelease];}}
- (NSURLSessionTaskState)state {@synchronized(self){return _state;}}
- (NSURLResponse *)response {@synchronized(self){return [[_response retain] autorelease];}}
- (NSError *)error {@synchronized(self){return [[_error retain] autorelease];}}
- (int64_t)countOfBytesReceived {@synchronized(self){return [_data length];}}
- (int64_t)countOfBytesExpectedToReceive {@synchronized(self){return _response?[_response expectedContentLength]:NSURLSessionTransferSizeUnknown;}}
- (NSString *)taskDescription {@synchronized(self){return [[_description retain] autorelease];}}
- (void)setTaskDescription:(NSString *)value {@synchronized(self){id old=_description;_description=[value copy];[old release];}}
- (id)copyWithZone:(NSZone *)zone {(void)zone;return [self retain];}
- (void)armTimeout {
    if(!_timer)return;
    double remaining=fmin(_lastActivity+[_request timeoutInterval],_started+_resourceTimeout)-monotonicSeconds();
    dispatch_source_set_timer(_timer,dispatch_time(DISPATCH_TIME_NOW,(int64_t)(fmax(.001,fmin(3600,remaining))*NSEC_PER_SEC)),DISPATCH_TIME_FOREVER,1000000);
}
- (void)resume {
    @synchronized(self){if(_state!=NSURLSessionTaskStateSuspended)return;_state=NSURLSessionTaskStateRunning;}
    dispatch_async(dispatch_get_main_queue(),^{
        @synchronized(self){
            if(_state!=NSURLSessionTaskStateRunning)return;
            _lastActivity=_started=monotonicSeconds();
            _timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
            dispatch_source_set_event_handler(_timer,^{
                @synchronized(self){
                    if(_state!=NSURLSessionTaskStateRunning)return;
                    double now=monotonicSeconds();
                    if(now<_lastActivity+[_request timeoutInterval] && now<_started+_resourceTimeout){[self armTimeout];return;}
                    [_connection cancel];[self finish:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorTimedOut userInfo:nil]];
                }
            });
            [self armTimeout];dispatch_resume(_timer);
            _connection=[[NSURLConnection alloc] initWithRequest:_request delegate:self startImmediately:NO];
            if(_connection){
                [_connection scheduleInRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
                [_connection start];
            }else [self finish:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorUnknown userInfo:nil]];
        }
    });
}
- (void)cancel {
    @synchronized(self){
        if(_state==NSURLSessionTaskStateCompleted || _state==NSURLSessionTaskStateCanceling)return;
        _state=NSURLSessionTaskStateCanceling;
    }
    dispatch_async(dispatch_get_main_queue(),^{
        [_connection cancel];
        [self finish:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil]];
    });
}
- (void)finish:(NSError *)error {
    // A cancellation racing transport completion wins if cancel was accepted first.
    @synchronized(self){
        if(_state==NSURLSessionTaskStateCompleted)return;
        if(_state==NSURLSessionTaskStateCanceling)error=[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorCancelled userInfo:nil];
        _error=[error retain];_state=NSURLSessionTaskStateCompleted;
        if(_timer){dispatch_source_cancel(_timer);dispatch_release(_timer);_timer=NULL;}
        NSData *data=error?nil:[[_data copy] autorelease];
        [[_session delegateQueue] addOperationWithBlock:^{
            void (^completion)(NSData *,NSURLResponse *,NSError *)=_completion;_completion=nil;
            @try {if(completion)completion(data,_response,_error);}
            @finally {[completion release];[_session completed:self];}
        }];
    }
}
- (NSURLRequest *)connection:(NSURLConnection *)connection willSendRequest:(NSURLRequest *)request redirectResponse:(NSURLResponse *)response {
    (void)connection;
    @synchronized(self){
        if(_state!=NSURLSessionTaskStateRunning)return nil;
        NSMutableURLRequest *next=[[request mutableCopy] autorelease];
        NSURL *from=[_current URL],*to=[next URL];
        BOOL sameOrigin=[[[from scheme] lowercaseString] isEqual:[[to scheme] lowercaseString]] &&
            [[[from host] lowercaseString] isEqual:[[to host] lowercaseString]] &&
            ([from port]==[to port] || [[from port] isEqual:[to port]]);
        // CFURLConnection recreates redirect requests without headers. Restore
        // them only on the same origin; never reattach credentials cross-origin.
        if([(NSHTTPURLResponse *)response statusCode]==307){
            [next setHTTPMethod:[_current HTTPMethod]];[next setHTTPBody:[_current HTTPBody]];
        }
        if([[_current HTTPMethod] isEqual:@"HEAD"])[next setHTTPMethod:@"HEAD"];
        BOOL changedMethod=![[_current HTTPMethod] isEqual:[next HTTPMethod]];
        if(response && sameOrigin)for(NSString *key in [_current allHTTPHeaderFields]) {
            NSString *lower=[key lowercaseString];
            if(changedMethod && ([lower isEqual:@"content-length"] || [lower isEqual:@"content-type"] || [lower isEqual:@"transfer-encoding"]))continue;
            if(![next valueForHTTPHeaderField:key])[next setValue:[_current valueForHTTPHeaderField:key] forHTTPHeaderField:key];
        }
        [_current release];_current=[next copy];_lastActivity=monotonicSeconds();[self armTimeout];return next;
    }
}
- (void)connection:(NSURLConnection *)connection didReceiveResponse:(NSURLResponse *)response {
    (void)connection;@synchronized(self){if(_state!=NSURLSessionTaskStateRunning)return;
        [_response release];_response=[response retain];[_data setLength:0];_lastActivity=monotonicSeconds();[self armTimeout];}
}
- (void)connection:(NSURLConnection *)connection didReceiveData:(NSData *)data {
    (void)connection;@synchronized(self){if(_state==NSURLSessionTaskStateRunning){[_data appendData:data];_lastActivity=monotonicSeconds();[self armTimeout];}}
}
- (void)connection:(NSURLConnection *)connection didFailWithError:(NSError *)error {(void)connection;[self finish:error];}
- (void)connectionDidFinishLoading:(NSURLConnection *)connection {(void)connection;[self finish:nil];}
- (void)dealloc {
    [_session release];[_request release];[_current release];[_connection release];
    [_data release];[_response release];[_error release];[_completion release];[_description release];[super dealloc];
}
@end
