import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }   // (the apps that were started keep running)
}

@main
struct RebarLauncherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = Launcher()
    init() { if let i = CommandLine.arguments.firstIndex(of: "--selftest") { exit(Launcher.selfTest(Array(CommandLine.arguments[(i + 1)...]))) } }
    var body: some Scene {
        WindowGroup {
            ContentView().environmentObject(model).frame(minWidth: 620, minHeight: 520)
        }
        .commands { CommandGroup(replacing: .newItem) {} }
    }
}

struct ContentView: View {
    @EnvironmentObject var model: Launcher
    @State private var filter = ""
    @State private var editing: AppEntry?

    private var shown: [AppEntry] { filter.isEmpty ? model.apps : model.apps.filter { $0.name.localizedCaseInsensitiveContains(filter) } }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Rebar").font(.title2).bold()
                    Text("Приложения для Apple Silicon (arm64), которые можно запустить на этом Mac").font(.callout).foregroundColor(.secondary)
                }
                Spacer()
                Button(action: { model.scan() }) { Image(systemName: "arrow.clockwise") }.help("Обновить список").disabled(model.scanning)
                Button("Добавить…") { choose() }.help("Добавить приложение или папку с приложениями (например, из открытого .dmg)")
            }
            TextField("Поиск", text: $filter)
            if shown.isEmpty {
                Spacer()
                Text(model.scanning ? "Ищу приложения…" : "Приложений только для arm64 не нашлось в «Программах». Нажмите «Добавить…» и укажите приложение или папку.")
                    .foregroundColor(.secondary).frame(maxWidth: .infinity)
                Spacer()
            } else {
                List { ForEach(shown) { row($0) } }
            }
            if !model.message.isEmpty {
                Text(model.message).font(.callout).foregroundColor(.red).fixedSize(horizontal: false, vertical: true)
            }
            Text("Работает не со всеми приложениями: rebar — эмулятор, а первый запуск может занять до минуты.")
                .font(.caption).foregroundColor(.secondary)
        }
        .padding(16)
        .sheet(item: $editing) { SettingsSheet(app: $0).environmentObject(model) }
        .onAppear {   // (tests)
            let e = ProcessInfo.processInfo.environment
            if let p = e["RL_SETTINGS"] { DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { editing = model.apps.first { $0.path == p } } }
            if let p = e["RL_LAUNCH"] { DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { if let a = model.apps.first(where: { $0.path == p }) { model.launch(a) } } }
        }
    }

    private func row(_ app: AppEntry) -> some View {
        HStack(spacing: 10) {
            Image(nsImage: model.icons[app.path] ?? NSImage()).resizable().frame(width: 36, height: 36)
            VStack(alignment: .leading, spacing: 1) {
                Text(app.name).font(.headline)
                Text([app.version, app.engine ?? "", "только arm64"].filter { !$0.isEmpty }.joined(separator: " · ")).font(.caption).foregroundColor(.secondary)
            }
            Spacer()
            if model.running[app.path] != nil {
                Text(model.status(app)).font(.caption).foregroundColor(model.status(app) == "Запущено" ? .green : .orange)
                Button("Остановить") { model.stop(app) }.help("Закрыть приложение и всё, что от него осталось")
                Button("Перезапустить") { model.restart(app) }.help("Закрыть и запустить заново: помогает, если приложение запущено, но окна нет")
            } else {
                Button("Запустить") { model.launch(app) }
            }
            Button(action: { editing = app }) { Image(systemName: "gearshape") }.buttonStyle(BorderlessButtonStyle()).help("Настройки этого приложения")
        }
        .padding(.vertical, 3)
    }

    private func choose() {
        let p = NSOpenPanel()
        p.canChooseFiles = true; p.canChooseDirectories = true; p.allowsMultipleSelection = false
        p.message = "Выберите приложение (.app) или папку, в которой искать приложения"
        p.directoryURL = URL(fileURLWithPath: "/Volumes")
        if p.runModal() == .OK, let u = p.url { model.add(u) }
    }
}

struct SettingsSheet: View {
    let app: AppEntry
    @EnvironmentObject var model: Launcher
    @Environment(\.presentationMode) private var presentation
    @State private var s = AppSettings()

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Image(nsImage: model.icons[app.path] ?? NSImage()).resizable().frame(width: 32, height: 32)
                Text(app.name).font(.title3).bold()
            }
            if app.isElectron {
                Toggle("Ускорение на видеокарте (GPU)", isOn: $s.gpu)
                hint("Окно рисует видеокарта (Metal): прокрутка заметно плавнее. Если окно пустое или в нём мусор, выключите.")
                Toggle("Без песочницы Chromium", isOn: $s.noSandbox)
                hint("Под rebar песочница не запускается. Выключайте, только если точно знаете зачем.")
                Toggle("Не спрашивать пароль Связки ключей", isOn: $s.mockKeychain)
                hint("Включено: ключ, которым приложение шифрует свои данные (вход в аккаунты и т. п.), не хранится в Связке ключей и защищён слабее. Выключено: macOS будет спрашивать пароль от вашей связки ключей.")
            }
            if model.vsCodeFolder(app) != nil {
                Toggle("Быстрый скролл (облегчённый редактор) — включён по умолчанию", isOn: $s.fastEditor)
                hint("Выключает в редакторе миникарту, закреплённые заголовки (sticky scroll), хлебные крошки, цветные скобки и подсветку совпадений и текущей строки. Прокрутка плавнее: около 60–68 кадров/с вместо ~50. В ваш settings.json добавляется один блок между строками «// >>> rebar» и «// <<< rebar»; выключите переключатель, и он удалится (копия файла до изменения: settings.json.rebar-backup). Настройки с теми же именами, которые вы сами задали ниже в файле, имеют приоритет.")
            }
            Toggle("Быстрый режим (JIT)", isOn: $s.jit)
            hint("rebar переводит код в машинный на лету, это в несколько раз быстрее. Выключайте только при поиске ошибок.")
            Text("Аргументы командной строки").font(.subheadline)
            TextField("например: --user-data-dir=\"/Users/me/my profile\"", text: $s.args)
            Text("Переменные окружения (по одной в строке: ИМЯ=значение)").font(.subheadline)
            TextEditor(text: $s.env).font(.system(.body, design: .monospaced)).frame(height: 60)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.secondary.opacity(0.4)))
            hint("Изменения применяются при следующем запуске.")
            HStack {
                Button("Показать лог") { NSWorkspace.shared.open(URL(fileURLWithPath: model.logPath(app))) }
                Button("Сбросить") { s = AppSettings() }
                Spacer()
                Button("Готово") { model.setSettings(s, for: app); presentation.wrappedValue.dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20).frame(width: 500)
        .onAppear { s = model.settings(app) }
    }

    private func hint(_ t: String) -> some View {
        Text(t).font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
    }
}
