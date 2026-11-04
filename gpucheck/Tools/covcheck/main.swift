import Cocoa
// covcheck <pid> <port> <seconds>   poll: window grab of the pid vs DevTools page screenshot of the port
// covcheck png <grab.png> <shot.png> coverage of two saved pictures, also with either one scaled 2x (HiDPI check)
func load(_ p: String) -> CGImage { let s = CGImageSourceCreateWithURL(URL(fileURLWithPath: p) as CFURL, nil)!; return CGImageSourceCreateImageAtIndex(s, 0, nil)! }
func scaled(_ i: CGImage, _ f: Int) -> CGImage {
    let ctx = CGContext(data: nil, width: i.width * f, height: i.height * f, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    ctx.interpolationQuality = .high; ctx.draw(i, in: CGRect(x: 0, y: 0, width: i.width * f, height: i.height * f)); return ctx.makeImage()!
}
let a = CommandLine.arguments
if a[1] == "png" {
    let g = load(a[2]), s = load(a[3])
    for (n, gg, ss) in [("1x/1x", g, s), ("grab 2x", scaled(g, 2), s), ("shot 2x", g, scaled(s, 2)), ("both 2x", scaled(g, 2), scaled(s, 2))] {
        let c = WindowGrab.coverage(shot: ss, grab: gg)
        print(n, gg.width, "x", gg.height, "vs", ss.width, "x", ss.height, "->", c.map { String(format: "salient %.1f%% coverage %.0f%%", $0.salient * 100, $0.coverage * 100) } ?? "nil")
    }
    exit(0)
}
let pid = pid_t(a[1])!, port = Int(a[2])!, secs = Double(a[3]) ?? 30
let t0 = Date()
while Date().timeIntervalSince(t0) < secs {
    if let w = WindowGrab.windows(pids: [pid], minSize: 200).first, let g = WindowGrab.grab(w.id) {
        if let s = CDP.pageScreenshot(port: port), let c = WindowGrab.coverage(shot: s, grab: g) {
            print(String(format: "%.0fs window %dx%d vs page %dx%d: page salient %.1f%%, coverage %.0f%% | %@", Date().timeIntervalSince(t0), g.width, g.height, s.width, s.height, c.salient * 100, c.coverage * 100, WindowGrab.stats(g).summary))
            WindowGrab.savePNG(g, to: "/tmp/cov_grab.png"); WindowGrab.savePNG(s, to: "/tmp/cov_shot.png")
        } else { print("window yes, page screenshot no") }
    } else { print("no window yet") }
    Thread.sleep(forTimeInterval: 3)
}
