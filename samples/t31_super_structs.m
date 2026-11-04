// A guest NSWindow subclass whose overrides call super with NSRect arguments (Electron's ElectronNSWindow does): the
// rectangles must reach AppKit intact through objc_msgSendSuper2.
#import <Cocoa/Cocoa.h>
@interface MyWin : NSWindow @end
@implementation MyWin
- (NSRect)constrainFrameRect:(NSRect)r toScreen:(NSScreen *)s { NSRect o = [super constrainFrameRect:r toScreen:s]; printf("  constrain %g %g %g %g -> %g %g %g %g\n", r.origin.x, r.origin.y, r.size.width, r.size.height, o.origin.x, o.origin.y, o.size.width, o.size.height); return o; }
- (NSRect)contentRectForFrameRect:(NSRect)r { NSRect o = [super contentRectForFrameRect:r]; printf("  contentRectForFrameRect %g %g %g %g -> %g %g %g %g\n", r.origin.x, r.origin.y, r.size.width, r.size.height, o.origin.x, o.origin.y, o.size.width, o.size.height); return o; }
- (NSRect)frameRectForContentRect:(NSRect)r { NSRect o = [super frameRectForContentRect:r]; printf("  frameRectForContentRect %g %g %g %g -> %g %g %g %g\n", r.origin.x, r.origin.y, r.size.width, r.size.height, o.origin.x, o.origin.y, o.size.width, o.size.height); return o; }
- (void)setFrame:(NSRect)r display:(BOOL)d { printf("  setFrame:display:%d %g %g %g %g\n", d, r.origin.x, r.origin.y, r.size.width, r.size.height); [super setFrame:r display:d]; }
@end
static void show(const char *t, NSWindow *w) { NSRect f = w.frame; printf("%-14s %g %g %g %g\n", t, f.origin.x, f.origin.y, f.size.width, f.size.height); }
int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        MyWin *w = [[MyWin alloc] initWithContentRect:NSMakeRect(0, 0, 1, 1) styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskFullSizeContentView backing:NSBackingStoreBuffered defer:YES];
        w.titlebarAppearsTransparent = YES; w.titleVisibility = NSWindowTitleHidden;
        show("created", w);
        [w setFrame:NSMakeRect(360, 112, 1200, 828) display:YES animate:NO]; show("setFrame", w);
        [w setContentMinSize:NSMakeSize(0, 0)]; show("minsize0", w);
        [w setContentMaxSize:NSMakeSize(1e300, 1e300)]; show("maxsize", w);
        [w setContentMinSize:NSMakeSize(400, 270)]; show("minsize", w);
        [w setFrame:NSMakeRect(40, 100, 800, 600) display:YES animate:NO]; show("setFrame2", w);
        [w setFrameOrigin:NSMakePoint(200, 300)]; show("origin", w);
    }
    return 0;
}
