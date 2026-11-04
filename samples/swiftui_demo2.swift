import SwiftUI
import Charts
struct Item: Identifiable { let id = UUID(); let name: String; let value: Double }
struct ContentView: View {
    @State private var sel: String? = "Charts"
    @State private var on = true
    @State private var text = "rebar"
    @State private var pick = 1
    @State private var slider = 0.4
    let data = (1...8).map { Item(name: "M\($0)", value: Double(($0 * 37) % 11 + 2)) }
    var body: some View {
        NavigationSplitView {
            List(["Charts", "Form", "Canvas"], id: \.self, selection: $sel) { Label($0, systemImage: "star") }
        } detail: {
            VStack(spacing: 10) {
                Text("SwiftUI 2 under rebar").font(.title2).bold()
                Chart(data) { BarMark(x: .value("m", $0.name), y: .value("v", $0.value)).foregroundStyle(.blue.gradient) }.frame(height: 110)
                Form {
                    Toggle("Enabled", isOn: $on)
                    TextField("Name", text: $text)
                    Picker("Mode", selection: $pick) { Text("A").tag(0); Text("B").tag(1); Text("C").tag(2) }.pickerStyle(.segmented)
                    Slider(value: $slider)
                }.frame(height: 120)
                Canvas { ctx, size in
                    for i in 0..<12 { let fi = Double(i); let r = CGRect(x: fi * size.width * 0.083, y: size.height * 0.5 - fi * 3.0, width: 14.0, height: fi * 6.0 + 6.0)
                        ctx.fill(Path(roundedRect: r, cornerRadius: 4), with: .color(Color.orange.opacity(0.4 + Double(i) * 0.05))) }
                }.frame(height: 60)
                Text("\(on ? "on" : "off") \(text) \(Int(slider * 100))%")
            }.padding()
        }.frame(width: 640, height: 480)
    }
}
@main struct Demo2App: App { var body: some Scene { WindowGroup { ContentView() } } }
