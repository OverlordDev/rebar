// ARMv8 cryptographic extensions for the interpreter: AES, SHA-1, SHA-256 and the CRC32 instructions.
// Apple silicon has all of them, and libraries (BoringSSL, zlib, Chromium) assume so without asking.
// Included from fpsimd.h; written from the ARM Architecture Reference Manual pseudocode.

static uint8_t g_aes_sbox[256], g_aes_inv[256];
static uint32_t g_crc_tab[2][256];  // [0] CRC-32 (0x04C11DB7 reflected), [1] CRC-32C (Castagnoli)
static bool g_crypto_ready;

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); b >>= 1; }
    return r;
}
static void crypto_init(void) {
    if (g_crypto_ready) return;
    for (int x = 0; x < 256; x++) {  // sbox = affine(inverse in GF(2^8))
        uint8_t inv = 0;
        if (x) for (int y = 1; y < 256; y++) if (gf_mul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
        uint8_t s = inv ^ (uint8_t)((inv << 1) | (inv >> 7)) ^ (uint8_t)((inv << 2) | (inv >> 6)) ^
                    (uint8_t)((inv << 3) | (inv >> 5)) ^ (uint8_t)((inv << 4) | (inv >> 4)) ^ 0x63;
        g_aes_sbox[x] = s;
        g_aes_inv[s] = (uint8_t)x;
    }
    static const uint32_t polys[2] = {0xEDB88320u, 0x82F63B78u};
    for (int p = 0; p < 2; p++)
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ polys[p] : c >> 1;
            g_crc_tab[p][n] = c;
        }
    g_crypto_ready = true;
}

static inline uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// CRC32{B,H,W,X} / CRC32C{B,H,W,X}: dp2src opcodes 16..23
static uint64_t crc32_insn(uint32_t crc, uint64_t data, int bytes, bool castagnoli) {
    crypto_init();
    const uint32_t *tab = g_crc_tab[castagnoli ? 1 : 0];
    for (int k = 0; k < bytes; k++) crc = tab[(crc ^ (uint32_t)(data >> (8 * k))) & 0xff] ^ (crc >> 8);
    return crc;
}

// AES single round steps; V registers hold 16 bytes in column-major order.
static void aes_step(VReg *d, const VReg *n, int kind) {
    crypto_init();
    uint8_t s[16], t[16];
    memcpy(s, d, 16);
    if (kind < 2) for (int k = 0; k < 16; k++) s[k] ^= n->b[k];  // AESE / AESD add the round key first
    switch (kind) {
    case 0:  // AESE: ShiftRows, SubBytes
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[c * 4 + r] = g_aes_sbox[s[((c + r) & 3) * 4 + r]];
        break;
    case 1:  // AESD: InvShiftRows, InvSubBytes
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) t[c * 4 + r] = g_aes_inv[s[((c - r) & 3) * 4 + r]];
        break;
    case 2:  // AESMC: MixColumns
        for (int c = 0; c < 4; c++) {
            const uint8_t *col = &s[c * 4];
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = gf_mul(col[r], 2) ^ gf_mul(col[(r + 1) & 3], 3) ^ col[(r + 2) & 3] ^ col[(r + 3) & 3];
        }
        break;
    default:  // AESIMC: InvMixColumns
        for (int c = 0; c < 4; c++) {
            const uint8_t *col = &s[c * 4];
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = gf_mul(col[r], 14) ^ gf_mul(col[(r + 1) & 3], 11) ^ gf_mul(col[(r + 2) & 3], 13) ^ gf_mul(col[(r + 3) & 3], 9);
        }
        break;
    }
    memcpy(d, t, 16);
}

static inline uint32_t sha_choose(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static inline uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t sha_major(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (x & z) | (y & z); }

// SHA1C / SHA1P / SHA1M (kind 0,1,2): Qd, Sn, Vm.4S
static void sha1_hash(VReg *d, const VReg *n, const VReg *m, int kind) {
    uint32_t X[4] = {d->s[0], d->s[1], d->s[2], d->s[3]}, Y = n->s[0];
    for (int e = 0; e < 4; e++) {
        uint32_t t = kind == 0 ? sha_choose(X[1], X[2], X[3]) : kind == 1 ? sha_parity(X[1], X[2], X[3]) : sha_major(X[1], X[2], X[3]);
        Y = Y + rol32(X[0], 5) + t + m->s[e];
        X[1] = rol32(X[1], 30);
        uint32_t top = X[3];                       // <Y, X> = ROL(Y : X, 32)
        X[3] = X[2]; X[2] = X[1]; X[1] = X[0]; X[0] = Y;
        Y = top;
    }
    for (int k = 0; k < 4; k++) d->s[k] = X[k];
}

static void sha1_su0(VReg *d, const VReg *n, const VReg *m) {  // SHA1SU0
    uint64_t lo = d->d[1], hi = n->d[0];                       // tmp = Vn<63:0> : Vd<127:64>
    VReg r;
    r.d[0] = d->d[0] ^ lo ^ m->d[0];
    r.d[1] = d->d[1] ^ hi ^ m->d[1];
    *d = r;
}
static void sha1_su1(VReg *d, const VReg *n) {                 // SHA1SU1
    uint32_t T[4] = {d->s[0] ^ n->s[1], d->s[1] ^ n->s[2], d->s[2] ^ n->s[3], d->s[3]};
    VReg r;
    r.s[0] = rol32(T[0], 1); r.s[1] = rol32(T[1], 1); r.s[2] = rol32(T[2], 1);
    r.s[3] = rol32(T[3], 1) ^ rol32(T[0], 2);
    *d = r;
}

static inline uint32_t sha256_sig0(uint32_t x) { return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22); }
static inline uint32_t sha256_sig1(uint32_t x) { return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25); }

static void sha256_hash(VReg *d, const VReg *n, const VReg *m, bool part1) {  // SHA256H / SHA256H2
    uint32_t X[4], Y[4];
    for (int k = 0; k < 4; k++) { X[k] = part1 ? d->s[k] : n->s[k]; Y[k] = part1 ? n->s[k] : d->s[k]; }
    for (int e = 0; e < 4; e++) {
        uint32_t chs = sha_choose(Y[0], Y[1], Y[2]), maj = sha_major(X[0], X[1], X[2]);
        uint32_t t = Y[3] + sha256_sig1(Y[0]) + chs + m->s[e];
        X[3] = t + X[3];
        Y[3] = t + sha256_sig0(X[0]) + maj;
        uint32_t topX = X[3], topY = Y[3];  // <Y, X> = ROL(Y : X, 32)
        uint32_t nx[4] = {topY, X[0], X[1], X[2]}, ny[4] = {Y[0], Y[1], Y[2], topX};
        // Y:X is a 256-bit value (Y high); rotating left by 32 moves the top word of Y to the bottom of X
        uint32_t newX[4] = {Y[3], X[0], X[1], X[2]}, newY[4] = {X[3], Y[0], Y[1], Y[2]};
        (void)nx; (void)ny;
        for (int k = 0; k < 4; k++) { X[k] = newX[k]; Y[k] = newY[k]; }
    }
    VReg *r = d;
    for (int k = 0; k < 4; k++) r->s[k] = part1 ? X[k] : Y[k];
}
static void sha256_su0(VReg *d, const VReg *n) {
    uint32_t T[4] = {d->s[1], d->s[2], d->s[3], n->s[0]};
    for (int e = 0; e < 4; e++) d->s[e] = d->s[e] + (ror32(T[e], 7) ^ ror32(T[e], 18) ^ (T[e] >> 3));
}
static void sha256_su1(VReg *d, const VReg *n, const VReg *m) {
    uint32_t T0[4] = {n->s[1], n->s[2], n->s[3], m->s[0]}, T1[2] = {m->s[2], m->s[3]}, R[4];
    uint32_t elt = T1[0]; R[0] = d->s[0] + T0[0] + (ror32(elt, 17) ^ ror32(elt, 19) ^ (elt >> 10));
    elt = T1[1];          R[1] = d->s[1] + T0[1] + (ror32(elt, 17) ^ ror32(elt, 19) ^ (elt >> 10));
    elt = R[0];           R[2] = d->s[2] + T0[2] + (ror32(elt, 17) ^ ror32(elt, 19) ^ (elt >> 10));
    elt = R[1];           R[3] = d->s[3] + T0[3] + (ror32(elt, 17) ^ ror32(elt, 19) ^ (elt >> 10));
    for (int k = 0; k < 4; k++) d->s[k] = R[k];
}

// Entry for the encodings that live in the AdvSIMD groups; returns false when `i` is not one of them.
static bool exec_crypto(CPU *c, uint32_t i) {
    int rd_ = B(i, 4, 0), rn_ = B(i, 9, 5), rm_ = B(i, 20, 16);
    if ((i & 0xff000000) == 0xce000000) {  // SHA3 extension: EOR3 / BCAX / RAX1 / XAR, on 128-bit vectors
        VReg *d = &c->vr[rd_];
        const VReg n = c->vr[rn_], m = c->vr[rm_];
        int ra_ = B(i, 14, 10);
        VReg r;
        if ((i & 0xffe08000) == 0xce000000) {        // EOR3 Vd, Vn, Vm, Va
            const VReg a = c->vr[ra_];
            r.d[0] = n.d[0] ^ m.d[0] ^ a.d[0]; r.d[1] = n.d[1] ^ m.d[1] ^ a.d[1];
        } else if ((i & 0xffe08000) == 0xce200000) { // BCAX Vd, Vn, Vm, Va: Vn ^ (Vm & ~Va)
            const VReg a = c->vr[ra_];
            r.d[0] = n.d[0] ^ (m.d[0] & ~a.d[0]); r.d[1] = n.d[1] ^ (m.d[1] & ~a.d[1]);
        } else if ((i & 0xffe0fc00) == 0xce608c00) { // RAX1 Vd.2D, Vn.2D, Vm.2D: Vn ^ ROL(Vm, 1)
            r.d[0] = n.d[0] ^ (m.d[0] << 1 | m.d[0] >> 63); r.d[1] = n.d[1] ^ (m.d[1] << 1 | m.d[1] >> 63);
        } else if ((i & 0xffe0f000) == 0xce608000 && B(i, 11, 10) != 3) {  // SHA512H / SHA512H2 / SHA512SU1: Vd, Vn, Vm (2D)
            #define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
            const VReg w = *d;
            int kind = B(i, 11, 10);
            if (kind == 0) {         // SHA512H Qd, Qn, Vm.2D
                uint64_t ms1 = ROR64(m.d[1], 14) ^ ROR64(m.d[1], 18) ^ ROR64(m.d[1], 41);
                uint64_t hi = (m.d[1] & n.d[0]) ^ (~m.d[1] & n.d[1]);
                hi += ms1 + w.d[1];
                uint64_t tmp = hi + m.d[0];
                ms1 = ROR64(tmp, 14) ^ ROR64(tmp, 18) ^ ROR64(tmp, 41);
                uint64_t lo = (tmp & m.d[1]) ^ (~tmp & n.d[0]);
                lo += ms1 + w.d[0];
                r.d[0] = lo; r.d[1] = hi;
            } else if (kind == 1) {  // SHA512H2 Qd, Qn, Vm.2D
                uint64_t ns0 = ROR64(m.d[0], 28) ^ ROR64(m.d[0], 34) ^ ROR64(m.d[0], 39);
                uint64_t hi = (n.d[0] & m.d[1]) ^ (n.d[0] & m.d[0]) ^ (m.d[1] & m.d[0]);
                hi += ns0 + w.d[1];
                ns0 = ROR64(hi, 28) ^ ROR64(hi, 34) ^ ROR64(hi, 39);
                uint64_t lo = (hi & m.d[0]) ^ (hi & m.d[1]) ^ (m.d[1] & m.d[0]);
                lo += ns0 + w.d[0];
                r.d[0] = lo; r.d[1] = hi;
            } else {                 // SHA512SU1 Vd.2D, Vn.2D, Vm.2D
                uint64_t s1 = ROR64(n.d[1], 19) ^ ROR64(n.d[1], 61) ^ (n.d[1] >> 6);
                r.d[1] = w.d[1] + s1 + m.d[1];
                s1 = ROR64(n.d[0], 19) ^ ROR64(n.d[0], 61) ^ (n.d[0] >> 6);
                r.d[0] = w.d[0] + s1 + m.d[0];
            }
        } else if ((i & 0xfffffc00) == 0xcec08000) {  // SHA512SU0 Vd.2D, Vn.2D
            const VReg w = *d;
            uint64_t s0 = ROR64(w.d[1], 1) ^ ROR64(w.d[1], 8) ^ (w.d[1] >> 7);
            r.d[0] = w.d[0] + s0;
            s0 = ROR64(n.d[0], 1) ^ ROR64(n.d[0], 8) ^ (n.d[0] >> 7);
            r.d[1] = w.d[1] + s0;
        } else if ((i & 0xffe00000) == 0xce800000) { // XAR Vd.2D, Vn.2D, Vm.2D, #imm6: ROR(Vn ^ Vm, imm6)
            int sh = B(i, 15, 10);
            for (int k = 0; k < 2; k++) {
                uint64_t x = n.d[k] ^ m.d[k];
                r.d[k] = sh ? (x >> sh | x << (64 - sh)) : x;
            }
        } else return false;
        *d = r;
        return true;
    }
    if ((i & 0xffffcc00) == 0x4e284800) {  // AESE / AESD / AESMC / AESIMC: 0x4e28{4,5,6,7}800
        aes_step(&c->vr[rd_], &c->vr[rn_], B(i, 13, 12));
        return true;
    }
    if ((i & 0xffe08c00) == 0x5e000000) {  // SHA1C/P/M/SU0, SHA256H/H2/SU1: 01011110 000 Rm 0 opc 00 Rn Rd
        VReg *d = &c->vr[rd_];
        const VReg n = c->vr[rn_], m = c->vr[rm_];
        switch (B(i, 14, 12)) {
        case 0: case 1: case 2: sha1_hash(d, &n, &m, B(i, 14, 12)); return true;
        case 3: sha1_su0(d, &n, &m); return true;
        case 4: sha256_hash(d, &n, &m, true); return true;
        case 5: sha256_hash(d, &n, &m, false); return true;
        case 6: sha256_su1(d, &n, &m); return true;
        }
        return false;
    }
    if ((i & 0xfffe0c00) == 0x5e280800) {  // SHA1H / SHA1SU1 / SHA256SU0: 01011110 00101000 opcode 10 Rn Rd
        VReg *d = &c->vr[rd_];
        const VReg n = c->vr[rn_];
        switch (B(i, 16, 12)) {
        case 0: { VReg t; memset(&t, 0, 16); t.s[0] = rol32(n.s[0], 30); *d = t; return true; }
        case 1: sha1_su1(d, &n); return true;
        case 2: sha256_su0(d, &n); return true;
        }
        return false;
    }
    return false;
}
