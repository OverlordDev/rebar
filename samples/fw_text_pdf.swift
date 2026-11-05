import Foundation
import CoreText
import CoreGraphics
import PDFKit
import AppKit
let attr = NSAttributedString(string: "Hello, rebar text layout!", attributes: [.font: NSFont(name: "Helvetica", size: 14)!])
let line = CTLineCreateWithAttributedString(attr)
var asc: CGFloat = 0, desc: CGFloat = 0, lead: CGFloat = 0
let width = CTLineGetTypographicBounds(line, &asc, &desc, &lead)
print("line", String(format: "%.1f %.1f %.1f", width, asc, desc), CTLineGetGlyphCount(line))
let fs = CTFramesetterCreateWithAttributedString(attr)
let size = CTFramesetterSuggestFrameSizeWithConstraints(fs, CFRange(location: 0, length: 0), nil, CGSize(width: 80, height: 1000), nil)
print("suggest", Int(size.width), Int(size.height))
let path = CGPath(rect: CGRect(x: 0, y: 0, width: 80, height: 1000), transform: nil)
let frame = CTFramesetterCreateFrame(fs, CFRange(location: 0, length: 0), path, nil)
print("lines", (CTFrameGetLines(frame) as! [CTLine]).count)
let desc2 = CTFontDescriptorCreateWithNameAndSize("Courier" as CFString, 12)
let f = CTFontCreateWithFontDescriptor(desc2, 12, nil)
print("font", CTFontCopyFamilyName(f), CTFontGetSize(f), CTFontGetGlyphCount(f) > 100)
// PDF
let data = NSMutableData()
var box = CGRect(x: 0, y: 0, width: 300, height: 200)
let ctx = CGContext(consumer: CGDataConsumer(data: data)!, mediaBox: &box, nil)!
ctx.beginPDFPage(nil); ctx.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 1)); ctx.fill(CGRect(x: 10, y: 10, width: 50, height: 50))
CTLineDraw(line, ctx); ctx.endPDFPage(); ctx.closePDF()
let doc = PDFDocument(data: data as Data)!
print("pdf pages", doc.pageCount, doc.page(at: 0)!.bounds(for: .mediaBox).size, doc.string?.contains("rebar") ?? false)
let page = PDFPage(image: NSImage(size: NSSize(width: 50, height: 50), flipped: false) { r in NSColor.blue.setFill(); r.fill(); return true })!
doc.insert(page, at: 1); print("pdf pages", doc.pageCount)
let sel = doc.findString("rebar", withOptions: []); print("find", sel.count)
