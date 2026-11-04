import Foundation

// Starting a program in its own process group (so the whole tree can be stopped), and reading the CPU types of an executable.

enum Proc {
    /// stdout and stderr go to `log` (truncated).
    static func spawn(_ exe: String, _ args: [String], env: [String: String] = [:], log: String) -> pid_t? {
        var fa: posix_spawn_file_actions_t? = nil
        posix_spawn_file_actions_init(&fa)
        defer { posix_spawn_file_actions_destroy(&fa) }
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        posix_spawn_file_actions_adddup2(&fa, 1, 2)
        var attr: posix_spawnattr_t? = nil
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETPGROUP))
        posix_spawnattr_setpgroup(&attr, 0)
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

    static func killTree(_ pid: pid_t) {
        Foundation.kill(-pid, SIGTERM)
        Foundation.kill(pid, SIGTERM)
        DispatchQueue.global().asyncAfter(deadline: .now() + 2) { Foundation.kill(-pid, SIGKILL); Foundation.kill(pid, SIGKILL) }
    }

    /// One line of `ps`: a process with its process group and command line.
    struct Entry { let pid: pid_t; let pgid: pid_t; let command: String; let exe: String }   // exe: the program the kernel runs (rebar, for a guest process: its command line shows the guest's own)

    /// All processes of this user (pid, process group, whole command line).
    static func snapshot() -> [Entry] {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/ps")
        p.arguments = ["-axww", "-o", "pid=,pgid=,command="]
        let pipe = Pipe()
        p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return [] }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        var out: [Entry] = []
        for line in (String(data: data, encoding: .utf8) ?? "").split(separator: "\n") {
            let parts = line.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: true)
            guard parts.count == 3, let pid = pid_t(parts[0]), let pg = pid_t(parts[1]) else { continue }
            var buf = [CChar](repeating: 0, count: 4096)
            let n = proc_pidpath(pid, &buf, UInt32(buf.count))
            out.append(Entry(pid: pid, pgid: pg, command: String(parts[2]), exe: n > 0 ? String(cString: buf) : ""))
        }
        return out
    }

    /// The environment variables of a process of this user (KERN_PROCARGS2: argc, the path, the arguments, then the environment), [] when it cannot be read.
    static func environment(of pid: pid_t) -> [String] {
        var mib: [Int32] = [CTL_KERN, KERN_PROCARGS2, pid]
        var size = 0
        guard sysctl(&mib, 3, nil, &size, nil, 0) == 0, size > 8 else { return [] }
        var buf = [UInt8](repeating: 0, count: size)
        guard sysctl(&mib, 3, &buf, &size, nil, 0) == 0 else { return [] }
        let argc = Int(buf[0]) | Int(buf[1]) << 8 | Int(buf[2]) << 16 | Int(buf[3]) << 24
        var strings: [String] = [], i = 4, cur: [UInt8] = []
        while i < size && buf[i] != 0 { i += 1 }           // the executable path
        while i < size && buf[i] == 0 { i += 1 }           // (padding)
        while i < size {
            if buf[i] == 0 { strings.append(String(decoding: cur, as: UTF8.self)); cur = [] } else { cur.append(buf[i]) }
            i += 1
        }
        return Array(strings.dropFirst(argc)).filter { $0.contains("=") }
    }

    /// A short stable name of an app for the REBAR_APP_ID marker that every process started from it carries (FNV-1a of the path).
    static func appID(_ path: String) -> String {
        var h: UInt64 = 0xcbf29ce484222325
        for b in path.utf8 { h = (h ^ UInt64(b)) &* 0x100000001b3 }
        return String(h, radix: 16)
    }

    /// SIGKILL now, and a short wait (at most three seconds) until the processes are gone: for leftovers that must not hold a port or a lock when the app starts again.
    static func killNow(_ pids: [pid_t], groups: [pid_t]) {
        let mine = getpgrp()
        for g in groups where g > 1 && g != mine { Foundation.kill(-g, SIGKILL) }
        for p in pids where p > 1 { Foundation.kill(p, SIGKILL) }
        for _ in 0..<30 {
            if pids.allSatisfy({ Foundation.kill($0, 0) != 0 }) { return }
            usleep(100_000)
        }
    }

    /// Stops these processes (and the process groups they lead, never the group this launcher is in): SIGTERM now, SIGKILL after two seconds.
    static func terminate(_ pids: [pid_t], groups: [pid_t]) {
        let mine = getpgrp()
        for g in groups where g > 1 && g != mine { Foundation.kill(-g, SIGTERM) }
        for p in pids where p > 1 { Foundation.kill(p, SIGTERM) }
        DispatchQueue.global().asyncAfter(deadline: .now() + 2) {
            for g in groups where g > 1 && g != mine { Foundation.kill(-g, SIGKILL) }
            for p in pids where p > 1 { Foundation.kill(p, SIGKILL) }
        }
    }
}

/// CPU types in the Mach-O header of an executable ("arm64", "x86_64").
func machoArchs(atPath path: String) -> [String] {
    guard let h = FileHandle(forReadingAtPath: path) else { return [] }
    defer { try? h.close() }
    let d = [UInt8](h.readData(ofLength: 4096))
    guard d.count >= 16 else { return [] }
    func be(_ o: Int) -> UInt32 { UInt32(d[o]) << 24 | UInt32(d[o + 1]) << 16 | UInt32(d[o + 2]) << 8 | UInt32(d[o + 3]) }
    func le(_ o: Int) -> UInt32 { UInt32(d[o + 3]) << 24 | UInt32(d[o + 2]) << 16 | UInt32(d[o + 1]) << 8 | UInt32(d[o]) }
    func name(_ t: UInt32) -> String { t == 0x0100000C ? "arm64" : t == 0x01000007 ? "x86_64" : t == 0x0200000C ? "arm64_32" : "cpu\(t)" }
    switch be(0) {
    case 0xCAFEBABE, 0xCAFEBABF:
        let stride = be(0) == 0xCAFEBABE ? 20 : 32
        let n = Int(be(4))
        guard n > 0, n < 8, 8 + stride * n <= d.count else { return [] }
        return (0..<n).map { name(be(8 + stride * $0)) }
    case 0xCFFAEDFE: return [name(le(4))]
    default: return []
    }
}

/// Splits a command line like a shell would, with '...' and "..." for arguments that contain spaces.
func splitArgs(_ s: String) -> [String] {
    var out: [String] = [], cur = "", quote: Character? = nil, has = false
    for ch in s {
        if let q = quote { if ch == q { quote = nil } else { cur.append(ch) } }
        else if ch == "\"" || ch == "'" { quote = ch; has = true }
        else if ch == " " || ch == "\t" || ch == "\n" { if has || !cur.isEmpty { out.append(cur); cur = ""; has = false } }
        else { cur.append(ch) }
    }
    if has || !cur.isEmpty { out.append(cur) }
    return out
}


/// REBAR_APP_ID of processes by pid (reading it costs a sysctl; a pid that is gone is dropped when it no longer shows up in a snapshot).
final class PidCache {
    private var map: [pid_t: String] = [:]
    private let lock = NSLock()
    func value(for pid: pid_t, load: () -> String?) -> String? {
        lock.lock(); defer { lock.unlock() }
        if let v = map[pid] { return v }
        guard let v = load() else { return nil }   // (not cached: a process just forked may not have its environment yet)
        map[pid] = v
        if map.count > 4000 { map.removeAll() }
        return v
    }
}


/// What matters about an executable for iOS apps: the CPU types, whether its code is FairPlay-encrypted (LC_ENCRYPTION_INFO cryptid 1) and whether it links UIKit or SwiftUI (of the arm64 slice).
func machoInfo(atPath path: String) -> (archs: [String], encrypted: Bool, usesUI: Bool)? {
    let archs = machoArchs(atPath: path)
    guard !archs.isEmpty, let h = FileHandle(forReadingAtPath: path) else { return nil }
    defer { try? h.close() }
    var base: UInt64 = 0
    let head = [UInt8](h.readData(ofLength: 4096))
    func be(_ o: Int) -> UInt32 { UInt32(head[o]) << 24 | UInt32(head[o + 1]) << 16 | UInt32(head[o + 2]) << 8 | UInt32(head[o + 3]) }
    if head.count >= 8, be(0) == 0xCAFEBABE || be(0) == 0xCAFEBABF {   // fat: find the arm64 slice
        let is64 = be(0) == 0xCAFEBABF, stride = is64 ? 32 : 20
        for i in 0..<Int(be(4)) where 8 + stride * (i + 1) <= head.count && be(8 + stride * i) == 0x0100000C {
            base = is64 ? UInt64(be(8 + stride * i + 8)) << 32 | UInt64(be(8 + stride * i + 12)) : UInt64(be(8 + stride * i + 8))
        }
    }
    try? h.seek(toOffset: base)
    let d = [UInt8](h.readData(ofLength: 1 << 17))
    func le(_ o: Int) -> UInt32 { UInt32(d[o + 3]) << 24 | UInt32(d[o + 2]) << 16 | UInt32(d[o + 1]) << 8 | UInt32(d[o]) }
    guard d.count > 32, le(0) == 0xFEEDFACF, le(4) == 0x0100000C else { return (archs, false, false) }
    var off = 32, enc = false, ui = false
    for _ in 0..<Int(le(16)) where off + 8 <= d.count {
        let cmd = le(off), size = Int(le(off + 4))
        if size < 8 || off + size > d.count { break }
        if cmd == 0x2C, size >= 20, le(off + 16) != 0 { enc = true }   // LC_ENCRYPTION_INFO_64
        if cmd == 0xC || cmd == 0x80000018 || cmd == 0x8000001F || cmd == 0x1F {   // LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB
            let no = off + Int(le(off + 8))
            if no < off + size, let end = d[no..<(off + size)].firstIndex(of: 0) {
                let n = String(decoding: d[no..<end], as: UTF8.self)
                if n.contains("/UIKit.framework/") || n.contains("/UIKitCore.framework/") || n.contains("/SwiftUI.framework/") { ui = true }
            }
        }
        off += size
    }
    return (archs, enc, ui)
}
