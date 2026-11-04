import Foundation
import AppKit
import WebKit
let app = NSApplication.shared; app.setActivationPolicy(.prohibited)
final class Nav: NSObject, WKNavigationDelegate { var done = false; var err: Error?
    func webView(_ w: WKWebView, didFinish n: WKNavigation!) { done = true }
    func webView(_ w: WKWebView, didFail n: WKNavigation!, withError e: Error) { err = e; done = true }
    func webView(_ w: WKWebView, didFailProvisionalNavigation n: WKNavigation!, withError e: Error) { err = e; done = true } }
func spin(_ cond: () -> Bool, _ secs: Double = 30) { let end = Date().addingTimeInterval(secs); while !cond() && Date() < end { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.02)) } }
let cfg = WKWebViewConfiguration(); let wv = WKWebView(frame: NSRect(x: 0, y: 0, width: 400, height: 300), configuration: cfg); let nav = Nav(); wv.navigationDelegate = nav
wv.loadHTMLString("<html><body style='background:#123456'><h1 id=t>Hello WebKit</h1><canvas id=c width=20 height=20></canvas><script>document.getElementById('t').dataset.x = String([1,2,3].map(x=>x*x)); const c=document.getElementById('c').getContext('2d'); c.fillStyle='#ff0000'; c.fillRect(0,0,20,20); window.px = Array.from(c.getImageData(1,1,1,1).data).join(',');</script></body></html>", baseURL: nil)
spin { nav.done }; print("loaded", nav.done, nav.err == nil)
var res: Any?; var finished = false
func js(_ s: String) -> Any? { finished = false; res = nil; wv.evaluateJavaScript(s) { r, e in res = r ?? e?.localizedDescription; finished = true }; spin { finished }; return res }
print("title text", js("document.getElementById('t').textContent") ?? "nil")
print("computed", js("[1,2,3].map(x=>x*x).join('-')") ?? "nil", js("document.getElementById('t').dataset.x") ?? "nil", js("window.px") ?? "nil")
print("features", js("typeof WebAssembly + ',' + typeof fetch + ',' + typeof Promise + ',' + (new Intl.NumberFormat('en-US').format(1234.5))") ?? "nil")
print("wasm", js("(()=>{ try { const m = new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11])); return new WebAssembly.Instance(m).exports.add(20, 22); } catch (e) { return 'err ' + e; } })()") ?? "nil")
print("async", js("(async()=>{ await new Promise(r => setTimeout(r, 10)); return 'awaited'; })()") ?? "nil")
var snap: NSImage?; var snapped = false; wv.takeSnapshot(with: nil) { i, _ in snap = i; snapped = true }; spin { snapped }; print("snapshot", Int(snap?.size.width ?? 0), Int(snap?.size.height ?? 0))
let rep = wv.bitmapImageRepForCachingDisplay(in: wv.bounds)!; wv.cacheDisplay(in: wv.bounds, to: rep); let c0 = rep.colorAt(x: 390, y: 290)!; print("pixel bg", Int(c0.redComponent * 255), Int(c0.greenComponent * 255), Int(c0.blueComponent * 255))
// local http server for a real network navigation
import Network
let l = try! NWListener(using: .tcp, on: .any); let ready = DispatchSemaphore(value: 0)
l.newConnectionHandler = { c in c.start(queue: .global()); c.receive(minimumIncompleteLength: 1, maximumLength: 4000) { _, _, _, _ in let b = "<html><title>From server</title><body>net ok</body></html>"; c.send(content: "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: \(b.utf8.count)\r\nConnection: close\r\n\r\n\(b)".data(using: .utf8), isComplete: true, completion: .contentProcessed { _ in }) } }
l.stateUpdateHandler = { if case .ready = $0 { ready.signal() } }; l.start(queue: .global()); ready.wait()
nav.done = false; wv.load(URLRequest(url: URL(string: "http://127.0.0.1:\(l.port!.rawValue)/")!)); spin { nav.done }; print("http nav", nav.done, nav.err == nil)
var ucs = false; let uc = WKUserContentController(); final class H: NSObject, WKScriptMessageHandler { var msg: Any?; func userContentController(_ u: WKUserContentController, didReceive m: WKScriptMessage) { msg = m.body } }
let h = H(); uc.add(h, name: "native"); let cfg2 = WKWebViewConfiguration(); cfg2.userContentController = uc; let wv2 = WKWebView(frame: .zero, configuration: cfg2); let nav2 = Nav(); wv2.navigationDelegate = nav2
wv2.loadHTMLString("<script>window.webkit.messageHandlers.native.postMessage({a: 1, b: 'two'})</script>", baseURL: nil); spin { h.msg != nil }; print("message handler", (h.msg as? [String: Any] ?? [:]).sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" })
