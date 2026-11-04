import Foundation

/// "Быстрый скролл" for VS Code: a block of editor settings that make scrolling cheaper (no minimap, no sticky scroll, ...).  The block sits between two marker
/// comments in the user's settings.json, so switching the option off removes exactly it and nothing else of the file is touched (the file may hold comments).
enum FastEditor {
    static let begin = "// >>> rebar: быстрый скролл (этот блок убирается выключением переключателя в лаунчере)"
    static let end = "// <<< rebar"
    static let settings: [(String, String)] = [
        ("editor.minimap.enabled", "false"),
        ("editor.stickyScroll.enabled", "false"),
        ("breadcrumbs.enabled", "false"),
        ("editor.bracketPairColorization.enabled", "false"),
        ("editor.guides.bracketPairs", "false"),
        ("editor.occurrencesHighlight", "\"off\""),
        ("editor.selectionHighlight", "false"),
        ("editor.renderLineHighlight", "\"none\""),
    ]

    /// The text of settings.json without the block (`on` false) or with the block put right after the opening brace (`on` true).
    static func apply(_ text: String, on: Bool) -> String {
        var lines = text.components(separatedBy: "\n")
        if let b = lines.firstIndex(where: { $0.trimmingCharacters(in: .whitespaces).hasPrefix("// >>> rebar") }),
           let e = lines[b...].firstIndex(where: { $0.trimmingCharacters(in: .whitespaces).hasPrefix("// <<< rebar") }) {
            lines.removeSubrange(b...e)
        }
        guard on else { return lines.joined(separator: "\n") }
        let block = (["    " + begin] + settings.map { "    \"\($0.0)\": \($0.1)," } + ["    " + end]).joined(separator: "\n")
        guard let k = lines.firstIndex(where: { $0.trimmingCharacters(in: .whitespaces).hasPrefix("{") }) else { return "{\n" + block + "\n}\n" }
        let line = lines[k], brace = line.firstIndex(of: "{")!
        let rest = String(line[line.index(after: brace)...])
        lines[k] = String(line[...brace]) + "\n" + block + (rest.trimmingCharacters(in: .whitespaces).isEmpty ? "" : "\n" + rest)
        return lines.joined(separator: "\n")
    }
}
