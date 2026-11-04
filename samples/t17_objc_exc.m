// Objective-C exceptions: @try/@catch/@finally, Foundation-thrown, through host frames, host catch.
#import <Cocoa/Cocoa.h>
#define OUT(fmt, ...) printf("%s\n", [[NSString stringWithFormat:fmt, ##__VA_ARGS__] UTF8String])

@interface MyExc : NSException @property (assign) int code; @end
@implementation MyExc @end

@interface Bomb : NSObject <NSSecureCoding> @end
@implementation Bomb
+ (BOOL)supportsSecureCoding { return YES; }
- (instancetype)initWithCoder:(NSCoder *)c { @throw [NSException exceptionWithName:@"BombException" reason:@"initWithCoder exploded" userInfo:nil]; }
- (void)encodeWithCoder:(NSCoder *)c {}
@end

@interface Tracker : NSObject
@property (copy) NSString *tag;
@end
@implementation Tracker
- (void)dealloc { printf("  dealloc %s\n", [_tag UTF8String]); }
@end

static void deep(int n) {
    Tracker *t = [Tracker new]; t.tag = [NSString stringWithFormat:@"deep%d", n];
    if (n == 0) @throw [NSException exceptionWithName:@"DeepException" reason:@"bottom reached" userInfo:@{@"depth": @0}];
    deep(n - 1);
}

int main(void) {
    @autoreleasepool {
        // 1. basic try/catch/finally
        @try { OUT(@"1 try"); @throw [NSException exceptionWithName:@"Basic" reason:@"basic reason" userInfo:@{@"k": @"v"}]; }
        @catch (NSException *e) { OUT(@"1 caught %@ / %@ / %@", e.name, e.reason, e.userInfo[@"k"]); }
        @finally { OUT(@"1 finally"); }

        // 2. non-NSException object, catch (id)
        @try { @throw @"a string"; } @catch (id o) { OUT(@"2 caught %@ (%@)", o, NSStringFromClass([o class])); }

        // 3. class matching
        for (int k = 0; k < 3; k++) {
            @try {
                if (k == 0) { MyExc *m = [[MyExc alloc] initWithName:@"My" reason:@"mine" userInfo:nil]; m.code = 77; @throw m; }
                if (k == 1) @throw [NSException exceptionWithName:@"Plain" reason:@"plain" userInfo:nil];
                @throw [NSNumber numberWithInt:5];
            }
            @catch (MyExc *e) { OUT(@"3.%d MyExc code=%d", k, e.code); }
            @catch (NSException *e) { OUT(@"3.%d NSException %@", k, e.name); }
            @catch (id o) { OUT(@"3.%d other %@", k, o); }
        }

        // 4. Foundation-thrown
        @try { id x = [@[] objectAtIndex:3]; (void)x; } @catch (NSException *e) { OUT(@"4a %@: %@", e.name, e.reason); }
        @try { NSMutableArray *a = [NSMutableArray array]; [a addObject:nil]; } @catch (NSException *e) { OUT(@"4b %@", e.name); }
        @try { [@"abc" substringFromIndex:10]; } @catch (NSException *e) { OUT(@"4c %@", e.name); }
        @try { [[NSObject new] performSelector:NSSelectorFromString(@"noSuchThing")]; } @catch (NSException *e) { OUT(@"4d %@: %@", e.name, [e.reason hasPrefix:@"-[NSObject noSuchThing]"] ? @"unrecognized selector" : e.reason); }
        @try { NSArray *a = [NSArray arrayWithObjects:@"x", nil, @"y", nil]; OUT(@"4e %lu", (unsigned long)a.count); } @catch (NSException *e) { OUT(@"4e %@", e.name); }
        @try { NSString *s = nil; NSArray *a = @[s ?: @"ok"]; OUT(@"4f %@", a[0]); } @catch (NSException *e) { OUT(@"4f %@", e.name); }
        @try { [NSException raise:@"RaisedFmt" format:@"value=%d str=%@", 5, @"five"]; } @catch (NSException *e) { OUT(@"4g %@ %@", e.name, e.reason); }
        @try { NSCAssert(NO, @"assertion text"); } @catch (NSException *e) { OUT(@"4h %@", e.name); }

        // 5. rethrow and nested finally ordering
        @try {
            @try { @throw [NSException exceptionWithName:@"Inner" reason:@"inner" userInfo:nil]; }
            @catch (NSException *e) { OUT(@"5 inner caught, rethrowing"); @throw; }
            @finally { OUT(@"5 inner finally"); }
        } @catch (NSException *e) { OUT(@"5 outer caught %@", e.name); }
        @finally { OUT(@"5 outer finally"); }

        // 6. ARC objects released while unwinding through deep frames
        @try { deep(3); } @catch (NSException *e) { OUT(@"6 caught %@ depth=%@", e.reason, e.userInfo[@"depth"]); }

        // 7. thrown from a block called by Foundation, caught outside (guest -> host -> guest)
        @try {
            [@[@1, @2, @3, @4] enumerateObjectsUsingBlock:^(id o, NSUInteger i, BOOL *stop) {
                OUT(@"7 visiting %@", o);
                if (i == 2) @throw [NSException exceptionWithName:@"FromBlock" reason:@"thrown in block" userInfo:nil];
            }];
        } @catch (NSException *e) { OUT(@"7 caught %@", e.reason); }
        @try {
            NSArray *sorted = [@[@3, @1, @2] sortedArrayUsingComparator:^NSComparisonResult(id a, id b) { @throw @"cmp"; }];
            (void)sorted;
        } @catch (id o) { OUT(@"7b caught %@", o); }

        // 8. exception caught by host code (NSKeyedUnarchiver) while guest frames sit in between
        long marker = 12345; NSString *keep = [NSString stringWithFormat:@"keep%ld", marker];
        NSError *err = nil;
        NSData *data = [NSKeyedArchiver archivedDataWithRootObject:[Bomb new] requiringSecureCoding:YES error:&err];
        err = nil;
        @try {
            id restored = [NSKeyedUnarchiver unarchivedObjectOfClass:[Bomb class] fromData:data error:&err];
            OUT(@"8 restored %@", restored);
        } @catch (NSException *e) { OUT(@"8 caught %@: %@", e.name, e.reason); }
        OUT(@"8 locals intact: %@ %ld", keep, marker);

        // 9. @synchronized releases its lock when an exception passes through
        id lock = [NSObject new];
        @try { @synchronized (lock) { @throw @"in sync"; } } @catch (id o) { OUT(@"9 %@", o); }
        @synchronized (lock) { OUT(@"9 relocked"); }

        // 10. many throws in a loop (state is reset every time)
        int caught = 0;
        for (int i = 0; i < 200; i++) { @try { @throw @(i); } @catch (NSNumber *n) { caught += [n intValue] == i; } }
        OUT(@"10 caught %d", caught);
    }
    return 0;
}
