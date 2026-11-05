import Foundation
import Synchronization
@objc protocol Svc { func add(_ a: Int, _ b: Int, reply: @escaping (Int) -> Void); func echo(_ s: String, reply: @escaping (String) -> Void) }
final class Impl: NSObject, Svc, NSXPCListenerDelegate {
    func add(_ a: Int, _ b: Int, reply: @escaping (Int) -> Void) { reply(a + b) }
    func echo(_ s: String, reply: @escaping (String) -> Void) { reply(s.uppercased()) }
    func listener(_ l: NSXPCListener, shouldAcceptNewConnection c: NSXPCConnection) -> Bool { c.exportedInterface = NSXPCInterface(with: Svc.self); c.exportedObject = self; c.resume(); return true }
}
let impl = Impl(); let listener = NSXPCListener.anonymous(); listener.delegate = impl; listener.resume()
let conn = NSXPCConnection(listenerEndpoint: listener.endpoint); conn.remoteObjectInterface = NSXPCInterface(with: Svc.self); conn.resume()
let sem = DispatchSemaphore(value: 0)
let proxy = conn.remoteObjectProxyWithErrorHandler { print("xpc error", $0) } as! Svc
proxy.add(2, 40) { print("xpc add", $0); proxy.echo("rebar") { print("xpc echo", $0); sem.signal() } }
print("xpc wait", sem.wait(timeout: .now() + 20) == .success)
// KVO / KVC
final class Obs: NSObject { @objc dynamic var v = 1; @objc dynamic var arr = [Int]() }
let o = Obs(); var seen: [Int] = []; let tok = o.observe(\.v, options: [.new, .old]) { _, ch in seen.append(ch.newValue!) }
o.v = 5; o.setValue(9, forKey: "v"); print("kvo", seen, o.value(forKey: "v")!, o.value(forKeyPath: "arr.@count")!)
_ = tok
// archiving
final class Item: NSObject, NSSecureCoding { static var supportsSecureCoding = true; var name: String; var n: Int; init(_ n: String, _ i: Int) { name = n; self.n = i }
    func encode(with c: NSCoder) { c.encode(name, forKey: "name"); c.encode(n, forKey: "n") }
    required init?(coder c: NSCoder) { name = c.decodeObject(of: NSString.self, forKey: "name")! as String; n = c.decodeInteger(forKey: "n") } }
let ar = try! NSKeyedArchiver.archivedData(withRootObject: [Item("a", 1), Item("b", 2)], requiringSecureCoding: true)
let back = try! NSKeyedUnarchiver.unarchivedArrayOfObjects(ofClass: Item.self, from: ar)!; print("archive", back.map { "\($0.name)\($0.n)" })
// undo, notification, operation queue, timer, runloop
let um = UndoManager(); var val = 0; um.registerUndo(withTarget: o, handler: { _ in val = -1 }); um.undo(); print("undo", val)
let nc = NotificationCenter(); var got = 0; let t2 = nc.addObserver(forName: .init("x"), object: nil, queue: nil) { _ in got += 1 }; nc.post(name: .init("x"), object: nil); print("notif", got); nc.removeObserver(t2)
let dnc = DistributedNotificationCenter.default(); var dgot = false; let ex = DispatchSemaphore(value: 0)
dnc.addObserver(forName: .init("dev.rebar.test"), object: nil, queue: .main) { _ in dgot = true; ex.signal() }
dnc.postNotificationName(.init("dev.rebar.test"), object: nil, userInfo: nil, deliverImmediately: true)
let end = Date().addingTimeInterval(10); while !dgot && Date() < end { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.1)) }; print("distributed", dgot)
let q = OperationQueue(); q.maxConcurrentOperationCount = 1; var order: [Int] = []; for i in 0..<5 { q.addOperation { order.append(i) } }; q.waitUntilAllOperationsAreFinished(); print("opqueue", order)
var ticks = 0; let timer = Timer(timeInterval: 0.05, repeats: true) { t in ticks += 1; if ticks == 4 { t.invalidate() } }; RunLoop.main.add(timer, forMode: .default)
let e2 = Date().addingTimeInterval(5); while timer.isValid && Date() < e2 { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.05)) }; print("timer", ticks)
// Synchronization
let mu = Mutex(0); DispatchQueue.concurrentPerform(iterations: 8) { _ in for _ in 0..<1000 { mu.withLock { $0 += 1 } } }; print("mutex", mu.withLock { $0 })
let at = Atomic<Int>(0); DispatchQueue.concurrentPerform(iterations: 4) { _ in for _ in 0..<500 { at.add(1, ordering: .relaxed) } }; print("atomic", at.load(ordering: .sequentiallyConsistent))
// dispatch sources
let src = DispatchSource.makeTimerSource(queue: .global()); var fired = 0; let ds = DispatchSemaphore(value: 0); src.schedule(deadline: .now(), repeating: .milliseconds(10)); src.setEventHandler { fired += 1; if fired == 3 { ds.signal() } }; src.resume(); ds.wait(); src.cancel(); print("dispatch timer", fired >= 3)
var fds: [Int32] = [0, 0]; pipe(&fds); let rs = DispatchSource.makeReadSource(fileDescriptor: fds[0], queue: .global()); let rsem = DispatchSemaphore(value: 0); var rd = 0; rs.setEventHandler { var b = [UInt8](repeating: 0, count: 16); rd = read(fds[0], &b, 16); rsem.signal() }; rs.resume(); write(fds[1], "hello", 5); rsem.wait(); print("read source", rd)
let sig = DispatchSource.makeSignalSource(signal: SIGUSR1, queue: .global()); let ssem = DispatchSemaphore(value: 0); signal(SIGUSR1, SIG_IGN); sig.setEventHandler { ssem.signal() }; sig.resume(); kill(getpid(), SIGUSR1); print("signal source", ssem.wait(timeout: .now() + 5) == .success)
