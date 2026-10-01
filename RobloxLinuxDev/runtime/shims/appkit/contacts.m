#import <Foundation/NSObject.h>
#import <Foundation/NSError.h>
#import <dispatch/dispatch.h>

@interface CNContactStore:NSObject @end
@implementation CNContactStore (RbxContacts)
// There is no host address-book adapter. Restricted accurately reports that
// this runtime cannot grant access; it must never report a fabricated grant.
+ (long)authorizationStatusForEntityType:(long)type {return 1;}
- (void)requestAccessForEntityType:(long)type completionHandler:(void (^)(BOOL,NSError *))completion {
    if(completion)dispatch_async(dispatch_get_main_queue(),^{
        completion(NO,[NSError errorWithDomain:@"CNErrorDomain" code:100 userInfo:nil]);
    });
}
@end
