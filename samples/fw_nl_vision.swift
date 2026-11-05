import Foundation
import NaturalLanguage
import CoreML
import Vision
import CoreGraphics
let text = "Apple opened a new store in London. Tim Cook said it was great!"
let tok = NLTokenizer(unit: .word); tok.string = text
var words: [String] = []; tok.enumerateTokens(in: text.startIndex..<text.endIndex) { r, _ in words.append(String(text[r])); return true }
print("words", words.count, words.prefix(4))
let sent = NLTokenizer(unit: .sentence); sent.string = text; print("sentences", sent.tokens(for: text.startIndex..<text.endIndex).count)
print("lang", NLLanguageRecognizer.dominantLanguage(for: "Bonjour tout le monde, comment allez-vous?")?.rawValue ?? "?")
let tagger = NLTagger(tagSchemes: [.nameType, .lexicalClass]); tagger.string = text
var names: [String] = []
tagger.enumerateTags(in: text.startIndex..<text.endIndex, unit: .word, scheme: .nameType, options: [.omitWhitespace, .omitPunctuation, .joinNames]) { t, r in if let t, t != .otherWord { names.append("\(text[r]):\(t.rawValue)") }; return true }
print("names", names)
tagger.setLanguage(.english, range: text.startIndex..<text.endIndex)
var verbs = 0; tagger.enumerateTags(in: text.startIndex..<text.endIndex, unit: .word, scheme: .lexicalClass, options: [.omitWhitespace]) { t, _ in if t == .verb { verbs += 1 }; return true }
print("verbs", verbs)
let emb = NLEmbedding.wordEmbedding(for: .english)
print("embedding", emb != nil, emb.map { String(format: "%.2f", $0.distance(between: "king", and: "queen")) } ?? "-")
let arr = try! MLMultiArray(shape: [2, 3], dataType: .float32); for i in 0..<6 { arr[i] = NSNumber(value: i) }
print("mlarray", arr.count, arr[5])
let prov = try! MLDictionaryFeatureProvider(dictionary: ["x": MLFeatureValue(double: 2)])
print("feature", prov.featureNames, prov.featureValue(for: "x")?.doubleValue ?? 0)
// Vision on a synthetic image: white rectangle on black
let w = 200, h = 200; let cs = CGColorSpaceCreateDeviceGray()
let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w, space: cs, bitmapInfo: 0)!
ctx.setFillColor(gray: 1, alpha: 1); ctx.fill(CGRect(x: 50, y: 60, width: 100, height: 80))
let img = ctx.makeImage()!
let req = VNDetectRectanglesRequest(); req.minimumConfidence = 0.5
try! VNImageRequestHandler(cgImage: img).perform([req])
print("rects", req.results?.count ?? -1)
let hist = VNGenerateImageFeaturePrintRequest()
try! VNImageRequestHandler(cgImage: img).perform([hist])
print("featureprint", hist.results?.count ?? -1)
