import Foundation
import Metal
import ImageIO

// Small helpers: starting processes (in their own process group, so the whole tree can be stopped), running commands, talking to a
// Chromium browser over the DevTools protocol, and collecting facts about the machine.

enum Proc {
    /// Starts `exe` with stdout/stderr going to files; with `group` the child leads a new process group (kill(-pid) reaches its descendants).
    static func spawn(_ exe: String, _ args: [String], env: [String: String] = [:], out: String, err: String? = nil, group: Bool = true) -> pid_t? {
        var fa: posix_spawn_file_actions_t? = nil
        posix_spawn_file_actions_init(&fa)
        defer { posix_spawn_file_actions_destroy(&fa) }
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        if let err = err { posix_spawn_file_actions_addopen(&fa, 2, err, O_WRONLY | O_CREAT | O_TRUNC, 0o644) } else { posix_spawn_file_actions_adddup2(&fa, 1, 2) }
        var attr: posix_spawnattr_t? = nil
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        if group {
            posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETPGROUP))
            posix_spawnattr_setpgroup(&attr, 0)
        }
        var environment = ProcessInfo.processInfo.environment
        for (k, v) in env { environment[k] = v }
        let argv: [UnsafeMutablePointer<CChar>?] = ([exe] + args).map { strdup($0) } + [nil]
        let envp: [UnsafeMutablePointer<CChar>?] = environment.map { strdup("\($0.key)=\($0.value)") } + [nil]
        defer { for p in argv { free(p) }; for p in envp { free(p) } }
        var pid: pid_t = 0
        return posix_spawn(&pid, exe, &fa, &attr, argv, envp) == 0 ? pid : nil
    }

    /// nil while the process runs, else its exit status (negative: killed by that signal).
    static func exitStatus(_ pid: pid_t) -> Int32? {
        var st: Int32 = 0
        let r = waitpid(pid, &st, WNOHANG)
        if r == 0 { return nil }
        if r < 0 { return 0 }
        return (st & 0x7f) == 0 ? (st >> 8) & 0xff : -(st & 0x7f)
    }

    static func killTree(_ pid: pid_t, marker: String? = nil) {
        kill(-pid, SIGTERM)
        kill(pid, SIGTERM)
        for _ in 0..<15 { if exitStatus(pid) != nil { break }; usleep(100_000) }
        kill(-pid, SIGKILL)
        kill(pid, SIGKILL)
        if let m = marker { _ = captureOut("/usr/bin/pkill", ["-9", "-f", m], timeout: 5) }
        _ = exitStatus(pid)
    }

    struct Result { var out: String; var err: String; var status: Int32; var timedOut: Bool }

    static func captureOut(_ exe: String, _ args: [String], env: [String: String] = [:], timeout: TimeInterval = 30) -> Result {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        if !env.isEmpty { p.environment = ProcessInfo.processInfo.environment.merging(env) { $1 } }
        let o = Pipe(), e = Pipe()
        p.standardOutput = o
        p.standardError = e
        p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch { return Result(out: "", err: "cannot run \(exe): \(error.localizedDescription)", status: -1, timedOut: false) }
        var od = Data(), ed = Data()
        let g = DispatchGroup()
        g.enter(); DispatchQueue.global().async { od = o.fileHandleForReading.readDataToEndOfFile(); g.leave() }
        g.enter(); DispatchQueue.global().async { ed = e.fileHandleForReading.readDataToEndOfFile(); g.leave() }
        let deadline = Date().addingTimeInterval(timeout)
        var timedOut = false
        while p.isRunning {
            if Date() > deadline { timedOut = true; p.terminate(); usleep(300_000); if p.isRunning { kill(p.processIdentifier, SIGKILL) }; break }
            usleep(50_000)
        }
        p.waitUntilExit()
        _ = g.wait(timeout: .now() + 2)
        return Result(out: String(decoding: od, as: UTF8.self), err: String(decoding: ed, as: UTF8.self), status: p.terminationStatus, timedOut: timedOut)
    }

    static func sh(_ cmd: String, timeout: TimeInterval = 20) -> String {
        let r = captureOut("/bin/sh", ["-c", cmd], timeout: timeout)
        return (r.out + r.err).trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

/// DevTools protocol on a browser's --remote-debugging-port: what the GPU process of that browser really uses, and what it rendered.
enum CDP {
    private final class Box { var value: Any?; let sem = DispatchSemaphore(value: 0) }

    private static func getJSON(_ port: Int, _ path: String) -> Any? {
        guard let url = URL(string: "http://127.0.0.1:\(port)\(path)") else { return nil }
        let b = Box()
        URLSession.shared.dataTask(with: url) { d, _, _ in
            if let d = d { b.value = try? JSONSerialization.jsonObject(with: d) }
            b.sem.signal()
        }.resume()
        return b.sem.wait(timeout: .now() + 4) == .success ? b.value : nil
    }

    /// One request/response on a DevTools websocket.
    private static func call(_ ws: String, _ method: String, _ params: String = "{}", timeout: TimeInterval) -> [String: Any]? {
        guard let url = URL(string: ws) else { return nil }
        let task = URLSession.shared.webSocketTask(with: url)
        task.maximumMessageSize = 64 * 1024 * 1024
        task.resume()
        let box = Box()
        func receive() {
            task.receive { result in
                guard case .success(let msg) = result else { box.sem.signal(); return }
                var text: String?
                switch msg { case .string(let t): text = t; case .data(let d): text = String(decoding: d, as: UTF8.self); @unknown default: break }
                if let t = text, let d = t.data(using: .utf8), let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any], (j["id"] as? Int) == 1 {
                    box.value = j["result"]; box.sem.signal()
                } else { receive() }
            }
        }
        receive()
        task.send(.string("{\"id\":1,\"method\":\"\(method)\",\"params\":\(params)}")) { _ in }
        _ = box.sem.wait(timeout: .now() + timeout)
        task.cancel(with: .goingAway, reason: nil)
        return box.value as? [String: Any]
    }

    static func gpuSummary(port: Int, timeout: TimeInterval = 8) -> String? {
        guard let v = getJSON(port, "/json/version") as? [String: Any], let ws = v["webSocketDebuggerUrl"] as? String else { return nil }
        return format(call(ws, "SystemInfo.getInfo", timeout: timeout))
    }

    /// What the page's compositor produced (not what WindowServer shows).
    static func pageScreenshot(port: Int, timeout: TimeInterval = 25) -> CGImage? {
        guard let list = getJSON(port, "/json") as? [[String: Any]] else { return nil }
        let pages = list.filter { ($0["type"] as? String) == "page" }
        guard let page = pages.first(where: { (($0["url"] as? String) ?? "").contains("workbench") }) ?? pages.first, let ws = page["webSocketDebuggerUrl"] as? String else { return nil }
        guard let r = call(ws, "Page.captureScreenshot", "{\"format\":\"png\"}", timeout: timeout), let b64 = r["data"] as? String, let data = Data(base64Encoded: b64),
              let src = CGImageSourceCreateWithData(data as CFData, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(src, 0, nil)
    }

    private static func format(_ r: [String: Any]?) -> String? {
        guard let gpu = r?["gpu"] as? [String: Any] else { return nil }
        var lines: [String] = []
        for d in (gpu["devices"] as? [[String: Any]]) ?? [] {
            lines.append("device: \(d["vendorString"] ?? "?") / \(d["deviceString"] ?? "?") (driver \(d["driverVendor"] ?? "?") \(d["driverVersion"] ?? "?"))")
        }
        if let aux = gpu["auxAttributes"] as? [String: Any] { lines.append("GL renderer: \(aux["glRenderer"] ?? "?")") }
        if let fs = gpu["featureStatus"] as? [String: Any] {
            let keys = ["gpu_compositing", "rasterization", "skia_graphite", "opengl", "webgl", "webgpu", "2d_canvas"]
            lines.append("features: " + keys.compactMap { k in (fs[k] as? String).map { "\(k)=\($0)" } }.joined(separator: ", "))
        }
        return lines.joined(separator: "\n")
    }
}

enum SysInfo {
    static func collect() -> String {
        var s = ""
        func add(_ title: String, _ body: String) { s += "== \(title)\n\(body)\n\n" }
        add("macOS", Proc.sh("sw_vers; uname -m"))
        add("Hardware", Proc.sh("sysctl -n machdep.cpu.brand_string; sysctl -n hw.model; echo \"RAM: $(( $(sysctl -n hw.memsize) / 1073741824 )) GB\""))
        var m = ""
        for d in MTLCopyAllDevices() {
            m += "\(d.name) | lowPower=\(d.isLowPower) removable=\(d.isRemovable) unifiedMemory=\(d.hasUnifiedMemory) registryID=\(d.registryID)\n"
        }
        add("Metal devices (seen by this app, x86_64)", m.isEmpty ? "none" : m.trimmingCharacters(in: .newlines))
        add("Display adapters", Proc.sh("system_profiler SPDisplaysDataType -detailLevel mini | grep -v -i -E 'serial|UUID'", timeout: 40))
        add("Loaded GPU-related kexts", Proc.sh("kmutil showloaded --list-only 2>/dev/null | grep -i -E 'AMD|Radeon|NVMTL|NVIDIA|Metal|GPU|Accelerator|Intel.*Graphics|AppleGraphics' | sed 's/  */ /g' | cut -c1-160 | head -40"))
        add("Boot", Proc.sh("nvram boot-args 2>&1; nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version 2>&1 | tr -d '\\000'; csrutil status 2>&1"))
        return s
    }
}
