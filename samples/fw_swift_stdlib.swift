import Foundation
protocol Shape { var area: Double { get }; func describe() -> String }
extension Shape { func describe() -> String { "area \(String(format: "%.2f", area))" } }
struct Circle: Shape { var r: Double; var area: Double { .pi * r * r } }
struct Rect: Shape { var w, h: Double; var area: Double { w * h } }
enum Tree<T: Comparable> { case leaf; indirect case node(Tree, T, Tree)
    func inserting(_ x: T) -> Tree { switch self { case .leaf: return .node(.leaf, x, .leaf); case let .node(l, v, r): return x < v ? .node(l.inserting(x), v, r) : .node(l, v, r.inserting(x)) } }
    func walk(_ f: (T) -> Void) { if case let .node(l, v, r) = self { l.walk(f); f(v); r.walk(f) } } }
struct Matrix<T: Numeric> { var d: [[T]]; static func * (a: Matrix, b: Matrix) -> Matrix { Matrix(d: a.d.map { row in (0..<b.d[0].count).map { j in zip(row, b.d.map { $0[j] }).reduce(0) { $0 + $1.0 * $1.1 } } }) } }
@propertyWrapper struct Clamped { var v: Int; let r: ClosedRange<Int>; var wrappedValue: Int { get { v } set { v = min(max(newValue, r.lowerBound), r.upperBound) } }; init(wrappedValue: Int, _ r: ClosedRange<Int>) { self.r = r; v = min(max(wrappedValue, r.lowerBound), r.upperBound) } }
struct S { @Clamped(0...10) var x = 50 }
let shapes: [any Shape] = [Circle(r: 2), Rect(w: 3, h: 4)]
print(shapes.map { $0.describe() })
var t = Tree<Int>.leaf; for x in [5, 3, 8, 1, 4, 9, 7] { t = t.inserting(x) }; var out = [Int](); t.walk { out.append($0) }; print(out)
print((Matrix(d: [[1, 2], [3, 4]]) * Matrix(d: [[5, 6], [7, 8]])).d)
var s = S(); s.x = -5; print(s.x)
print(Dictionary(grouping: (1...20), by: { $0 % 3 }).mapValues { $0.count }.sorted { $0.key < $1.key })
print("héllo wörld 🌍".uppercased(), "héllo".count, "héllo".utf8.count, "abc".reversed().map(String.init).joined())
print(try! Regex("[a-z]+(\\d+)").firstMatch(in: "abc123 def45")!.output[1].substring!)
print([3, 1, 2].sorted(by: >), (1...10).reduce(0, +), stride(from: 0, to: 20, by: 5).map { $0 })
let u = UUID(uuidString: "E621E1F8-C36C-495A-93FC-0C247A3E6E5F")!; print(u.uuidString.lowercased())
var rng = SystemRandomNumberGenerator(); _ = rng.next()
struct Seeded: RandomNumberGenerator { var s: UInt64; mutating func next() -> UInt64 { s = s &* 6364136223846793005 &+ 1442695040888963407; return s } }
var g = Seeded(s: 42); print((0..<5).map { _ in Int.random(in: 0..<100, using: &g) })
print(Double(1) / 3, Float(2).squareRoot(), 7.0.truncatingRemainder(dividingBy: 3), Int.max &+ 1 == Int.min)
let opt: Int? = nil; print(opt ?? -1, opt.map { $0 + 1 } as Any)
print(String(describing: Mirror(reflecting: Circle(r: 1)).children.map { $0.label! }))
