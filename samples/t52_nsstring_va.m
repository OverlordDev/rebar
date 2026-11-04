// -[NSString initWithFormat:arguments:] and the locale form with a va_list taken from the caller's variadic arguments (what logging helpers such as Sparkle's and
// Parallels' installer's do).  rebar once took the format string for the va_list, so the first "%@" printed garbage and the second one crashed.
#import <Foundation/Foundation.h>
#include <stdarg.h>
#include <stdio.h>
static NSString *fmt(NSString *f, ...) {
    va_list ap; va_start(ap, f);
    NSString *s = [[NSString alloc] initWithFormat:f arguments:ap];
    va_end(ap);
    return s;
}
static NSString *fmtl(NSString *f, ...) {
    va_list ap; va_start(ap, f);
    NSString *s = [[NSString alloc] initWithFormat:f locale:nil arguments:ap];
    va_end(ap);
    return s;
}
static NSString *fmtc(NSString *f, ...) {
    va_list ap; va_start(ap, f);
    NSString *s = [[NSString alloc] initWithFormat:f arguments:ap];
    va_end(ap);
    return s;
}
int main() { @autoreleasepool {
    printf("%s\n", [fmt(@"=== Log initialized in %@ mode. Application version: %@", @"debug", @"1.8.1") UTF8String]);
    printf("%s\n", [fmt(@"%d + %ld = %@ (%.3f)", 7, 35L, @"forty-two", 3.14159) UTF8String]);
    printf("%s\n", [fmtl(@"%@/%@/%d", @"a", @"b", 3) UTF8String]);
    printf("%s\n", [fmtc(@"%@ %@ %@ %@ %@ %@ %@ %@ %@ %@", @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"10") UTF8String]);
    printf("%s\n", [[NSString stringWithFormat:@"%@-%d-%@", @"x", 5, @"y"] UTF8String]);
} return 0; }
