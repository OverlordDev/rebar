// mode "ios":   render green into an IOSurface-backed Metal texture and show the IOSurface as a CALayer's contents
// mode "metal": render green to a CAMetalLayer drawable and present it
// Prints the IOSurface pixel as seen by the CPU; the window (at 100,100 size 300x300) is checked from outside with screencapture.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#include <stdio.h>
#include <string.h>
static void fill(id<MTLDevice> dev, id<MTLTexture> tex, id<MTLDrawable> drawable, MTLStorageMode sm, bool sync) {
    id<MTLCommandQueue> q = [dev newCommandQueue];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
    if (sync) { id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder]; [b synchronizeResource:tex]; [b endEncoding]; }
    if (drawable) [cb presentDrawable:drawable];
    [cb commit];
    [cb waitUntilCompleted];
}
int main(int argc, char **argv) {
    @autoreleasepool {
        const char *mode = argc > 1 ? argv[1] : "ios";
        int sm = argc > 2 ? atoi(argv[2]) : 1;   // 0 shared, 1 managed, 2 private (ios mode)
        int sync = argc > 3 ? atoi(argv[3]) : 0;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 500, 300, 300) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        win.level = NSScreenSaverWindowLevel;
        NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 300)];
        v.wantsLayer = YES;
        win.contentView = v;
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!strcmp(mode, "ios")) {
            NSDictionary *props = @{ (id)kIOSurfaceWidth: @300, (id)kIOSurfaceHeight: @300, (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
            IOSurfaceRef surf = IOSurfaceCreate((CFDictionaryRef)props);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:300 height:300 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = sm == 0 ? MTLStorageModeShared : sm == 1 ? MTLStorageModeManaged : MTLStorageModePrivate;
            id<MTLTexture> tex = [dev newTextureWithDescriptor:td iosurface:surf plane:0];
            if (!tex) { printf("newTexture(iosurface) failed\n"); return 1; }
            printf("iosurface texture storageMode=%d\n", (int)tex.storageMode);
            fill(dev, tex, nil, td.storageMode, sync);
            IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
            uint8_t *p = IOSurfaceGetBaseAddress(surf);
            printf("CPU sees IOSurface pixel BGRA = %u %u %u %u (green would be 0 255 0 255)\n", p[150*IOSurfaceGetBytesPerRow(surf)+600], p[150*IOSurfaceGetBytesPerRow(surf)+601], p[150*IOSurfaceGetBytesPerRow(surf)+602], p[150*IOSurfaceGetBytesPerRow(surf)+603]);
            IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
            v.layer.contents = (__bridge id)surf;
        } else {
            CAMetalLayer *ml = [CAMetalLayer layer];
            ml.device = dev; ml.pixelFormat = MTLPixelFormatBGRA8Unorm; ml.drawableSize = CGSizeMake(300, 300); ml.framebufferOnly = YES;
            ml.frame = v.bounds;
            [v.layer addSublayer:ml];
            id<CAMetalDrawable> d = [ml nextDrawable];
            printf("drawable %s\n", d ? "ok" : "NULL");
            if (d) fill(dev, d.texture, d, MTLStorageModePrivate, false);
        }
        fflush(stdout);
        for (int i = 0; i < 300; i++) { [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]]; }
    }
    return 0;
}
