import Foundation
import AudioToolbox
import CoreAudio
import AVFoundation
import Network
import Security
import LocalAuthentication
import os
var asbd = AudioStreamBasicDescription(mSampleRate: 44100, mFormatID: kAudioFormatLinearPCM, mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, mBytesPerPacket: 4, mFramesPerPacket: 1, mBytesPerFrame: 4, mChannelsPerFrame: 2, mBitsPerChannel: 16, mReserved: 0)
var out = asbd; out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked; out.mBitsPerChannel = 32; out.mBytesPerFrame = 8; out.mBytesPerPacket = 8
var conv: AudioConverterRef?; print("converter", AudioConverterNew(&asbd, &out, &conv))
var pcm: [Int16] = [0, 0, 16384, -16384, 32767, 0]; var fl = [Float](repeating: 0, count: 6); var sz = UInt32(24)
let st = pcm.withUnsafeMutableBytes { p in fl.withUnsafeMutableBytes { f in AudioConverterConvertBuffer(conv!, 12, p.baseAddress!, &sz, f.baseAddress!) } }
print("convert", st, sz, fl.map { String(format: "%.2f", $0) })
var devs = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain); var dsz: UInt32 = 0
print("audio devices size", AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &devs, 0, nil, &dsz))
let eng = AVAudioEngine(); let fmt = AVAudioFormat(standardFormatWithSampleRate: 44100, channels: 1)!
let player = AVAudioPlayerNode(); eng.attach(player); eng.connect(player, to: eng.mainMixerNode, format: fmt)
try! eng.enableManualRenderingMode(.offline, format: fmt, maximumFrameCount: 1024); try! eng.start()
let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: 1024)!; buf.frameLength = 1024; for i in 0..<1024 { buf.floatChannelData![0][i] = sinf(Float(i) * 0.1) }
player.scheduleBuffer(buf, at: nil); player.play()
let outb = AVAudioPCMBuffer(pcmFormat: eng.manualRenderingFormat, frameCapacity: 512)!; let rs = try! eng.renderOffline(512, to: outb)
print("offline render", rs.rawValue, outb.frameLength, abs(outb.floatChannelData![0][5]) > 0.1)
let q = DispatchQueue(label: "net"); let sem = DispatchSemaphore(value: 0); var got = ""
let listener = try! NWListener(using: .tcp, on: .any)
listener.newConnectionHandler = { c in c.start(queue: q); c.receive(minimumIncompleteLength: 1, maximumLength: 100) { d, _, _, _ in got = String(data: d ?? Data(), encoding: .utf8) ?? ""; c.send(content: "pong".data(using: .utf8), completion: .contentProcessed { _ in }); } }
listener.stateUpdateHandler = { s in if case .ready = s { sem.signal() } }; listener.start(queue: q); sem.wait()
let conn = NWConnection(host: "127.0.0.1", port: listener.port!, using: .tcp); var reply = ""
conn.stateUpdateHandler = { s in if case .ready = s { conn.send(content: "ping".data(using: .utf8), completion: .contentProcessed { _ in }); conn.receive(minimumIncompleteLength: 1, maximumLength: 100) { d, _, _, _ in reply = String(data: d ?? Data(), encoding: .utf8) ?? ""; sem.signal() } } }
conn.start(queue: q); _ = sem.wait(timeout: .now() + 10); print("network", got, reply)
let attrs: [String: Any] = [kSecAttrKeyType as String: kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeySizeInBits as String: 256]
var err: Unmanaged<CFError>?; let key = SecKeyCreateRandomKey(attrs as CFDictionary, &err)!
let sig = SecKeyCreateSignature(key, .ecdsaSignatureMessageX962SHA256, Data("msg".utf8) as CFData, &err)!
print("seckey", SecKeyVerifySignature(SecKeyCopyPublicKey(key)!, .ecdsaSignatureMessageX962SHA256, Data("msg".utf8) as CFData, sig, &err), SecKeyVerifySignature(SecKeyCopyPublicKey(key)!, .ecdsaSignatureMessageX962SHA256, Data("other".utf8) as CFData, sig, &err))
var rnd = [UInt8](repeating: 0, count: 8); print("secrandom", SecRandomCopyBytes(kSecRandomDefault, 8, &rnd))
let la = LAContext(); var lerr: NSError?; _ = la.canEvaluatePolicy(.deviceOwnerAuthentication, error: &lerr); print("localauth ok")
let log = Logger(subsystem: "dev.rebar", category: "t"); log.info("hello \(42)"); print("oslog ok")
