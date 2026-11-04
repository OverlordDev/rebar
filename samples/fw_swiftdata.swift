import Foundation
import SwiftData
import CoreData
@Model final class Person { var name: String; var age: Int; @Relationship(deleteRule: .cascade) var pets: [Pet] = []; init(name: String, age: Int) { self.name = name; self.age = age } }
@Model final class Pet { var name: String; var owner: Person?; init(name: String) { self.name = name } }
@MainActor func run() throws {
    let cfg = ModelConfiguration(isStoredInMemoryOnly: true)
    let container = try ModelContainer(for: Person.self, Pet.self, configurations: cfg); let ctx = container.mainContext
    let a = Person(name: "Ann", age: 33), b = Person(name: "Bob", age: 41), c = Person(name: "Cy", age: 19)
    [a, b, c].forEach { ctx.insert($0) }; a.pets.append(Pet(name: "Rex")); a.pets.append(Pet(name: "Tom")); b.pets.append(Pet(name: "Fido")); try ctx.save()
    let all = try ctx.fetch(FetchDescriptor<Person>(sortBy: [SortDescriptor(\.age)])); print("fetch", all.map { "\($0.name):\($0.age):\($0.pets.count)" })
    let older = try ctx.fetch(FetchDescriptor<Person>(predicate: #Predicate { $0.age > 30 }, sortBy: [SortDescriptor(\.name, order: .reverse)])); print("predicate", older.map { $0.name })
    print("count", try ctx.fetchCount(FetchDescriptor<Pet>()))
    b.age = 42; try ctx.save(); print("update", try ctx.fetch(FetchDescriptor<Person>(predicate: #Predicate { $0.name == "Bob" })).first!.age)
    ctx.delete(a); try ctx.save(); print("cascade", try ctx.fetchCount(FetchDescriptor<Person>()), try ctx.fetchCount(FetchDescriptor<Pet>()))
    var d = FetchDescriptor<Person>(sortBy: [SortDescriptor(\.age)]); d.fetchLimit = 1; print("limit", try ctx.fetch(d).map { $0.name })
    try ctx.transaction { ctx.insert(Person(name: "Dee", age: 50)) }; print("transaction", try ctx.fetchCount(FetchDescriptor<Person>()))
}
try MainActor.assumeIsolated { try run() }
// Core Data underneath, with a programmatic model
let model = NSManagedObjectModel(); let e = NSEntityDescription(); e.name = "Item"; e.managedObjectClassName = "NSManagedObject"
let at = NSAttributeDescription(); at.name = "title"; at.attributeType = .stringAttributeType; let an = NSAttributeDescription(); an.name = "n"; an.attributeType = .integer64AttributeType; e.properties = [at, an]; model.entities = [e]
let pc = NSPersistentContainer(name: "T", managedObjectModel: model); let sd = NSPersistentStoreDescription(); sd.type = NSInMemoryStoreType; pc.persistentStoreDescriptions = [sd]
var lerr: Error?; pc.loadPersistentStores { _, e in lerr = e }; print("coredata store", lerr == nil)
let mc = pc.viewContext; for i in 1...5 { let o = NSEntityDescription.insertNewObject(forEntityName: "Item", into: mc); o.setValue("t\(i)", forKey: "title"); o.setValue(i * i, forKey: "n") }; try mc.save()
let fr = NSFetchRequest<NSManagedObject>(entityName: "Item"); fr.predicate = NSPredicate(format: "n > 4"); fr.sortDescriptors = [NSSortDescriptor(key: "n", ascending: false)]; print("coredata fetch", try mc.fetch(fr).map { $0.value(forKey: "title") as! String })
