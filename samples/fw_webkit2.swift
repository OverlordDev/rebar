import Foundation
import AppKit
import WebKit
import Network
let app = NSApplication.shared; app.setActivationPolicy(.prohibited)
final class Nav: NSObject, WKNavigationDelegate { var done = false; func webView(_ w: WKWebView, didFinish n: WKNavigation!) { done = true }; func webView(_ w: WKWebView, didFail n: WKNavigation!, withError e: Error) { done = true }; func webView(_ w: WKWebView, didFailProvisionalNavigation n: WKNavigation!, withError e: Error) { print("nav error", (e as NSError).code); done = true } }
func spin(_ cond: () -> Bool, _ secs: Double = 40) { let end = Date().addingTimeInterval(secs); while !cond() && Date() < end { RunLoop.main.run(mode: .default, before: Date().addingTimeInterval(0.02)) } }
let l = try! NWListener(using: .tcp, on: .any); let ready = DispatchSemaphore(value: 0)
l.newConnectionHandler = { c in c.start(queue: .global()); c.receive(minimumIncompleteLength: 1, maximumLength: 8000) { d, _, _, _ in
    let req = String(decoding: d ?? Data(), as: UTF8.self); let path = req.split(separator: " ").dropFirst().first.map(String.init) ?? "/"
    var body = "<html><body style='margin:0;background:#ff8000'><script>fetch('/data.json').then(r => r.json()).then(j => { document.title = 'fetched ' + j.v; });</script></body></html>"; var ct = "text/html"
    if path.hasPrefix("/data.json") { body = "{\"v\": 42}"; ct = "application/json" }
    c.send(content: "HTTP/1.1 200 OK\r\nContent-Type: \(ct)\r\nContent-Length: \(body.utf8.count)\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n\(body)".data(using: .utf8), isComplete: true, completion: .contentProcessed { _ in }) } }
l.stateUpdateHandler = { if case .ready = $0 { ready.signal() } }; l.start(queue: .global()); ready.wait()
let port = l.port!.rawValue
let cfg = WKWebViewConfiguration(); cfg.websiteDataStore = .nonPersistent(); let wv = WKWebView(frame: NSRect(x: 0, y: 0, width: 200, height: 120), configuration: cfg); let nav = Nav(); wv.navigationDelegate = nav
let win = NSWindow(contentRect: wv.frame, styleMask: [.titled], backing: .buffered, defer: false); win.contentView = wv; win.orderBack(nil)
wv.load(URLRequest(url: URL(string: "http://127.0.0.1:\(port)/")!)); spin { nav.done }; spin { wv.title == "fetched 42" }; print("fetch title", wv.title ?? "nil")
var res: Any?; var fin = false
func js(_ s: String) -> String { fin = false; res = nil; wv.callAsyncJavaScript(s, arguments: [:], in: nil, in: .page) { r in switch r { case .success(let v): res = v; case .failure(let e): res = "err \((e as NSError).code)" }; fin = true }; spin { fin }; return "\(res ?? "nil")" }
print("localStorage", js("localStorage.setItem('k','v1'); return localStorage.getItem('k')"))
print("indexeddb", js("""
return await new Promise((resolve) => { const r = indexedDB.open('db', 1); r.onupgradeneeded = () => r.result.createObjectStore('s'); r.onsuccess = () => { const tx = r.result.transaction('s', 'readwrite'); tx.objectStore('s').put('stored', 'key'); tx.oncomplete = () => { const g = r.result.transaction('s').objectStore('s').get('key'); g.onsuccess = () => resolve(g.result); }; }; r.onerror = () => resolve('error'); });
"""))
print("webgl", js("const c = document.createElement('canvas'); const gl = c.getContext('webgl') || c.getContext('experimental-webgl'); if (!gl) return 'none'; gl.clearColor(0, 1, 0, 1); gl.clear(gl.COLOR_BUFFER_BIT); const p = new Uint8Array(4); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, p); return Array.from(p).join(',')"))
print("crypto", js("const h = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc')); return Array.from(new Uint8Array(h)).slice(0, 4).map(x => x.toString(16)).join('')"))
print("fetch+worker", js("const w = new Worker(URL.createObjectURL(new Blob(['onmessage = e => postMessage(e.data * 2)']))); return await new Promise(r => { w.onmessage = e => r(e.data); w.postMessage(21); })"))
print("css animation", js("const d = document.createElement('div'); d.style.cssText = 'width:10px;height:10px;transition:width 0.05s'; document.body.appendChild(d); d.getBoundingClientRect(); d.style.width = '50px'; await new Promise(r => setTimeout(r, 200)); return getComputedStyle(d).width"))
print("regex+intl", js("return [new Intl.DateTimeFormat('en-US', {timeZone: 'UTC'}).format(new Date(0)), 'a1b22'.replace(/\\d+/g, m => '<' + m + '>'), (0.1 + 0.2).toFixed(2), [3,1,2].toSorted().join('')].join('|')"))
print("webaudio/media", js("return typeof AudioContext + ',' + typeof MediaRecorder + ',' + typeof OffscreenCanvas + ',' + (document.createElement('video').canPlayType('video/mp4') !== '')"))
let rep = wv.bitmapImageRepForCachingDisplay(in: wv.bounds)!; wv.cacheDisplay(in: wv.bounds, to: rep); let p = rep.colorAt(x: 100, y: 100)!; print("rendered bg", p.alphaComponent > 0)
var pdf: Data?; var pd = false; wv.createPDF { r in pdf = try? r.get(); pd = true }; spin { pd }; print("pdf", (pdf?.count ?? 0) > 500)
