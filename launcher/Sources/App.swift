import SwiftUI
import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var appearanceWatch: NSKeyValueObservation?
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }   // (the apps that were started keep running)
    func applicationDidFinishLaunching(_ notification: Notification) {
        applyIcon()
        appearanceWatch = NSApp.observe(\.effectiveAppearance) { [weak self] _, _ in DispatchQueue.main.async { self?.applyIcon() } }
    }
    /// The Dock shows the dark icon while the system is dark and the light one otherwise (the file in Finder is the light one: macOS before 26 has no icon that follows the appearance).
    private func applyIcon() {
        let dark = NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        if let url = Bundle.main.url(forResource: dark ? "icon-dark" : "icon-light", withExtension: "png"), let img = NSImage(contentsOf: url) { NSApp.applicationIconImage = img }
    }
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
    @State private var wrapperOpen = false

    private var shown: [AppEntry] { filter.isEmpty ? model.apps : model.apps.filter { $0.name.localizedCaseInsensitiveContains(filter) } }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Rebar").font(.title2).bold()
                    Text("Apps for Apple Silicon (arm64) that you can run on this Mac").font(.callout).foregroundColor(.secondary)
                }
                Spacer()
                WrapperCheckbox(wrap: model.wrapper, apps: model.apps)
                Button(action: { wrapperOpen = true }) { Image(systemName: "gearshape.2") }.help("Wrapper: which apps, folders, environment")
                Button(action: { model.scan() }) { Image(systemName: "arrow.clockwise") }.help("Refresh the list").disabled(model.scanning)
                Button("Add…") { choose() }.help("Add an app, an iOS app (.ipa) or a folder with apps (for example from an open .dmg)")
            }
            TextField("Search", text: $filter)
            if shown.isEmpty {
                Spacer()
                Text(model.scanning ? "Looking for apps…" : "No arm64-only apps found in Applications. Click “Add…” and choose an app or a folder.")
                    .foregroundColor(.secondary).frame(maxWidth: .infinity)
                Spacer()
            } else {
                List { ForEach(shown) { row($0) } }
            }
            if !model.message.isEmpty {
                Text(model.message).font(.callout).foregroundColor(.red).fixedSize(horizontal: false, vertical: true)
            }
            Text("Does not work with every app: rebar is an emulator, and the first launch can take up to a minute.")
                .font(.caption).foregroundColor(.secondary)
        }
        .padding(16)
        .sheet(item: $editing) { SettingsSheet(app: $0).environmentObject(model) }
        .sheet(isPresented: $wrapperOpen) { WrapperSheet(wrap: model.wrapper).environmentObject(model) }
        .onAppear { model.wrapper.refresh(extraApps: model.apps.filter { $0.ios == nil && $0.path.hasSuffix(".app") }.map { $0.path }) }
        .onAppear {   // (tests)
            let e = ProcessInfo.processInfo.environment
            if let p = e["RL_SETTINGS"] { DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { editing = model.apps.first { $0.path == p } } }
            if e["RL_WRAPPER"] != nil { DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { wrapperOpen = true } }
            if let p = e["RL_LAUNCH"] { DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { if let a = model.apps.first(where: { $0.path == p }) { model.launch(a) } } }
        }
    }

    private func row(_ app: AppEntry) -> some View {
        HStack(spacing: 10) {
            Image(nsImage: model.icons[app.path] ?? NSImage()).resizable().frame(width: 36, height: 36)
            VStack(alignment: .leading, spacing: 1) {
                Text(app.name).font(.headline)
                Text(app.ios.map { [app.version, "iOS app", $0.encrypted ? "encrypted (FairPlay): cannot run" : "arm64"].filter { !$0.isEmpty }.joined(separator: " · ") } ?? [app.version, app.engine ?? "", "arm64 only"].filter { !$0.isEmpty }.joined(separator: " · ")).font(.caption).foregroundColor(.secondary)
            }
            Spacer()
            if model.running[app.path] != nil {
                Text(model.status(app)).font(.caption).foregroundColor(model.status(app) == "Running" ? .green : .orange)
                Button("Stop") { model.stop(app) }.help("Close the app and everything left of it")
                Button("Restart") { model.restart(app) }.help("Close and start again: helps when the app is running but has no window")
            } else {
                Button("Launch") { model.launch(app) }.disabled(app.ios?.encrypted == true)
            }
            Button(action: { editing = app }) { Image(systemName: "gearshape") }.buttonStyle(BorderlessButtonStyle()).help("Settings of this app")
        }
        .padding(.vertical, 3)
    }

    private func choose() {
        let p = NSOpenPanel()
        p.canChooseFiles = true; p.canChooseDirectories = true; p.allowsMultipleSelection = false
        p.message = "Choose an app (.app), an iOS app (.ipa) or a folder to search for apps"
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
                Toggle("GPU acceleration", isOn: $s.gpu)
                hint("The window is drawn by the GPU (Metal): scrolling is much smoother. If the window is blank or shows garbage, turn it off.")
                Toggle("No Chromium sandbox", isOn: $s.noSandbox)
                hint("The sandbox does not run under rebar. Turn it off only if you know why.")
                Toggle("Do not ask for the Keychain password", isOn: $s.mockKeychain)
                hint("On: the key the app uses to encrypt its data (account sign-ins and the like) is not kept in the Keychain and is protected less well. Off: macOS will ask for your keychain password.")
            }
            if model.vsCodeFolder(app) != nil {
                Toggle("Fast scrolling (lighter editor), on by default", isOn: $s.fastEditor)
                hint("Turns off the minimap, sticky scroll headers, breadcrumbs, bracket colouring and match / current-line highlighting in the editor. Scrolling is smoother: about 60–68 frames/s instead of ~50. One block between the lines “// >>> rebar” and “// <<< rebar” is added to your settings.json; turn the switch off and it is removed (a copy of the file from before the change: settings.json.rebar-backup). Settings with the same names that you set yourself further down in the file take priority.")
            }
            Toggle("Fast mode (JIT)", isOn: $s.jit)
            hint("rebar translates code to machine code on the fly, several times faster. Turn it off only when hunting bugs.")
            Toggle("Show the Metal performance HUD", isOn: $s.hud)
            hint("Apple's overlay with frames per second, frame time, GPU time and memory, drawn over the program's Metal windows (games, 3D apps). Programs that do not use Metal show nothing.")
            Text("Command-line arguments").font(.subheadline)
            TextField("for example: --user-data-dir=\"/Users/me/my profile\"", text: $s.args)
            Text("Environment variables (one per line: NAME=value)").font(.subheadline)
            TextEditor(text: $s.env).font(.system(.body, design: .monospaced)).frame(height: 60)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.secondary.opacity(0.4)))
            hint("Changes take effect on the next launch.")
            HStack {
                Button("Show log") { NSWorkspace.shared.open(URL(fileURLWithPath: model.logPath(app))) }
                Button("Reset") { s = AppSettings() }
                Spacer()
                Button("Done") { model.setSettings(s, for: app); presentation.wrappedValue.dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20).frame(width: 500)
        .onAppear { s = model.settings(app) }
    }

    private func hint(_ t: String) -> some View {
        Text(t).font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
    }
}

/// The one checkbox of the main window: the background agent that wraps arm64-only apps as they appear (details: the gear next to it).
struct WrapperCheckbox: View {
    @ObservedObject var wrap: WrapperModel
    let apps: [AppEntry]
    var body: some View {
        Toggle("Start apps by themselves", isOn: Binding(get: { wrap.agentOn }, set: { wrap.setAgent($0, extraApps: apps.filter { $0.ios == nil && $0.path.hasSuffix(".app") }.map { $0.path }) }))
            .toggleStyle(CheckboxToggleStyle()).disabled(!wrap.available || wrap.busy)
            .help("A background agent wraps arm64-only apps as they appear: a double click, the Dock and Steam then start them through rebar without this window")
    }
}
