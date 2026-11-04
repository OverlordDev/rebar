import Foundation
struct P: Codable, Equatable { var name: String; var age: Int; var tags: [String]; var when: Date; var url: URL }
let p = P(name: "Ann", age: 33, tags: ["a", "b"], when: Date(timeIntervalSince1970: 1_700_000_000), url: URL(string: "https://example.com/x?y=1")!)
let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]; enc.dateEncodingStrategy = .iso8601
let data = try! enc.encode(p); print(String(data: data, encoding: .utf8)!)
let dec = JSONDecoder(); dec.dateDecodingStrategy = .iso8601; print(try! dec.decode(P.self, from: data) == p)
let pl = try! PropertyListEncoder().encode(["k": [1, 2, 3]]); print(try! PropertyListDecoder().decode([String: [Int]].self, from: pl))
var cal = Calendar(identifier: .gregorian); cal.timeZone = TimeZone(identifier: "UTC")!
let d = Date(timeIntervalSince1970: 1_700_000_000); print(cal.dateComponents([.year, .month, .day, .hour, .weekday], from: d))
let f = DateFormatter(); f.locale = Locale(identifier: "en_US_POSIX"); f.timeZone = TimeZone(identifier: "UTC"); f.dateFormat = "yyyy-MM-dd'T'HH:mm:ss"; print(f.string(from: d))
let nf = NumberFormatter(); nf.locale = Locale(identifier: "en_US"); nf.numberStyle = .currency; print(nf.string(from: 1234.5)!)
print(ByteCountFormatter.string(fromByteCount: 123_456_789, countStyle: .file))
let tmp = NSTemporaryDirectory() + "fwtest_\(getpid()).txt"; try! "line1\nline2\n".write(toFile: tmp, atomically: true, encoding: .utf8)
print(try! String(contentsOfFile: tmp).split(separator: "\n"), FileManager.default.fileExists(atPath: tmp), (try! FileManager.default.attributesOfItem(atPath: tmp))[.size] as! Int)
try! FileManager.default.removeItem(atPath: tmp)
print(URLComponents(string: "https://a.b/c?d=1&e=2")!.queryItems!.map { "\($0.name)=\($0.value!)" })
print("a,b;c".components(separatedBy: CharacterSet(charactersIn: ",;")), "  trim ".trimmingCharacters(in: .whitespaces))
print(Data("hello".utf8).base64EncodedString(), Data(base64Encoded: "aGVsbG8=").map { String(decoding: $0, as: UTF8.self) } as Any)
let nsp = NSPredicate(format: "age > %d AND name BEGINSWITH %@", 20, "A"); print(nsp.evaluate(with: ["age": 33, "name": "Ann"] as NSDictionary))
print(Measurement(value: 5, unit: UnitLength.kilometers).converted(to: .miles).value.rounded())
let sema = DispatchSemaphore(value: 0); var res = 0; DispatchQueue.global().async { res = 7; sema.signal() }; sema.wait(); print("gcd", res)
let q = OperationQueue(); var sum = 0; let lock = NSLock(); for i in 1...10 { q.addOperation { lock.lock(); sum += i; lock.unlock() } }; q.waitUntilAllOperationsAreFinished(); print("ops", sum)
print(ProcessInfo.processInfo.processorCount > 0, Locale(identifier: "ru_RU").localizedString(forRegionCode: "US") as Any)
print(NSRegularExpression.escapedPattern(for: "a.b*c"), try! NSRegularExpression(pattern: "(\\d+)-(\\d+)").numberOfMatches(in: "1-2 33-44", range: NSRange(location: 0, length: 9)))
