import Foundation
import AVFoundation
import VideoToolbox
import CoreMedia
import CoreVideo
import CoreImage
import MetalPerformanceShaders
import Metal
// Core Image
let ci = CIImage(color: CIColor(red: 1, green: 0, blue: 0)).cropped(to: CGRect(x: 0, y: 0, width: 32, height: 32))
let blur = ci.applyingFilter("CIGaussianBlur", parameters: [kCIInputRadiusKey: 2]).cropped(to: CGRect(x: 0, y: 0, width: 32, height: 32))
let cictx = CIContext(); var px = [UInt8](repeating: 0, count: 4); cictx.render(blur, toBitmap: &px, rowBytes: 4, bounds: CGRect(x: 16, y: 16, width: 1, height: 1), format: .RGBA8, colorSpace: CGColorSpaceCreateDeviceRGB()); print("coreimage", px)
print("filters", CIFilter.filterNames(inCategory: kCICategoryBlur).count > 3)
// VideoToolbox H264 encode of synthetic frames
var encoded = 0; var sessionOut: VTCompressionSession?
let st = VTCompressionSessionCreate(allocator: nil, width: 64, height: 64, codecType: kCMVideoCodecType_H264, encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &sessionOut)
print("vt create", st)
if let s = sessionOut {
    VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
    VTCompressionSessionPrepareToEncodeFrames(s)
    for i in 0..<5 {
        var pb: CVPixelBuffer?; CVPixelBufferCreate(nil, 64, 64, kCVPixelFormatType_32BGRA, nil, &pb)
        CVPixelBufferLockBaseAddress(pb!, []); memset(CVPixelBufferGetBaseAddress(pb!), Int32(i * 40), CVPixelBufferGetBytesPerRow(pb!) * 64); CVPixelBufferUnlockBaseAddress(pb!, [])
        VTCompressionSessionEncodeFrame(s, imageBuffer: pb!, presentationTimeStamp: CMTime(value: Int64(i), timescale: 30), duration: .invalid, frameProperties: nil, infoFlagsOut: nil) { status, _, buf in if status == 0, buf != nil { encoded += 1 } }
    }
    VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid); VTCompressionSessionInvalidate(s)
}
print("vt encoded", encoded > 0)
// AVAssetWriter -> AVAsset read back
let url = URL(fileURLWithPath: NSTemporaryDirectory() + "rebar_fw_media.mov"); try? FileManager.default.removeItem(at: url)
let w = try! AVAssetWriter(outputURL: url, fileType: .mov)
let inp = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: 64, AVVideoHeightKey: 64])
let ad = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: inp, sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: 64, kCVPixelBufferHeightKey as String: 64])
w.add(inp); w.startWriting(); w.startSession(atSourceTime: .zero)
for i in 0..<10 { var pb: CVPixelBuffer?; CVPixelBufferPoolCreatePixelBuffer(nil, ad.pixelBufferPool!, &pb); while !inp.isReadyForMoreMediaData { usleep(1000) }; ad.append(pb!, withPresentationTime: CMTime(value: Int64(i), timescale: 10)) }
inp.markAsFinished(); let sem = DispatchSemaphore(value: 0); w.finishWriting { sem.signal() }; sem.wait()
let asset = AVURLAsset(url: url); let dur = CMTimeGetSeconds(asset.duration); let tr = asset.tracks(withMediaType: .video)
print("avasset", w.status.rawValue, String(format: "%.1f", dur), tr.count, tr.first.map { Int($0.naturalSize.width) } ?? 0)
let gen = AVAssetImageGenerator(asset: asset); let im = try? gen.copyCGImage(at: CMTime(value: 3, timescale: 10), actualTime: nil); print("frame", im?.width ?? -1)
let rd = try! AVAssetReader(asset: asset); let ro = AVAssetReaderTrackOutput(track: tr[0], outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]); rd.add(ro); rd.startReading()
var frames = 0; while ro.copyNextSampleBuffer() != nil { frames += 1 }; print("decoded frames", frames); try? FileManager.default.removeItem(at: url)
// MPS
if let dev = MTLCreateSystemDefaultDevice() {
    print("mps supported", MPSSupportsMTLDevice(dev))
    let q = dev.makeCommandQueue()!; let a = MPSMatrixDescriptor(rows: 2, columns: 2, rowBytes: 8, dataType: .float32)
    let ba = dev.makeBuffer(bytes: [Float](arrayLiteral: 1, 2, 3, 4), length: 16)!, bb = dev.makeBuffer(bytes: [Float](arrayLiteral: 5, 6, 7, 8), length: 16)!, bc = dev.makeBuffer(length: 16)!
    let mm = MPSMatrixMultiplication(device: dev, transposeLeft: false, transposeRight: false, resultRows: 2, resultColumns: 2, interiorColumns: 2, alpha: 1, beta: 0)
    let cb = q.makeCommandBuffer()!; mm.encode(commandBuffer: cb, leftMatrix: MPSMatrix(buffer: ba, descriptor: a), rightMatrix: MPSMatrix(buffer: bb, descriptor: a), resultMatrix: MPSMatrix(buffer: bc, descriptor: a)); cb.commit(); cb.waitUntilCompleted()
    print("mps gemm", Array(UnsafeBufferPointer(start: bc.contents().assumingMemoryBound(to: Float.self), count: 4)))
}
