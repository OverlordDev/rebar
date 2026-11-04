// .app bundle behaviour: Info.plist, resources, NSApplicationMain, default activation policy.
#import <Cocoa/Cocoa.h>
@interface Del : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *w;
@end
@implementation Del
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSBundle *b = [NSBundle mainBundle];
    printf("bundle: %s id=%s\n", [[b.bundlePath lastPathComponent] UTF8String], [b.bundleIdentifier UTF8String]);
    printf("info: %s v%s\n", [[b objectForInfoDictionaryKey:@"CFBundleName"] UTF8String], [[b objectForInfoDictionaryKey:@"RebarKey"] UTF8String]);
    NSString *res = [b pathForResource:@"hello" ofType:@"txt"];
    printf("resource: %s => %s", [[res lastPathComponent] UTF8String], [[NSString stringWithContentsOfFile:res encoding:NSUTF8StringEncoding error:nil] UTF8String]);
    printf("policy before: %ld\n", (long)NSApp.activationPolicy);
    self.w = [[NSWindow alloc] initWithContentRect:NSMakeRect(300, 300, 280, 120) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                           backing:NSBackingStoreBuffered defer:NO];
    self.w.title = @"Bundle test";
    NSTextField *t = [NSTextField labelWithString:@"Hello from a bundled arm64 app"];
    t.frame = NSMakeRect(20, 50, 240, 22);
    [self.w.contentView addSubview:t];
    [self.w makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    const char *hold = getenv("T13_HOLD");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((hold ? atof(hold) : 1.0) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        printf("visible=%d policy=%ld\n", self.w.isVisible, (long)NSApp.activationPolicy);
        fflush(stdout);
        [NSApp terminate:nil];
    });
}
@end
static Del *g_del;  // NSApplication.delegate is weak
int main(int argc, const char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        g_del = [Del new];
        app.delegate = g_del;
    }
    return NSApplicationMain(argc, argv);
}
