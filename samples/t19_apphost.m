// An exception thrown by guest code inside NSApplication's event handling is caught by AppKit (host frames).
#import <Cocoa/Cocoa.h>
static int g_handled;
@interface ThrowApp : NSApplication @end
@implementation ThrowApp
- (void)sendEvent:(NSEvent *)e {
    if (e.type == NSEventTypeApplicationDefined && e.data1 == 1) { g_handled++; @throw [NSException exceptionWithName:@"InEvent" reason:@"thrown from sendEvent:" userInfo:nil]; }
    [super sendEvent:e];
}
- (void)reportException:(NSException *)ex { printf("reportException: %s\n", [ex.name UTF8String]); fflush(stdout); }
@end
int main(void) {
    @autoreleasepool {
        long keepA = 1111, keepB = 2222; NSString *keepS = [NSString stringWithFormat:@"S%ld", keepA];
        ThrowApp *app = (ThrowApp *)[ThrowApp sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        dispatch_async(dispatch_get_main_queue(), ^{
            for (int i = 0; i < 3; i++)
                [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:1 data2:0] atStart:NO];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
                printf("events thrown: %d\n", g_handled); fflush(stdout);
                [NSApp stop:nil];
                [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:2 data2:0] atStart:YES];
            });
        });
        [app run];
        printf("run returned; locals %ld %ld %s\n", keepA, keepB, [keepS UTF8String]);
    }
    return 0;
}
