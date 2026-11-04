import Foundation
import Metal
import MetalKit
import MetalPerformanceShaders
guard let dev = MTLCreateSystemDefaultDevice() else { print("no metal device"); exit(0) }
print("device ok", dev.name.isEmpty ? "unnamed" : "named")
let q = dev.makeCommandQueue()!
let src = """
#include <metal_stdlib>
using namespace metal;
kernel void addmul(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]], device float *o [[buffer(2)]], uint i [[thread_position_in_grid]]) { o[i] = a[i] * 2.0 + b[i]; }
kernel void fill(texture2d<float, access::write> t [[texture(0)]], uint2 g [[thread_position_in_grid]]) { t.write(float4(float(g.x) / 16.0, float(g.y) / 16.0, 0.5, 1), g); }
"""
let lib = try! dev.makeLibrary(source: src, options: nil)
print("functions", lib.functionNames.sorted())
let pso = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "addmul")!)
let n = 1024
let a = (0..<n).map { Float($0) }, b = (0..<n).map { Float($0 % 7) }
let ba = dev.makeBuffer(bytes: a, length: n * 4)!, bb = dev.makeBuffer(bytes: b, length: n * 4)!, bo = dev.makeBuffer(length: n * 4)!
var cb = q.makeCommandBuffer()!; var enc = cb.makeComputeCommandEncoder()!
enc.setComputePipelineState(pso); enc.setBuffer(ba, offset: 0, index: 0); enc.setBuffer(bb, offset: 0, index: 1); enc.setBuffer(bo, offset: 0, index: 2)
enc.dispatchThreads(MTLSize(width: n, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)); enc.endEncoding(); cb.commit(); cb.waitUntilCompleted()
let o = Array(UnsafeBufferPointer(start: bo.contents().assumingMemoryBound(to: Float.self), count: n))
print("compute", cb.status == .completed, o[0], o[10], o[1023], o == (0..<n).map { a[$0] * 2 + b[$0] })
let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 16, height: 16, mipmapped: false); td.usage = [.shaderWrite, .shaderRead]; td.storageMode = .shared
let tex = dev.makeTexture(descriptor: td)!
let p2 = try! dev.makeComputePipelineState(function: lib.makeFunction(name: "fill")!)
cb = q.makeCommandBuffer()!; enc = cb.makeComputeCommandEncoder()!; enc.setComputePipelineState(p2); enc.setTexture(tex, index: 0)
enc.dispatchThreads(MTLSize(width: 16, height: 16, depth: 1), threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)); enc.endEncoding(); cb.commit(); cb.waitUntilCompleted()
var px = [UInt8](repeating: 0, count: 16 * 16 * 4); tex.getBytes(&px, bytesPerRow: 64, from: MTLRegionMake2D(0, 0, 16, 16), mipmapLevel: 0)
print("texture", px[0..<4].map { Int($0) }, px[(5 * 16 + 9) * 4..<(5 * 16 + 9) * 4 + 4].map { Int($0) })
// render pass
let rsrc = """
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; float4 c; };
vertex V vs(uint id [[vertex_id]]) { float2 P[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) }; V v; v.p = float4(P[id], 0, 1); v.c = float4(1, 0.5, 0, 1); return v; }
fragment float4 fs(V v [[stage_in]]) { return v.c; }
"""
let rl = try! dev.makeLibrary(source: rsrc, options: nil)
let rd = MTLRenderPipelineDescriptor(); rd.vertexFunction = rl.makeFunction(name: "vs"); rd.fragmentFunction = rl.makeFunction(name: "fs"); rd.colorAttachments[0].pixelFormat = .rgba8Unorm
let rp = try! dev.makeRenderPipelineState(descriptor: rd)
let rtd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 8, height: 8, mipmapped: false); rtd.usage = [.renderTarget, .shaderRead]; rtd.storageMode = .managed
let rt = dev.makeTexture(descriptor: rtd)!
let pass = MTLRenderPassDescriptor(); pass.colorAttachments[0].texture = rt; pass.colorAttachments[0].loadAction = .clear; pass.colorAttachments[0].storeAction = .store; pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
cb = q.makeCommandBuffer()!; let re = cb.makeRenderCommandEncoder(descriptor: pass)!; re.setRenderPipelineState(rp); re.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3); re.endEncoding()
let bl = cb.makeBlitCommandEncoder()!; bl.synchronize(resource: rt); bl.endEncoding(); cb.commit(); cb.waitUntilCompleted()
var rpx = [UInt8](repeating: 0, count: 8 * 8 * 4); rt.getBytes(&rpx, bytesPerRow: 32, from: MTLRegionMake2D(0, 0, 8, 8), mipmapLevel: 0); print("render", rpx[0..<4].map { Int($0) }, cb.status == .completed)
print("mps supported", MPSSupportsMTLDevice(dev))
if MPSSupportsMTLDevice(dev) {
    let m1 = MPSMatrixDescriptor(rows: 4, columns: 4, rowBytes: 16, dataType: .float32)
    let ma = [Float](repeating: 1, count: 16), mb = (0..<16).map { Float($0) }
    let A = MPSMatrix(buffer: dev.makeBuffer(bytes: ma, length: 64)!, descriptor: m1), B = MPSMatrix(buffer: dev.makeBuffer(bytes: mb, length: 64)!, descriptor: m1), C = MPSMatrix(buffer: dev.makeBuffer(length: 64)!, descriptor: m1)
    cb = q.makeCommandBuffer()!; MPSMatrixMultiplication(device: dev, transposeLeft: false, transposeRight: false, resultRows: 4, resultColumns: 4, interiorColumns: 4, alpha: 1, beta: 0).encode(commandBuffer: cb, leftMatrix: A, rightMatrix: B, resultMatrix: C); cb.commit(); cb.waitUntilCompleted()
    print("mps gemm", Array(UnsafeBufferPointer(start: C.data.contents().assumingMemoryBound(to: Float.self), count: 4)))
}
