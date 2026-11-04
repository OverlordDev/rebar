import Foundation
import AppKit

struct AppEntry: Identifiable, Hashable {
    let path: String
    var id: String { path }
    let name: String
    let version: String
    let exe: String
    let engine: String?      // "Electron" or "Chromium" for an app built on Chromium (it takes Chromium's switches), else nil
    var isElectron: Bool { engine != nil }
    var ios: IOSInfo? = nil  // an iOS app (an .ipa or an unpacked iOS .app): path is its .app folder (Info.plist and the executable lie directly in it)
}

/// What is known about an iOS app from its executable.
struct IOSInfo: Hashable {
    let usesUI: Bool         // links UIKit or SwiftUI: needs rebar-ios (the Mac Catalyst UIKit) in a host .app bundle
    let encrypted: Bool      // FairPlay (LC_ENCRYPTION_INFO cryptid 1): the code cannot be read, nothing here decrypts it
}

/// What is set for one application. The defaults are what works best for Electron apps (VS Code and friends).
struct AppSettings: Codable, Equatable {
    var gpu = true            // Chromium: draw on the video card (Metal)
    var noSandbox = true      // Chromium: no sandbox (it cannot be set up under rebar)
    var mockKeychain = true   // Chromium: do not ask for the Keychain password
    var jit = true            // rebar's translator; off = plain interpreter (slow, for troubleshooting)
    var args = ""             // extra command-line arguments
    var env = ""              // extra environment variables, one NAME=value per line
    var fastEditor = true     // VS Code and its relatives: editor settings that make scrolling cheaper, 60 fps instead of 50 (see FastEditor); on by default, off in the app's settings

    init() {}
    init(from d: Decoder) throws {   // (a settings.json written by an older version has no fastEditor)
        let c = try d.container(keyedBy: Keys.self)
        gpu = try c.decodeIfPresent(Bool.self, forKey: .gpu) ?? true
        noSandbox = try c.decodeIfPresent(Bool.self, forKey: .noSandbox) ?? true
        mockKeychain = try c.decodeIfPresent(Bool.self, forKey: .mockKeychain) ?? true
        jit = try c.decodeIfPresent(Bool.self, forKey: .jit) ?? true
        args = try c.decodeIfPresent(String.self, forKey: .args) ?? ""
        env = try c.decodeIfPresent(String.self, forKey: .env) ?? ""
        fastEditor = try c.decodeIfPresent(Bool.self, forKey: .fastEditor) ?? true
    }
    private enum Keys: String, CodingKey { case gpu, noSandbox, mockKeychain, jit, args, env, fastEditor }
}

private struct StoreFile: Codable {
    var extra: [String] = []                       // apps and folders the user added
    var apps: [String: AppSettings] = [:]          // by app path
}

final class Launcher: NSObject, ObservableObject {
    @Published var apps: [AppEntry] = []
    @Published var running: [String: pid_t] = [:]
    @Published var windowless = Set<String>()     // running apps none of whose processes has a window (yet)
    @Published var scanning = false
    @Published var message = ""
    private(set) var icons: [String: NSImage] = [:]

    private let support: URL
    private let logs: URL
    private var store = StoreFile()
    private var started: [String: Date] = [:]
    private var timer: Timer?
    fileprivate var owned = Set<String>()                              // apps this launcher started itself (its children: waitpid tells how they ended)
    fileprivate var instance: [String: (pids: [pid_t], groups: [pid_t])] = [:]   // every process found for a running app, by app path (also those left over from an earlier launcher)
    private var ticks = 0, scanningInstances = false
    static let markerCache = PidCache()
    private var rebar: String { support.appendingPathComponent("bin/rebar").path }
    private var rebarIOS: String { support.appendingPathComponent("bin/rebar-ios").path }
    private var iosHostRoot: String {   // (proc_pidpath gives the real path)
        let p = support.appendingPathComponent("ios-host").path
        var buf = [CChar](repeating: 0, count: Int(PATH_MAX))
        return (realpath(p, &buf).map { String(cString: $0) } ?? p) + "/"
    }
    private var ipaRoot: String { support.appendingPathComponent("ipa").path }

    override init() {
        let fm = FileManager.default
        if let d = ProcessInfo.processInfo.environment["RL_SUPPORT"] { support = URL(fileURLWithPath: d) }   // (for tests)
        else { support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("RebarLauncher") }
        logs = fm.urls(for: .libraryDirectory, in: .userDomainMask)[0].appendingPathComponent("Logs/RebarLauncher")
        super.init()
        try? fm.createDirectory(at: support, withIntermediateDirectories: true)
        try? fm.createDirectory(at: logs, withIntermediateDirectories: true)
        if let d = try? Data(contentsOf: support.appendingPathComponent("settings.json")), let s = try? JSONDecoder().decode(StoreFile.self, from: d) { store = s }
        prepareRebar()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.poll() }
        scan()
    }

    // MARK: rebar

    /// rebar travels inside the app; it runs from a copy in Application Support (an app that came from the internet may be moved to a read-only place).
    private func prepareRebar() {
        let fm = FileManager.default
        guard let src = Bundle.main.url(forResource: "rebar", withExtension: nil, subdirectory: "bin") else { message = "rebar is not in the app."; return }
        let dst = support.appendingPathComponent("bin/rebar")
        try? fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
        if (try? Data(contentsOf: src)) != (try? Data(contentsOf: dst)) { try? fm.removeItem(at: dst); try? fm.copyItem(at: src, to: dst) }
        chmod(dst.path, 0o755)
        removexattr(dst.path, "com.apple.quarantine", 0)
        if let ios = Bundle.main.url(forResource: "rebar-ios", withExtension: nil, subdirectory: "bin") {   // rebar as a Mac Catalyst process, for iOS apps that use UIKit
            let idst = dst.deletingLastPathComponent().appendingPathComponent("rebar-ios")
            if (try? Data(contentsOf: ios)) != (try? Data(contentsOf: idst)) { try? fm.removeItem(at: idst); try? fm.copyItem(at: ios, to: idst); chmod(idst.path, 0o755); removexattr(idst.path, "com.apple.quarantine", 0) }
        }
        if let sec = Bundle.main.url(forResource: "librebar_sec.dylib", withExtension: nil, subdirectory: "bin") {   // the keychain of iOS apps
            let sdst = dst.deletingLastPathComponent().appendingPathComponent("librebar_sec.dylib")
            if (try? Data(contentsOf: sec)) != (try? Data(contentsOf: sdst)) { try? fm.removeItem(at: sdst); try? fm.copyItem(at: sec, to: sdst); removexattr(sdst.path, "com.apple.quarantine", 0) }
        }
        if let shim = Bundle.main.url(forResource: "librebar_exec.dylib", withExtension: nil, subdirectory: "bin") {   // (rebar looks for it next to itself)
            let sdst = dst.deletingLastPathComponent().appendingPathComponent("librebar_exec.dylib")
            if (try? Data(contentsOf: shim)) != (try? Data(contentsOf: sdst)) { try? fm.removeItem(at: sdst); try? fm.copyItem(at: shim, to: sdst) }
            removexattr(sdst.path, "com.apple.quarantine", 0)
        }
    }

    // MARK: finding the apps

    func scan() {
        scanning = true
        let extra = store.extra
        DispatchQueue.global().async {
            let fm = FileManager.default
            var roots = ["/Applications", NSHomeDirectory() + "/Applications"]
            var direct: [String] = []
            for e in extra { if e.hasSuffix(".app") || e.hasSuffix(".ipa") { direct.append(e) } else { roots.append(e) } }
            var candidates = direct
            var ipas: [String] = direct.filter { $0.hasSuffix(".ipa") }
            candidates.removeAll { $0.hasSuffix(".ipa") }
            for r in roots {
                for n in (try? fm.contentsOfDirectory(atPath: r)) ?? [] {
                    let p = r + "/" + n
                    if n.hasSuffix(".app") { candidates.append(p) }
                    else if n.hasSuffix(".ipa") { ipas.append(p) }
                    else {   // one folder level down (/Applications/Utilities, a vendor folder, ...)
                        var isDir: ObjCBool = false
                        if fm.fileExists(atPath: p, isDirectory: &isDir), isDir.boolValue {
                            for m in (try? fm.contentsOfDirectory(atPath: p)) ?? [] where m.hasSuffix(".app") { candidates.append(p + "/" + m) }
                        }
                    }
                }
            }
            for ipa in ipas { if let app = self.unpackIPA(ipa) { candidates.append(app) } }
            var found: [AppEntry] = [], icons: [String: NSImage] = [:], seen = Set<String>()
            for p in candidates where seen.insert(p).inserted {
                if !fm.fileExists(atPath: p + "/Contents/Info.plist"), let e = self.iosEntry(p) { found.append(e); icons[p] = NSWorkspace.shared.icon(for: .application); continue }   // (an iOS app: Info.plist in the folder itself)
                guard let info = NSDictionary(contentsOfFile: p + "/Contents/Info.plist"), let exeName = info["CFBundleExecutable"] as? String else { continue }
                let exe = p + "/Contents/MacOS/" + exeName
                let archs = machoArchs(atPath: exe)
                guard archs.contains("arm64"), !archs.contains("x86_64") else { continue }   // only what this Mac cannot run by itself
                let name = ((p as NSString).lastPathComponent as NSString).deletingPathExtension   // as Finder shows it (VS Code's CFBundleDisplayName is just "Code")
                found.append(AppEntry(path: p, name: name, version: (info["CFBundleShortVersionString"] as? String) ?? "", exe: exe, engine: Launcher.chromiumEngine(p)))
                icons[p] = NSWorkspace.shared.icon(forFile: p)
            }
            found.sort { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
            DispatchQueue.main.async { self.apps = found; self.icons = icons; self.scanning = false }
        }
    }

    // MARK: iOS apps (.ipa)

    /// Unpacks an .ipa (once per file and modification time) into Application Support and returns the Payload/*.app folder, or nil.
    private func unpackIPA(_ ipa: String) -> String? {
        let fm = FileManager.default
        let mtime = ((try? fm.attributesOfItem(atPath: ipa))?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        let dest = ipaRoot + "/" + Proc.appID(ipa + String(mtime))
        let payload = dest + "/Payload"
        if !fm.fileExists(atPath: dest + "/.done") {
            try? fm.removeItem(atPath: dest)
            try? fm.createDirectory(atPath: dest, withIntermediateDirectories: true)
            let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/unzip"); p.arguments = ["-q", "-o", ipa, "-d", dest]
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
            guard (try? p.run()) != nil else { return nil }
            p.waitUntilExit()
            guard p.terminationStatus <= 1 else { return nil }
            fm.createFile(atPath: dest + "/.done", contents: nil)
        }
        return ((try? fm.contentsOfDirectory(atPath: payload)) ?? []).first { $0.hasSuffix(".app") }.map { payload + "/" + $0 }
    }

    /// The entry of an iOS app folder (flat: Info.plist and the executable inside), or nil when it is not one that rebar can run.
    private func iosEntry(_ p: String) -> AppEntry? {
        guard let info = NSDictionary(contentsOfFile: p + "/Info.plist"), let exeName = info["CFBundleExecutable"] as? String else { return nil }
        let exe = p + "/" + exeName
        guard let m = machoInfo(atPath: exe), m.archs.contains("arm64") else { return nil }
        let name = (info["CFBundleDisplayName"] as? String) ?? (info["CFBundleName"] as? String) ?? exeName
        return AppEntry(path: p, name: name, version: (info["CFBundleShortVersionString"] as? String) ?? "", exe: exe, engine: nil, ios: IOSInfo(usesUI: m.usesUI, encrypted: m.encrypted))
    }

    /// UIKit (Mac Catalyst) only runs in a process that lives in an .app: its scene server looks the client up by bundle. So an iOS app that uses UIKit runs from a small host bundle:
    /// Contents/MacOS/<Name> is rebar-ios, Contents/Resources is the iOS .app folder (nibs, assets), Contents/Info.plist is the app's with the executable renamed. Returns the host executable.
    private func hostBundle(_ app: AppEntry) -> String? {
        let fm = FileManager.default
        let name = (app.exe as NSString).lastPathComponent
        let host = iosHostRoot + Proc.appID(app.path) + "/" + name + ".app"
        let exe = host + "/Contents/MacOS/" + name, stamp = host + "/.stamp"
        let want = "\((try? fm.attributesOfItem(atPath: rebarIOS))?[.modificationDate] as? Date ?? .distantPast) \(app.path)"
        if (try? String(contentsOfFile: stamp, encoding: .utf8)) == want, fm.isExecutableFile(atPath: exe) { return exe }
        try? fm.removeItem(atPath: host)
        guard fm.fileExists(atPath: rebarIOS) else { return nil }
        do {
            try fm.createDirectory(atPath: host + "/Contents/MacOS", withIntermediateDirectories: true)
            try fm.copyItem(atPath: rebarIOS, toPath: exe)
            try fm.createSymbolicLink(atPath: host + "/Contents/Resources", withDestinationPath: app.path)
            var plist = (NSDictionary(contentsOfFile: app.path + "/Info.plist") as? [String: Any]) ?? [:]
            plist["CFBundleExecutable"] = name
            plist.removeValue(forKey: "NSPrincipalClass")
            plist["CFBundlePackageType"] = "APPL"
            plist["LSMinimumSystemVersion"] = "12.0"
            if plist["CFBundleVersion"] == nil { plist["CFBundleVersion"] = "1" }
            try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0).write(to: URL(fileURLWithPath: host + "/Contents/Info.plist"))
        } catch { return nil }
        for (tool, args) in [("/usr/bin/codesign", ["--force", "--sign", "-", host]),
                             ("/System/Library/Frameworks/CoreServices.framework/Versions/A/Frameworks/LaunchServices.framework/Versions/A/Support/lsregister", ["-f", host])] {
            let p = Process(); p.executableURL = URL(fileURLWithPath: tool); p.arguments = args
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
            if (try? p.run()) != nil { p.waitUntilExit() }
        }
        try? want.write(toFile: stamp, atomically: true, encoding: .utf8)
        return exe
    }

    /// Electron apps carry "Electron Framework"; other Chromium-based apps (ChatGPT's "Codex Framework", CEF apps) carry a renamed framework that still holds ANGLE's libEGL.
    static func chromiumEngine(_ app: String) -> String? {
        let fm = FileManager.default
        if fm.fileExists(atPath: app + "/Contents/Frameworks/Electron Framework.framework") { return "Electron" }
        for n in (try? fm.contentsOfDirectory(atPath: app + "/Contents/Frameworks")) ?? [] where n.hasSuffix(".framework") {
            if fm.fileExists(atPath: app + "/Contents/Frameworks/" + n + "/Libraries/libEGL.dylib") { return "Chromium" }
        }
        return nil
    }

    /// Adds an application, or a folder to look in.
    func add(_ url: URL) {
        if !store.extra.contains(url.path) { store.extra.append(url.path); save() }
        scan()
    }

    // MARK: settings

    func settings(_ app: AppEntry) -> AppSettings { store.apps[app.path] ?? AppSettings() }
    func setSettings(_ s: AppSettings, for app: AppEntry) { store.apps[app.path] = s == AppSettings() ? nil : s; save(); applyFastEditor(s.fastEditor, app) }

    /// The folder in Application Support where an app built from VS Code's sources keeps its user data (its product.json says: "nameShort"), else nil.
    func vsCodeFolder(_ app: AppEntry) -> String? {
        guard app.isElectron, let d = try? Data(contentsOf: URL(fileURLWithPath: app.path + "/Contents/Resources/app/product.json")),
              let j = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any], j["applicationName"] != nil, let n = j["nameShort"] as? String else { return nil }
        return n
    }
    /// Puts the FastEditor block into the user's settings.json of a VS Code app (or takes it out); the first time, the file is copied to settings.json.rebar-backup.
    private func applyFastEditor(_ on: Bool, _ app: AppEntry) {
        guard let folder = vsCodeFolder(app) else { return }
        let fm = FileManager.default
        let url = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent(folder + "/User/settings.json")
        let old = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
        if old.isEmpty && !on { return }
        let new = FastEditor.apply(old, on: on)
        if new == old { return }
        if on && !old.isEmpty {
            let bak = url.appendingPathExtension("rebar-backup")
            if !fm.fileExists(atPath: bak.path) { try? fm.copyItem(at: url, to: bak) }
        }
        try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? new.write(to: url, atomically: true, encoding: .utf8)
    }
    private func save() {
        if let d = try? JSONEncoder().encode(store) { try? d.write(to: support.appendingPathComponent("settings.json")) }
    }

    func logPath(_ app: AppEntry) -> String { logs.appendingPathComponent(app.name.replacingOccurrences(of: "/", with: "-") + ".log").path }

    // MARK: running

    func launch(_ app: AppEntry) {
        if !FileManager.default.isExecutableFile(atPath: rebar) { prepareRebar() }   // (something removed the working copy while the launcher was open)
        refreshInstances(sync: true)   // (an instance that an earlier launcher started and nobody sees would take this launch over and quit at once)
        guard running[app.path] == nil else { message = "“\(app.name)” is already running (maybe without a window): click “Restart”."; return }
        message = ""
        let s = settings(app)
        applyFastEditor(s.fastEditor, app)
        var args: [String] = []
        if app.isElectron {
            if s.noSandbox { args.append("--no-sandbox") }
            if s.mockKeychain { args.append("--use-mock-keychain") }
            args += s.gpu ? ["--disable-gpu-sandbox", "--use-angle=metal", "--ignore-gpu-blocklist", "--enable-gpu-rasterization"] : ["--disable-gpu"]
        }
        args += splitArgs(s.args)
        var env: [String: String] = ["REBAR_APP_ID": Proc.appID(app.path)]   // (everything the app starts carries it, also what outlives it: a helper service on its own)
        // What the apps need and a launcher opened from the Dock does not have (a shell, or an Electron app that started this one, passes them on): libmalloc's nano
        // zone off (rebar starts itself again with it off when it is missing), and for the video card ANGLE's own block on Metal for Nvidia GPUs lifted (the GPU
        // driver of this Mac makes Metal work there; without it Chromium finds no GPU and falls back to the processor).
        env["MallocNanoZone"] = "0"
        if s.gpu { env["ANGLE_FEATURE_OVERRIDES_DISABLED"] = "disableMetalOnNvidia" }
        if !s.jit { env["REBAR_JIT"] = "0" }
        for line in s.env.split(separator: "\n") {
            guard let eq = line.firstIndex(of: "="), eq != line.startIndex else { continue }
            env[String(line[..<eq]).trimmingCharacters(in: .whitespaces)] = String(line[line.index(after: eq)...])
        }
        var program = rebar, programArgs = [app.exe] + args
        if let ios = app.ios {
            if ios.encrypted { message = "“\(app.name)” is encrypted (FairPlay, from the App Store): its code cannot be read, so it cannot run. Only apps you built yourself or have the decrypted binary of work."; return }
            env["REBAR_IOS"] = "1"; env["REBAR_LIB_DIR"] = (rebar as NSString).deletingLastPathComponent
            if ios.usesUI {
                env["REBAR_IOS_UI"] = "1"
                guard let host = hostBundle(app) else { message = "UIKit apps need rebar-ios next to rebar (it is made by build.sh with Xcode's vtool)."; return }
                program = host; programArgs = [app.exe] + args
                env["CFProcessPath"] = host; env["REBAR_CFPROCESSPATH"] = host
            } else { env["CFProcessPath"] = app.exe }
        }
        guard let pid = Proc.spawn(program, programArgs, env: env, log: logPath(app)) else { message = "Could not start rebar."; return }
        running[app.path] = pid
        owned.insert(app.path)
        started[app.path] = Date()
    }

    func stop(_ app: AppEntry) {
        guard let pid = running[app.path] else { return }
        if owned.contains(app.path) { Proc.killTree(pid) }
        if let i = instance[app.path] { Proc.terminate(i.pids, groups: i.groups) }
    }

    /// Stops the app together with everything that is left of it, waits until it is gone and starts it again.
    func restart(_ app: AppEntry) {
        stop(app)
        message = "Restarting “\(app.name)”…"
        var tries = 0
        func again() {
            tries += 1
            refreshInstances(sync: true)
            if running[app.path] == nil || (owned.contains(app.path) && Proc.exitStatus(running[app.path]!) != nil) {
                running[app.path] = nil; owned.remove(app.path); instance[app.path] = nil
                message = ""; launch(app)
            } else if tries < 16 { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { again() } }
            else { message = "“\(app.name)” did not close within 8 seconds; close it yourself (Activity Monitor) and start it again." }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { again() }
    }

    /// What the row of a running app says: it is still coming up (our own start, no window yet, less than two minutes), it runs without a window, or it is simply running.
    func status(_ app: AppEntry) -> String {
        guard running[app.path] != nil else { return "" }
        if !windowless.contains(app.path) { return "Running" }
        if owned.contains(app.path), Date().timeIntervalSince(started[app.path] ?? .distantPast) < 120 { return "Starting…" }
        return "Running, no window"
    }

    /// Looks for running processes of every app (they are rebar processes whose command line starts with the app's path), whoever started them.
    func refreshInstances(sync: Bool = false) {
        let apps = self.apps, rebar = self.rebar, iosRoot = self.iosHostRoot
        let work = { () -> [String: (leader: pid_t, pids: [pid_t], groups: [pid_t])] in
            let procs = Proc.snapshot()
            // processes of our rebar that carry an app's marker (REBAR_APP_ID) belong to that app wherever their program lives
            let ours = procs.filter { $0.exe == rebar || $0.exe.hasPrefix(iosRoot) }
            var marked: [String: [Proc.Entry]] = [:]
            for p in ours {
                if let id = Launcher.markerCache.value(for: p.pid, load: { Proc.environment(of: p.pid).first { $0.hasPrefix("REBAR_APP_ID=") }.map { String($0.dropFirst(13)) } }) { marked[id, default: []].append(p) }
            }
            var found: [String: (leader: pid_t, pids: [pid_t], groups: [pid_t])] = [:]
            for a in apps {
                // (a guest process shows its own command line; one started by an older rebar shows "rebar <program> ...")
                let main = rebar + " " + a.exe, any = rebar + " " + a.path + "/Contents/", own = a.path + "/Contents/"
                var mine = ours.filter { $0.command.hasPrefix(any) || $0.command.hasPrefix(own) || (a.ios != nil && $0.command.hasPrefix(a.exe)) }   // (an iOS app is a flat folder: its processes show its executable)
                for p in marked[Proc.appID(a.path)] ?? [] where !mine.contains(where: { $0.pid == p.pid }) { mine.append(p) }
                if mine.isEmpty { continue }
                guard let leader = mine.filter({ $0.command.hasPrefix(main) || $0.command.hasPrefix(a.exe) }).map({ $0.pid }).min() else {
                    // only what an app left behind after it ended or crashed (helpers, a keyboard monitor, a service that outlived it and would hold its port): not a running app, so it goes, at once
                    Proc.killNow(mine.map { $0.pid }, groups: Array(Set(mine.filter { $0.pgid == $0.pid }.map { $0.pgid })))
                    continue
                }
                // a process group is only ever stopped as a whole when its leader is one of our processes
                let groups = Set(mine.filter { $0.pgid == $0.pid }.map { $0.pgid })
                found[a.path] = (leader, mine.map { $0.pid }, Array(groups))
            }
            return found
        }
        let apply = { (found: [String: (leader: pid_t, pids: [pid_t], groups: [pid_t])]) in
            var owners = Set<pid_t>()
            for w in (CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]]) ?? [] {
                guard (w[kCGWindowLayer as String] as? Int) == 0, let pid = w[kCGWindowOwnerPID as String] as? Int32,
                      let b = w[kCGWindowBounds as String] as? [String: Any], (b["Width"] as? Double ?? 0) > 60, (b["Height"] as? Double ?? 0) > 60 else { continue }
                owners.insert(pid)
            }
            for a in self.apps {
                if let f = found[a.path] {
                    if f.pids.contains(where: { owners.contains($0) }) { self.windowless.remove(a.path) } else { self.windowless.insert(a.path) }
                    self.instance[a.path] = (f.pids, f.groups)
                    if self.running[a.path] == nil { self.running[a.path] = f.leader }
                } else if !self.owned.contains(a.path) {   // (an own child that is gone is noticed by waitpid in poll)
                    self.instance[a.path] = nil
                    self.running[a.path] = nil
                    self.windowless.remove(a.path)
                }
            }
        }
        if sync { apply(work()); return }
        if scanningInstances { return }
        scanningInstances = true
        DispatchQueue.global().async {
            let f = work()
            DispatchQueue.main.async { apply(f); self.scanningInstances = false }
        }
    }

    /// `RebarLauncher --selftest <app name> [seconds]`: what a click on "Launch" does, without a window (for checking from a script, over ssh or at night):
    /// launches the app, prints its row's status and the windows of its processes every few seconds, then stops it. Exit code 0 when a window came up.
    static func selfTest(_ args: [String]) -> Int32 {
        let l = Launcher()
        func spin(_ s: Double) { RunLoop.current.run(until: Date().addingTimeInterval(s)) }
        var waited = 0.0
        while l.scanning || l.apps.isEmpty, waited < 30 { spin(0.2); waited += 0.2 }
        guard let want = args.first, let app = l.apps.first(where: { $0.name == want || $0.path == want }) else {
            print("no such app; found: " + l.apps.map { $0.name }.joined(separator: ", ")); return 2
        }
        let secs = args.count > 1 ? Double(args[1]) ?? 60 : 60
        l.launch(app)
        print("launched «\(app.name)»: pid \(l.running[app.path].map { String($0) } ?? "none") \(l.message)")
        var sawWindow = false, t = 0.0
        while t < secs {
            spin(3); t += 3
            l.refreshInstances(sync: true)
            l.poll()
            let pids = Set(l.instance[app.path]?.pids ?? [])
            var wins: [String] = []
            for w in (CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as? [[String: Any]]) ?? [] {
                guard (w[kCGWindowLayer as String] as? Int) == 0, let pid = w[kCGWindowOwnerPID as String] as? Int32, pids.contains(pid),
                      let b = w[kCGWindowBounds as String] as? [String: Any], (b["Width"] as? Double ?? 0) > 60, (b["Height"] as? Double ?? 0) > 60 else { continue }
                wins.append("\(pid):\(Int(b["Width"] as? Double ?? 0))x\(Int(b["Height"] as? Double ?? 0))")
            }
            if !wins.isEmpty { sawWindow = true }
            print(String(format: "%3.0fs status=«%@» processes=%d windows=%@ %@", t, l.status(app), pids.count, wins.joined(separator: ","), l.message))
            if l.running[app.path] == nil && !l.owned.contains(app.path) && t > 6 { break }   // it ended
        }
        l.stop(app)
        spin(3)
        l.refreshInstances(sync: true)
        if let i = l.instance[app.path] { Proc.killNow(i.pids, groups: i.groups) }
        return sawWindow ? 0 : 1
    }

    fileprivate func poll() {
        ticks += 1
        if ticks % 2 == 0 { refreshInstances() }
        for (path, pid) in running where owned.contains(path) {
            guard let status = Proc.exitStatus(pid) else { continue }
            running[path] = nil
            owned.remove(path)
            windowless.remove(path)
            let name = apps.first { $0.path == path }?.name ?? path
            let quick = Date().timeIntervalSince(started[path] ?? .distantPast) < 30
            if status != 0 && status != -SIGTERM && status != -SIGKILL && quick {
                message = "“\(name)” closed right away (code \(status)). Not every app works under rebar; see the log for details (gear → “Show log”)."
            } else if status == 0 && Date().timeIntervalSince(started[path] ?? .distantPast) < 10 {
                message = "“\(name)” exited right away without an error. That happens when it is already running (maybe without a window) and the new launch was told to do nothing: if “\(name)” is now listed as running, click “Restart”."
            }
            refreshInstances()   // (it may have left processes behind)
        }
    }
}
