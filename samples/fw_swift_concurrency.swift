import Foundation
actor Counter { var n = 0; func add(_ k: Int) -> Int { n += k; return n } }
struct Box: Sendable { let v: Int }
func fib(_ n: Int) async -> Int { n < 2 ? n : await fib(n - 1) + fib(n - 2) }
@main struct M {
    static func main() async {
        let c = Counter()
        await withTaskGroup(of: Int.self) { g in for i in 1...20 { g.addTask { await c.add(i) } }; var s = 0; for await v in g { s += v > 0 ? 1 : 0 }; print("group", s) }
        print("actor", await c.add(0))
        print("fib", await fib(15))
        let t = Task { () -> Int in try? await Task.sleep(nanoseconds: 10_000_000); return 42 }
        print("task", await t.value)
        let stream = AsyncStream<Int> { k in for i in 1...5 { k.yield(i * i) }; k.finish() }
        var acc = [Int](); for await v in stream { acc.append(v) }; print("stream", acc)
        async let a = fib(10); async let b = fib(11); print("async let", await a + b)
        do { try await withThrowingTaskGroup(of: Int.self) { g in g.addTask { throw CancellationError() }; for try await _ in g {} } } catch { print("caught", type(of: error)) }
        let r = await withCheckedContinuation { (k: CheckedContinuation<String, Never>) in DispatchQueue.global().async { k.resume(returning: "continuation") } }
        print(r)
        print("main actor", await MainActor.run { Thread.isMainThread })
    }
}
