import Foundation
import SQLite3
import Compression
import NaturalLanguage
import Network
import Security
import zlib
var db: OpaquePointer?; sqlite3_open(":memory:", &db)
sqlite3_exec(db, "create table t(id integer primary key, name text, v real); insert into t(name,v) values('a',1.5),('b',2.5),('c',4);", nil, nil, nil)
var st: OpaquePointer?; sqlite3_prepare_v2(db, "select name, sum(v) over (order by id) from t where v > ?", -1, &st, nil); sqlite3_bind_double(st, 1, 1.0)
var rows = [String](); while sqlite3_step(st) == SQLITE_ROW { rows.append("\(String(cString: sqlite3_column_text(st, 0)))=\(sqlite3_column_double(st, 1))") }; print("sqlite", rows, String(cString: sqlite3_libversion()).prefix(1))
sqlite3_finalize(st); sqlite3_close(db)
let raw = Data((0..<4000).map { UInt8($0 % 17) })
for alg in [COMPRESSION_ZLIB, COMPRESSION_LZFSE, COMPRESSION_LZ4] {
    var dst = [UInt8](repeating: 0, count: 8000); let n = raw.withUnsafeBytes { compression_encode_buffer(&dst, 8000, $0.bindMemory(to: UInt8.self).baseAddress!, raw.count, nil, alg) }
    var back = [UInt8](repeating: 0, count: 5000); let m = compression_decode_buffer(&back, 5000, dst, n, nil, alg); print("compression", alg.rawValue, n < 4000, m, Array(back[0..<m]) == Array(raw))
}
var zl = [UInt8](repeating: 0, count: 8000); var zlen = uLongf(8000); _ = raw.withUnsafeBytes { compress(&zl, &zlen, $0.bindMemory(to: UInt8.self).baseAddress!, uLong(raw.count)) }; print("zlib", zlen, crc32(0, raw.withUnsafeBytes { $0.bindMemory(to: UInt8.self).baseAddress! }, uInt(raw.count)))
let tok = NLTokenizer(unit: .word); let text = "Hello, brave new world. Привет мир!"; tok.string = text; var words = [String](); tok.enumerateTokens(in: text.startIndex..<text.endIndex) { r, _ in words.append(String(text[r])); return true }; print("tokens", words)
let lang = NLLanguageRecognizer(); lang.processString("Это русский текст"); print("language", lang.dominantLanguage!.rawValue)
let tagger = NLTagger(tagSchemes: [.lexicalClass]); tagger.string = "The quick fox jumps"; var tags = [String](); tagger.enumerateTags(in: tagger.string!.startIndex..<tagger.string!.endIndex, unit: .word, scheme: .lexicalClass) { t, _ in tags.append(t?.rawValue ?? "-"); return true }; print("tags", tags)
var err: Unmanaged<CFError>?; let keyAttrs: [String: Any] = [kSecAttrKeyType as String: kSecAttrKeyTypeRSA, kSecAttrKeySizeInBits as String: 2048]
if let k = SecKeyCreateRandomKey(keyAttrs as CFDictionary, &err), let pub = SecKeyCopyPublicKey(k) { let sig = SecKeyCreateSignature(k, .rsaSignatureMessagePKCS1v15SHA256, Data("m".utf8) as CFData, &err)!; print("seckey", SecKeyVerifySignature(pub, .rsaSignatureMessagePKCS1v15SHA256, Data("m".utf8) as CFData, sig, &err), (sig as Data).count) } else { print("seckey unavailable") }
var rnd = [UInt8](repeating: 0, count: 16); print("random", SecRandomCopyBytes(kSecRandomDefault, 16, &rnd) == errSecSuccess)
let sem = DispatchSemaphore(value: 0); var received = ""
let listener = try! NWListener(using: .tcp, on: .any); listener.newConnectionHandler = { c in c.start(queue: .global()); c.receive(minimumIncompleteLength: 1, maximumLength: 100) { d, _, _, _ in received = String(decoding: d ?? Data(), as: UTF8.self); c.send(content: Data("pong".utf8), completion: .contentProcessed { _ in }) } }
listener.stateUpdateHandler = { st in if case .ready = st { let c = NWConnection(host: "127.0.0.1", port: listener.port!, using: .tcp); c.start(queue: .global()); c.send(content: Data("ping".utf8), completion: .contentProcessed { _ in }); c.receive(minimumIncompleteLength: 1, maximumLength: 10) { d, _, _, _ in print("network", received, String(decoding: d ?? Data(), as: UTF8.self)); sem.signal() } } }
listener.start(queue: .global()); print("wait", sem.wait(timeout: .now() + 10) == .success)
