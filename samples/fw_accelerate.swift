import Accelerate
import simd
let a: [Float] = (1...8).map { Float($0) }, b: [Float] = (1...8).map { Float($0) * 0.5 }
print("vadd", vDSP.add(a, b))
print("dot", vDSP.dot(a, b), "sum", vDSP.sum(a), "max", vDSP.maximum(a))
print("sq", vForce.sqrt(a).map { String(format: "%.3f", $0) })
// FFT
let n = 16; var re = (0..<n).map { Float(sin(2 * Double.pi * 2 * Double($0) / Double(n))) }; var im = [Float](repeating: 0, count: n)
re.withUnsafeMutableBufferPointer { rp in im.withUnsafeMutableBufferPointer { ip in
    var sc = DSPSplitComplex(realp: rp.baseAddress!, imagp: ip.baseAddress!)
    let setup = vDSP_create_fftsetup(4, FFTRadix(kFFTRadix2))!
    vDSP_fft_zip(setup, &sc, 1, 4, FFTDirection(kFFTDirection_Forward)); vDSP_destroy_fftsetup(setup)
} }
print("fft", zip(re, im).map { String(format: "%.2f", sqrt($0 * $0 + $1 * $1)) })
// BLAS / LAPACK
var A: [Float] = [1, 2, 3, 4], B: [Float] = [5, 6, 7, 8], C = [Float](repeating: 0, count: 4)
cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, 2, 2, 2, 1, A, 2, B, 2, 0, &C, 2)
print("gemm", C)
var m: [Double] = [2, 1, 1, 3], rhs: [Double] = [3, 5]; var nn: __CLPK_integer = 2, nrhs: __CLPK_integer = 1, lda: __CLPK_integer = 2, ldb: __CLPK_integer = 2, info: __CLPK_integer = 0; var ipiv = [__CLPK_integer](repeating: 0, count: 2)
dgesv_(&nn, &nrhs, &m, &lda, &ipiv, &rhs, &ldb, &info)
print("gesv", rhs.map { String(format: "%.4f", $0) }, info)
// vImage
var src = [UInt8](repeating: 100, count: 16), dst = [UInt8](repeating: 0, count: 16)
src.withUnsafeMutableBytes { s in dst.withUnsafeMutableBytes { d in
    var sb = vImage_Buffer(data: s.baseAddress, height: 4, width: 4, rowBytes: 4), db = vImage_Buffer(data: d.baseAddress, height: 4, width: 4, rowBytes: 4)
    vImageContrastStretch_Planar8(&sb, &db, vImage_Flags(kvImageNoFlags))
} }
print("vimage", dst.prefix(4))
let q = simd_quatf(angle: .pi / 2, axis: simd_normalize(simd_float3(0, 0, 1)))
print("simd", q.act(simd_float3(1, 0, 0)).x.rounded(), simd_float4x4(diagonal: simd_float4(2, 2, 2, 1)).determinant)
// BNNS
let x = BNNSNDArrayDescriptor.allocate(initializingFrom: [1, -2, 3, -4] as [Float], shape: .vector(4))
print("bnns", x.shape.size)
x.deallocate()
