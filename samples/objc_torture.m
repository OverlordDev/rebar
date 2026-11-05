#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dispatch/dispatch.h>
#import <CoreGraphics/CoreGraphics.h>

typedef struct { int a; double b; char c[5]; } Weird;
static NSMutableArray *g_log;
@protocol Greeter <NSObject> - (NSString *)greet:(NSString *)n; @optional - (int)extra; @end
@interface Base : NSObject <Greeter> { @public int _ivar; } @property (nonatomic, copy) NSString *name; @property (atomic, assign) CGRect box; - (CGPoint)pointFor:(double)x; - (CGSize)sizeBig; - (Weird)weird; @end
@implementation Base
+ (void)load { if (!g_log) g_log = [NSMutableArray new]; [g_log addObject:@"Base+load"]; }
+ (void)initialize { if (!g_log) g_log = [NSMutableArray new]; [g_log addObject:[NSString stringWithFormat:@"%@ initialize", self]]; }
- (NSString *)greet:(NSString *)n { return [NSString stringWithFormat:@"hello %@ from %@", n, self.name ?: @"?"]; }
- (CGPoint)pointFor:(double)x { return CGPointMake(x, x * 2); }
- (CGSize)sizeBig { return CGSizeMake(3, 4); }
- (Weird)weird { return (Weird){7, 2.5, "abcd"}; }
@end
@interface Derived : Base @end
@implementation Derived - (NSString *)greet:(NSString *)n { return [[super greet:n] uppercaseString]; } @end

@interface Fwd : NSObject @end
@implementation Fwd
- (id)forwardingTargetForSelector:(SEL)s { return [s == @selector(length) ? @"forwarded" : nil copy]; }
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s { return [NSMethodSignature signatureWithObjCTypes:"i@:i"]; }
- (void)forwardInvocation:(NSInvocation *)inv { int a; [inv getArgument:&a atIndex:2]; int r = a * 10; [inv setReturnValue:&r]; }
@end

int main(void) {
  @autoreleasepool {
    Base *b = [Base new]; b.name = @"Bob"; Derived *d = [Derived new]; d.name = @"Dee";
    printf("%s | %s\n", [[b greet:@"x"] UTF8String], [[d greet:@"y"] UTF8String]);
    CGPoint p = [b pointFor:1.5]; CGSize s = [b sizeBig]; b.box = CGRectMake(1, 2, 3, 4); CGRect r = b.box;
    printf("structs %.1f,%.1f %.0fx%.0f rect %.0f %.0f %.0f %.0f\n", p.x, p.y, s.width, s.height, r.origin.x, r.origin.y, r.size.width, r.size.height);
    Weird w = [b weird]; printf("weird %d %.1f %s\n", w.a, w.b, w.c);
    // runtime class creation, methods, ivars, associated objects
    Class K = objc_allocateClassPair([NSObject class], "Dyn", 0); class_addIvar(K, "_v", sizeof(int), 2, "i");
    class_addMethod(K, @selector(twice:), imp_implementationWithBlock(^int(id self, int x) { return x * 2; }), "i@:i"); objc_registerClassPair(K);
    id dyn = [K new]; printf("dyn %d %s\n", ((int (*)(id, SEL, int))objc_msgSend)(dyn, @selector(twice:), 21), class_getName(object_getClass(dyn)));
    objc_setAssociatedObject(dyn, "k", @"assoc", OBJC_ASSOCIATION_RETAIN); printf("assoc %s\n", [objc_getAssociatedObject(dyn, "k") UTF8String]);
    // swizzle
    Method m1 = class_getInstanceMethod([Base class], @selector(sizeBig)); IMP orig = method_getImplementation(m1);
    method_setImplementation(m1, imp_implementationWithBlock(^CGSize(id self) { CGSize o = ((CGSize (*)(id, SEL))orig)(self, @selector(sizeBig)); return CGSizeMake(o.width * 10, o.height * 10); }));
    s = [b sizeBig]; printf("swizzled %.0fx%.0f\n", s.width, s.height);
    // forwarding
    Fwd *f = [Fwd new]; printf("fwd %lu %d\n", (unsigned long)[(id)f length], ((int (*)(id, SEL, int))objc_msgSend)(f, NSSelectorFromString(@"mystery:"), 4));
    // protocol conformance and respondsTo
    printf("proto %d %d %d\n", [b conformsToProtocol:@protocol(Greeter)], [b respondsToSelector:@selector(extra)], [Derived instancesRespondToSelector:@selector(greet:)]);
    // blocks: captured vars, __block, copy, nested
    __block int counter = 0; int captured = 5; void (^inc)(void) = ^{ counter += captured; }; inc(); inc(); void (^heap)(void) = [inc copy]; heap(); printf("blocks %d\n", counter);
    NSMutableArray *bl = [NSMutableArray new]; for (int i = 0; i < 3; i++) [bl addObject:[^int{ return i * i; } copy]]; int tot = 0; for (int (^q)(void) in bl) tot += q(); printf("blocklist %d\n", tot);
    // weak
    __weak id wk; @autoreleasepool { id tmp = [NSObject new]; wk = tmp; printf("weak alive %d\n", wk != nil); } printf("weak after %d\n", wk == nil);
    // exceptions
    @try { @try { [NSException raise:@"X" format:@"boom %d", 1]; } @finally { printf("finally\n"); } } @catch (NSException *e) { printf("caught %s %s\n", [e.name UTF8String], [e.reason UTF8String]); }
    @try { [@[] objectAtIndex:3]; } @catch (NSException *e) { printf("range %s\n", [e.name UTF8String]); }
    // @synchronized + GCD
    __block int shared = 0; id lock = [NSObject new]; dispatch_group_t g = dispatch_group_create(); dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    for (int i = 0; i < 8; i++) dispatch_group_async(g, q, ^{ for (int j = 0; j < 500; j++) @synchronized (lock) { shared++; } });
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER); printf("sync %d\n", shared);
    dispatch_queue_t sq = dispatch_queue_create("s", DISPATCH_QUEUE_CONCURRENT); __block int rw = 0; for (int i = 0; i < 100; i++) dispatch_barrier_async(sq, ^{ rw++; }); dispatch_barrier_sync(sq, ^{}); printf("barrier %d\n", rw);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0); dispatch_async(q, ^{ dispatch_semaphore_signal(sem); }); printf("sem %ld\n", dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_apply(4, q, ^(size_t i) { @synchronized (lock) { shared += (int)i; } }); printf("apply %d\n", shared);
    NSOperationQueue *oq = [NSOperationQueue new]; NSMutableArray *order = [NSMutableArray new]; NSBlockOperation *o1 = [NSBlockOperation blockOperationWithBlock:^{ @synchronized (order) { [order addObject:@1]; } }], *o2 = [NSBlockOperation blockOperationWithBlock:^{ @synchronized (order) { [order addObject:@2]; } }];
    [o2 addDependency:o1]; [oq addOperations:@[o2, o1] waitUntilFinished:YES]; printf("ops %s\n", [[order componentsJoinedByString:@","] UTF8String]);
    // Foundation odds and ends
    NSArray *arr = @[@{@"n": @"b", @"v": @2}, @{@"n": @"a", @"v": @1}, @{@"n": @"c", @"v": @3}];
    printf("kvc %s %s %s\n", [[[arr valueForKeyPath:@"@sum.v"] description] UTF8String], [[[arr valueForKeyPath:@"@max.n"] description] UTF8String], [[[arr valueForKey:@"n"] componentsJoinedByString:@""] UTF8String]);
    NSArray *sorted = [arr sortedArrayUsingDescriptors:@[[NSSortDescriptor sortDescriptorWithKey:@"n" ascending:YES]]]; printf("sorted %s\n", [[[sorted valueForKey:@"n"] componentsJoinedByString:@""] UTF8String]);
    printf("predicate %lu %d\n", (unsigned long)[[arr filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"v > 1 AND n != 'c'"]] count], [[NSPredicate predicateWithFormat:@"SELF MATCHES %@", @"a.*z"] evaluateWithObject:@"abcz"]);
    NSNumberFormatter *nf = [NSNumberFormatter new]; nf.numberStyle = NSNumberFormatterCurrencyStyle; nf.locale = [NSLocale localeWithLocaleIdentifier:@"en_US"]; printf("fmt %s\n", [[nf stringFromNumber:@1234.5] UTF8String]);
    printf("format %s\n", [[NSString stringWithFormat:@"%@|%5.2f|%lld|%-4d|%c|%@", @"s", 3.14159, 1LL << 40, 7, 'z', @[@1]] UTF8String]);
    NSData *js = [NSJSONSerialization dataWithJSONObject:@{@"a": @[@1, @2.5, @"x", [NSNull null]]} options:NSJSONWritingSortedKeys error:nil]; printf("json %s\n", [[[NSString alloc] initWithData:js encoding:NSUTF8StringEncoding] UTF8String]);
    NSCache *cache = [NSCache new]; [cache setObject:@"v" forKey:@"k"]; printf("cache %s\n", [[cache objectForKey:@"k"] UTF8String]);
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[b methodSignatureForSelector:@selector(pointFor:)]]; double dx = 4; [inv setSelector:@selector(pointFor:)]; [inv setArgument:&dx atIndex:2]; [inv invokeWithTarget:b]; CGPoint rp; [inv getReturnValue:&rp]; printf("invocation %.0f,%.0f\n", rp.x, rp.y);
    printf("tagged %s %lu\n", object_getClassName(@(42)), (unsigned long)[@"short" hash] % 1);
    printf("log %s\n", [[g_log componentsJoinedByString:@","] UTF8String]);
  }
  return 0;
}
