import SwiftUI
import Charts
import Observation
import AppKit
import TipKit
@Observable final class M { var n = 0 }
struct P: Identifiable { let id: Int; let x: Double; let y: Double }
@MainActor func run() {
    let m = M()
    let v = VStack { Text("n=\(m.n)").font(.title); Rectangle().fill(.blue).frame(width: 40, height: 20)
        Chart((0..<8).map { P(id: $0, x: Double($0), y: Double(($0 * 5) % 7)) }) { LineMark(x: .value("x", $0.x), y: .value("y", $0.y)); PointMark(x: .value("x", $0.x), y: .value("y", $0.y)) }.frame(width: 160, height: 80)
        Gauge(value: 0.4) { Text("g") }.gaugeStyle(.accessoryLinear).frame(width: 100)
        Grid { GridRow { Text("a"); Text("b") } }
    }.padding().frame(width: 200)
    let r = ImageRenderer(content: v); r.scale = 1
    let cg = r.cgImage; print("render", cg?.width ?? -1, cg?.height ?? -1)
    if let cg { let rep = NSBitmapImageRep(cgImage: cg); var nonwhite = 0; for y in stride(from: 0, to: rep.pixelsHigh, by: 4) { for x in stride(from: 0, to: rep.pixelsWide, by: 4) { if let c = rep.colorAt(x: x, y: y), c.alphaComponent > 0.5 { nonwhite += 1 } } }; print("pixels", nonwhite > 50) }
    let host = NSHostingView(rootView: v); host.frame = NSRect(x: 0, y: 0, width: 200, height: 300); host.layoutSubtreeIfNeeded()
    print("hosting fit", Int(host.fittingSize.width), host.fittingSize.height > 100)
    let win = NSWindow(contentRect: host.frame, styleMask: [.titled], backing: .buffered, defer: false); win.contentView = host
    let bm = host.bitmapImageRepForCachingDisplay(in: host.bounds)!; host.cacheDisplay(in: host.bounds, to: bm); print("cacheDisplay", bm.pixelsWide)
    m.n = 5; print("observation", m.n)
    let tip = Tip_Probe(); print("tip", String(describing: type(of: tip.title)))
}
struct Tip_Probe: Tip { var title: Text { Text("hi") } }
MainActor.assumeIsolated { run() }
