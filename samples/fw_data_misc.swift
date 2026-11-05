import Foundation
import Contacts
import EventKit
import Compression
import ImageIO
import UniformTypeIdentifiers
import CoreGraphics
let c = CNMutableContact(); c.givenName = "Ada"; c.familyName = "Lovelace"; c.phoneNumbers = [CNLabeledValue(label: CNLabelWork, value: CNPhoneNumber(stringValue: "+1 555 0100"))]
let vcard = try! CNContactVCardSerialization.data(with: [c]); print("vcard", String(data: vcard, encoding: .utf8)!.contains("FN:Ada Lovelace"))
print("fmt", CNContactFormatter.string(from: c, style: .fullName) ?? "?")
let back = try! CNContactVCardSerialization.contacts(with: vcard); print("roundtrip", back.first?.familyName ?? "?")
let addr = CNMutablePostalAddress(); addr.city = "London"; addr.country = "UK"
print("postal", CNPostalAddressFormatter.string(from: addr, style: .mailingAddress).replacingOccurrences(of: "\n", with: "|"))
let ev = EKEventStore.authorizationStatus(for: .event); print("ek status ok", ev.rawValue >= 0)
let comp = Data(repeating: 65, count: 5000)
let packed = (try! (comp as NSData).compressed(using: .lzfse)) as Data; print("lzfse", packed.count < 200, (try! (packed as NSData).decompressed(using: .lzfse) as Data).count)
for alg in [COMPRESSION_ZLIB, COMPRESSION_LZ4, COMPRESSION_LZMA] {
    var out = [UInt8](repeating: 0, count: 6000); let n = comp.withUnsafeBytes { compression_encode_buffer(&out, 6000, $0.bindMemory(to: UInt8.self).baseAddress!, 5000, nil, alg) }
    var dec = [UInt8](repeating: 0, count: 6000); let m = compression_decode_buffer(&dec, 6000, out, n, nil, alg); print("alg", alg.rawValue, n < 400, m)
}
let w = 16, h = 16; let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
ctx.setFillColor(red: 1, green: 0.5, blue: 0, alpha: 1); ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
let img = ctx.makeImage()!
for t in [UTType.png, .jpeg, .tiff, .gif, .heic] {
    let d = NSMutableData(); if let dest = CGImageDestinationCreateWithData(d, t.identifier as CFString, 1, nil) { CGImageDestinationAddImage(dest, img, nil); let ok = CGImageDestinationFinalize(dest)
        let src = CGImageSourceCreateWithData(d, nil); print("imageio", t.preferredFilenameExtension ?? "?", ok, src.flatMap { CGImageSourceGetCount($0) } ?? -1, src.flatMap { CGImageSourceCreateImageAtIndex($0, 0, nil)?.width } ?? -1) } else { print("imageio", t.identifier, "no encoder") }
}
print("uttype", UTType("public.mp3")?.conforms(to: .audio) ?? false, UTType(filenameExtension: "swift")?.identifier ?? "?")
