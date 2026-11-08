import Foundation
import AppKit
import SwiftUI

/// An arm64-only app as tools/rebar-wrap (`rebar-wrap list --json`) sees it.
struct WrapApp: Identifiable, Hashable, Decodable {
    let path: String
    let name: String
    var wrapped: Bool
    let quarantined: Bool
    let writable: Bool      // the folder of the app's executable can be changed by this user (an App Store app's is not)
    var env: String         // what the wrapped app starts with (one NAME=value per line)
    var id: String { path }
}

/// The wrapper: arm64-only apps that start by themselves (double click, Dock, `open`, Steam) because their main executable is wrapped, and the background agent that wraps new ones.
/// Everything is done by tools/rebar-wrap, which travels inside the app and runs from a copy in Application Support (see Launcher.prepareRebar).
final class WrapperModel: NSObject, ObservableObject {
    @Published var agentOn = false
    @Published var apps: [WrapApp] = []
    @Published var folders: [String] = []
    @Published var busy = false
    @Published var message = ""
    @Published var log = ""

    private let script: String, shim: String, rebar: String
    private var extra: [String] = []
    private let label: String?                       // (tests: another agent label and folders, so that nothing real is touched)
    private let foldersFile = NSHomeDirectory() + "/Library/Application Support/rebar/autowrap-folders"

    init(script: String, shim: String, rebar: String) {
        self.script = script; self.shim = shim; self.rebar = rebar
        label = ProcessInfo.processInfo.environment["RL_WRAP_LABEL"]
        super.init()
        loadFolders()
    }

    var available: Bool { FileManager.default.isExecutableFile(atPath: rebar) && FileManager.default.fileExists(atPath: script) && FileManager.default.fileExists(atPath: shim) }

    // MARK: running tools/rebar-wrap

    @discardableResult
    private func tool(_ args: [String]) -> (code: Int32, out: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        p.arguments = [script] + args
        var env = ProcessInfo.processInfo.environment
        env["REBAR_BIN"] = rebar; env["REBAR_WRAP_SHIM"] = shim
        if let l = label { env["REBAR_WRAP_LABEL"] = l }
        p.environment = env
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        do { try p.run() } catch { return (127, "python3 could not be started: \(error.localizedDescription)") }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(data: data, encoding: .utf8) ?? "")
    }

    private func background(_ work: @escaping () -> Void, then: (() -> Void)? = nil) {
        busy = true
        DispatchQueue.global().async {
            work()
            DispatchQueue.main.async { self.busy = false; then?() }
        }
    }

    // MARK: state

    /// Reads the agent state and the apps (a few hundred ms: off the main thread).
    func refresh(extraApps: [String]) {
        extra = extraApps
        let dirs = folders.isEmpty ? [] : folders
        background({
            let on = self.tool(["agent", "status"]).code == 0
            let r = self.tool(["list", "--json"] + dirs + extraApps)
            var list: [WrapApp] = []
            if r.code == 0, let d = r.out.data(using: .utf8), let a = try? JSONDecoder().decode([WrapApp].self, from: d) { list = a }
            var seen = Set<String>(), uniq: [WrapApp] = []
            for a in list where !seen.contains(a.path) { seen.insert(a.path); uniq.append(a) }
            DispatchQueue.main.async { self.agentOn = on; self.apps = uniq.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending } }
        })
    }

    private func loadFolders() {
        let text = (try? String(contentsOfFile: foldersFile, encoding: .utf8)) ?? ""
        folders = text.split(separator: "\n").map { String($0).trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty && !$0.hasPrefix("#") }
    }

    private func saveFolders() {
        try? FileManager.default.createDirectory(atPath: (foldersFile as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        try? (folders.joined(separator: "\n") + "\n").write(toFile: foldersFile, atomically: true, encoding: .utf8)
    }

    // MARK: actions

    /// The one checkbox: the background agent that wraps arm64-only apps in /Applications, ~/Applications and the folders below as they appear.
    func setAgent(_ on: Bool, extraApps: [String]) {
        message = ""
        background({
            let r = on ? self.tool(["agent", "install"]) : self.tool(["agent", "uninstall"])
            if on { _ = self.tool(["auto"]) }   // (what is already there is wrapped now, not at the next change)
            DispatchQueue.main.async { if r.code != 0 { self.message = r.out.trimmingCharacters(in: .whitespacesAndNewlines) } }
        }, then: { self.refresh(extraApps: extraApps) })
    }

    func setWrapped(_ app: WrapApp, _ on: Bool, extraApps: [String]) {
        message = ""
        background({
            let r = self.tool([on ? "enable" : "disable", app.path])
            DispatchQueue.main.async { self.message = r.out.contains(": wrapped") || r.out.contains("restored") || r.out.contains("put back") || r.out.contains("already") ? "" : r.out.trimmingCharacters(in: .whitespacesAndNewlines) }
        }, then: { self.refresh(extraApps: extraApps) })
    }

    func wrapAll(extraApps: [String]) {
        message = ""
        background({
            var out = self.tool(["auto"]).out
            for a in self.extra where a.hasSuffix(".app") { out += self.tool(["enable", a]).out }
            DispatchQueue.main.async { self.message = "" }
        }, then: { self.refresh(extraApps: extraApps) })
    }

    func saveEnv(_ app: WrapApp, _ text: String) {
        let pairs = text.split(separator: "\n").map { String($0).trimmingCharacters(in: .whitespaces) }.filter { $0.contains("=") && !$0.hasPrefix("#") }
        _ = tool(pairs.isEmpty ? ["env", app.path, "--clear"] : ["env", app.path] + pairs)
        if let i = apps.firstIndex(of: app) { apps[i].env = pairs.joined(separator: "\n") }
    }

    func addFolder(_ path: String, extraApps: [String]) {
        guard !path.isEmpty, !folders.contains(path) else { return }
        folders.append(path); saveFolders(); folderChanged(extraApps)
    }

    func removeFolder(_ path: String, extraApps: [String]) {
        folders.removeAll { $0 == path }; saveFolders(); folderChanged(extraApps)
    }

    /// The agent watches the folders it was installed with: a changed list reinstalls it.
    private func folderChanged(_ extraApps: [String]) {
        if agentOn { setAgent(true, extraApps: extraApps) } else { refresh(extraApps: extraApps) }
    }

    /// An App Store app is owned by root: its executable cannot be replaced. One administrator password hands the folder of the executable to this user (nothing else changes).
    func makeWritable(_ app: WrapApp, extraApps: [String]) {
        let dir = ((app.path as NSString).appendingPathComponent("Contents/MacOS"))
        let esc = { (s: String) in s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") }
        let src = "do shell script \"/usr/sbin/chown -R \(NSUserName()) '\(esc(dir))'\" with administrator privileges"
        var err: NSDictionary?
        NSAppleScript(source: src)?.executeAndReturnError(&err)
        message = err == nil ? "" : "Could not change the folder: \((err?[NSAppleScript.errorMessage] as? String) ?? "cancelled")"
        refresh(extraApps: extraApps)
    }

    func loadLog() {
        let f = NSHomeDirectory() + "/Library/Application Support/rebar/autowrap.log"
        log = (try? String(contentsOfFile: f, encoding: .utf8)).map { String($0.suffix(6000)) } ?? "(no log yet)"
    }
}

/// The window: the checkbox, the folders, the apps with their switches and environment.
struct WrapperSheet: View {
    @EnvironmentObject var model: Launcher
    @ObservedObject var wrap: WrapperModel
    @Environment(\.presentationMode) private var presentation
    @State private var editing: WrapApp?
    @State private var showLog = false

    private var extraApps: [String] { model.apps.filter { $0.ios == nil && $0.path.hasSuffix(".app") }.map { $0.path } }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Start apps by themselves").font(.title3).bold()
                Spacer()
                if wrap.busy { ProgressView().scaleEffect(0.6) }
            }
            Toggle("Wrap new arm64-only apps in the background", isOn: Binding(get: { wrap.agentOn }, set: { wrap.setAgent($0, extraApps: extraApps) }))
                .toggleStyle(CheckboxToggleStyle())
            hint("A wrapped app starts by a double click, from the Dock, with “open” and from Steam: its main executable is a universal file, the original arm64 code next to a tiny Intel program that starts rebar. The agent (a LaunchAgent, runs only when a folder below changes) wraps apps as they appear and wraps again an app that updated itself. Nothing else about the app changes; “Wrap” off puts the original file back.")
            GroupBox(label: Text("Watched folders")) {
                VStack(alignment: .leading, spacing: 4) {
                    HStack { Text("/Applications  and  ~/Applications").font(.caption).foregroundColor(.secondary); Spacer() }
                    ForEach(wrap.folders, id: \.self) { f in
                        HStack { Text(f).font(.caption); Spacer(); Button(action: { wrap.removeFolder(f, extraApps: extraApps) }) { Image(systemName: "minus.circle") }.buttonStyle(BorderlessButtonStyle()) }
                    }
                    HStack { Button("Add folder…") { chooseFolder() }; Spacer() }
                }.padding(4)
            }
            HStack {
                Text("Apps").font(.headline)
                Spacer()
                Button("Wrap all now") { wrap.wrapAll(extraApps: extraApps) }.help("Wrap every arm64-only app in the watched folders and in your list")
                Button(action: { wrap.refresh(extraApps: extraApps) }) { Image(systemName: "arrow.clockwise") }.help("Refresh")
            }
            if wrap.apps.isEmpty {
                Text(wrap.busy ? "Looking…" : "No arm64-only apps in the watched folders.").foregroundColor(.secondary).frame(maxWidth: .infinity, minHeight: 80)
            } else {
                List { ForEach(wrap.apps) { row($0) } }.frame(minHeight: 160)
            }
            if !wrap.message.isEmpty { Text(wrap.message).font(.callout).foregroundColor(.red).fixedSize(horizontal: false, vertical: true) }
            if !wrap.available { Text("The wrapper files are missing from this copy of the app.").foregroundColor(.red).font(.callout) }
            HStack {
                Button("Agent log") { wrap.loadLog(); showLog = true }
                Spacer()
                Button("Done") { presentation.wrappedValue.dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(18).frame(width: 640, height: 640)
        .onAppear { wrap.refresh(extraApps: extraApps) }
        .sheet(item: $editing) { EnvSheet(wrap: wrap, app: $0) }
        .sheet(isPresented: $showLog) {
            VStack(alignment: .leading) {
                Text("Agent log").font(.headline)
                ScrollView { Text(wrap.log).font(.system(.caption, design: .monospaced)).frame(maxWidth: .infinity, alignment: .leading) }
                HStack { Spacer(); Button("Close") { showLog = false } }
            }.padding(16).frame(width: 560, height: 380)
        }
    }

    private func row(_ a: WrapApp) -> some View {
        HStack(spacing: 8) {
            Image(nsImage: NSWorkspace.shared.icon(forFile: a.path)).resizable().frame(width: 28, height: 28)
            VStack(alignment: .leading, spacing: 1) {
                Text(a.name).font(.headline)
                Text(a.wrapped ? "wrapped: starts by itself" : (a.writable ? "arm64 only: needs rebar" : "arm64 only; the app's folder belongs to the system")).font(.caption).foregroundColor(.secondary)
            }
            Spacer()
            if !a.writable && !a.wrapped {
                Button("Allow changes…") { wrap.makeWritable(a, extraApps: extraApps) }.help("Asks for the administrator password once and gives you the folder Contents/MacOS of this app")
            } else {
                Toggle("Wrap", isOn: Binding(get: { a.wrapped }, set: { wrap.setWrapped(a, $0, extraApps: extraApps) })).toggleStyle(CheckboxToggleStyle())
            }
            Button(action: { editing = a }) { Image(systemName: a.env.isEmpty ? "gearshape" : "gearshape.fill") }.buttonStyle(BorderlessButtonStyle()).help("Environment this app starts with")
        }
        .padding(.vertical, 2)
    }

    private func chooseFolder() {
        let p = NSOpenPanel()
        p.canChooseFiles = false; p.canChooseDirectories = true; p.allowsMultipleSelection = false
        p.message = "Choose a folder with apps (for example a Steam library)"
        if p.runModal() == .OK, let u = p.url { wrap.addFolder(u.path, extraApps: extraApps) }
    }

    private func hint(_ t: String) -> some View { Text(t).font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true) }
}

/// The environment a wrapped app starts with (a double click or Steam gives it none of rebar's switches or Steam's ids).
struct EnvSheet: View {
    @ObservedObject var wrap: WrapperModel
    let app: WrapApp
    @Environment(\.presentationMode) private var presentation
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(app.name).font(.title3).bold()
            Text("Environment variables when the app starts by itself (one per line: NAME=value)").font(.subheadline)
            TextEditor(text: $text).font(.system(.body, design: .monospaced)).frame(height: 150)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Color.secondary.opacity(0.4)))
            Text("Examples: REBAR_NO_METAL_RT=1 (a game that dies while it builds ray tracing shaders), SteamAppId=1091500 (a Steam game: Steam sets it itself when it starts the game), REBAR_JIT=0. A variable that the program that starts the app sets itself is kept.")
                .font(.caption).foregroundColor(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Cancel") { presentation.wrappedValue.dismiss() }
                Spacer()
                Button("Save") { wrap.saveEnv(app, text); presentation.wrappedValue.dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20).frame(width: 520)
        .onAppear { text = app.env }
    }
}
