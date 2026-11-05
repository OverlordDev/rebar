// SwiftUI + SwiftData (@Model, @Query, ModelContainer), Table, Map, TextEditor, fileImporter, NavigationSplitView, @Observable with @Bindable, ScrollView(.horizontal), TabView driven from a task.
import SwiftUI
import SwiftData
import MapKit
import Observation
import UniformTypeIdentifiers

@Model final class Note { var title: String; var stamp: Date; init(title: String) { self.title = title; self.stamp = Date() } }

@Observable final class Settings { var name = "rebar"; var count = 1 }

struct Row: Identifiable { let id = UUID(); let a: String; let b: Int }

struct ContentView: View {
    @Environment(\.modelContext) private var ctx
    @Query(sort: \Note.title) private var notes: [Note]
    @State private var settings = Settings()
    @State private var sel: String? = "one"
    @State private var text = "multi\nline"
    @State private var rows = (1...5).map { Row(a: "r\($0)", b: $0 * 3) }
    @State private var showImporter = false
    @State private var cam = MapCameraPosition.region(MKCoordinateRegion(center: .init(latitude: 50, longitude: 30), span: .init(latitudeDelta: 1, longitudeDelta: 1)))
    @State private var tab = 0
    var body: some View {
        TabView(selection: $tab) {
            NavigationSplitView {
                List(["one", "two", "three"], id: \.self, selection: $sel) { Text($0) }
                    .toolbar { ToolbarItem(placement: .primaryAction) { Button("add") { ctx.insert(Note(title: "n\(notes.count)")) } } }
            } detail: {
                if let sel { Text("detail \(sel) notes \(notes.count)") } else { ContentUnavailableView("none", systemImage: "tray") }
            }.tag(0)
            Table(rows) { TableColumn("a", value: \.a); TableColumn("b") { Text("\($0.b)") } }.tag(1)
            Form {
                Section("s1") { @Bindable var s = settings; TextField("name", text: $s.name); Stepper("c \(s.count)", value: $s.count) }
                Section("s2") { TextEditor(text: $text).frame(height: 80) }
                Section { Button("import") { showImporter = true } }
            }.fileImporter(isPresented: $showImporter, allowedContentTypes: [.plainText]) { _ in }.tag(2)
            Map(position: $cam) { Marker("m", coordinate: .init(latitude: 50, longitude: 30)) }.tag(3)
            ScrollView(.horizontal) { HStack { ForEach(0..<30) { i in Text("\(i)").frame(width: 40, height: 40).background(.mint) } } }.tag(4)
        }
        .task {
            func nap() async { try? await Task.sleep(nanoseconds: 600_000_000) }
            await nap(); ctx.insert(Note(title: "a")); ctx.insert(Note(title: "b")); try? ctx.save()
            for t in 1...4 { await nap(); tab = t }
            await nap(); settings.count = 7; settings.name = "x"; showImporter = true
            await nap(); showImporter = false; tab = 0
            await nap()
            let c = (try? ctx.fetchCount(FetchDescriptor<Note>())) ?? -1
            print("notes \(notes.count) fetch \(c) settings \(settings.name) \(settings.count) sel \(sel ?? "-")"); fflush(stdout); exit(0)
        }
    }
}

@main struct DemoApp: App { var body: some Scene { WindowGroup { ContentView() }.modelContainer(for: Note.self, inMemory: true) } }
