import Foundation
import CryptoKit
import os
import Accelerate
func hex<S: Sequence>(_ s: S) -> String where S.Element == UInt8 { s.map { String(format: "%02x", $0) }.joined() }
let msg = Data("hello rebar".utf8)
print(hex(SHA256.hash(data: msg)).prefix(16), hex(SHA512.hash(data: msg)).count, hex(Insecure.MD5.hash(data: msg)))
let key = SymmetricKey(data: Data(repeating: 7, count: 32))
print(hex(HMAC<SHA256>.authenticationCode(for: msg, using: key)).prefix(16))
let box = try! AES.GCM.seal(msg, using: key, nonce: AES.GCM.Nonce(data: Data(repeating: 1, count: 12))); print(hex(box.ciphertext), try! AES.GCM.open(box, using: key) == msg)
let ch = try! ChaChaPoly.seal(msg, using: key, nonce: ChaChaPoly.Nonce(data: Data(repeating: 2, count: 12))); print(ch.tag.count, try! ChaChaPoly.open(ch, using: key) == msg)
let priv = try! P256.Signing.PrivateKey(rawRepresentation: Data(repeating: 3, count: 32)); let sig = try! priv.signature(for: msg); print(priv.publicKey.isValidSignature(sig, for: msg), priv.publicKey.rawRepresentation.count)
let c25 = Curve25519.KeyAgreement.PrivateKey(); let c25b = Curve25519.KeyAgreement.PrivateKey(); print(try! c25.sharedSecretFromKeyAgreement(with: c25b.publicKey) == c25b.sharedSecretFromKeyAgreement(with: c25.publicKey))
print(HKDF<SHA256>.deriveKey(inputKeyMaterial: key, salt: Data(), info: Data("i".utf8), outputByteCount: 16).withUnsafeBytes { Data($0).count })
let log = Logger(subsystem: "rebar.test", category: "t"); log.info("logger works \(42)"); print("logger ok")
var a = [Float](repeating: 2, count: 8), b2 = [Float](repeating: 3, count: 8), r = [Float](repeating: 0, count: 8)
vDSP.multiply(a, b2, result: &r); print(r, vDSP.sum(r), vDSP.maximum(r)); a = vDSP.add(a, b2); print(a)
var x = [Double](repeating: 0, count: 16); for i in 0..<16 { x[i] = sin(Double(i) * 0.4) }; print(vDSP.sum(x.map { $0 * $0 }).rounded(toPlaces: 4))
extension Double { func rounded(toPlaces p: Int) -> Double { let m = pow(10, Double(p)); return (self * m).rounded() / m } }
var A: [Double] = [2, 1, 1, 3], B: [Double] = [1, 2]; var n: __CLPK_integer = 2, nrhs: __CLPK_integer = 1, lda = n, ldb = n, info: __CLPK_integer = 0; var ipiv = [__CLPK_integer](repeating: 0, count: 2)
dgesv_(&n, &nrhs, &A, &lda, &ipiv, &B, &ldb, &info); print("lapack", B.map { $0.rounded(toPlaces: 4) }, info)
