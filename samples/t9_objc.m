// ObjC/Foundation conformance: built for arm64 (under rebar) and x86_64 (native); stdout must match.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdio.h>

#define OUT(fmt, ...) printf("%s\n", [[NSString stringWithFormat:fmt, ##__VA_ARGS__] UTF8String])

static int g_load_order;
static NSMutableString *g_log;

@protocol Greeter <NSObject>
- (NSString *)greeting;
@end

@interface Animal : NSObject <Greeter>
@property (nonatomic, copy) NSString *name;
@property (nonatomic, assign) NSInteger legs;
@property (nonatomic, assign) double weight;
@property (nonatomic, strong) NSMutableArray *tags;
- (instancetype)initWithName:(NSString *)n legs:(NSInteger)l;
+ (NSString *)kingdom;
@end

@implementation Animal {
    int _secret;
    char _flag;
}
+ (void)load { printf("Animal +load order=%d\n", g_load_order++); }
+ (void)initialize { if (self == [Animal class]) printf("Animal +initialize\n"); }
+ (NSString *)kingdom { return @"Animalia"; }
- (instancetype)initWithName:(NSString *)n legs:(NSInteger)l {
    if ((self = [super init])) { _name = [n copy]; _legs = l; _secret = 41; _flag = 1; _tags = [NSMutableArray array]; }
    return self;
}
- (NSString *)greeting { return [NSString stringWithFormat:@"I am %@ with %ld legs", self.name, (long)self.legs]; }
- (NSString *)description { return [NSString stringWithFormat:@"<Animal %@ secret=%d>", self.name, _secret + 1]; }
- (BOOL)isEqual:(id)o { return [o isKindOfClass:[Animal class]] && [[o name] isEqualToString:self.name]; }
- (NSUInteger)hash { return [self.name hash]; }
@end

@interface Dog : Animal
@property (nonatomic, assign) BOOL barks;
@end
@implementation Dog
+ (void)load { printf("Dog +load order=%d\n", g_load_order++); }
- (instancetype)initWithName:(NSString *)n legs:(NSInteger)l { if ((self = [super initWithName:n legs:l])) _barks = YES; return self; }
- (NSString *)greeting { return [[super greeting] stringByAppendingString:@" and I bark"]; }
+ (NSString *)kingdom { return [[super kingdom] stringByAppendingString:@"/Canidae"]; }
@end

@interface NSString (Rebar)
- (NSString *)rebarShout;
- (NSUInteger)rebarVowels;
@end
@implementation NSString (Rebar)
- (NSString *)rebarShout { return [[self uppercaseString] stringByAppendingString:@"!"]; }
- (NSUInteger)rebarVowels {
    NSUInteger n = 0;
    for (NSUInteger i = 0; i < self.length; i++) if (strchr("aeiouAEIOU", [self characterAtIndex:i])) n++;
    return n;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        OUT(@"argc=%d name=%s", argc, [[[NSProcessInfo processInfo] arguments][0] lastPathComponent].UTF8String);
        Animal *a = [[Animal alloc] initWithName:@"Rex" legs:4];
        Dog *d = [[Dog alloc] initWithName:@"Fido" legs:4];
        d.weight = 12.5;
        OUT(@"%@ | %@", a.greeting, d.greeting);
        OUT(@"desc: %@ / %@", a, d);
        OUT(@"kingdoms: %@ %@", [Animal kingdom], [Dog kingdom]);
        OUT(@"props: %ld %.2f %d class=%s super=%s", (long)d.legs, d.weight, d.barks, class_getName([d class]), class_getName([d superclass]));
        OUT(@"conforms=%d responds=%d kind=%d member=%d", [a conformsToProtocol:@protocol(Greeter)], [d respondsToSelector:@selector(greeting)],
            [d isKindOfClass:[Animal class]], [d isMemberOfClass:[Animal class]]);
        if ([a isEqual:[[Animal alloc] initWithName:@"Rex" legs:2]]) OUT(@"isEqual ok");
        OUT(@"category: %@ %lu", [@"hello world" rebarShout], (unsigned long)[@"Objective C" rebarVowels]);

        NSString *s = @"The quick brown fox";
        NSRange r = [s rangeOfString:@"quick"];
        OUT(@"range %lu %lu | %d %d", (unsigned long)r.location, (unsigned long)r.length, [s hasPrefix:@"The"], [s hasSuffix:@"cat"]);
        OUT(@"substr: %@ | %@ | %lu", [s substringWithRange:NSMakeRange(4, 5)], [s componentsSeparatedByString:@" "], (unsigned long)s.length);
        OUT(@"format: %d %5.2f %@ %s %c %lld %x", 42, 3.14159, @"obj", "cstr", 'q', 123456789012LL, 255);

        NSArray *arr = @[@"pear", @"apple", @"fig", @"banana"];
        NSArray *sorted = [arr sortedArrayUsingComparator:^NSComparisonResult(id x, id y) { return [x compare:y]; }];
        OUT(@"sorted: %@", [sorted componentsJoinedByString:@","]);
        __block NSUInteger total = 0;
        [arr enumerateObjectsUsingBlock:^(id o, NSUInteger idx, BOOL *stop) { total += [o length]; if (idx == 2) *stop = YES; }];
        OUT(@"enumerated total=%lu", (unsigned long)total);
        NSArray *lens = [[arr valueForKey:@"length"] sortedArrayUsingSelector:@selector(compare:)];
        OUT(@"kvc lens: %@", lens);

        NSDictionary *dict = @{@"one": @1, @"two": @2.5, @"three": @"3"};
        NSMutableString *keys = [NSMutableString string];
        for (NSString *k in [dict.allKeys sortedArrayUsingSelector:@selector(compare:)]) [keys appendFormat:@"%@=%@;", k, dict[k]];
        OUT(@"dict: %@", keys);
        NSDictionary *d2 = [NSDictionary dictionaryWithObjectsAndKeys:@"v1", @"k1", @"v2", @"k2", nil];
        NSArray *a2 = [NSArray arrayWithObjects:@"x", @"y", @"z", nil];
        NSSet *set = [NSSet setWithObjects:@1, @2, @2, @3, nil];
        OUT(@"varargs: %lu %lu %lu %@", (unsigned long)d2.count, (unsigned long)a2.count, (unsigned long)set.count, d2[@"k2"]);

        NSNumber *n1 = @42, *n2 = @3.5, *n3 = @YES;
        OUT(@"numbers: %@ %@ %@ %d %.1f %lld", n1, n2, n3, [n1 intValue], [n2 doubleValue], [@(9007199254740993LL) longLongValue]);
        OUT(@"compare: %ld %ld", (long)[n1 compare:@43], (long)[@"b" compare:@"a"]);

        NSData *data = [@"binary\x01\x02" dataUsingEncoding:NSUTF8StringEncoding];
        OUT(@"data: %lu %@", (unsigned long)data.length, [data base64EncodedStringWithOptions:0]);
        NSError *err = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"a": @[@1, @2, @{@"b": @NO}], @"s": @"str"} options:NSJSONWritingSortedKeys error:&err];
        OUT(@"json: %@", [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding]);
        id parsed = [NSJSONSerialization JSONObjectWithData:[@"{\"x\":[1,2,3],\"y\":{\"z\":true}}" dataUsingEncoding:NSUTF8StringEncoding] options:0 error:&err];
        OUT(@"parsed: %@ %@", parsed[@"x"][1], parsed[@"y"][@"z"]);

        NSDate *date = [NSDate dateWithTimeIntervalSince1970:86400 * 365];
        NSDateFormatter *fmt = [[NSDateFormatter alloc] init];
        fmt.dateFormat = @"yyyy-MM-dd HH:mm"; fmt.timeZone = [NSTimeZone timeZoneWithName:@"UTC"]; fmt.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
        OUT(@"date: %@", [fmt stringFromDate:date]);
        NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"rebar_t9.txt"];
        [@"file contents\nline 2" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:&err];
        NSString *back = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&err];
        OUT(@"file: %@ exists=%d", [back componentsSeparatedByString:@"\n"], [[NSFileManager defaultManager] fileExistsAtPath:path]);
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block int fromQueue = 0;
        dispatch_async(dispatch_get_global_queue(0, 0), ^{ fromQueue = 7; dispatch_semaphore_signal(sem); });
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        OUT(@"gcd: %d", fromQueue);
        NSLog(@"NSLog line %d %@", 5, @"ok");
    }
    return 0;
}
