// The host's Swift runtime must know the types and protocol conformances of the guest's own module: lookup by name, dynamic casts, Codable.
import Foundation
struct Point: Codable, Equatable, CustomStringConvertible { var x: Int; var y: Int; var description: String { "P(\(x),\(y))" } }
enum Mode: String, Codable { case fast, slow }
let json = #"{"x":3,"y":4}"#.data(using: .utf8)!
let p = try JSONDecoder().decode(Point.self, from: json)
print("decoded:", p, p == Point(x: 3, y: 4))
print("encoded:", String(data: try JSONEncoder().encode([Mode.fast, Mode.slow]), encoding: .utf8)!)
let anyv: Any = p
print("casts:", (anyv as? CustomStringConvertible) != nil, (anyv as? any Equatable) != nil, (anyv as? Codable) != nil, (anyv as? Int) == nil)
print("by name:", _typeByName("t37.Point") != nil, _typeByName("t37.Mode") != nil, _typeByName("t37.Nope") == nil)
print("describing:", String(describing: Point.self), String(reflecting: Mode.self))
