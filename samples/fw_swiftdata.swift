// SwiftData and the SwiftUI Query wrapper: Optional<SwiftUI.Transaction> is laid out differently by the arm64 compiler (none is 1) and by the x86_64 SwiftUI (none is 2), so a Query built by an arm64 program crashed when copied.
import Foundation
import SwiftData
import SwiftUI

@Model final class Note { var title: String; var n: Int; init(title: String, n: Int) { self.title = title; self.n = n } }

@inline(never) func copyIt<T>(_ x: T) -> T { let y = x; return y }

@MainActor func run() throws {
    let q = Query<Note, [Note]>(sort: \Note.title)
    let q2 = copyIt(q)
    _ = q2
    print("query copied")
    let d = Query<Note, [Note]>(filter: #Predicate<Note> { $0.n > 1 }, sort: \Note.n, order: .reverse)
    _ = copyIt(d)
    print("query with filter copied")
    let container = try ModelContainer(for: Note.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true))
    let ctx = container.mainContext
    for (i, t) in ["b", "c", "a"].enumerated() { ctx.insert(Note(title: t, n: i)) }
    try ctx.save()
    let all = try ctx.fetch(FetchDescriptor<Note>(sortBy: [SortDescriptor(\Note.title)]))
    print("fetched", all.map(\.title))
    let big = try ctx.fetch(FetchDescriptor<Note>(predicate: #Predicate { $0.n >= 1 }, sortBy: [SortDescriptor(\Note.n)]))
    print("filtered", big.map(\.title), "count", try ctx.fetchCount(FetchDescriptor<Note>()))
    all[0].n = 99
    try ctx.save()
    print("changed", try ctx.fetch(FetchDescriptor<Note>(predicate: #Predicate { $0.n == 99 })).map(\.title))
    ctx.delete(all[1]); try ctx.save()
    print("after delete", try ctx.fetchCount(FetchDescriptor<Note>()))
}
MainActor.assumeIsolated { try! run() }
