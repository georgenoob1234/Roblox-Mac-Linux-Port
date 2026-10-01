// WKWebView objects forward to the GTK host. No browser runs in the Darwin ABI process.
#import <Foundation/NSURLRequest.h>
#import <Foundation/NSError.h>
#import <Foundation/NSDate.h>
#import <Foundation/NSHTTPCookie.h>
#import <Foundation/NSNull.h>
static char stateKey;
static BOOL validUserAgent(NSString *agent) {
    const unsigned char *p=(const unsigned char *)[agent UTF8String];
    if(!p || !*p)return NO;
    for(;*p;p++)if(*p<32 || *p>126)return NO;
    return YES;
}
static NSMutableDictionary *stateFor(id object) {
    NSMutableDictionary *state=objc_getAssociatedObject(object,&stateKey);
    if(!state){state=[NSMutableDictionary dictionary];objc_setAssociatedObject(object,&stateKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}return state;
}
@interface WKWebView:NSObject @end
@interface WKWebViewConfiguration:NSObject @end
@interface WKPreferences:NSObject @end
@interface WKUserContentController:NSObject @end
@interface WKWebsiteDataStore:NSObject + (id)defaultDataStore; @end
@interface WKHTTPCookieStore:NSObject @end
@interface WKUserScript:NSObject @end
@interface NSObject (BrowserCallbacks)
- (id)configuration; - (id)userContentController; - (id)websiteDataStore; - (id)httpCookieStore;
- (void)webView:(id)view didStartProvisionalNavigation:(id)navigation;
- (void)webView:(id)view didCommitNavigation:(id)navigation;
- (void)webView:(id)view didFinishNavigation:(id)navigation;
- (void)webView:(id)view didFailProvisionalNavigation:(id)navigation withError:(id)error;
- (void)webView:(id)view decidePolicyForNavigationAction:(id)action decisionHandler:(void (^)(long))handler;
- (void)userContentController:(id)controller didReceiveScriptMessage:(id)message;
- (void)closeButtonAction:(id)sender;
- (id)title;
@end
static long request(NSMutableDictionary *message,void (^completion)(id,NSError *)) {
    long number=++nextRequest;
    if(completion){id block=[completion copy];[callbacks setObject:block forKey:@(number)];[block release];}
    [message setObject:@(number) forKey:@"request"];sendMessage(message);[message release];return number;
}
@implementation WKWebViewConfiguration (RbxConfiguration)
- (id)preferences {id p=stateFor(self)[@"preferences"];if(!p)stateFor(self)[@"preferences"]=p=[[[WKPreferences alloc] init] autorelease];return p;}
- (id)userContentController {id p=stateFor(self)[@"controller"];if(!p)stateFor(self)[@"controller"]=p=[[[WKUserContentController alloc] init] autorelease];return p;}
- (void)setUserContentController:(id)value {stateFor(self)[@"controller"]=value;}
- (id)websiteDataStore {return [WKWebsiteDataStore defaultDataStore];}
- (void)setApplicationNameForUserAgent:(id)value {stateFor(self)[@"agent"]=value;}
- (id)applicationNameForUserAgent {return stateFor(self)[@"agent"];}
@end
@implementation WKPreferences (RbxPreferences)
- (void)setValue:(id)value forKey:(NSString *)key {stateFor(self)[key]=value;}
- (id)valueForKey:(NSString *)key {return stateFor(self)[key];}
@end
@implementation WKWebsiteDataStore (RbxDataStore)
+ (id)defaultDataStore {static id store;if(!store)store=[self new];return store;}
- (id)httpCookieStore {static id store;if(!store)store=[WKHTTPCookieStore new];return store;}
@end
@implementation WKHTTPCookieStore (RbxCookies)
- (void)setCookie:(NSHTTPCookie *)cookie completionHandler:(void (^)(void))completion {
    NSMutableDictionary *c=[NSMutableDictionary dictionaryWithDictionary:@{@"name":[cookie name],@"value":[cookie value],@"domain":[cookie domain],@"path":[cookie path],@"secure":@([[[cookie properties] objectForKey:@"Secure"] boolValue]),@"httpOnly":@([[[cookie properties] objectForKey:@"HTTPOnly"] boolValue])}];
    if([cookie expiresDate])c[@"expires"]=@([[cookie expiresDate] timeIntervalSince1970]);
    request([@{@"op":@"cookie-set",@"cookie":c} mutableCopy],^(id value,NSError *error){if(completion)completion();});
}
- (void)getAllCookies:(void (^)(NSArray *))completion {
    request([@{@"op":@"cookies-get"} mutableCopy],^(id value,NSError *error){
        NSMutableArray *cookies=[NSMutableArray array];
        for(NSDictionary *c in value) {
            NSMutableDictionary *props=[NSMutableDictionary dictionaryWithDictionary:@{@"Name":c[@"name"],@"Value":c[@"value"],@"Domain":c[@"domain"],@"Path":c[@"path"],@"Version":@0}];
            if([c[@"secure"] boolValue])props[@"Secure"]=@YES;
            if([c[@"httpOnly"] boolValue])props[@"HTTPOnly"]=@YES;
            if(c[@"expires"])props[@"Expires"]=[NSDate dateWithTimeIntervalSince1970:[c[@"expires"] doubleValue]];
            NSHTTPCookie *cookie=[NSHTTPCookie cookieWithProperties:props];if(cookie)[cookies addObject:cookie];
        }
        if(completion)completion(cookies);
    });
}
@end
@implementation WKUserScript (RbxScript)
- (id)initWithSource:(NSString *)source injectionTime:(long)time forMainFrameOnly:(BOOL)main {
    self=[self init];stateFor(self)[@"script"]=source;stateFor(self)[@"atEnd"]=@(time==1);stateFor(self)[@"mainOnly"]=@(main);return self;
}
@end
@implementation WKUserContentController (RbxScripts)
- (void)addScriptMessageHandler:(id)handler name:(NSString *)name {
    NSMutableDictionary *handlers=stateFor(self)[@"handlers"];if(!handlers)stateFor(self)[@"handlers"]=handlers=[NSMutableDictionary dictionary];
    handlers[name]=handler;
}
- (void)removeScriptMessageHandlerForName:(NSString *)name {[stateFor(self)[@"handlers"] removeObjectForKey:name];}
- (void)addUserScript:(id)script {
    NSMutableArray *scripts=stateFor(self)[@"scripts"];if(!scripts)stateFor(self)[@"scripts"]=scripts=[NSMutableArray array];[scripts addObject:script];
}
@end
@interface RbxFrameInfo:NSObject { @public NSURLRequest *_request; }
@end
@implementation RbxFrameInfo
- (id)request {return _request;}
- (void)dealloc {[_request release];[super dealloc];}
@end
@interface RbxNavigation:NSObject { @public NSURLRequest *_request;long _type; }
@end
@implementation RbxNavigation
- (NSURLRequest *)request {return _request;}
- (long)navigationType {return _type;}
- (id)targetFrame {
    // New-window requests are handled by the GTK host; these actions target an
    // existing frame. nil tells Roblox to open an external desktop browser.
    RbxFrameInfo *frame=[[[RbxFrameInfo alloc] init] autorelease];frame->_request=[_request retain];return frame;
}
- (id)sourceFrame {return nil;}
- (void)dealloc {[_request release];[super dealloc];}
@end
@interface RbxScriptMessage:NSObject { @public id _body;NSString *_name;id _webView; }
@end
@implementation RbxScriptMessage
- (id)body {return _body;}- (id)name {return _name;}- (id)webView {return _webView;}
- (id)frameInfo {return nil;}
- (void)dealloc {[_body release];[_name release];[_webView release];[super dealloc];}
@end
@interface RbxWebView:NSView { @public long _id;id _configuration,_navigationDelegate,_UIDelegate;NSURL *_URL;NSString *_title;BOOL _loading,_back,_forward,_attached;id _navigation; }
- (id)initWithFrame:(NSRect)frame configuration:(id)configuration;
@end
@implementation WKWebView (RbxNativeView)
+ (id)allocWithZone:(NSZone *)zone {return (id)[RbxWebView allocWithZone:zone];}
@end
@implementation RbxWebView
- (id)initWithFrame:(NSRect)frame {return [self initWithFrame:frame configuration:[[[WKWebViewConfiguration alloc] init] autorelease]];}
- (id)initWithFrame:(NSRect)frame configuration:(id)configuration {
    self=[super initWithFrame:frame];if(self){_id=++nextView;_configuration=[configuration retain];views[@(_id)]=self;}return self;
}
- (id)configuration {return _configuration;}
- (void)setNavigationDelegate:(id)delegate {_navigationDelegate=delegate;}
- (id)navigationDelegate {return _navigationDelegate;}
- (void)setUIDelegate:(id)delegate {_UIDelegate=delegate;}
- (id)UIDelegate {return _UIDelegate;}
- (NSURL *)URL {return _URL;}- (NSString *)title {return _title;}
- (BOOL)isLoading {return _loading;}- (double)estimatedProgress {return _loading?0.1:1.0;}
- (BOOL)canGoBack {return _back;}- (BOOL)canGoForward {return _forward;}
- (NSString *)customUserAgent {return stateFor(self)[@"customAgent"];}
- (NSString *)userAgent {
    NSString *custom=[self customUserAgent];if(validUserAgent(custom))return custom;
    NSString *base=[NSString stringWithUTF8String:getenv("ROBLOX_MAC_WEBKIT_USER_AGENT")?:""];
    NSString *application=[_configuration applicationNameForUserAgent];
    return validUserAgent(application)?[NSString stringWithFormat:@"%@ %@",base,application]:base;
}
- (void)setCustomUserAgent:(NSString *)agent {
    if(agent)stateFor(self)[@"customAgent"]=[[agent copy] autorelease];else [stateFor(self) removeObjectForKey:@"customAgent"];
    sendMessage(@{@"op":@"user-agent",@"view":@(_id),@"agent":[self userAgent]});
}
- (void)setUserAgent:(NSString *)agent {[self setCustomUserAgent:agent];}
- (id)loadRequest:(NSURLRequest *)urlRequest {
    id controller=[_configuration userContentController];
    
    for(NSString *name in stateFor(controller)[@"handlers"])sendMessage(@{@"op":@"handler",@"view":@(_id),@"name":name});
    for(id script in stateFor(controller)[@"scripts"]) {
        NSMutableDictionary *m=[NSMutableDictionary dictionaryWithDictionary:stateFor(script)];m[@"op"]=@"script";m[@"view"]=@(_id);sendMessage(m);
    }
    [_navigation release];_navigation=[NSObject new];
    id title=[_navigationDelegate respondsToSelector:@selector(title)]?[_navigationDelegate title]:nil;
    sendMessage(@{@"op":@"load",@"view":@(_id),@"url":[[urlRequest URL] absoluteString],@"headers":[urlRequest allHTTPHeaderFields]?:@{},@"panelTitle":[title isKindOfClass:[NSString class]]?title:@"",@"delegate":@([_navigationDelegate respondsToSelector:@selector(webView:decidePolicyForNavigationAction:decisionHandler:)]),@"agent":[self userAgent]});return _navigation;
}
- (void)evaluateJavaScript:(NSString *)script completionHandler:(void (^)(id,NSError *))completion {
    request([@{@"op":@"eval",@"view":@(_id),@"script":script} mutableCopy],completion);
}
- (id)goBack {sendMessage(@{@"op":@"back",@"view":@(_id)});return _navigation;}
- (id)goForward {sendMessage(@{@"op":@"forward",@"view":@(_id)});return _navigation;}
- (id)reload {sendMessage(@{@"op":@"reload",@"view":@(_id)});return _navigation;}
- (void)stopLoading {sendMessage(@{@"op":@"stop",@"view":@(_id)});}
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    BOOL attached=[self window]!=nil;
    if(_attached && !attached)[self rbxCloseHostedView];
    _attached=attached;
}
- (void)rbxCloseHostedView {
    if(views[@(_id)]!=self)return;
    
    [self retain];sendMessage(@{@"op":@"close",@"view":@(_id)});
    _navigationDelegate=nil;_UIDelegate=nil;[views removeObjectForKey:@(_id)];[self release];
}
- (void)removeFromSuperview {if([self superview])[self rbxCloseHostedView];[super removeFromSuperview];}
- (void)dealloc {[_configuration release];[_URL release];[_title release];[_navigation release];[super dealloc];}
@end
static void receiveMessage(NSDictionary *message) {
    NSString *type=message[@"event"];
    if([type isEqual:@"launch-url"]) {
        if(deliverClientURL(message[@"url"]))sendMessage(@{@"op":@"return-to-game"});
        else fprintf(stderr,"roblox-mac: embedded launch link could not be delivered\n");
        return;
    }
    if([type isEqual:@"reply"]) {
        id key=message[@"request"];void (^block)(id,NSError *)=[[callbacks objectForKey:key] copy];[callbacks removeObjectForKey:key];
        if(block){block(message[@"value"],message[@"error"]?[NSError errorWithDomain:@"RobloxEmbeddedBrowser" code:1 userInfo:@{NSLocalizedDescriptionKey:message[@"error"]}]:nil);[block release];}return;
    }
    RbxWebView *view=views[message[@"view"]];if(!view)return;
    id delegate=view->_navigationDelegate;
    if([type isEqual:@"state"]) {
        [view->_URL release];view->_URL=[[NSURL URLWithString:message[@"url"]] retain];
        [view->_title release];view->_title=[message[@"title"] copy];
        view->_back=[message[@"back"] boolValue];view->_forward=[message[@"forward"] boolValue];view->_loading=[message[@"loading"] boolValue];
    } else if([type isEqual:@"load"]) {
        long stage=[message[@"stage"] longValue];
        if(stage==0 && [delegate respondsToSelector:@selector(webView:didStartProvisionalNavigation:)])[delegate webView:view didStartProvisionalNavigation:view->_navigation];
        if(stage==2 && [delegate respondsToSelector:@selector(webView:didCommitNavigation:)])[delegate webView:view didCommitNavigation:view->_navigation];
        if(stage==3 && [delegate respondsToSelector:@selector(webView:didFinishNavigation:)])[delegate webView:view didFinishNavigation:view->_navigation];
    } else if([type isEqual:@"navigation"]) {
        RbxNavigation *action=[[[RbxNavigation alloc] init] autorelease];action->_request=[[NSURLRequest requestWithURL:[NSURL URLWithString:message[@"url"]]] retain];action->_type=[message[@"type"] longValue];if(action->_type==5)action->_type=-1;
        void (^decide)(long)=^(long allow){sendMessage(@{@"op":@"policy",@"decision":message[@"decision"],@"allow":@(allow!=0)});};
        if([delegate respondsToSelector:@selector(webView:decidePolicyForNavigationAction:decisionHandler:)])[delegate webView:view decidePolicyForNavigationAction:action decisionHandler:decide];else decide(1);
    } else if([type isEqual:@"message"]) {
        id controller=[view->_configuration userContentController],handler=[[stateFor(controller)[@"handlers"][message[@"name"]] retain] autorelease];
        RbxScriptMessage *m=[[[RbxScriptMessage alloc] init] autorelease];m->_body=[message[@"body"] retain];m->_name=[message[@"name"] copy];m->_webView=[view retain];
        [handler userContentController:controller didReceiveScriptMessage:m];
    } else if([type isEqual:@"closed"]) {
        if([delegate respondsToSelector:@selector(closeButtonAction:)])[delegate closeButtonAction:nil];
    } else if([type isEqual:@"error"] && [delegate respondsToSelector:@selector(webView:didFailProvisionalNavigation:withError:)]) {
        [delegate webView:view didFailProvisionalNavigation:view->_navigation withError:[NSError errorWithDomain:@"RobloxEmbeddedBrowser" code:1 userInfo:@{NSLocalizedDescriptionKey:message[@"message"]}]];
    }
}
