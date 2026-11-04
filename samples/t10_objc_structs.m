#import <Foundation/Foundation.h>
#define OUT(fmt, ...) printf("%s\n", [[NSString stringWithFormat:fmt, ##__VA_ARGS__] UTF8String])
@interface Geo : NSObject
- (NSPoint)pointPlusX:(double)dx from:(NSPoint)p;
- (NSSize)scale:(NSSize)s by:(double)k;
- (NSRange)shift:(NSRange)r by:(NSUInteger)n;
- (float)halve:(float)f;
- (BOOL)isPositive:(double)d;
- (char)signOf:(int)i;
@end
@implementation Geo
- (NSPoint)pointPlusX:(double)dx from:(NSPoint)p { return NSMakePoint(p.x + dx, p.y * 2); }
- (NSSize)scale:(NSSize)s by:(double)k { return NSMakeSize(s.width * k, s.height * k); }
- (NSRange)shift:(NSRange)r by:(NSUInteger)n { return NSMakeRange(r.location + n, r.length * 2); }
- (float)halve:(float)f { return f / 2; }
- (BOOL)isPositive:(double)d { return d > 0; }
- (char)signOf:(int)i { return i < 0 ? -1 : i > 0 ? 1 : 0; }
@end
int main(void) {
    @autoreleasepool {
        Geo *g = [Geo new];
        NSPoint p = [g pointPlusX:1.5 from:NSMakePoint(2, 3.25)];
        NSSize s = [g scale:NSMakeSize(4, 5) by:2.5];
        NSRange r = [g shift:NSMakeRange(10, 3) by:5];
        OUT(@"guest->guest: %.2f %.2f | %.1f %.1f | %lu %lu | %.2f %d %d %d", p.x, p.y, s.width, s.height, (unsigned long)r.location, (unsigned long)r.length,
            [g halve:5.0f], [g isPositive:-1.0], [g isPositive:2.0], [g signOf:-9]);
        NSValue *vp = [NSValue valueWithPoint:NSMakePoint(7.5, -1.25)];
        NSValue *vs = [NSValue valueWithSize:NSMakeSize(3, 4)];
        NSValue *vr = [NSValue valueWithRange:NSMakeRange(100, 7)];
        NSPoint p2 = [vp pointValue]; NSSize s2 = [vs sizeValue]; NSRange r2 = [vr rangeValue];
        OUT(@"guest->host: %.2f %.2f | %.1f %.1f | %lu %lu", p2.x, p2.y, s2.width, s2.height, (unsigned long)r2.location, (unsigned long)r2.length);
        NSString *str = @"alpha beta gamma";
        NSRange found = [str rangeOfString:@"beta" options:NSCaseInsensitiveSearch range:NSMakeRange(0, str.length)];
        OUT(@"host range: %lu %lu %@", (unsigned long)found.location, (unsigned long)found.length, NSStringFromRange(found));
        OUT(@"%@ %@", NSStringFromPoint(p2), NSStringFromSize(s2));
        NSValue *vrect = [NSValue valueWithRect:NSMakeRect(1, 2, 3, 4)];
        NSRect rr = [vrect rectValue];
        OUT(@"rect: %.1f %.1f %.1f %.1f", rr.origin.x, rr.origin.y, rr.size.width, rr.size.height);
    }
    return 0;
}
