// A larger iOS SwiftUI app: @Observable model, async Task, sheet, List with ForEach and swipe-free rows, animation, Form with Toggle/Slider/Picker, Charts, state driven from a timer for ~8 s (AttributeGraph updates).
import SwiftUI
import Charts
import Observation

@Observable final class Model {
    var ticks = 0
    var items: [String] = (1...20).map { "item \($0)" }
    var log: [String] = []
    func tick() { ticks += 1; if ticks % 3 == 0 { items.insert("new \(ticks)", at: 0) }; if ticks % 5 == 0, !items.isEmpty { items.removeLast() } }
}

struct Point: Identifiable { let id: Int; let x: Int; let y: Double }

struct ContentView: View {
    @State private var model = Model()
    @State private var sheet = false
    @State private var on = true
    @State private var level = 0.5
    @State private var choice = 1
    @State private var tab = 0
    var body: some View {
        VStack(spacing: 0) {
            Picker("tab", selection: $tab) { Text("List").tag(0); Text("Form").tag(1); Text("Chart").tag(2) }.pickerStyle(.segmented).padding()
            switch tab {
            case 0:
                NavigationStack {
                    List { ForEach(model.items, id: \.self) { Text($0) } }.navigationTitle("Items \(model.ticks)")
                        .toolbar { Button("Sheet") { sheet = true } }
                }
            case 1:
                Form {
                    Toggle("On", isOn: $on)
                    Slider(value: $level)
                    Picker("Pick", selection: $choice) { Text("A").tag(0); Text("B").tag(1); Text("C").tag(2) }
                    Text("level \(level, specifier: "%.2f")").animation(.easeInOut, value: level)
                }
            default:
                Chart((0..<10).map { Point(id: $0, x: $0, y: Double($0 * $0 % 7)) }) { LineMark(x: .value("x", $0.x), y: .value("y", $0.y)) }.padding()
            }
        }
        .sheet(isPresented: $sheet) { Text("Sheet \(model.ticks)").presentationDetents([.medium]) }
        .task {
            for i in 0..<16 {
                try? await Task.sleep(nanoseconds: 500_000_000)
                model.tick()
                if i == 2 { tab = 1; level = 0.9 }
                if i == 5 { tab = 2 }
                if i == 8 { tab = 0; sheet = true }
                if i == 11 { sheet = false }
            }
            print("done ticks \(model.ticks) items \(model.items.count) tab \(tab) level \(level)"); fflush(stdout); exit(0)
        }
    }
}

@main struct DemoApp: App { var body: some Scene { WindowGroup { ContentView() } } }
