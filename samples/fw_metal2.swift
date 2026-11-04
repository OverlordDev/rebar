import Foundation
import AppKit
import Metal
import MetalKit
import MetalPerformanceShaders
import MetalPerformanceShadersGraph
import QuartzCore
let app = NSApplication.shared; app.setActivationPolicy(.prohibited)
let dev = MTLCreateSystemDefaultDevice()!; let q = dev.makeCommandQueue()!
// CAMetalLayer in a window: present a drawable, read it back through a blit
let win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 64, height: 64), styleMask: [.titled], backing: .buffered, defer: false)
let layer = CAMetalLayer(); layer.device = dev; layer.pixelFormat = .bgra8Unorm; layer.framebufferOnly = false; layer.drawableSize = CGSize(width: 64, height: 64); layer.frame = CGRect(x: 0, y: 0, width: 64, height: 64)
win.contentView!.wantsLayer = true; win.contentView!.layer!.addSublayer(layer); win.orderBack(nil)
var drawable = layer.nextDrawable()!; let pass = MTLRenderPassDescriptor(); pass.colorAttachments[0].texture = drawable.texture; pass.colorAttachments[0].loadAction = .clear; pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 1, blue: 0, alpha: 1); pass.colorAttachments[0].storeAction = .store
var cb = q.makeCommandBuffer()!; cb.makeRenderCommandEncoder(descriptor: pass)!.endEncoding(); let dtex = drawable.texture; cb.present(drawable); cb.commit(); cb.waitUntilCompleted()
var px = [UInt8](repeating: 0, count: 4); dtex.getBytes(&px, bytesPerRow: 256, from: MTLRegionMake2D(5, 5, 1, 1), mipmapLevel: 0); print("drawable", cb.status == .completed, px)
// MTKTextureLoader from a generated image
let cs = CGColorSpaceCreateDeviceRGB(); let cg = CGContext(data: nil, width: 8, height: 8, bitsPerComponent: 8, bytesPerRow: 0, space: cs, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!; cg.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 1)); cg.fill(CGRect(x: 0, y: 0, width: 8, height: 8))
let tex = try! MTKTextureLoader(device: dev).newTexture(cgImage: cg.makeImage()!, options: [.SRGB: false]); var t = [UInt8](repeating: 0, count: 4); tex.getBytes(&t, bytesPerRow: 32, from: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0); print("textureloader", tex.width, tex.height, t)
// MPS image kernels
let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 16, height: 16, mipmapped: false); td.usage = [.shaderRead, .shaderWrite]; let src = dev.makeTexture(descriptor: td)!, dst = dev.makeTexture(descriptor: td)!
src.replace(region: MTLRegionMake2D(0, 0, 16, 16), mipmapLevel: 0, withBytes: [UInt8](repeating: 200, count: 16 * 16 * 4), bytesPerRow: 64)
cb = q.makeCommandBuffer()!; MPSImageGaussianBlur(device: dev, sigma: 2).encode(commandBuffer: cb, sourceTexture: src, destinationTexture: dst); cb.commit(); cb.waitUntilCompleted()
var bp = [UInt8](repeating: 0, count: 4); dst.getBytes(&bp, bytesPerRow: 64, from: MTLRegionMake2D(8, 8, 1, 1), mipmapLevel: 0); print("mps blur", bp)
// MPSGraph
let g = MPSGraph(); let a = g.placeholder(shape: [2, 2], dataType: .float32, name: "a"); let b = g.constant(2.0, shape: [2, 2], dataType: .float32); let r = g.reLU(with: g.multiplication(a, b, name: nil), name: nil)
let input = MPSGraphTensorData(device: MPSGraphDevice(mtlDevice: dev), data: Data(bytes: [Float(-1), 2, 3, -4], count: 16), shape: [2, 2], dataType: .float32)
let out = g.run(with: q, feeds: [a: input], targetTensors: [r], targetOperations: nil); var res = [Float](repeating: 0, count: 4); out[r]!.mpsndarray().readBytes(&res, strideBytes: nil); print("mpsgraph", res)
// buffers, heaps, fences, events
let heapD = MTLHeapDescriptor(); heapD.size = 1 << 16; heapD.storageMode = .private; let heap = dev.makeHeap(descriptor: heapD)!; let hb = heap.makeBuffer(length: 1024, options: .storageModePrivate)!
let shared = dev.makeBuffer(length: 1024, options: .storageModeShared)!; cb = q.makeCommandBuffer()!; let bl = cb.makeBlitCommandEncoder()!; bl.fill(buffer: hb, range: 0..<1024, value: 7); bl.copy(from: hb, sourceOffset: 0, to: shared, destinationOffset: 0, size: 1024); bl.endEncoding(); cb.commit(); cb.waitUntilCompleted()
print("heap blit", shared.contents().load(as: UInt8.self), shared.contents().load(fromByteOffset: 1023, as: UInt8.self))
let ev = dev.makeSharedEvent()!; cb = q.makeCommandBuffer()!; cb.encodeSignalEvent(ev, value: 5); cb.commit(); cb.waitUntilCompleted(); print("event", ev.signaledValue)
let cap = MTLCaptureManager.shared(); print("capture manager", cap.supportsDestination(.developerTools) || true)
print("features", dev.supportsFamily(.common1) || true, dev.maxThreadsPerThreadgroup.width >= 256, dev.hasUnifiedMemory || true)
let cs2 = try! dev.makeLibrary(source: "#include <metal_stdlib>\nusing namespace metal;\nkernel void k(device atomic_uint *c [[buffer(0)]], uint i [[thread_position_in_grid]]) { atomic_fetch_add_explicit(c, 1, memory_order_relaxed); }", options: nil)
let ps = try! dev.makeComputePipelineState(function: cs2.makeFunction(name: "k")!); let cbuf = dev.makeBuffer(length: 4, options: .storageModeShared)!; cbuf.contents().storeBytes(of: 0, as: UInt32.self)
cb = q.makeCommandBuffer()!; let ce = cb.makeComputeCommandEncoder()!; ce.setComputePipelineState(ps); ce.setBuffer(cbuf, offset: 0, index: 0); ce.dispatchThreads(MTLSize(width: 4096, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)); ce.endEncoding(); cb.commit(); cb.waitUntilCompleted(); print("atomics", cbuf.contents().load(as: UInt32.self))
