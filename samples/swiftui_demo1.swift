import SwiftUI
struct ContentView: View {
    @State private var n = 0
    var body: some View {
        VStack(spacing: 12) {
            Text("SwiftUI under rebar").font(.title)
            Text("count \(n)")
            Button("Add") { n += 1 }
            List(0..<5, id: \.self) { Text("row \($0)") }.frame(height: 120)
        }.padding().frame(width: 320, height: 320)
    }
}
@main struct DemoApp: App { var body: some Scene { WindowGroup { ContentView() } } }
