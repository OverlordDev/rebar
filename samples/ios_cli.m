// An iOS (platform 2) command-line program: built with the iphoneos SDK, packed into an IPA by run_tests.sh and started with tools/ipa.py.
#import <Foundation/Foundation.h>
#include <sys/sysctl.h>
int main(int argc, char **argv) { @autoreleasepool {
    NSDictionary *d = @{@"k": @[@1, @2, @3]}; NSData *j = [NSJSONSerialization dataWithJSONObject:d options:NSJSONWritingSortedKeys error:nil];
    printf("json %s\n", [[[NSString alloc] initWithData:j encoding:NSUTF8StringEncoding] UTF8String]);
    printf("bundle %s\n", [[[NSBundle mainBundle] bundleIdentifier] UTF8String] ?: "(none)");
    char buf[64]; size_t n = sizeof buf; sysctlbyname("hw.machine", buf, &n, NULL, 0); printf("hw.machine %s\n", buf);
    printf("args %d\n", argc - 1);
} return 0; }
