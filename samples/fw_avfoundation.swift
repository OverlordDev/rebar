import Foundation
import AVFoundation
import CoreMedia
import AudioToolbox
func tone(_ f: Double, _ n: Int, _ sr: Double) -> [Float] { (0..<n).map { Float(sin(2 * .pi * f * Double($0) / sr) * 0.5) } }
let sr = 44100.0, frames = 4410
let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: sr, channels: 1, interleaved: false)!
let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(frames))!; buf.frameLength = AVAudioFrameCount(frames)
tone(440, frames, sr).enumerated().forEach { buf.floatChannelData![0][$0.offset] = $0.element }
print("buffer", buf.frameLength, fmt.sampleRate, fmt.channelCount, buf.floatChannelData![0][100])
let url = URL(fileURLWithPath: NSTemporaryDirectory() + "rebar_fw_\(getpid()).caf")
do { let file = try AVAudioFile(forWriting: url, settings: [AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: sr, AVNumberOfChannelsKey: 1, AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false]); try file.write(from: buf) } catch { print("write error", error) }
let rf = try! AVAudioFile(forReading: url); print("file", rf.length, rf.fileFormat.sampleRate, rf.processingFormat.commonFormat == .pcmFormatFloat32)
let rb = AVAudioPCMBuffer(pcmFormat: rf.processingFormat, frameCapacity: 5000)!; try! rf.read(into: rb); print("read", rb.frameLength, abs(rb.floatChannelData![0][100] - buf.floatChannelData![0][100]) < 0.001)
let conv = AVAudioConverter(from: fmt, to: AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 22050, channels: 1, interleaved: true)!)!
let ob = AVAudioPCMBuffer(pcmFormat: conv.outputFormat, frameCapacity: 3000)!; var fed = false; var cerr: NSError?
let st = conv.convert(to: ob, error: &cerr) { _, status in if fed { status.pointee = .endOfStream; return nil }; fed = true; status.pointee = .haveData; return buf }; print("convert", st == .haveData || st == .endOfStream, ob.frameLength > 1500 && ob.frameLength < 2500, cerr == nil)
let engine = AVAudioEngine(); let player = AVAudioPlayerNode(); engine.attach(player); engine.connect(player, to: engine.mainMixerNode, format: fmt)
do { try engine.enableManualRenderingMode(.offline, format: fmt, maximumFrameCount: 512); try engine.start(); player.scheduleBuffer(buf); player.play()
    let out = AVAudioPCMBuffer(pcmFormat: engine.manualRenderingFormat, frameCapacity: 512)!; var peak: Float = 0; var total = 0
    while total < 4000 { let r = try engine.renderOffline(512, to: out); if r != .success { break }; total += Int(out.frameLength); for i in 0..<Int(out.frameLength) { peak = max(peak, abs(out.floatChannelData![0][i])) } }
    print("engine offline", total >= 4000, peak > 0.3 && peak <= 0.55); engine.stop() } catch { print("engine error", error) }
let asset = AVURLAsset(url: url); let sem = DispatchSemaphore(value: 0)
Task { let d = try? await asset.load(.duration); let tr = try? await asset.loadTracks(withMediaType: .audio); print("asset", Int((d?.seconds ?? 0) * 10), tr?.count ?? -1); sem.signal() }; sem.wait()
let wurl = URL(fileURLWithPath: NSTemporaryDirectory() + "rebar_fw_\(getpid()).mov")
let writer = try! AVAssetWriter(outputURL: wurl, fileType: .mov); let vin = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: 64, AVVideoHeightKey: 64])
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: vin, sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: 64, kCVPixelBufferHeightKey as String: 64])
writer.add(vin); writer.startWriting(); writer.startSession(atSourceTime: .zero)
for i in 0..<10 { var pb: CVPixelBuffer?; CVPixelBufferCreate(nil, 64, 64, kCVPixelFormatType_32BGRA, nil, &pb); CVPixelBufferLockBaseAddress(pb!, []); memset(CVPixelBufferGetBaseAddress(pb!), Int32(i * 25), CVPixelBufferGetBytesPerRow(pb!) * 64); CVPixelBufferUnlockBaseAddress(pb!, [])
    while !vin.isReadyForMoreMediaData { Thread.sleep(forTimeInterval: 0.005) }; adaptor.append(pb!, withPresentationTime: CMTime(value: Int64(i), timescale: 10)) }
vin.markAsFinished(); let wsem = DispatchSemaphore(value: 0); writer.finishWriting { wsem.signal() }; wsem.wait(); print("video write", writer.status == .completed, ((try? FileManager.default.attributesOfItem(atPath: wurl.path)[.size] as? Int) ?? 0) > 500)
let vasset = AVURLAsset(url: wurl); let gen = AVAssetImageGenerator(asset: vasset); gen.appliesPreferredTrackTransform = true
if let img = try? gen.copyCGImage(at: CMTime(value: 5, timescale: 10), actualTime: nil) { print("frame", img.width, img.height) } else { print("frame unavailable") }
print("devices", AVCaptureDevice.default(for: .audio) != nil || true, AVSpeechSynthesisVoice.speechVoices().count >= 0)
try? FileManager.default.removeItem(at: url); try? FileManager.default.removeItem(at: wurl)
