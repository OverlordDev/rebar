import SwiftUI
import AppKit

@main
struct RebarGPUCheckApp: App {
    @StateObject private var runner = Runner()
    var body: some Scene {
        WindowGroup {
            ContentView().environmentObject(runner).frame(minWidth: 780, minHeight: 760)
                .onAppear { if CommandLine.arguments.contains("--autorun") { runner.autoQuit = true; if let only = ProcessInfo.processInfo.environment["RGC_ONLY"] { runner.runOnly(only.components(separatedBy: ",")) } else { runner.runAll() } } }
        }
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}

struct ContentView: View {
    @EnvironmentObject var r: Runner

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Rebar GPU Check").font(.title2).bold()
                Text("Проверяет, показывает ли картинку arm64-приложение с GPU-рендером (Metal), запущенное через rebar на этой видеокарте. Весь тест занимает несколько минут: появятся окна Chrome и VS Code, их не нужно трогать.")
                    .font(.callout).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            targetBox
            if !r.screenAccess {
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundColor(.orange)
                    Text("Нет доступа к записи экрана: приложение не сможет само увидеть окна и будет спрашивать вас. Разрешите в Системные настройки → Конфиденциальность и безопасность → Запись экрана и перезапустите приложение.")
                        .font(.callout).fixedSize(horizontal: false, vertical: true)
                    Button("Открыть настройки") {
                        if let u = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") { NSWorkspace.shared.open(u) }
                    }
                }
            }
            VStack(spacing: 6) { ForEach(r.tests) { TestRow(test: $0) } }
            HStack {
                Button(action: { r.runAll() }) { Label("Запустить все тесты", systemImage: "play.fill") }.disabled(r.busy)
                Button(action: { r.stop() }) { Label("Стоп", systemImage: "stop.fill") }.disabled(!r.busy)
                Toggle("Подробный лог GPU (медленнее)", isOn: $r.verbose).disabled(r.busy)
                Spacer()
                Button(action: { r.saveReport() }) { Label("Сохранить отчёт", systemImage: "square.and.arrow.down") }.disabled(r.busy)
                Button("Копировать") { r.copyReport() }.disabled(r.busy)
            }
            if !r.verdictText.isEmpty {
                Text(r.verdictText).font(.callout).padding(8).frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.accentColor.opacity(0.12)).cornerRadius(6).fixedSize(horizontal: false, vertical: true)
            }
            if !r.reportPath.isEmpty { Text("Отчёт: \(r.reportPath) — отправьте этот файл.").font(.caption).foregroundColor(.secondary) }
            LogView(text: r.logText).frame(minHeight: 170)
            Text("В отчёт попадают только данные о системе и видеокарте, логи тестов и скриншоты окон тестов. Личных файлов приложение не читает.").font(.caption).foregroundColor(.secondary)
        }
        .padding(16)
    }

    var targetBox: some View {
        HStack(spacing: 8) {
            Image(systemName: "shippingbox").foregroundColor(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text("VS Code для arm64").font(.subheadline).bold()
                Text(r.electronNote).font(.caption).foregroundColor(.secondary)
            }
            Spacer()
            if let p = r.downloadProgress { ProgressView(value: p).frame(width: 140) }
            Button("Скачать VS Code \(Runner.vscodeVersion)") { r.downloadElectron() }.disabled(r.busy || r.downloadProgress != nil)
            Button("Выбрать…") { r.chooseElectron() }.disabled(r.busy)
        }
        .padding(8).background(Color.secondary.opacity(0.08)).cornerRadius(6)
    }
}

struct TestRow: View {
    @ObservedObject var test: Test
    @EnvironmentObject var r: Runner

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            icon.frame(width: 20, height: 20)
            VStack(alignment: .leading, spacing: 1) {
                Text(test.title).font(.subheadline).bold()
                Text(test.summary.isEmpty ? test.detail : test.summary).font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Spacer()
            Button("Запустить") { r.run(test) }.disabled(r.busy)
        }
    }

    @ViewBuilder var icon: some View {
        switch test.status {
        case .idle: Image(systemName: "circle").foregroundColor(.secondary)
        case .running: ProgressView().scaleEffect(0.5)
        case .ok: Image(systemName: "checkmark.circle.fill").foregroundColor(.green)
        case .bad: Image(systemName: "xmark.octagon.fill").foregroundColor(.red)
        case .skipped: Image(systemName: "minus.circle").foregroundColor(.gray)
        case .error: Image(systemName: "exclamationmark.triangle.fill").foregroundColor(.orange)
        }
    }
}

struct LogView: NSViewRepresentable {
    let text: String
    func makeNSView(context: Context) -> NSScrollView {
        let sv = NSTextView.scrollableTextView()
        if let tv = sv.documentView as? NSTextView {
            tv.isEditable = false
            tv.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
            tv.backgroundColor = NSColor.textBackgroundColor
        }
        return sv
    }
    func updateNSView(_ sv: NSScrollView, context: Context) {
        guard let tv = sv.documentView as? NSTextView, tv.string != text else { return }
        tv.string = text
        tv.scrollToEndOfDocument(nil)
    }
}
