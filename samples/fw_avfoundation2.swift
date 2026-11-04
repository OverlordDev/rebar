import Foundation
import AVFoundation
import AVFAudio
import AudioToolbox
import CoreAudio
import CoreMedia
let sr = 48000.0
let fmt = AVAudioFormat(standardFormatWithSampleRate: sr, channels: 2)!
let url = URL(fileURLWithPath: NSTemporaryDirectory() + "rebar_avf2_\(getpid()).m4a")
do { let f = try AVAudioFile(forWriting: url, settings: [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: sr, AVNumberOfChannelsKey: 2, AVEncoderBitRateKey: 96000])
    let b = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: 48000)!; b.frameLength = 48000; for c in 0..<2 { for i in 0..<48000 { b.floatChannelData![c][i] = Float(sin(2 * .pi * 330 * Double(i) / sr)) * 0.4 } }; try f.write(from: b); print("aac write ok") } catch { print("aac error", error) }
let rf = try! AVAudioFile(forReading: url); print("aac read", rf.fileFormat.settings[AVFormatIDKey] as? UInt32 == kAudioFormatMPEG4AAC, abs(Int(rf.length) - 48000) < 3000, rf.fileFormat.channelCount)
// AVPlayer playback with a time observer
let player = AVPlayer(url: url); var ticks = 0; let obs = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 20), queue: .main) { _ in ticks += 1 }
let item = player.currentItem!; let ready = DispatchSemaphore(value: 0); var st: AVPlayerItem.Status = .unknown
let kv = item.observe(\.status, options: [.initial, .new]) { i, _ in st = i.status; if i.status != .unknown { ready.signal() } }
let end = Date().addingTimeInterval(20); while st == .unknown && Date() < end { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.02)) }; print("item status", st == .readyToPlay)
player.volume = 0; player.play(); let e2 = Date().addingTimeInterval(1.5); while Date() < e2 { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.02)) }
print("playing", player.rate > 0 || CMTimeGetSeconds(player.currentTime()) > 0.2, ticks > 3, CMTimeGetSeconds(item.duration) > 0.9); player.pause(); player.removeTimeObserver(obs); _ = kv
// composition + export
let comp = AVMutableComposition(); let a = AVURLAsset(url: url); let tr = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid)!
let src = try! await a.loadTracks(withMediaType: .audio).first!; try! tr.insertTimeRange(CMTimeRange(start: .zero, duration: CMTime(value: 24000, timescale: 48000)), of: src, at: .zero); try! tr.insertTimeRange(CMTimeRange(start: .zero, duration: CMTime(value: 24000, timescale: 48000)), of: src, at: CMTime(value: 24000, timescale: 48000))
let out = URL(fileURLWithPath: NSTemporaryDirectory() + "rebar_avf2_out_\(getpid()).m4a"); try? FileManager.default.removeItem(at: out)
let ex = AVAssetExportSession(asset: comp, presetName: AVAssetExportPresetAppleM4A)!; ex.outputURL = out; ex.outputFileType = .m4a; await ex.export(); print("export", ex.status == .completed, ((try? FileManager.default.attributesOfItem(atPath: out.path)[.size] as? Int) ?? 0) > 2000)
// reader
let oa = AVURLAsset(url: out); let ar = try! AVAssetReader(asset: oa); let ro = AVAssetReaderTrackOutput(track: try! await oa.loadTracks(withMediaType: .audio).first!, outputSettings: [AVFormatIDKey: kAudioFormatLinearPCM, AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false, AVLinearPCMIsNonInterleaved: false]); ar.add(ro); ar.startReading()
var samples = 0; while let sb = ro.copyNextSampleBuffer() { samples += CMSampleBufferGetNumSamples(sb) }; print("reader", samples > 40000 && samples < 56000, ar.status == .completed)
// speech synthesis to buffers
let synth = AVSpeechSynthesizer(); let utt = AVSpeechUtterance(string: "Hello"); var got = 0; let sd = DispatchSemaphore(value: 0)
synth.write(utt) { buf in if let p = buf as? AVAudioPCMBuffer { got += Int(p.frameLength); if p.frameLength == 0 { sd.signal() } } }; _ = sd.wait(timeout: .now() + 20); print("speech", got > 1000)
// audio units and CoreAudio
var desc = AudioComponentDescription(componentType: kAudioUnitType_Effect, componentSubType: kAudioUnitSubType_Delay, componentManufacturer: kAudioUnitManufacturer_Apple, componentFlags: 0, componentFlagsMask: 0); print("audio component", AudioComponentFindNext(nil, &desc) != nil)
let eng = AVAudioEngine(); let delay = AVAudioUnitDelay(); eng.attach(delay); print("unit effect", delay.delayTime > 0)
var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain); var sz: UInt32 = 0; print("coreaudio devices query", AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &sz) == 0)
let tap = AVAudioMixerNode(); eng.attach(tap); print("mixer", tap.outputVolume)
try? FileManager.default.removeItem(at: url); try? FileManager.default.removeItem(at: out)
