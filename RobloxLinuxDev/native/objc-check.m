typedef unsigned long NSUInteger;
typedef struct {double x,y,w,h;} Rect;
@interface NSObject {id isa;}
+ (id)alloc;
- (id)init;
- (void)release;
- (id)class;
- (void*)methodForSelector:(SEL)selector;
+ (void*)instanceMethodForSelector:(SEL)selector;
@end
@interface NSNumber:NSObject
+ (id)numberWithDouble:(double)value;
- (double)doubleValue;
@end
extern int puts(const char *);
static volatile int loaded;
@interface ArmExample:NSObject {NSUInteger _value;}
+ (void)load;
- (void)setValue:(NSUInteger)value;
- (Rect)adjust:(Rect)rect;
@end
@implementation ArmExample
+ (void)load {loaded=42;}
- (void)setValue:(NSUInteger)value {_value=value;}
- (Rect)adjust:(Rect)rect {rect.x+=_value;rect.h+=_value;return rect;}
@end
int main(void) {
    if(loaded!=42)return 1;
    ArmExample *object=[[ArmExample alloc] init];
    [object setValue:7];Rect r=[object adjust:(Rect){1,2,3,4}];
    if(r.x!=8 || r.y!=2 || r.w!=3 || r.h!=11)return 2;
    void *guest=[object methodForSelector:@selector(adjust:)];
    if(guest!=[ArmExample instanceMethodForSelector:@selector(adjust:)])return 3;
    r=((Rect(*)(id,SEL,Rect))guest)(object,@selector(adjust:),(Rect){2,3,4,5});
    if(r.x!=9 || r.y!=3 || r.w!=4 || r.h!=12)return 4;
    NSNumber *number=[NSNumber numberWithDouble:3.25];
    void *native=[number methodForSelector:@selector(doubleValue)];
    for(unsigned i=0;i<1000;++i)if(native!=[number methodForSelector:@selector(doubleValue)])return 5;
    if(((double(*)(id,SEL))native)(number,@selector(doubleValue))!=3.25)return 6;
    native=[(id)[number class] instanceMethodForSelector:@selector(doubleValue)];
    if(((double(*)(id,SEL))native)(number,@selector(doubleValue))!=3.25)return 7;
    [object release];
    puts("PASS: ARM64 Objective-C classes, ivars, structure returns, native/guest IMP lookup, FP calls and stable IMP identity");
    return 0;
}
