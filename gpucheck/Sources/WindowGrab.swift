import Cocoa

// Looks at what WindowServer actually shows of a window: the windows of a process, a grab of one of them and a few numbers about the pixels.
// CGWindowListCreateImage is unavailable in the macOS 15 SDK but still in the system, so it is looked up at run time.
private typealias CreateImageFn = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?

struct WindowInfo {
    let id: UInt32
    let pid: pid_t
    let owner: String
    let title: String
    let bounds: CGRect
    let layer: Int
}

struct PixelStats {
    var width = 0, height = 0
    var distinctColors = 0       // colours that cover at least 0.1% of the sampled pixels
    var nonBlackFraction = 0.0   // sampled pixels that are not (nearly) black
    var meanRGB = (0.0, 0.0, 0.0)
    var summary: String {
        String(format: "%dx%d, %d dominant colours, %.0f%% non-black, mean RGB %.0f/%.0f/%.0f", width, height, distinctColors, nonBlackFraction * 100, meanRGB.0, meanRGB.1, meanRGB.2)
    }
    /// A window that shows a user interface has several colours and some light pixels; a black/blank one has one.
    var looksLikeContent: Bool { distinctColors >= 4 && nonBlackFraction > 0.05 }
}

enum WindowGrab {
    static func windows(pids: Set<pid_t>? = nil, minSize: CGFloat = 0) -> [WindowInfo] {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] else { return [] }
        var out: [WindowInfo] = []
        for w in list {
            let pid = pid_t((w[kCGWindowOwnerPID as String] as? Int) ?? 0)
            if let pids = pids, !pids.contains(pid) { continue }
            var r = CGRect.zero
            if let d = w[kCGWindowBounds as String] as? NSDictionary { CGRectMakeWithDictionaryRepresentation(d, &r) }
            if r.width < minSize || r.height < minSize { continue }
            out.append(WindowInfo(id: UInt32((w[kCGWindowNumber as String] as? Int) ?? 0), pid: pid, owner: (w[kCGWindowOwnerName as String] as? String) ?? "",
                                  title: (w[kCGWindowName as String] as? String) ?? "", bounds: r, layer: (w[kCGWindowLayer as String] as? Int) ?? 0))
        }
        return out
    }

    static func grab(_ id: UInt32) -> CGImage? {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else { return nil }   // RTLD_DEFAULT
        let fn = unsafeBitCast(sym, to: CreateImageFn.self)
        // kCGWindowListOptionIncludingWindow (8), kCGWindowImageBoundsIgnoreFraming (1)
        return fn(CGRect.null, 8, id, 1)?.takeRetainedValue()
    }

    static func stats(_ img: CGImage) -> PixelStats {
        var st = PixelStats(width: img.width, height: img.height)
        guard img.width > 0, img.height > 0 else { return st }
        let w = min(img.width, 400), h = min(img.height, 300)   // downscaled copy: cheap, and enough to see structure
        var buf = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &buf, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return st }
        ctx.interpolationQuality = .low
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        var hist: [UInt32: Int] = [:]
        var nonBlack = 0, sr = 0.0, sg = 0.0, sb = 0.0
        for i in 0..<(w * h) {
            let r = Int(buf[i * 4]), g = Int(buf[i * 4 + 1]), b = Int(buf[i * 4 + 2])
            sr += Double(r); sg += Double(g); sb += Double(b)
            if r + g + b > 24 { nonBlack += 1 }
            hist[UInt32(r >> 3) << 16 | UInt32(g >> 3) << 8 | UInt32(b >> 3), default: 0] += 1
        }
        let n = Double(w * h)
        st.distinctColors = hist.values.filter { Double($0) >= n * 0.001 }.count
        st.nonBlackFraction = Double(nonBlack) / n
        st.meanRGB = (sr / n, sg / n, sb / n)
        return st
    }

    static func savePNG(_ img: CGImage, to path: String) {
        let rep = NSBitmapImageRep(cgImage: img)
        if let data = rep.representation(using: .png, properties: [:]) { try? data.write(to: URL(fileURLWithPath: path)) }
    }

    /// How much of what Chromium rendered (`shot`, from DevTools) is also in what WindowServer shows (`grab`)?
    /// Both are reduced to a small grey picture; "salient" pixels differ clearly from the picture's most common grey (text, icons, buttons);
    /// the result is the share of the shot's salient pixels that have a salient pixel nearby in the grab. nil: the shot has too little content to judge.
    static func coverage(shot: CGImage, grab: CGImage) -> (salient: Double, coverage: Double)? {
        let W = 300, H = 200
        // The page spans the width of the window and sits at its bottom (a browser has its tab strip and toolbar above it). The grab may be at backing
        // resolution (2x on a HiDPI display) while the DevTools shot is not, or the other way round, so the region is found by scale, never by pixel count.
        let f = Double(grab.width) / Double(max(shot.width, 1))
        let sh = min(grab.height, max(1, Int((Double(shot.height) * f).rounded())))
        guard let region = grab.cropping(to: CGRect(x: 0, y: grab.height - sh, width: grab.width, height: sh)) else { return nil }
        func gray(_ img: CGImage) -> [UInt8]? {
            var buf = [UInt8](repeating: 0, count: W * H)
            guard let ctx = CGContext(data: &buf, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W, space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return nil }
            ctx.interpolationQuality = .medium
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: W, height: H))
            return buf
        }
        func salient(_ g: [UInt8]) -> [Bool] {
            var hist = [Int](repeating: 0, count: 256)
            for v in g { hist[Int(v)] += 1 }
            let mode = hist.enumerated().max { $0.element < $1.element }!.offset
            return g.map { abs(Int($0) - mode) > 10 }
        }
        guard let a = gray(shot), let b = gray(region) else { return nil }
        let sa = salient(a), sb = salient(b)
        let total = sa.filter { $0 }.count
        let frac = Double(total) / Double(W * H)
        guard frac > 0.003 else { return (frac, 0) }
        var hit = 0
        for y in 0..<H { for x in 0..<W where sa[y * W + x] {
            var found = false
            for dy in -2...2 where !found { for dx in -2...2 {
                let xx = x + dx, yy = y + dy
                if xx >= 0, xx < W, yy >= 0, yy < H, sb[yy * W + xx] { found = true; break }
            } }
            if found { hit += 1 }
        } }
        return (frac, Double(hit) / Double(total))
    }
}
