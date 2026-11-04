#import <Foundation/Foundation.h>
static void handler(NSException *e) { printf("uncaught handler: %s | %s\n", [e.name UTF8String], [e.reason UTF8String]); fflush(stdout); }
int main(void) {
    @autoreleasepool {
        NSSetUncaughtExceptionHandler(&handler);
        printf("about to throw\n"); fflush(stdout);
        @throw [NSException exceptionWithName:@"Unhandled" reason:@"no catch here" userInfo:nil];
    }
    return 0;
}
