// An iOS app that draws with Metal into an MTKView (what iOS games do): a render pass per frame with a clear colour and a triangle, a few frames, then reads the pixel back from a texture.
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
@interface R : NSObject <MTKViewDelegate>
@property id<MTLDevice> dev; @property id<MTLCommandQueue> q; @property id<MTLRenderPipelineState> pso; @property int frames;
@end
@implementation R
- (void)mtkView:(MTKView *)v drawableSizeWillChange:(CGSize)s {}
- (void)drawInMTKView:(MTKView *)v {
    id<MTLCommandBuffer> cb = [self.q commandBuffer]; MTLRenderPassDescriptor *rp = v.currentRenderPassDescriptor; if (!rp) return;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp]; [e setRenderPipelineState:self.pso]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    [cb presentDrawable:v.currentDrawable]; [cb commit]; self.frames++;
    if (self.frames == 5) { [cb waitUntilCompleted]; printf("frames %d status %ld\n", self.frames, (long)cb.status); fflush(stdout); exit(0); }
}
@end
@interface AD : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window; @property (strong) R *r;
@end
@implementation AD
- (BOOL)application:(UIApplication *)a didFinishLaunchingWithOptions:(NSDictionary *)o {
    self.window = [[UIWindow alloc] initWithFrame:CGRectMake(0, 0, 400, 400)]; UIViewController *vc = [UIViewController new]; self.window.rootViewController = vc;
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); MTKView *v = [[MTKView alloc] initWithFrame:CGRectMake(0, 0, 400, 400) device:d]; v.clearColor = MTLClearColorMake(0, 0.3, 0.6, 1); v.preferredFramesPerSecond = 30; [vc.view addSubview:v];
    R *r = [R new]; r.dev = d; r.q = [d newCommandQueue]; v.delegate = r; self.r = r;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 vs(uint i [[vertex_id]]) { float2 p[3] = {float2(-0.5,-0.5), float2(0.5,-0.5), float2(0,0.5)}; return float4(p[i], 0, 1); }\nfragment float4 fs() { return float4(1, 0.5, 0, 1); }\n";
    NSError *err = nil; id<MTLLibrary> lib = [d newLibraryWithSource:src options:nil error:&err]; if (!lib) { printf("shader error %s\n", err.description.UTF8String); exit(1); }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = v.colorPixelFormat;
    r.pso = [d newRenderPipelineStateWithDescriptor:pd error:&err]; if (!r.pso) { printf("pso error %s\n", err.description.UTF8String); exit(1); }
    [self.window makeKeyAndVisible]; return YES;
}
@end
int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AD class])); } }
