// Errors thrown by the host's Swift code into the guest and back (swifterror), Codable called back by the host, in a loop long enough for the translator.
import Foundation
struct Item: Codable, Equatable { var id: Int; var name: String; var tags: [String]? }
struct Wrapper: Codable { var items: [Item]; var total: Int }
let good = #"{"items":[{"id":1,"name":"a","tags":["x","y"]},{"id":2,"name":"b"}],"total":2}"#.data(using: .utf8)!
let bad1 = #"{"items":[{"id":1}],"total":1}"#.data(using: .utf8)!        // missing name
let bad2 = #"{"items":[{"id":"x","name":"a"}],"total":1}"#.data(using: .utf8)!   // wrong type
let bad3 = #"{"items": [ {"id":1,"name":"a"} "#.data(using: .utf8)!       // truncated
var ok = 0, missing = 0, mismatch = 0, corrupt = 0, other = 0, sum = 0
for i in 0..<300 {
    let data = [good, bad1, bad2, bad3][i % 4]
    do { let w = try JSONDecoder().decode(Wrapper.self, from: data); ok += 1; sum += w.items.reduce(0) { $0 + $1.id } + w.total }
    catch let e as DecodingError {
        switch e { case .keyNotFound: missing += 1; case .typeMismatch: mismatch += 1; case .dataCorrupted: corrupt += 1; default: other += 1 }
    } catch { other += 1 }
}
print("ok", ok, "missing", missing, "mismatch", mismatch, "corrupt", corrupt, "other", other, "sum", sum)
let q = DispatchQueue(label: "t38")
var caught = 0
for i in 0..<200 { do { _ = try q.sync { () throws -> Int in if i % 3 == 0 { throw DecodingError.dataCorrupted(.init(codingPath: [], debugDescription: "x")) }; return i } } catch { caught += 1 } }
print("caught", caught)
