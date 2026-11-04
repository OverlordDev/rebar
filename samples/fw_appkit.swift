import AppKit
import SwiftUI
let app = NSApplication.shared; app.setActivationPolicy(.prohibited)
let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 200, height: 100), styleMask: [.titled], backing: .buffered, defer: false)
let tf = NSTextField(labelWithString: "Hello rebar"); tf.frame = NSRect(x: 10, y: 10, width: 180, height: 24); w.contentView!.addSubview(tf)
let btn = NSButton(title: "OK", target: nil, action: nil); btn.frame = NSRect(x: 10, y: 50, width: 60, height: 24); w.contentView!.addSubview(btn)
print("views", w.contentView!.subviews.count, tf.stringValue, btn.title, w.frame.size)
let bmp = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 40, pixelsHigh: 40, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bmp); NSColor.red.setFill(); NSRect(x: 0, y: 0, width: 40, height: 40).fill(); NSColor.white.setFill(); NSBezierPath(ovalIn: NSRect(x: 10, y: 10, width: 20, height: 20)).fill(); NSGraphicsContext.restoreGraphicsState()
let c = bmp.colorAt(x: 0, y: 0)!; let m = bmp.colorAt(x: 20, y: 20)!; print("drawn", Int(c.redComponent * 255), Int(c.greenComponent * 255), Int(m.greenComponent * 255))
let png = bmp.representation(using: .png, properties: [:])!; print("png", png.count > 100, NSImage(data: png)!.size)
let str = NSAttributedString(string: "Metrics test", attributes: [.font: NSFont.systemFont(ofSize: 14)]); print("textsize", Int(str.size().width) > 40, NSFont.systemFont(ofSize: 14).pointSize)
let pb = NSPasteboard(name: NSPasteboard.Name("rebar.test")); pb.clearContents(); pb.setString("clip", forType: .string); print("pasteboard", pb.string(forType: .string)!)
let menu = NSMenu(); menu.addItem(withTitle: "Item", action: nil, keyEquivalent: "i"); print("menu", menu.items.count, menu.item(at: 0)!.keyEquivalent)
let hv = NSHostingView(rootView: VStack { Text("SwiftUI in AppKit"); Button("b") {} }.frame(width: 160, height: 80)); hv.frame = NSRect(x: 0, y: 0, width: 160, height: 80); hv.layoutSubtreeIfNeeded(); print("hosting", hv.fittingSize.width > 0, hv.frame.size)
let rep = hv.bitmapImageRepForCachingDisplay(in: hv.bounds)!; hv.cacheDisplay(in: hv.bounds, to: rep); var nonblank = 0; for x in stride(from: 0, to: 160, by: 4) { for y in stride(from: 0, to: 80, by: 4) { if let col = rep.colorAt(x: x, y: y), col.alphaComponent > 0 { nonblank += 1 } } }; print("rendered", nonblank > 10)
print("screen", NSScreen.screens.count >= 1, NSApp.isRunning, NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.finder") != nil)
let ud = UserDefaults(suiteName: "rebar.fw.test")!; ud.set([1, 2], forKey: "k"); ud.set("v", forKey: "s"); print("defaults", ud.array(forKey: "k") as! [Int], ud.string(forKey: "s")!); ud.removePersistentDomain(forName: "rebar.fw.test")
