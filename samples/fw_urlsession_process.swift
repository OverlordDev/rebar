import Foundation
import Network
let sem = DispatchSemaphore(value: 0)
let l = try! NWListener(using: .tcp, on: .any)
l.newConnectionHandler = { c in c.start(queue: .global()); c.receive(minimumIncompleteLength: 1, maximumLength: 2000) { d, _, _, _ in
    let body = "{\"ok\":true}"; let resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n\(body)"
    c.send(content: resp.data(using: .utf8), isComplete: true, completion: .contentProcessed { _ in }) } }
l.stateUpdateHandler = { s in if case .ready = s {
    let url = URL(string: "http://127.0.0.1:\(l.port!.rawValue)/x")!
    URLSession.shared.dataTask(with: url) { d, r, e in print("urlsession", (r as? HTTPURLResponse)?.statusCode ?? -1, String(decoding: d ?? Data(), as: UTF8.self), e == nil); sem.signal() }.resume() } }
l.start(queue: .global()); print("done", sem.wait(timeout: .now() + 15) == .success)
let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/echo"); p.arguments = ["child", "process"]; let pipe = Pipe(); p.standardOutput = pipe; try! p.run(); p.waitUntilExit(); print("process", String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines), p.terminationStatus)
let fh = FileHandle(forWritingAtPath: "/dev/null")!; fh.write(Data("x".utf8)); print("filehandle ok")
let t0 = Date(); Thread.sleep(forTimeInterval: 0.05); print("sleep", Date().timeIntervalSince(t0) >= 0.05)
let th = Thread { print("thread", Thread.isMainThread) }; th.start(); Thread.sleep(forTimeInterval: 0.1)
print(ProcessInfo.processInfo.environment["HOME"] != nil, Bundle.main.bundlePath.isEmpty, CommandLine.arguments.count)
