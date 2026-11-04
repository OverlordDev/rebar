import Foundation
import CoreGraphics
import CoreText
import ImageIO
import UniformTypeIdentifiers
import CoreImage
func checksum(_ d: Data) -> UInt32 { var h: UInt32 = 2166136261; for b in d { h = (h ^ UInt32(b)) &* 16777619 }; return h }
let w = 64, h = 48
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: cs, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
ctx.setFillColor(CGColor(red: 0.1, green: 0.4, blue: 0.9, alpha: 1)); ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
ctx.setFillColor(CGColor(red: 1, green: 0.8, blue: 0, alpha: 1)); ctx.fillEllipse(in: CGRect(x: 10, y: 8, width: 40, height: 30))
ctx.setStrokeColor(CGColor(gray: 1, alpha: 1)); ctx.setLineWidth(3); ctx.move(to: CGPoint(x: 0, y: 0)); ctx.addCurve(to: CGPoint(x: 64, y: 48), control1: CGPoint(x: 64, y: 0), control2: CGPoint(x: 0, y: 48)); ctx.strokePath()
let img = ctx.makeImage()!; let px = Data(bytes: ctx.data!, count: ctx.bytesPerRow * h)
print("bitmap", w, h, ctx.bytesPerRow, String(checksum(px), radix: 16))
let gctx = CGContext(data: nil, width: 32, height: 32, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceGray(), colors: [CGColor(gray: 0, alpha: 1), CGColor(gray: 1, alpha: 1)] as CFArray, locations: [0, 1])!
gctx.drawLinearGradient(grad, start: .zero, end: CGPoint(x: 32, y: 0), options: []); print("gradient", String(checksum(Data(bytes: gctx.data!, count: gctx.bytesPerRow * 32)), radix: 16))
let out = NSMutableData(); let dst = CGImageDestinationCreateWithData(out, UTType.png.identifier as CFString, 1, nil)!; CGImageDestinationAddImage(dst, img, nil); print("png", CGImageDestinationFinalize(dst), out.length > 100)
let src = CGImageSourceCreateWithData(out as CFData, nil)!; let back = CGImageSourceCreateImageAtIndex(src, 0, nil)!; print("decoded", back.width, back.height, CGImageSourceGetType(src)! as String)
let jp = NSMutableData(); let jd = CGImageDestinationCreateWithData(jp, UTType.jpeg.identifier as CFString, 1, nil)!; CGImageDestinationAddImage(jd, img, [kCGImageDestinationLossyCompressionQuality: 0.8] as CFDictionary); print("jpeg", CGImageDestinationFinalize(jd), jp.length > 100)
let font = CTFontCreateWithName("Helvetica" as CFString, 24, nil); let att = NSAttributedString(string: "Rebar test", attributes: [NSAttributedString.Key(kCTFontAttributeName as String): font])
let line = CTLineCreateWithAttributedString(att); let b = CTLineGetBoundsWithOptions(line, []); print("text", Int(b.width), Int(b.height), CTLineGetGlyphCount(line), CTFontGetSize(font))
let fs = CTFramesetterCreateWithAttributedString(att); let fr = CTFramesetterCreateFrame(fs, CFRange(location: 0, length: 0), CGPath(rect: CGRect(x: 0, y: 0, width: 80, height: 100), transform: nil), nil); print("lines", (CTFrameGetLines(fr) as! [CTLine]).count)
let ci = CIContext(options: [.useSoftwareRenderer: true]); let f = CIFilter(name: "CIGaussianBlur", parameters: [kCIInputImageKey: CIImage(cgImage: img), kCIInputRadiusKey: 2.0])!
let outCI = f.outputImage!.cropped(to: CGRect(x: 0, y: 0, width: w, height: h)); let rendered = ci.createCGImage(outCI, from: outCI.extent)!; print("coreimage", rendered.width, rendered.height)
let pdfData = NSMutableData(); var box = CGRect(x: 0, y: 0, width: 200, height: 100); let pdf = CGContext(consumer: CGDataConsumer(data: pdfData)!, mediaBox: &box, nil)!; pdf.beginPDFPage(nil); pdf.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 1)); pdf.fill(CGRect(x: 10, y: 10, width: 50, height: 50)); pdf.endPDFPage(); pdf.closePDF()
let pdoc = CGPDFDocument(CGDataProvider(data: pdfData)!)!; print("pdf", pdoc.numberOfPages, pdoc.page(at: 1)!.getBoxRect(.mediaBox).size)
let path = CGMutablePath(); path.addRoundedRect(in: CGRect(x: 0, y: 0, width: 10, height: 10), cornerWidth: 2, cornerHeight: 2); print("path", path.boundingBox, path.contains(CGPoint(x: 5, y: 5)))
print("affine", CGAffineTransform(rotationAngle: .pi / 2).concatenating(CGAffineTransform(translationX: 5, y: 0)).a.rounded(), CGPoint(x: 1, y: 0).applying(CGAffineTransform(rotationAngle: .pi / 2)).y)
