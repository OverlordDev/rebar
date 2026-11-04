import Foundation
import Combine
import Observation
@Observable final class Model { var count = 0; var name = "a" }
final class Box { @Published var v = 0 }
var cancels = Set<AnyCancellable>()
let b = Box(); var seen = [Int](); b.$v.sink { seen.append($0) }.store(in: &cancels); for i in 1...3 { b.v = i * i }; print("published", seen)
var got = [String](); [1, 2, 3].publisher.map { $0 * 2 }.filter { $0 > 2 }.collect().sink { got.append("\($0)") }.store(in: &cancels); print("pipeline", got)
let subj = PassthroughSubject<String, Never>(); var s2 = [String](); subj.debounce(for: .milliseconds(1), scheduler: RunLoop.main).sink { s2.append($0) }.store(in: &cancels); subj.send("x")
var zipped = [(Int, String)](); Publishers.Zip([1, 2].publisher, ["a", "b"].publisher).sink { zipped.append($0) }.store(in: &cancels); print("zip", zipped)
let m = Model(); var changes = 0
func track() { withObservationTracking { _ = m.count } onChange: { changes += 1; track() } }
track(); m.count = 1; m.count = 2; m.name = "b"; print("observation changes", changes)
RunLoop.main.run(until: Date().addingTimeInterval(0.1)); print("debounce", s2)
let fut = Future<Int, Never> { $0(.success(9)) }; var fv = 0; fut.sink { fv = $0 }.store(in: &cancels); print("future", fv)
let t = Timer.publish(every: 0.01, on: .main, in: .common).autoconnect(); var ticks = 0; t.sink { _ in ticks += 1 }.store(in: &cancels); RunLoop.main.run(until: Date().addingTimeInterval(0.1)); print("timer ok", ticks > 2)
