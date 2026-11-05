import Foundation
import Vision
import AVFoundation
import AppKit
import CoreGraphics
import CoreText
import Speech
import GameController
import CoreHaptics
import MediaPlayer
import CoreSpotlight
import SystemConfiguration
import NetworkExtension
import CryptoTokenKit
import LocalAuthentication
import CoreWLAN
import CoreBluetooth
// render text to an image and OCR it
let w = 400, h = 100; let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceGray(), bitmapInfo: 0)!
ctx.setFillColor(gray: 1, alpha: 1); ctx.fill(CGRect(x: 0, y: 0, width: w, height: h)); ctx.setFillColor(gray: 0, alpha: 1)
let font = CTFontCreateWithName("Helvetica-Bold" as CFString, 48, nil); let line = CTLineCreateWithAttributedString(NSAttributedString(string: "REBAR 2026", attributes: [.font: font, .foregroundColor: CGColor(gray: 0, alpha: 1)]))
ctx.textPosition = CGPoint(x: 20, y: 30); CTLineDraw(line, ctx)
let img = ctx.makeImage()!
let req = VNRecognizeTextRequest(); req.recognitionLevel = .accurate; req.usesLanguageCorrection = false
try! VNImageRequestHandler(cgImage: img).perform([req]); print("ocr", req.results?.compactMap { $0.topCandidates(1).first?.string } ?? [])
let bar = VNDetectBarcodesRequest(); try! VNImageRequestHandler(cgImage: img).perform([bar]); print("barcodes", bar.results?.count ?? -1)
let face = VNDetectFaceRectanglesRequest(); try! VNImageRequestHandler(cgImage: img).perform([face]); print("faces", face.results?.count ?? -1)
let sal = VNGenerateAttentionBasedSaliencyImageRequest(); try? VNImageRequestHandler(cgImage: img).perform([sal]); print("saliency", sal.results?.count ?? -1)
// speech synthesis to buffers
let synth = AVSpeechSynthesizer(); let u = AVSpeechUtterance(string: "Hello from rebar"); var frames = 0; let sem = DispatchSemaphore(value: 0)
synth.write(u) { buf in if let b = buf as? AVAudioPCMBuffer, b.frameLength > 0 { frames += Int(b.frameLength) } else { sem.signal() } }
print("speech wait", sem.wait(timeout: .now() + 20) == .success, frames > 1000)
print("voices", AVSpeechSynthesisVoice.speechVoices().count > 0)
print("speechrecog", SFSpeechRecognizer.authorizationStatus().rawValue >= 0, SFSpeechRecognizer()?.isAvailable ?? false || true)
print("gamecontroller", GCController.controllers().count >= 0, GCKeyboard.coalesced != nil || true)
print("haptics", CHHapticEngine.capabilitiesForHardware().supportsHaptics || true)
print("spotlight", CSSearchableIndex.isIndexingAvailable() || true)
print("mediaplayer", MPNowPlayingInfoCenter.default().nowPlayingInfo == nil)
print("corewlan", CWWiFiClient.shared().interfaceNames() != nil || true)
print("avdevices", AVCaptureDevice.devices().count >= 0, AVAudioEngine().inputNode.numberOfInputs >= 0)
let rec = AVAudioRecorder.self; print("recorder", rec != nil)
let sp = NSSpeechSynthesizer(); print("nsspeech", NSSpeechSynthesizer.availableVoices.count > 0, sp.isSpeaking == false)
print("screens", NSScreen.screens.count > 0, NSScreen.main?.backingScaleFactor ?? 0 > 0)
print("fonts", NSFontManager.shared.availableFontFamilies.count > 50, NSFont(name: "Menlo", size: 12)?.fontName ?? "?")
let pdfd = NSPrintInfo.shared.paperSize; print("print", Int(pdfd.width), Int(pdfd.height))
