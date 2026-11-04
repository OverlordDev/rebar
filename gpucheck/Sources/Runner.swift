import Foundation
import AppKit
import CryptoKit

enum Status { case idle, running, ok, bad, skipped, error }

final class Test: ObservableObject, Identifiable {
    let id: String
    let title: String
    let detail: String
    @Published var status: Status = .idle
    @Published var summary = ""
    var shows: Bool?          // GUI tests: did WindowServer show a picture (nil: unknown or not run)
    var report = ""           // everything the report says about this test
    init(_ id: String, _ title: String, _ detail: String) { self.id = id; self.title = title; self.detail = detail }
}

struct GuiResult {
    var shows: Bool?
    var detail = ""
    var image: CGImage?
    var exit: Int32?
    var cdp: String?
    var stderrPath = ""
    var coverage: Double?     // share of what the page rendered that is visible in the window
    var shot: CGImage?
}

final class Runner: NSObject, ObservableObject, URLSessionDownloadDelegate {
    static let vscodeVersion = "1.144.0"
    static let vscodeURL = "https://update.code.visualstudio.com/\(vscodeVersion)/darwin-arm64/stable"

    @Published var tests: [Test] = [
        Test("core", "rebar работает на этой машине", "arm64-программа и бенчмарк через rebar дают те же результаты, что нативный x86_64"),
        Test("metal", "Metal → IOSurface → окно (маленькие тесты)", "Зелёный квадрат: arm64 через rebar и нативный x86_64, оба способа вывода"),
        Test("chrome", "Нативный Chrome с Metal (контроль)", "Тот же Chromium и те же флаги GPU, но без rebar"),
        Test("electron", "Нативное Electron-приложение с Metal (контроль)", "Electron, как VS Code, без rebar и с теми же флагами GPU (VS Code, Antigravity, Cursor… если установлены)"),
        Test("vsc_soft", "VS Code (arm64) через rebar, без GPU", "Контроль: обычный программный рендер"),
        Test("vsc_gpu", "VS Code (arm64) через rebar, с GPU", "Главный тест: --use-angle=metal"),
    ]
    @Published var logText = ""
    @Published var busy = false
    @Published var verdictText = ""
    @Published var electronApp: URL?
    @Published var electronNote = ""
    @Published var downloadProgress: Double?
    @Published var verbose = false
    @Published var screenAccess = CGPreflightScreenCaptureAccess()
    @Published var reportPath = ""
    var autoQuit = false   // --autorun: run everything, save the report, quit

    private let q = DispatchQueue(label: "rgc.run")
    private let logLock = NSLock()
    private var cancelled = false
    private var currentPid: pid_t = 0
    private var currentMarker: String?
    private var sysInfo = "collecting…"
    private let support: URL
    private var binDir: URL { support.appendingPathComponent("bin") }
    private var runDir: URL
    private var tmpDir: URL
    private var payloadZip: URL? {
        let u = Bundle.main.resourceURL?.appendingPathComponent("payload/VSCode-darwin-arm64.zip")
        return u.flatMap { FileManager.default.fileExists(atPath: $0.path) ? $0 : nil }
    }

    override init() {
        let fm = FileManager.default
        support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("RebarGPUCheck")
        let stamp = Runner.stamp()
        runDir = support.appendingPathComponent("runs/\(stamp)")
        tmpDir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("rgc-\(stamp)")
        super.init()
        try? fm.createDirectory(at: runDir, withIntermediateDirectories: true)
        try? fm.createDirectory(at: tmpDir, withIntermediateDirectories: true)
        NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil, queue: nil) { [weak self] _ in self?.stop() }
        q.async { [weak self] in
            guard let self = self else { return }
            self.prepareBin()
            let info = SysInfo.collect()
            DispatchQueue.main.async { self.sysInfo = info }
        }
        locateElectron()
    }

    static func stamp() -> String { let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"; return f.string(from: Date()) }

    // MARK: files

    private func prepareBin() {
        let fm = FileManager.default
        try? fm.createDirectory(at: binDir, withIntermediateDirectories: true)
        guard let src = Bundle.main.resourceURL?.appendingPathComponent("bin"), let names = try? fm.contentsOfDirectory(atPath: src.path) else { return }
        for n in names {
            let from = src.appendingPathComponent(n), to = binDir.appendingPathComponent(n)
            let a = (try? fm.attributesOfItem(atPath: from.path)[.size] as? Int) ?? -1, b = (try? fm.attributesOfItem(atPath: to.path)[.size] as? Int) ?? -2
            if a != b || (try? Data(contentsOf: from)) != (try? Data(contentsOf: to)) {
                try? fm.removeItem(at: to)
                try? fm.copyItem(at: from, to: to)
            }
            chmod(to.path, 0o755)
            removexattr(to.path, "com.apple.quarantine", 0)
        }
    }

    private func bin(_ n: String) -> String { binDir.appendingPathComponent(n).path }
    private var rebar: String { bin("rebar") }

    func log(_ s: String) {
        logLock.lock(); defer { logLock.unlock() }
        let line = s.hasSuffix("\n") ? s : s + "\n"
        if let h = try? FileHandle(forWritingTo: runDir.appendingPathComponent("run.log")) { h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); try? h.close() }
        else { try? line.write(to: runDir.appendingPathComponent("run.log"), atomically: false, encoding: .utf8) }
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.logText += line
            if self.logText.count > 200_000 { self.logText = String(self.logText.suffix(150_000)) }
        }
    }

    // MARK: the Electron app (VS Code) used by tests 4 and 5

    /// The CPU types in the app's main executable (read from the Mach-O header: /usr/bin/lipo would ask a machine without developer tools to install them).
    private func archs(of app: URL) -> String? {
        guard let exe = codeExecutable(app), let h = FileHandle(forReadingAtPath: exe) else { return nil }
        defer { try? h.close() }
        let d = [UInt8](h.readData(ofLength: 4096))
        guard d.count >= 16 else { return nil }
        func be(_ o: Int) -> UInt32 { UInt32(d[o]) << 24 | UInt32(d[o + 1]) << 16 | UInt32(d[o + 2]) << 8 | UInt32(d[o + 3]) }
        func le(_ o: Int) -> UInt32 { UInt32(d[o + 3]) << 24 | UInt32(d[o + 2]) << 16 | UInt32(d[o + 1]) << 8 | UInt32(d[o]) }
        func name(_ t: UInt32) -> String { t == 0x0100000C ? "arm64" : t == 0x01000007 ? "x86_64" : t == 0x0200000C ? "arm64_32" : "cpu\(t)" }
        switch be(0) {
        case 0xCAFEBABE, 0xCAFEBABF:
            let stride = be(0) == 0xCAFEBABE ? 20 : 32
            let n = Int(be(4))
            guard n > 0, n < 8, 8 + stride * n <= d.count else { return nil }
            return (0..<n).map { name(be(8 + stride * $0)) }.joined(separator: " ")
        case 0xCFFAEDFE: return name(le(4))
        default: return nil
        }
    }

    private func codeExecutable(_ app: URL) -> String? {
        guard let info = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")), let exe = info["CFBundleExecutable"] as? String else { return nil }
        return app.appendingPathComponent("Contents/MacOS/\(exe)").path
    }

    func locateElectron() {
        let fm = FileManager.default
        var candidates: [URL] = []
        if let p = UserDefaults.standard.string(forKey: "electronApp") { candidates.append(URL(fileURLWithPath: p)) }
        candidates.append(support.appendingPathComponent("vscode/Visual Studio Code.app"))
        for c in candidates where fm.fileExists(atPath: c.path) {
            if archs(of: c) == "arm64" { electronApp = c; electronNote = "\(c.lastPathComponent), arm64"; return }
        }
        electronApp = nil
        electronNote = payloadZip != nil ? "VS Code arm64 лежит внутри приложения, распакуется при первом тесте" : "VS Code arm64 не найден: скачайте или выберите"
    }

    func chooseElectron() {
        let p = NSOpenPanel()
        p.canChooseFiles = true; p.canChooseDirectories = false; p.allowedFileTypes = ["app"]
        p.message = "Выберите Electron-приложение для arm64 (например Visual Studio Code, версия Apple Silicon)"
        p.directoryURL = URL(fileURLWithPath: "/Applications")
        guard p.runModal() == .OK, let u = p.url else { return }
        let a = archs(of: u)
        if a == "arm64" { UserDefaults.standard.set(u.path, forKey: "electronApp"); locateElectron() }
        else { electronNote = "\(u.lastPathComponent): архитектуры \(a ?? "?"), нужна версия только для arm64" }
    }

    func downloadElectron() {
        guard downloadProgress == nil, let url = URL(string: Runner.vscodeURL) else { return }
        downloadProgress = 0
        electronNote = "Скачиваю VS Code \(Runner.vscodeVersion) (arm64)…"
        let s = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
        s.downloadTask(with: url).resume()
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64, totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard totalBytesExpectedToWrite > 0 else { return }
        let f = Double(totalBytesWritten) / Double(totalBytesExpectedToWrite)
        DispatchQueue.main.async { self.downloadProgress = f; self.electronNote = String(format: "Скачиваю VS Code: %.0f%% из %.0f МБ", f * 100, Double(totalBytesExpectedToWrite) / 1_048_576) }
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        let zip = support.appendingPathComponent("dl/vscode.zip")
        try? FileManager.default.createDirectory(at: zip.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: zip)
        do { try FileManager.default.moveItem(at: location, to: zip) } catch { DispatchQueue.main.async { self.electronNote = "Не удалось сохранить файл: \(error.localizedDescription)"; self.downloadProgress = nil }; return }
        DispatchQueue.main.async { self.electronNote = "Распаковываю…" }
        q.async {
            let ok = self.extract(zip)
            try? FileManager.default.removeItem(at: zip)
            DispatchQueue.main.async { self.downloadProgress = nil; self.locateElectron(); if !ok { self.electronNote = "Не удалось распаковать архив" } }
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let e = error { DispatchQueue.main.async { self.downloadProgress = nil; self.electronNote = "Ошибка загрузки: \(e.localizedDescription)" } }
    }

    @discardableResult private func extract(_ zip: URL) -> Bool {
        let dest = support.appendingPathComponent("vscode")
        try? FileManager.default.removeItem(at: dest)
        try? FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
        log("распаковка \(zip.lastPathComponent) → \(dest.path)")
        let r = Proc.captureOut("/usr/bin/ditto", ["-x", "-k", zip.path, dest.path], timeout: 900)
        return r.status == 0 && FileManager.default.fileExists(atPath: dest.appendingPathComponent("Visual Studio Code.app").path)
    }

    private func ensureElectron() -> URL? {
        if let e = electronApp { return e }
        if let z = payloadZip, extract(z) { DispatchQueue.main.sync { locateElectron() }; return electronApp }
        return nil
    }

    // MARK: running

    func runAll() { start(tests) }
    func runOnly(_ ids: [String]) { start(tests.filter { ids.contains($0.id) }) }
    func run(_ t: Test) { start([t]) }

    private func start(_ list: [Test]) {
        guard !busy else { return }
        if !CGPreflightScreenCaptureAccess() { _ = CGRequestScreenCaptureAccess() }
        screenAccess = CGPreflightScreenCaptureAccess()
        busy = true; cancelled = false; verdictText = ""
        for t in list { t.status = .idle; t.summary = ""; t.shows = nil; t.report = "" }
        q.async {
            for t in list where !self.cancelled { self.execute(t) }
            DispatchQueue.main.async {
                self.busy = false; self.verdictText = self.verdict(); self.log("\n" + self.verdictText)
                if self.autoQuit { self.saveReport(); DispatchQueue.main.asyncAfter(deadline: .now() + 6) { NSApp.terminate(nil) } }
            }
        }
    }

    func stop() {
        cancelled = true
        let pid = currentPid
        if pid > 0 { Proc.killTree(pid, marker: currentMarker) }
    }

    private func set(_ t: Test, _ s: Status, _ summary: String, report: String = "") {
        t.report = summary + (report.isEmpty ? "" : "\n" + report)
        DispatchQueue.main.async { t.status = s; t.summary = summary }
        log("[\(t.title)] \(summary)")
    }

    private func execute(_ t: Test) {
        DispatchQueue.main.async { t.status = .running; t.summary = "" }
        log("\n=== \(t.title)")
        switch t.id {
        case "core": testCore(t)
        case "metal": testMetal(t)
        case "chrome": testChrome(t)
        case "electron": testElectronNative(t)
        case "vsc_soft": testVSCode(t, gpu: false)
        default: testVSCode(t, gpu: true)
        }
    }

    // MARK: window-based runs

    private func askUser(_ message: String) -> Bool {
        var r = false
        DispatchQueue.main.sync {
            let a = NSAlert()
            a.messageText = message
            a.addButton(withTitle: "Да, вижу")
            a.addButton(withTitle: "Нет, пусто или чёрное")
            r = a.runModal() == .alertFirstButtonReturn
        }
        return r
    }

    private func guiRun(_ name: String, exe: String, args: [String], env: [String: String] = [:], timeout: TimeInterval, minWin: CGFloat, cdpPort: Int? = nil, marker: String? = nil,
                        manualSettle: TimeInterval = 20, blankIsUnknown: Bool = false, manualQuestion: String, accept: (PixelStats) -> Bool) -> GuiResult {
        var res = GuiResult()
        let out = runDir.appendingPathComponent("\(name).out").path, err = runDir.appendingPathComponent("\(name).err").path
        res.stderrPath = err
        guard let pid = Proc.spawn(exe, args, env: env, out: out, err: err) else { res.detail = "не удалось запустить \(exe)"; return res }
        currentPid = pid; currentMarker = marker
        defer { Proc.killTree(pid, marker: marker); currentPid = 0; currentMarker = nil }
        let t0 = Date()
        let canGrab = CGPreflightScreenCaptureAccess()
        var windowSince: Date?, lastStats: PixelStats?, windows = 0, heuristicSeen = false, nextShot = Date(), maxSalient = 0.0, badSince: Date?
        while Date().timeIntervalSince(t0) < timeout && !cancelled {
            if let st = Proc.exitStatus(pid) { res.exit = st; break }
            let ws = WindowGrab.windows(pids: [pid], minSize: minWin).sorted { $0.bounds.width * $0.bounds.height > $1.bounds.width * $1.bounds.height }
            windows = max(windows, ws.count)
            if let w = ws.first {
                if windowSince == nil { windowSince = Date() }
                if canGrab {
                    if let img = WindowGrab.grab(w.id) {
                        let st = WindowGrab.stats(img)
                        lastStats = st; res.image = img
                        if accept(st) { heuristicSeen = true }
                        if let port = cdpPort {
                            // is the window showing what the page rendered? (a window can show only the solid-colour layers, or nothing)
                            if Date() >= nextShot {
                                nextShot = Date().addingTimeInterval(3)
                                if let shot = CDP.pageScreenshot(port: port) {
                                    let fresh = WindowGrab.grab(w.id) ?? img   // the shot took a while: compare with a grab made right after it
                                    guard let c = WindowGrab.coverage(shot: shot, grab: fresh) else { continue }
                                    res.shot = shot; res.image = fresh; res.coverage = c.coverage; maxSalient = max(maxSalient, c.salient)
                                    log(String(format: "  [%@ %.0f с] страница нарисовала %.1f%% значимых пикселей, в окне видно %.0f%% из них", name, Date().timeIntervalSince(t0), c.salient * 100, c.coverage * 100))
                                    // the page has painted but the window stays wrong for a long time: no need to wait for the full timeout
                                    if c.salient > 0.03 && c.coverage < 0.6 { if badSince == nil { badSince = Date() } } else { badSince = nil }
                                    if let b = badSince, Date().timeIntervalSince(b) > 45 { break }
                                    if c.salient > 0.003 && c.coverage >= 0.6 {
                                        res.shows = true
                                        res.detail = String(format: "окно показывает то, что нарисовала страница (%.0f%% значимых пикселей видно), через %.0f с", c.coverage * 100, Date().timeIntervalSince(t0))
                                        break
                                    }
                                }
                            }
                        } else if accept(st) {
                            res.shows = true; res.detail = String(format: "окно показало картинку через %.0f с (%@)", Date().timeIntervalSince(t0), st.summary); break
                        }
                    }
                } else if Date().timeIntervalSince(windowSince!) > manualSettle {
                    res.shows = askUser(manualQuestion)
                    res.detail = "ответ пользователя (нет доступа к записи экрана): " + (res.shows! ? "картинка есть" : "пусто")
                    break
                }
            }
            Thread.sleep(forTimeInterval: 1.5)
        }
        if res.shows == nil && res.exit == nil && !cancelled && windows > 0 && canGrab {
            if let c = res.coverage, maxSalient > 0.003 {
                res.shows = false
                res.detail = String(format: "страница нарисовала картинку (%.1f%% значимых пикселей), но окно показывает только %.0f%% её (нужно ≥60%%) за %d с — остальное не выводится на экран. Окно: %@", maxSalient * 100, c * 100, Int(Date().timeIntervalSince(t0)), lastStats?.summary ?? "?")
            } else if res.coverage != nil {
                // the page's own screenshot is empty: for a control app that only means it had not painted anything (a login page, a slow start) and says nothing about the display
                res.shows = blankIsUnknown ? nil : false
                res.detail = "по данным DevTools страница сама ничего не нарисовала (скриншот страницы пустой)" + (blankIsUnknown ? ", сравнивать не с чем: повторите тест." : ".") + " Окно: " + (lastStats?.summary ?? "?")
            } else if heuristicSeen && cdpPort != nil {
                res.shows = true   // no DevTools to compare with: the window is not blank
                res.detail = "окно не пустое (сравнить со страницей через DevTools не удалось) — " + (lastStats?.summary ?? "")
            } else {
                res.shows = false
                res.detail = "окно есть, но за \(Int(timeout)) с так и не показало картинку (" + (lastStats?.summary ?? "?") + ")"
            }
        } else if res.shows == nil {
            if let e = res.exit { res.detail = "процесс завершился с кодом \(e) до появления картинки" }
            else if cancelled { res.detail = "остановлено" }
            else if windows == 0 { res.detail = "окно не появилось за \(Int(timeout)) с" }
        }
        if let port = cdpPort, res.exit == nil, !cancelled {
            for _ in 0..<3 { if let s = CDP.gpuSummary(port: port) { res.cdp = s; break }; Thread.sleep(forTimeInterval: 2) }
        }
        if let img = res.image { WindowGrab.savePNG(img, to: runDir.appendingPathComponent("\(name).png").path) }
        if let shot = res.shot { WindowGrab.savePNG(shot, to: runDir.appendingPathComponent("\(name)_page.png").path) }
        return res
    }

    private func tail(_ path: String, filter: String? = nil, max: Int = 60) -> String {
        guard var text = try? String(contentsOfFile: path, encoding: .utf8) else { return "" }
        if text.utf8.count > 3_000_000 { text = String(text.suffix(3_000_000)) }
        var lines = text.components(separatedBy: "\n")
        if let f = filter, let re = try? NSRegularExpression(pattern: f, options: [.caseInsensitive]) {
            lines = lines.filter { re.firstMatch(in: $0, range: NSRange($0.startIndex..., in: $0)) != nil }
        }
        return lines.suffix(max).joined(separator: "\n")
    }

    private func verdictFor(_ r: GuiResult) -> (Status, Bool?) {
        if r.shows == true { return (.ok, true) }
        if r.shows == false { return (.bad, false) }
        return (.error, nil)
    }

    // MARK: the tests

    private func testCore(_ t: Test) {
        let t0 = Date()
        let hello = Proc.captureOut(rebar, [bin("hello_arm64")], timeout: 30)
        let helloOK = hello.out.contains("hello arm64")
        let a = Proc.captureOut(rebar, [bin("bench_mix_arm"), "1"], timeout: 180)
        let n = Proc.captureOut(bin("bench_mix_x86"), ["1"], timeout: 60)
        let same = !a.out.isEmpty && a.out == n.out
        let dt = Date().timeIntervalSince(t0)
        let detail = "hello: \(helloOK ? "ok" : "FAIL (\(hello.out.prefix(100)) \(hello.err.prefix(300)))")\nbench via rebar:\n\(a.out)\(a.err.prefix(500))\nbench native:\n\(n.out)"
        if helloOK && same { set(t, .ok, String(format: "arm64-программы под rebar работают, результаты совпадают с нативными (%.1f с)", dt), report: detail) }
        else { set(t, .bad, helloOK ? "бенчмарк через rebar дал другие результаты, чем нативный" : "rebar не смог запустить простую arm64-программу", report: detail) }
        t.shows = nil
    }

    private func testMetal(_ t: Test) {
        guard CGPreflightScreenCaptureAccess() else { set(t, .skipped, "пропущено: нужен доступ к записи экрана (Системные настройки → Конфиденциальность)"); return }
        func green(_ s: PixelStats) -> Bool { s.meanRGB.1 > 150 && s.meanRGB.0 < 90 && s.meanRGB.2 < 90 }
        var lines: [String] = [], good = 0, total = 0
        let cases: [(String, String, [String], Bool)] = [
            ("ios", "arm64 через rebar, IOSurface как contents слоя", [bin("ios_arm64"), "ios"], true),
            ("ios", "нативный x86_64, IOSurface как contents слоя", [bin("ios_x86_64"), "ios"], false),
            ("metal", "arm64 через rebar, CAMetalLayer", [bin("ios_arm64"), "metal"], true),
            ("metal", "нативный x86_64, CAMetalLayer", [bin("ios_x86_64"), "metal"], false),
        ]
        for (i, c) in cases.enumerated() where !cancelled {
            total += 1
            let exe = c.3 ? rebar : c.2[0]
            let args = c.3 ? c.2 : Array(c.2.dropFirst())
            let r = guiRun("metal\(i)", exe: exe, args: args, timeout: 25, minWin: 100, manualQuestion: "", accept: green)
            let ok = r.shows == true
            if ok { good += 1 }
            lines.append("\(ok ? "OK  " : "FAIL") \(c.1): \(r.detail) | \(tail(r.stderrPath, filter: "iosurface|pixel|drawable|fail", max: 4).replacingOccurrences(of: "\n", with: " / "))")
            log(lines.last!)
        }
        set(t, good == total ? .ok : .bad, "\(good) из \(total) вариантов показали зелёный квадрат", report: lines.joined(separator: "\n"))
        t.shows = good == total
    }

    private func findBrowser() -> (String, String)? {
        for n in ["Google Chrome", "Microsoft Edge", "Brave Browser", "Chromium", "Vivaldi", "Opera"] {
            let app = URL(fileURLWithPath: "/Applications/\(n).app")
            if let exe = codeExecutable(app), let a = archs(of: app), a.contains("x86_64") { return (n, exe) }
        }
        return nil
    }

    /// A home directory of its own for the apps under test: VS Code restored the tester's last folder (and would show their files in the report's screenshots)
    /// through state kept under the real home, even with its own --user-data-dir.
    private func isolatedHome(_ name: String) -> [String: String] {
        let h = tmpDir.appendingPathComponent("home_\(name)").path
        try? FileManager.default.createDirectory(atPath: h, withIntermediateDirectories: true)
        return ["HOME": h, "CFFIXED_USER_HOME": h]
    }

    private func findNativeElectron() -> (String, String)? {
        let fm = FileManager.default
        for n in ["Visual Studio Code", "Visual Studio Code - Insiders", "VSCodium", "Cursor", "Antigravity", "Windsurf", "Discord", "Slack", "Notion", "Obsidian"] {
            let app = URL(fileURLWithPath: "/Applications/\(n).app")
            guard fm.fileExists(atPath: app.appendingPathComponent("Contents/Frameworks/Electron Framework.framework").path),
                  let exe = codeExecutable(app), let a = archs(of: app), a.contains("x86_64") else { continue }
            return (n, exe)
        }
        return nil
    }

    private func testElectronNative(_ t: Test) {
        guard let (name, exe) = findNativeElectron() else {
            set(t, .skipped, "пропущено: не нашёл в /Applications Electron-приложение с x86_64 (VS Code, Antigravity, Cursor, Discord…)"); return
        }
        let prof = tmpDir.appendingPathComponent("electron").path
        let r = guiRun("electron", exe: exe, args: ["--user-data-dir=\(prof)", "--extensions-dir=\(prof)/ext", "--use-angle=metal", "--ignore-gpu-blocklist", "--enable-gpu-rasterization",
                                                    "--remote-debugging-port=9444"], env: isolatedHome("electron"),
                       timeout: 90, minWin: 300, cdpPort: 9444, marker: prof, manualSettle: 15, blankIsUnknown: true, manualQuestion: "Видно ли в окне приложения «\(name)» нормальный интерфейс (текст, значки), а не пустой фон?", accept: { $0.looksLikeContent })
        try? FileManager.default.removeItem(atPath: prof)
        let (st, shows) = verdictFor(r)
        t.shows = shows
        set(t, st, "\(name): " + (r.detail.isEmpty ? "—" : r.detail), report: "GPU по данным приложения:\n\(r.cdp ?? "(не удалось получить)")")
        if let c = r.cdp { log(c) }
    }

    private func testChrome(_ t: Test) {
        guard let (name, exe) = findBrowser() else { set(t, .skipped, "пропущено: нет Chrome/Edge/Brave в /Applications"); return }
        let prof = tmpDir.appendingPathComponent("chrome").path
        let page = "data:text/html,<body style='background:%23127;color:%23fff;font:60px sans-serif'>CONTROL <span style='color:%23fa0'>orange</span></body>"
        let r = guiRun("chrome", exe: exe, args: ["--user-data-dir=\(prof)", "--use-angle=metal", "--ignore-gpu-blocklist", "--enable-gpu-rasterization", "--remote-debugging-port=9441",
                                                  "--no-first-run", "--no-default-browser-check", "--window-position=60,60", "--window-size=900,600", page],
                       timeout: 60, minWin: 300, cdpPort: 9441, marker: prof, blankIsUnknown: true, manualQuestion: "Видно ли в окне браузера тёмно-синий фон и надпись CONTROL orange?", accept: { $0.looksLikeContent })
        try? FileManager.default.removeItem(atPath: prof)
        let (st, shows) = verdictFor(r)
        t.shows = shows
        set(t, st, "\(name): " + (r.detail.isEmpty ? "—" : r.detail), report: "GPU по данным браузера:\n\(r.cdp ?? "(не удалось получить)")")
        if let c = r.cdp { log(c) }
    }

    private func testVSCode(_ t: Test, gpu: Bool) {
        guard let app = ensureElectron(), let code = codeExecutable(app) else { set(t, .skipped, "пропущено: нет VS Code для arm64 (кнопки «Скачать» / «Выбрать» вверху)"); return }
        let name = gpu ? "vscode_gpu" : "vscode_soft"
        let prof = tmpDir.appendingPathComponent(name).path
        let port = gpu ? 9443 : 9442
        var args = [code, "--no-sandbox", "--use-mock-keychain", "--user-data-dir=\(prof)", "--remote-debugging-port=\(port)", "--enable-logging=stderr"]
        args += gpu ? ["--disable-gpu-sandbox", "--use-angle=metal", "--ignore-gpu-blocklist", "--enable-gpu-rasterization"] : ["--disable-gpu"]
        var env = isolatedHome(name)
        env["REBAR_FAULT_LOG"] = runDir.appendingPathComponent("\(name).faults").path
        if verbose && gpu { env["REBAR_GPU_SYNC"] = "2"; env["REBAR_TRACE_FILE"] = runDir.appendingPathComponent("\(name).trace").path }
        log("запуск: rebar \(args.joined(separator: " ").replacingOccurrences(of: NSHomeDirectory(), with: "~"))")
        let r = guiRun(name, exe: rebar, args: args, env: env, timeout: 170, minWin: 300, cdpPort: port, marker: prof, manualSettle: 45,
                       manualQuestion: "Видны ли в окне VS Code текст и значки (слева Explorer, в центре приветствие с логотипом, справа чат)? Только тёмный фон или одно всплывающее уведомление — это «Нет».", accept: { $0.looksLikeContent })
        try? FileManager.default.removeItem(atPath: prof)
        let (st, shows) = verdictFor(r)
        t.shows = shows
        let faults = (try? String(contentsOfFile: runDir.appendingPathComponent("\(name).faults").path, encoding: .utf8)) ?? ""
        let hl = tail(r.stderrPath, filter: "gpu|angle|metal|iosurface|fatal|crash|error|fail|unsupported", max: 80)
        set(t, st, r.detail.isEmpty ? "—" : r.detail, report: "GPU по данным Chromium внутри rebar:\n\(r.cdp ?? "(не удалось получить)")\n\nфатальные ошибки rebar:\n\(faults.isEmpty ? "(нет)" : String(faults.prefix(4000)))\n\nстрочки лога про GPU:\n\(hl)")
        if let c = r.cdp { log(c) }
    }

    // MARK: verdict and report

    private func shows(_ id: String) -> Bool? { tests.first { $0.id == id }?.shows }
    private func status(_ id: String) -> Status { tests.first { $0.id == id }?.status ?? .idle }

    func verdict() -> String {
        let gpu = shows("vsc_gpu"), soft = shows("vsc_soft"), chrome = shows("chrome"), electron = shows("electron")
        guard status("vsc_gpu") != .idle, status("vsc_gpu") != .skipped, status("vsc_gpu") != .running else { return "Главный тест (VS Code с GPU) не выполнялся." }
        func s(_ b: Bool?) -> String { b == nil ? "нет данных" : (b! ? "картинка есть" : "пусто") }
        let table = "Нативный Chrome с Metal: \(s(chrome)); нативный Electron с Metal: \(s(electron)); VS Code через rebar без GPU: \(s(soft)); VS Code через rebar с GPU: \(s(gpu))."
        var t: String
        if gpu == true {
            t = "Вывод: GPU-режим через rebar на этой видеокарте РАБОТАЕТ: окно показывает то, что нарисовала страница."
        } else if gpu == false {
            if electron == false || (electron == nil && chrome == false) {
                t = "Вывод: даже нативное приложение без rebar (Electron/Chrome) с теми же флагами Metal не показывает картинку. Значит проблема на уровне драйвера или системы этой машины, а не rebar."
            } else if electron == true && soft == true {
                t = "Вывод: нативный Electron с GPU и VS Code через rebar без GPU работают, а VS Code через rebar с GPU страницу рисует, но окно её не показывает. Значит причина в связке rebar ↔ Metal/CoreAnimation, а не в драйвере этой видеокарты."
            } else if chrome == true && soft == true {
                t = "Вывод: нативный Chrome и программный режим работают, а VS Code через rebar с GPU страницу рисует, но окно её не показывает. Скорее всего причина в rebar, а не в драйвере (нативного Electron для контроля не нашлось)."
            } else { t = "Вывод: GPU-режим через rebar не показал картинку, но контрольные тесты неполные. Нужен отчёт целиком." }
        } else { t = "Вывод: результат главного теста неопределён (процесс упал или окно не появилось). Нужен отчёт целиком." }
        return table + "\n" + t
    }

    func buildReport() -> String {
        var s = "Rebar GPU Check — report\nDate: \(Date())\nApp: \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] ?? "?")\n"
        if let d = try? Data(contentsOf: URL(fileURLWithPath: rebar)) { s += "rebar: \(d.count) bytes, sha256 \(SHA256.hash(data: d).map { String(format: "%02x", $0) }.joined())\n" }
        s += "Screen recording access: \(CGPreflightScreenCaptureAccess())\n\n"
        s += "RESULT\n\(verdict())\n\n"
        for t in tests { s += "TEST \(t.id): \(t.title)\n  status: \(t.status), shows picture: \(t.shows.map { String($0) } ?? "n/a")\n\(t.report.split(separator: "\n", omittingEmptySubsequences: false).map { "  " + $0 }.joined(separator: "\n"))\n\n" }
        s += "SYSTEM\n\(sysInfo)\n"
        return s.replacingOccurrences(of: NSHomeDirectory(), with: "~").replacingOccurrences(of: NSUserName(), with: "user")
    }

    func saveReport() {
        let fm = FileManager.default
        try? buildReport().write(to: runDir.appendingPathComponent("report.txt"), atomically: true, encoding: .utf8)
        let desktop = fm.urls(for: .desktopDirectory, in: .userDomainMask)[0]
        let zip = desktop.appendingPathComponent("RebarGPUCheck-report-\(Runner.stamp()).zip")
        q.async {
            let r = Proc.captureOut("/usr/bin/ditto", ["-c", "-k", "--keepParent", self.runDir.path, zip.path], timeout: 120)
            DispatchQueue.main.async {
                if r.status == 0 { self.reportPath = zip.path; NSWorkspace.shared.activateFileViewerSelecting([zip]) }
                else { self.log("не удалось создать zip: \(r.err)") }
            }
        }
    }

    func copyReport() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(buildReport(), forType: .string)
    }
}
