import Foundation
import SwiftData
enum V1: VersionedSchema { static var versionIdentifier = Schema.Version(1, 0, 0); static var models: [any PersistentModel.Type] { [V1.Note.self] }
    @Model final class Note { var text: String; init(text: String) { self.text = text } } }
enum V2: VersionedSchema { static var versionIdentifier = Schema.Version(2, 0, 0); static var models: [any PersistentModel.Type] { [V2.Note.self] }
    @Model final class Note { var text: String; var pinned: Bool = false; init(text: String, pinned: Bool = false) { self.text = text; self.pinned = pinned } } }
enum Plan: SchemaMigrationPlan { static var schemas: [any VersionedSchema.Type] { [V1.self, V2.self] }; static var stages: [MigrationStage] { [.lightweight(fromVersion: V1.self, toVersion: V2.self)] } }
@Model final class Tag { @Attribute(.unique) var name: String; var weight: Double; var meta: [String: Int]; var created: Date; var raw: Data?
    init(name: String, weight: Double = 1, meta: [String: Int] = [:]) { self.name = name; self.weight = weight; self.meta = meta; self.created = Date(timeIntervalSince1970: 1_700_000_000) } }
struct Info: Codable, Hashable { var a: Int; var b: String }
@Model final class Doc { var title: String; var info: Info; var kinds: [String]; init(title: String, info: Info, kinds: [String]) { self.title = title; self.info = info; self.kinds = kinds } }
@ModelActor actor Worker { func addTags(_ n: Int) throws { for i in 0..<n { modelContext.insert(Tag(name: "w\(i)", weight: Double(i))) }; try modelContext.save() }; func count() throws -> Int { try modelContext.fetchCount(FetchDescriptor<Tag>()) } }
@MainActor func main() async throws {
    let c = try ModelContainer(for: Tag.self, Doc.self, configurations: ModelConfiguration(isStoredInMemoryOnly: true)); let ctx = c.mainContext
    ctx.insert(Tag(name: "x", meta: ["a": 1, "b": 2])); ctx.insert(Tag(name: "x", weight: 9)); try ctx.save()   // unique constraint: upsert
    let t = try ctx.fetch(FetchDescriptor<Tag>()); print("unique upsert", t.count)
    ctx.insert(Doc(title: "d1", info: Info(a: 7, b: "seven"), kinds: ["k1", "k2"])); try ctx.save(); let d = try ctx.fetch(FetchDescriptor<Doc>()).first!; print("codable attr", d.info.a, d.info.b, d.kinds)
    let w = Worker(modelContainer: c); try await w.addTags(20); print("model actor", try await w.count(), try ctx.fetchCount(FetchDescriptor<Tag>()))
    let top = try ctx.fetch({ var f = FetchDescriptor<Tag>(sortBy: [SortDescriptor(\.weight, order: .reverse)]); f.fetchLimit = 3; return f }()); print("sorted", top.map { $0.name })
    var seen = 0; try ctx.enumerate(FetchDescriptor<Tag>(), batchSize: 5) { _ in seen += 1 }; print("enumerate", seen)
    try ctx.delete(model: Tag.self, where: #Predicate { $0.weight > 15 }); try ctx.save(); print("batch delete", try ctx.fetchCount(FetchDescriptor<Tag>()))
    let u = URL(fileURLWithPath: NSTemporaryDirectory() + "rebar_sd_\(getpid())"); try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
    do { let c1 = try ModelContainer(for: Schema(versionedSchema: V1.self), migrationPlan: nil, configurations: ModelConfiguration(url: u.appendingPathComponent("m.store"))); c1.mainContext.insert(V1.Note(text: "old")); try c1.mainContext.save() }
    let c2 = try ModelContainer(for: Schema(versionedSchema: V2.self), migrationPlan: Plan.self, configurations: ModelConfiguration(url: u.appendingPathComponent("m.store"))); let notes = try c2.mainContext.fetch(FetchDescriptor<V2.Note>()); print("migration", notes.map { "\($0.text):\($0.pinned)" })
    let c3 = try ModelContainer(for: Tag.self, configurations: ModelConfiguration(url: u.appendingPathComponent("p.store"))); c3.mainContext.insert(Tag(name: "persisted")); try c3.mainContext.save()
    let c4 = try ModelContainer(for: Tag.self, configurations: ModelConfiguration(url: u.appendingPathComponent("p.store"))); print("persisted", try c4.mainContext.fetch(FetchDescriptor<Tag>()).map { $0.name }); try? FileManager.default.removeItem(at: u)
}
try await main()
