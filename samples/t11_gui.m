// AppKit conformance: programmatic window, custom drawRect:, target/action, run loop, GCD on main queue.
// Built for arm64 (under rebar) and x86_64 (native); stdout must match.
#import <Cocoa/Cocoa.h>
#include <stdio.h>

static int g_clicks, g_draws;

@interface CanvasView : NSView
@property (nonatomic, assign) CGFloat phase;
@end
@implementation CanvasView
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)dirty {
    g_draws++;
    [[NSColor colorWithCalibratedRed:0.1 green:0.2 blue:0.4 alpha:1] setFill];
    NSRectFill(self.bounds);
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSetRGBFillColor(ctx, 1, 0.5, 0, 1);
    CGContextFillRect(ctx, CGRectMake(10, 10, 80, 50));
    CGContextSetRGBStrokeColor(ctx, 0, 1, 0.5, 1);
    CGContextSetLineWidth(ctx, 3);
    CGContextStrokeRect(ctx, CGRectInset(CGRectMake(20, 70, 100, 60), 4, 4));
    CGContextSaveGState(ctx);
    CGContextConcatCTM(ctx, CGAffineTransformMakeRotation(0.3 + self.phase));
    CGContextSetRGBFillColor(ctx, 0.9, 0.9, 0.2, 1);
    CGContextFillEllipseInRect(ctx, CGRectMake(120, 20, 60, 40));
    CGContextRestoreGState(ctx);
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(130, 90, 90, 50) xRadius:10 yRadius:10];
    [[NSColor whiteColor] setStroke]; p.lineWidth = 2; [p stroke];
    NSDictionary *attrs = @{NSFontAttributeName: [NSFont systemFontOfSize:14], NSForegroundColorAttributeName: [NSColor whiteColor]};
    [@"Hello from arm64" drawInRect:NSMakeRect(12, 150, 200, 20) withAttributes:attrs];
    NSSize ts = [@"Hello from arm64" sizeWithAttributes:attrs];
    printf("text size %.1f x %.1f dirty %.0f,%.0f %.0fx%.0f\n", ts.width, ts.height, dirty.origin.x, dirty.origin.y, dirty.size.width, dirty.size.height);
}
@end

@interface AppDel : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@property (strong) CanvasView *canvas;
@property (strong) NSTextField *label;
- (void)clicked:(id)sender;
@end
@implementation AppDel
- (void)clicked:(id)sender {
    g_clicks++;
    self.label.stringValue = [NSString stringWithFormat:@"clicked %d", g_clicks];
}
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    printf("didFinishLaunching %s\n", [n.name UTF8String]);
    NSRect frame = NSMakeRect(200, 200, 320, 260);
    self.window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"rebar";
    self.window.delegate = self;
    NSView *content = self.window.contentView;
    self.canvas = [[CanvasView alloc] initWithFrame:NSMakeRect(0, 40, 320, 180)];
    [content addSubview:self.canvas];
    NSButton *b = [NSButton buttonWithTitle:@"Click me" target:self action:@selector(clicked:)];
    b.frame = NSMakeRect(10, 8, 100, 28);
    [content addSubview:b];
    self.label = [NSTextField labelWithString:@"not clicked"];
    self.label.frame = NSMakeRect(120, 12, 180, 20);
    [content addSubview:self.label];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    const char *hold = getenv("T11_HOLD"); double delay = hold ? atof(hold) : 0.8;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [b performClick:nil];
        [b performClick:nil];
        printf("label: %s\n", [self.label.stringValue UTF8String]);
        NSBitmapImageRep *rep = [self.canvas bitmapImageRepForCachingDisplayInRect:self.canvas.bounds];
        [self.canvas cacheDisplayInRect:self.canvas.bounds toBitmapImageRep:rep];
        unsigned long long h = 1469598103934665603ULL; unsigned char *px = rep.bitmapData;
        for (NSInteger i = 0; i < rep.bytesPerRow * rep.pixelsHigh; i++) { h ^= px[i]; h *= 1099511628211ULL; }
        printf("bitmap %ldx%ld hash %016llx\n", (long)rep.pixelsWide, (long)rep.pixelsHigh, h);
        printf("frame: %.0f %.0f %.0f %.0f\n", self.window.frame.origin.x, self.window.frame.origin.y, self.window.frame.size.width, self.window.frame.size.height);
        printf("draws>0: %d clicks: %d\n", g_draws > 0, g_clicks);
        fflush(stdout);
        [NSApp terminate:nil];
    });
}
- (void)windowWillClose:(NSNotification *)n { printf("windowWillClose\n"); }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)s { printf("shouldTerminate\n"); return NSTerminateNow; }
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDel *d = [AppDel new];
        app.delegate = d;
        [app run];
    }
    return 0;
}
