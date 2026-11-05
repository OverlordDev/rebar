// SwiftUI breadth test: NavigationStack(path:) + NavigationLink(value:), LazyVGrid, ScrollView/ScrollViewReader, Canvas, TimelineView, custom Layout, Grid, GeometryReader, matchedGeometryEffect, transitions, @AppStorage, @FocusState, ObservableObject/@EnvironmentObject, Combine, DatePicker, Stepper, DisclosureGroup, Menu, alert, confirmationDialog, searchable, ViewThatFits, ImageRenderer, Shape/Path, gradients, drag gesture state.
import SwiftUI
import Combine

final class Store: ObservableObject {
    @Published var count = 0
    @Published var names = ["ann", "bob", "cy", "dee", "eve"]
    var cancellables = Set<AnyCancellable>()
}

struct Flow: Layout {
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        var w = 0.0, h = 0.0
        for s in subviews { let z = s.sizeThatFits(.unspecified); w += z.width; h = max(h, z.height) }
        return CGSize(width: w, height: h)
    }
    func placeSubviews(in b: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = b.minX
        for s in subviews { let z = s.sizeThatFits(.unspecified); s.place(at: CGPoint(x: x, y: b.minY), proposal: .unspecified); x += z.width }
    }
}

struct Star: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path(); let c = CGPoint(x: r.midX, y: r.midY)
        for i in 0..<10 { let a: CGFloat = CGFloat(i) * .pi / 5 - .pi / 2; let rad: CGFloat = i % 2 == 0 ? r.width / 2 : r.width / 4
            let pt = CGPoint(x: c.x + rad * cos(a), y: c.y + rad * sin(a)); if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) } }
        p.closeSubpath(); return p
    }
}

enum Route: Hashable { case detail(String), deep(Int) }

struct ContentView: View {
    @EnvironmentObject var store: Store
    @AppStorage("rebar.swiftui3.counter") private var stored = 0
    @State private var path: [Route] = []
    @State private var tab = 0
    @State private var date = Date(timeIntervalSince1970: 86400 * 365)
    @State private var step = 3
    @State private var text = ""
    @State private var search = ""
    @State private var alert = false
    @State private var dialog = false
    @State private var flip = false
    @State private var drag = CGSize.zero
    @State private var expanded = false
    @State private var scrollTo = 0
    @FocusState private var focused: Bool
    @Namespace private var ns

    var body: some View {
        TabView(selection: $tab) {
            NavigationStack(path: $path) {
                List(store.names.filter { search.isEmpty || $0.contains(search) }, id: \.self) { n in
                    NavigationLink(value: Route.detail(n)) { Text(n) }
                }
                .searchable(text: $search)
                .navigationTitle("Names")
                .navigationDestination(for: Route.self) { r in
                    switch r {
                    case .detail(let n): VStack { Text("detail \(n)"); NavigationLink("deeper", value: Route.deep(n.count)) }
                    case .deep(let k): Text("deep \(k)")
                    }
                }
            }.tabItem { Label("Nav", systemImage: "list.bullet") }.tag(0)

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 4), spacing: 6) {
                        ForEach(0..<80, id: \.self) { i in
                            RoundedRectangle(cornerRadius: 6).fill(Color(hue: Double(i) / 80, saturation: 0.6, brightness: 0.9)).frame(height: 40).overlay(Text("\(i)").font(.caption)).id(i)
                        }
                    }.padding()
                }.onChange(of: scrollTo) { _, v in withAnimation { proxy.scrollTo(v, anchor: .top) } }
            }.tabItem { Label("Grid", systemImage: "square.grid.2x2") }.tag(1)

            ScrollView {
                VStack(spacing: 12) {
                    Flow { Text("a").padding(); Text("bb").padding(); Text("ccc").padding() }
                    Grid { GridRow { Text("1"); Text("2") }; GridRow { Text("3"); Text("4") } }
                    GeometryReader { g in Text("w \(Int(g.size.width))").frame(width: g.size.width, height: 20) }.frame(height: 20)
                    TimelineView(.animation(minimumInterval: 0.2)) { c in Text("t \(Int(c.date.timeIntervalSince1970) % 100)") }
                    Canvas { ctx, size in
                        ctx.fill(Star().path(in: CGRect(x: 0, y: 0, width: size.height, height: size.height)), with: .linearGradient(Gradient(colors: [.yellow, .red]), startPoint: .zero, endPoint: CGPoint(x: size.height, y: size.height)))
                        ctx.stroke(Path(ellipseIn: CGRect(x: 80, y: 5, width: 60, height: 40)), with: .color(.blue), lineWidth: 3)
                        ctx.draw(Text("canvas"), at: CGPoint(x: 200, y: 25))
                    }.frame(height: 60)
                    Star().fill(AngularGradient(colors: [.red, .green, .blue, .red], center: .center)).frame(width: 60, height: 60).shadow(radius: 4).blur(radius: 0.5)
                    HStack {
                        if flip { Circle().fill(.red).matchedGeometryEffect(id: "m", in: ns).frame(width: 30, height: 30) }
                        Spacer()
                        if !flip { Circle().fill(.red).matchedGeometryEffect(id: "m", in: ns).frame(width: 30, height: 30) }
                    }.animation(.spring(), value: flip)
                    if flip { Text("transition").transition(.move(edge: .leading).combined(with: .opacity)) }
                    Text("drag \(Int(drag.width))").offset(drag).gesture(DragGesture().onChanged { drag = $0.translation }.onEnded { _ in drag = .zero })
                    ViewThatFits { Text("a very long piece of text that will not fit narrow widths at all, really").frame(width: 2000); Text("short") }
                    DatePicker("date", selection: $date, displayedComponents: .date)
                    Stepper("step \(step)", value: $step, in: 0...9)
                    DisclosureGroup("more", isExpanded: $expanded) { Text("hidden") }
                    Menu("menu") { Button("one") {}; Button("two") {} }
                    TextField("type", text: $text).focused($focused).textFieldStyle(.roundedBorder)
                    ProgressView(value: 0.3); ProgressView()
                    Button("alert") { alert = true }.alert("hey", isPresented: $alert) { Button("ok") {} }
                    Button("dialog") { dialog = true }.confirmationDialog("pick", isPresented: $dialog) { Button("x") {}; Button("y") {} }
                    Text("ctx").contextMenu { Button("copy") {} }
                    LabeledContent("label", value: "value")
                }.padding()
            }.tabItem { Label("Misc", systemImage: "ellipsis") }.tag(2)
        }
        .task {
            func nap() async { try? await Task.sleep(nanoseconds: 450_000_000) }
            await nap(); path = [.detail("ann")]
            await nap(); path.append(.deep(3))
            await nap(); path = []; search = "e"
            await nap(); search = ""; tab = 1
            await nap(); scrollTo = 60
            await nap(); tab = 2
            await nap(); flip = true; expanded = true; step = 5; focused = true; text = "hello"
            await nap(); alert = true
            await nap(); alert = false; dialog = true
            await nap(); dialog = false; drag = CGSize(width: 12, height: 3)
            await nap(); flip = false; tab = 0
            await nap()
            while summary.isEmpty { await nap() }
            print(summary)
            print("ui path \(path.count) tab \(tab) step \(step) text \(text) expanded \(expanded) drag \(Int(drag.width))"); fflush(stdout); exit(0)
        }
    }
}

@main struct DemoApp: App {
    @StateObject var store = Store()
    @Environment(\.scenePhase) var phase
    var body: some Scene {
        WindowGroup { ContentView().environmentObject(store).task { await drive(store) } }
    }
}

@MainActor func drive(_ store: Store) async {
    var got: [Int] = []
    store.$count.sink { got.append($0) }.store(in: &store.cancellables)
    for i in 1...6 { try? await Task.sleep(nanoseconds: 400_000_000); store.count = i }
    let r = ImageRenderer(content: ZStack { Color.red; Star().fill(.white) }.frame(width: 40, height: 40))
    r.scale = 1
    let img = r.cgImage
    UserDefaults.standard.set(41, forKey: "rebar.swiftui3.counter")
    summary = "combine \(got) render \(img?.width ?? -1)x\(img?.height ?? -1) stored \(UserDefaults.standard.integer(forKey: "rebar.swiftui3.counter"))"
}
@MainActor var summary = ""
