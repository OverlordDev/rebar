// Swift's calling convention across the bridge: generic methods of the standard library, a closure run by DispatchQueue.sync (self in x20, a thrown error in x21),
// a result that does not fit in registers.
import Foundation
struct Boom: Error {}
func parse(_ s: String) throws -> Int { guard let v = Int(s) else { throw Boom() }; return v }
let q = DispatchQueue(label: "t36")
let names = ["alpha", "beta", "gamma"]
print("array element:", names[1], names.count, names.contains("gamma"))
print("sync result:", q.sync { names.map { $0.count }.reduce(0, +) })
do { let v: Int = try q.sync { try parse("42") }; print("sync try ok:", v) } catch { print("unexpected error") }
do { let v: Int = try q.sync { try parse("x") }; print("unexpected", v) } catch { print("sync caught:", type(of: error)) }
let big = q.sync { (1...6).map { $0 * $0 } }
print("big result:", big)
let d: [String: Int] = ["a": 1, "b": 2]
print("dictionary:", d["b"] ?? -1, d.keys.sorted())
