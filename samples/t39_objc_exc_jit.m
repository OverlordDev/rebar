// Exceptions raised by the host (Foundation) through guest frames that have ARC cleanups and callee-saved D registers, in a loop long enough for the translator.
#import <Foundation/Foundation.h>
@interface T : NSObject { @public double d; } - (void)work:(double)x i:(int)i; @end
@implementation T
- (void)work:(double)x i:(int)i {
    double keep = x * 1.5, keep2 = x * 0.25;
    NSNumber *a = @(x * 2); NSNumber *b = @(x * 3);
    NSMutableArray *arr = [NSMutableArray array];
    [arr addObject:a]; [arr addObject:b];
    if (i % 3 == 0) { [arr objectAtIndex:99]; }                       // NSRangeException
    if (i % 7 == 0) { [NSNull null]; [[NSMutableArray array] addObject:nil]; }   // NSInvalidArgumentException
    d = keep + keep2 + [a doubleValue] + [b doubleValue] + (double)arr.count;
}
@end
int main(void) {
    T *t = [T new]; int caught = 0, range = 0; double sum = 0;
    for (int i = 0; i < 900; i++) {
        @try { [t work:(double)i i:i]; sum += t->d; }
        @catch (NSException *e) { caught++; if ([e.name isEqualToString:NSRangeException]) range++; }
    }
    printf("caught %d (range %d) sum %g\n", caught, range, sum);
    return 0;
}
