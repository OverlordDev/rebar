import Foundation
import SQLite3
import zlib
import Compression
import libxml2
import CoreFoundation
var db: OpaquePointer?; sqlite3_open(":memory:", &db)
sqlite3_exec(db, "create table t(a int, b text); insert into t values (1,'x'),(2,'y'),(3,'z'); create virtual table f using fts5(body); insert into f values('the quick brown fox'),('lazy dog');", nil, nil, nil)
var stmt: OpaquePointer?; sqlite3_prepare_v2(db, "select sum(a), group_concat(b) from t", -1, &stmt, nil); sqlite3_step(stmt); print("sqlite", sqlite3_column_int(stmt, 0), String(cString: sqlite3_column_text(stmt, 1))); sqlite3_finalize(stmt)
sqlite3_prepare_v2(db, "select count(*) from f where f match 'fox'", -1, &stmt, nil); sqlite3_step(stmt); print("fts5", sqlite3_column_int(stmt, 0)); sqlite3_finalize(stmt)
sqlite3_prepare_v2(db, "select json_extract('{\"a\":[1,2,{\"b\":7}]}', '$.a[2].b')", -1, &stmt, nil); sqlite3_step(stmt); print("json", sqlite3_column_int(stmt, 0)); sqlite3_finalize(stmt)
print("sqlite version", String(cString: sqlite3_libversion()).split(separator: ".").first!); sqlite3_close(db)
let src = Data((0..<10000).map { UInt8($0 % 97) }); var dl = compressBound(10000); var dst = [UInt8](repeating: 0, count: Int(dl))
print("zlib", src.withUnsafeBytes { compress(&dst, &dl, $0.bindMemory(to: UInt8.self).baseAddress!, 10000) }, dl < 3000, crc32(0, [1, 2, 3], 3), adler32(1, [1, 2, 3], 3))
var strm = z_stream(); deflateInit2_(&strm, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)); var gz = [UInt8](repeating: 0, count: 20000)
src.withUnsafeBytes { s in strm.next_in = UnsafeMutablePointer(mutating: s.bindMemory(to: UInt8.self).baseAddress!); strm.avail_in = 10000; gz.withUnsafeMutableBytes { strm.next_out = $0.bindMemory(to: UInt8.self).baseAddress!; strm.avail_out = 20000; deflate(&strm, Z_FINISH) } }
print("gzip magic", gz[0], gz[1], strm.total_in); deflateEnd(&strm)
let xml = "<r><a k=\"1\">x</a><a k=\"2\">y</a></r>"; let doc = xmlReadMemory(xml, Int32(xml.utf8.count), nil, nil, 0)!; let root = xmlDocGetRootElement(doc)!
var cnt = 0; var c = root.pointee.children; while let n = c { if n.pointee.type == XML_ELEMENT_NODE { cnt += 1 }; c = n.pointee.next }; print("libxml2", cnt); xmlFreeDoc(doc)
let x = XMLDocument(); _ = x
let xd = try! XMLDocument(xmlString: "<r><a>1</a><a>2</a></r>", options: []); print("xpath", try! xd.nodes(forXPath: "//a").count, xd.rootElement()!.name!)
let parser = XMLParser(data: Data("<r><i id='1'/><i id='2'/></r>".utf8)); final class D: NSObject, XMLParserDelegate { var n = 0; func parser(_ p: XMLParser, didStartElement e: String, namespaceURI: String?, qualifiedName: String?, attributes a: [String: String]) { if e == "i" { n += 1 } } }
let dd = D(); parser.delegate = dd; parser.parse(); print("xmlparser", dd.n)
let re = try! NSRegularExpression(pattern: "(\\d+)-(\\w+)", options: []); let m = re.firstMatch(in: "id 42-abc!", range: NSRange(location: 0, length: 10))!; print("nsregex", m.numberOfRanges, (("id 42-abc!" as NSString).substring(with: m.range(at: 2))))
print("icu", Locale(identifier: "de_DE").localizedString(forRegionCode: "FR") ?? "?", NumberFormatter.localizedString(from: 1234567.891, number: .decimal), DateFormatter.dateFormat(fromTemplate: "yMMMd", options: 0, locale: Locale(identifier: "en_GB")) ?? "?")
let cal = Calendar(identifier: .japanese); print("calendar", cal.component(.era, from: Date(timeIntervalSince1970: 1_700_000_000)), Calendar(identifier: .hebrew).component(.year, from: Date(timeIntervalSince1970: 1_700_000_000)))
let cf = CFStringTokenizerCreate(nil, "日本語のテキスト" as CFString, CFRangeMake(0, 8), 0, CFLocaleCopyCurrent()); var toks = 0; while CFStringTokenizerAdvanceToNextToken(cf) != [] { toks += 1 }; print("cf tokenizer", toks > 1)
print("transliterate", "Привет" .applyingTransform(.toLatin, reverse: false) ?? "?", "ÅÖÜ".folding(options: .diacriticInsensitive, locale: nil))
