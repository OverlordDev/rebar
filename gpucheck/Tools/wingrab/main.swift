import Cocoa
// wingrab <pid[,pid...]> [out.png]: windows of the processes and what they show
let pids = Set(CommandLine.arguments[1].split(separator: ",").compactMap { pid_t($0) })
let ws = WindowGrab.windows(pids: pids, minSize: 50)
if ws.isEmpty { print("no windows"); exit(1) }
for (i, w) in ws.enumerated() {
    guard let img = WindowGrab.grab(w.id) else { print("window \(w.id) \(w.owner) '\(w.title)' layer \(w.layer) \(w.bounds): grab failed"); continue }
    print("window \(w.id) \(w.owner) '\(w.title)' layer \(w.layer) \(Int(w.bounds.width))x\(Int(w.bounds.height)): \(WindowGrab.stats(img).summary) -> \(WindowGrab.stats(img).looksLikeContent ? "CONTENT" : "BLANK")")
    if CommandLine.arguments.count > 2 { WindowGrab.savePNG(img, to: CommandLine.arguments[2].replacingOccurrences(of: ".png", with: "_\(i).png")) }
}
