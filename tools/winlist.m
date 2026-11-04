// Prints the on-screen windows (> 60x60, layer 0) as "pid owner width height name", for checking from a script that an app really opened a window.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
int main(void) {
    @autoreleasepool {
        NSArray *ws = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID));
        for (NSDictionary *w in ws) {
            if ([w[(id)kCGWindowLayer] intValue] != 0) continue;
            NSDictionary *b = w[(id)kCGWindowBounds];
            if ([b[@"Width"] doubleValue] <= 60 || [b[@"Height"] doubleValue] <= 60) continue;
            printf("%d %s %d %d\n", [w[(id)kCGWindowOwnerPID] intValue], [w[(id)kCGWindowOwnerName] UTF8String] ?: "?", (int)[b[@"Width"] doubleValue], (int)[b[@"Height"] doubleValue]);
        }
    }
    return 0;
}
