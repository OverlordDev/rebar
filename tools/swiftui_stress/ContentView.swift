import SwiftUI

// A SwiftUI stress test for the Xcode canvas / previews and for any SwiftUI app run through rebar: a big diffing List, a LazyVGrid, a Canvas redrawn
// 20 times a second, a stack of AnyHashable-keyed ForEach rows (type-erased identities, optional-of-optional selections), animations, a sheet,
// and a timer that keeps mutating state. Paste it as ContentView.swift in a macOS SwiftUI app (macOS 13+).

struct Item: Identifiable, Hashable {
    let id: Int
    var title: String
    var value: Double
    var tags: [String]
}

struct ContentView: View {
    @State private var items: [Item] = (0..<3000).map { Item(id: $0, title: "Row \($0)", value: Double($0 % 100) / 100, tags: ["a\($0 % 7)", "b\($0 % 13)"]) }
    @State private var selection: AnyHashable? = nil
    @State private var nested: AnyHashable?? = .some(nil)
    @State private var mode = 0
    @State private var auto = true
    @State private var tick = 0
    @State private var showSheet = false
    @State private var query = ""
    @State private var sortUp = true
    @State private var phase = 0.0
    private let timer = Timer.publish(every: 0.05, on: .main, in: .common).autoconnect()

    var filtered: [Item] {
        let base = query.isEmpty ? items : items.filter { $0.title.localizedCaseInsensitiveContains(query) || $0.tags.contains(query) }
        return base.sorted { sortUp ? $0.value < $1.value : $0.value > $1.value }
    }

    var body: some View {
        VStack(spacing: 10) {
            HStack {
                Text("Stress \(tick)").font(.title2.monospacedDigit())
                Spacer()
                Toggle("auto", isOn: $auto)
                Button("Sheet") { showSheet = true }
                Button(sortUp ? "Sort ↓" : "Sort ↑") { withAnimation(.spring()) { sortUp.toggle() } }
                Button("Shuffle") { withAnimation(.easeInOut(duration: 0.4)) { items.shuffle() } }
            }
            TextField("filter (title or tag a0…a6)", text: $query).textFieldStyle(.roundedBorder)
            Picker("", selection: $mode) {
                Text("List").tag(0); Text("Grid").tag(1); Text("Canvas").tag(2); Text("AnyHashable").tag(3); Text("Nested").tag(4)
            }.pickerStyle(.segmented)
            Group {
                switch mode {
                case 0: listView
                case 1: gridView
                case 2: canvasView
                case 3: anyHashableView
                default: nestedView
                }
            }
            .frame(minWidth: 520, minHeight: 360)
            .transition(.opacity.combined(with: .scale))
            Text("selection: \(String(describing: selection))  nested: \(String(describing: nested))").font(.caption).lineLimit(1)
        }
        .padding()
        .animation(.default, value: mode)
        .onReceive(timer) { _ in
            guard auto else { return }
            tick += 1
            phase += 0.07
            if tick % 4 == 0 { mutate() }
            if tick % 40 == 0 { mode = (mode + 1) % 5 }
        }
        .sheet(isPresented: $showSheet) {
            VStack(spacing: 12) {
                Text("A sheet over a live, mutating view").font(.headline)
                ProgressView(value: Double(tick % 100), total: 100)
                Button("Close") { showSheet = false }.keyboardShortcut(.defaultAction)
            }.padding(30).frame(width: 360)
        }
    }

    private func mutate() {
        guard !items.isEmpty else { return }
        for _ in 0..<40 {
            let i = Int.random(in: 0..<items.count)
            items[i].value = Double.random(in: 0...1)
            items[i].title = "Row \(items[i].id) · \(Int.random(in: 0..<1000))"
        }
        if tick % 20 == 0 { let a = Int.random(in: 0..<items.count), b = Int.random(in: 0..<items.count); items.swapAt(a, b) }
        if tick % 8 == 0 {
            selection = [AnyHashable(items[0].id), AnyHashable(items[0].title), AnyHashable(items[0].tags), nil][tick / 8 % 4]
            nested = [.some(nil), .some(AnyHashable(tick)), nil][tick / 8 % 3]
        }
    }

    private var listView: some View {
        List(filtered) { item in
            HStack {
                Circle().fill(Color(hue: item.value, saturation: 0.8, brightness: 0.9)).frame(width: 12, height: 12)
                Text(item.title)
                Spacer()
                ForEach(item.tags, id: \.self) { Text($0).font(.caption2).padding(3).background(.quaternary, in: RoundedRectangle(cornerRadius: 4)) }
                ProgressView(value: item.value).frame(width: 80)
            }
            .contentShape(Rectangle())
            .onTapGesture { selection = AnyHashable(item) }
        }
    }

    private var gridView: some View {
        ScrollView {
            LazyVGrid(columns: Array(repeating: GridItem(.flexible(minimum: 40)), count: 12), spacing: 4) {
                ForEach(filtered.prefix(1200)) { item in
                    RoundedRectangle(cornerRadius: 4)
                        .fill(Color(hue: item.value, saturation: 0.7, brightness: 0.95))
                        .frame(height: 28)
                        .overlay(Text("\(item.id)").font(.system(size: 9).monospacedDigit()))
                        .scaleEffect(1 + 0.15 * sin(phase + Double(item.id)))
                }
            }
        }
    }

    private var canvasView: some View {
        TimelineView(.animation) { ctx in
            Canvas { gc, size in
                let t = ctx.date.timeIntervalSinceReferenceDate
                for i in 0..<600 {
                    let a = Double(i) * 0.21 + t
                    let r = 40 + Double(i % 60) * Double(min(size.width, size.height)) / 130
                    let p = CGPoint(x: size.width / 2 + r * cos(a), y: size.height / 2 + r * sin(a * 1.3))
                    gc.fill(Path(ellipseIn: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10)), with: .color(Color(hue: (Double(i) / 600 + t / 7).truncatingRemainder(dividingBy: 1), saturation: 0.8, brightness: 1)))
                }
            }
        }
    }

    private var anyHashableView: some View {
        let keys: [AnyHashable] = (0..<400).map { i in
            switch i % 5 { case 0: return AnyHashable(i); case 1: return AnyHashable("k\(i)"); case 2: return AnyHashable(Double(i) / 3); case 3: return AnyHashable([i, i + 1]); default: return AnyHashable(items[i % items.count].tags) }
        }
        return ScrollView {
            LazyVStack(alignment: .leading) {
                ForEach(Array(keys.enumerated()), id: \.element) { n, key in
                    HStack { Text("\(n)").frame(width: 40); Text(String(describing: key.base)).lineLimit(1); Spacer() }
                        .padding(.horizontal, 6).background(selection == key ? Color.accentColor.opacity(0.3) : .clear)
                        .onTapGesture { selection = selection == key ? nil : key }
                }
            }
        }
    }

    private var nestedView: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(0..<30, id: \.self) { i in
                let v: AnyHashable?? = i % 3 == 0 ? .some(nil) : (i % 3 == 1 ? nil : .some(AnyHashable(i)))
                HStack { Text("#\(i)").frame(width: 40); Text(String(describing: v)).font(.system(.body, design: .monospaced)); Spacer(); Text(v == nested ? "== nested" : "") }
            }
            Spacer()
        }
    }
}

#Preview { ContentView() }
