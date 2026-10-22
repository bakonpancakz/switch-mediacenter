// Locked folders: a folder on the PC with a .mediacenter_pin file in it (the
// PIN inside) goes to the card encrypted: its files, their names, thumbnails
// and details. The Switch shows a padlock until the PIN is entered.
//
// Everything both sides need is here, header-only, so the PC tool and the
// Switch app run the same code and can't disagree (the PC's tests check it
// against the published test vectors):
//   - keys: PBKDF2-HMAC-SHA256 of the PIN with a random salt, then HMAC to
//     split it into an encryption key and a tagging key
//   - AES-256 in counter mode: any byte of a file can be decrypted alone, so
//     videos seek and a 4 GB file split for FAT32 is just more offsets
//   - names and picture identities as HMAC tags; a check tag to tell a wrong PIN
//   - the format of a locked folder's item list
//
// On the Switch, AES-CTR and HMAC-SHA256 run on the CPU's crypto instructions
// (libnx). checkHardware() compares them with the portable code once; on any
// difference the portable code is used instead, so the result is always the same.
//
// What it's for: keeping private pictures private from someone browsing the
// Switch or reading its SD card. A short PIN can be guessed by someone
// determined with the card (each guess is made slow, but there are few).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace lockbox {

#ifdef __SWITCH__
inline bool& useHardware() {
    static bool on = false;  // set by checkHardware()
    return on;
}
#endif

constexpr const char* PIN_FILE = ".mediacenter_pin";
constexpr int ITERATIONS = 100000;  // per PIN guess: ~0.05 s on a PC, about half a second on the Switch

// ---------------------------------------------------------------- SHA-256

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len = 0;
    size_t used = 0;

    Sha256() { reset(); }
    void reset() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h, init, sizeof(h));
        len = 0;
        used = 0;
    }
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        len += n;
        while (n) {
            size_t take = 64 - used < n ? 64 - used : n;
            memcpy(buf + used, p, take);
            used += take;
            p += take;
            n -= take;
            if (used == 64) {
                block(buf);
                used = 0;
            }
        }
    }
    void finish(uint8_t out[32]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80, zero = 0;
        update(&pad, 1);
        while (used != 56) update(&zero, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(l, 8);
        for (int i = 0; i < 8; i++) {
            out[4 * i] = (uint8_t)(h[i] >> 24);
            out[4 * i + 1] = (uint8_t)(h[i] >> 16);
            out[4 * i + 2] = (uint8_t)(h[i] >> 8);
            out[4 * i + 3] = (uint8_t)h[i];
        }
    }
};

inline void sha256(const void* data, size_t n, uint8_t out[32]) {
    Sha256 s;
    s.update(data, n);
    s.finish(out);
}

// HMAC-SHA256 with the key's inner and outer states worked out once (PBKDF2
// runs it a hundred thousand times).
struct Hmac {
    Sha256 inner, outer;
#ifdef __SWITCH__
    HmacSha256Context hw;
#endif
    Hmac(const void* key, size_t keyLen) {
#ifdef __SWITCH__
        hmacSha256ContextCreate(&hw, key, keyLen);
#endif
        uint8_t k[64] = {}, pad[64];
        if (keyLen > 64) sha256(key, keyLen, k);
        else memcpy(k, key, keyLen);
        for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
        inner.update(pad, 64);
        for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
        outer.update(pad, 64);
    }
    void mac(const void* data, size_t n, uint8_t out[32]) const {
#ifdef __SWITCH__
        if (useHardware()) {
            HmacSha256Context c = hw;
            hmacSha256ContextUpdate(&c, data, n);
            hmacSha256ContextGetMac(&c, out);
            return;
        }
#endif
        portableMac(data, n, out);
    }
    void portableMac(const void* data, size_t n, uint8_t out[32]) const {
        Sha256 i = inner, o = outer;
        uint8_t d[32];
        i.update(data, n);
        i.finish(d);
        o.update(d, 32);
        o.finish(out);
    }
};

inline void pbkdf2(const std::string& pass, const uint8_t* salt, size_t saltLen, int iterations, uint8_t* out, size_t outLen) {
    Hmac h(pass.data(), pass.size());
    for (uint32_t blockNo = 1; outLen; blockNo++) {
        std::vector<uint8_t> s(salt, salt + saltLen);
        for (int i = 3; i >= 0; i--) s.push_back((uint8_t)(blockNo >> (8 * i)));
        uint8_t u[32], t[32];
        h.mac(s.data(), s.size(), u);
        memcpy(t, u, 32);
        for (int i = 1; i < iterations; i++) {
            h.mac(u, 32, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        size_t take = outLen < 32 ? outLen : 32;
        memcpy(out, t, take);
        out += take;
        outLen -= take;
    }
}

// ---------------------------------------------------------------- AES-256 (encryption only: all counter mode needs)

struct Aes256 {
    uint32_t rk[60];

    struct Tables {
        uint8_t sbox[256];
        uint32_t te[4][256];
        Tables() {
            // the S-box from its definition (inverse in GF(2^8), then the affine map)
            uint8_t p = 1, q = 1;
            do {
                p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));
                q ^= q << 1;
                q ^= q << 2;
                q ^= q << 4;
                if (q & 0x80) q ^= 0x09;
                uint8_t x = (uint8_t)(q ^ (uint8_t)(q << 1 | q >> 7) ^ (uint8_t)(q << 2 | q >> 6) ^ (uint8_t)(q << 3 | q >> 5) ^
                                      (uint8_t)(q << 4 | q >> 4));
                sbox[p] = x ^ 0x63;
            } while (p != 1);
            sbox[0] = 0x63;
            for (int i = 0; i < 256; i++) {
                uint8_t s = sbox[i], s2 = (uint8_t)(s << 1 ^ (s & 0x80 ? 0x1B : 0)), s3 = s2 ^ s;
                uint32_t t = (uint32_t)s2 << 24 | (uint32_t)s << 16 | (uint32_t)s << 8 | s3;
                for (int r = 0; r < 4; r++) te[r][i] = r ? (t >> (8 * r)) | (t << (32 - 8 * r)) : t;
            }
        }
    };
    static const Tables& tables() {
        static const Tables t;
        return t;
    }

    explicit Aes256(const uint8_t key[32] = nullptr) {
        if (key) setKey(key);
    }
    void setKey(const uint8_t key[32]) {
        const Tables& T = tables();
        for (int i = 0; i < 8; i++) rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
        uint32_t rcon = 0x01000000;
        for (int i = 8; i < 60; i++) {
            uint32_t t = rk[i - 1];
            auto sub = [&](uint32_t w) {
                return (uint32_t)T.sbox[w >> 24] << 24 | (uint32_t)T.sbox[(w >> 16) & 0xff] << 16 |
                       (uint32_t)T.sbox[(w >> 8) & 0xff] << 8 | T.sbox[w & 0xff];
            };
            if (i % 8 == 0) {
                t = sub((t << 8) | (t >> 24)) ^ rcon;
                rcon = (rcon << 1) ^ (rcon & 0x80000000 ? 0x1B000000 : 0);
            } else if (i % 8 == 4)
                t = sub(t);
            rk[i] = rk[i - 8] ^ t;
        }
    }
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        const Tables& T = tables();
        uint32_t s[4], t[4];
        for (int i = 0; i < 4; i++)
            s[i] = ((uint32_t)in[4 * i] << 24 | (uint32_t)in[4 * i + 1] << 16 | (uint32_t)in[4 * i + 2] << 8 | in[4 * i + 3]) ^ rk[i];
        for (int r = 1; r < 14; r++) {
            for (int i = 0; i < 4; i++)
                t[i] = T.te[0][s[i] >> 24] ^ T.te[1][(s[(i + 1) & 3] >> 16) & 0xff] ^ T.te[2][(s[(i + 2) & 3] >> 8) & 0xff] ^
                       T.te[3][s[(i + 3) & 3] & 0xff] ^ rk[4 * r + i];
            memcpy(s, t, sizeof(s));
        }
        for (int i = 0; i < 4; i++) {
            uint32_t v = ((uint32_t)T.sbox[s[i] >> 24] << 24 | (uint32_t)T.sbox[(s[(i + 1) & 3] >> 16) & 0xff] << 16 |
                          (uint32_t)T.sbox[(s[(i + 2) & 3] >> 8) & 0xff] << 8 | T.sbox[s[(i + 3) & 3] & 0xff]) ^
                         rk[56 + i];
            out[4 * i] = (uint8_t)(v >> 24);
            out[4 * i + 1] = (uint8_t)(v >> 16);
            out[4 * i + 2] = (uint8_t)(v >> 8);
            out[4 * i + 3] = (uint8_t)v;
        }
    }
};

// Counter mode: the keystream block for byte offset o is AES(nonce || o / 16),
// both big-endian. XORs buf (offset `offset` in the file) in place: the same
// call encrypts and decrypts.
inline void portableCtr(const Aes256& aes, uint64_t nonce, uint64_t offset, uint8_t* buf, size_t n) {
    uint8_t ctrBlock[16], ks[16];
    for (int i = 0; i < 8; i++) ctrBlock[i] = (uint8_t)(nonce >> (56 - 8 * i));
    while (n) {
        uint64_t blockNo = offset / 16;
        size_t skip = (size_t)(offset % 16), take = 16 - skip < n ? 16 - skip : n;
        for (int i = 0; i < 8; i++) ctrBlock[8 + i] = (uint8_t)(blockNo >> (56 - 8 * i));
        aes.encryptBlock(ctrBlock, ks);
        for (size_t i = 0; i < take; i++) buf[i] ^= ks[skip + i];
        buf += take;
        offset += take;
        n -= take;
    }
}

inline void ctr(const Aes256& aes, const uint8_t key[32], uint64_t nonce, uint64_t offset, uint8_t* buf, size_t n) {
#ifdef __SWITCH__
    if (useHardware() && n) {
        uint8_t c[16];
        uint64_t blockNo = offset / 16;
        for (int i = 0; i < 8; i++) {
            c[i] = (uint8_t)(nonce >> (56 - 8 * i));
            c[8 + i] = (uint8_t)(blockNo >> (56 - 8 * i));
        }
        Aes256CtrContext ctx;
        aes256CtrContextCreate(&ctx, key, c);
        size_t skip = (size_t)(offset % 16);
        if (skip) {  // into the block: use up its first bytes
            uint8_t waste[16] = {};
            aes256CtrCrypt(&ctx, waste, waste, skip);
        }
        aes256CtrCrypt(&ctx, buf, buf, n);
        return;
    }
#else
    (void)key;
#endif
    portableCtr(aes, nonce, offset, buf, n);
}

// ---------------------------------------------------------------- keys

struct Keys {
    uint8_t enc[32] = {}, mac[32] = {};
    Aes256 aes;

    // Slow on purpose (ITERATIONS): every PIN guess costs this much.
    void derive(const std::string& pin, const uint8_t* salt, size_t saltLen, int iterations) {
        uint8_t master[32];
        pbkdf2(pin, salt, saltLen, iterations, master, 32);
        Hmac h(master, 32);
        h.mac("enc", 3, enc);
        h.mac("mac", 3, mac);
        aes.setKey(enc);
    }
    // Encrypts or decrypts n bytes at `offset` of a file encrypted with `nonce`.
    void crypt(uint64_t nonce, uint64_t offset, uint8_t* buf, size_t n) const { ctr(aes, enc, nonce, offset, buf, n); }
    // An HMAC of msg as hex, its first `chars` digits: file names, picture
    // tags, the PIN check.
    std::string tag(const std::string& msg, int chars = 32) const {
        uint8_t d[32];
        Hmac(mac, 32).mac(msg.data(), msg.size(), d);
        static const char* hex = "0123456789abcdef";
        std::string s;
        for (int i = 0; i < 32 && (int)s.size() < chars; i++) {
            s += hex[d[i] >> 4];
            s += hex[d[i] & 15];
        }
        return s.substr(0, chars);
    }
    std::string check() const { return tag("mediacenter pin check"); }
    // A per-file nonce: tied to the file's name and version, so a changed file
    // never reuses another's keystream
    uint64_t nonceFor(const std::string& msg) const {
        uint8_t d[32];
        Hmac(mac, 32).mac(msg.data(), msg.size(), d);
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = v << 8 | d[i];
        return v;
    }
};

#ifdef __SWITCH__
// Turns on the CPU's crypto instructions if they give exactly what the portable
// code does (an unaligned counter-mode run and an HMAC). Returns whether they're used.
inline bool checkHardware() {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 3);
    Aes256 aes(key);
    uint8_t a[150], b[150];
    for (int i = 0; i < 150; i++) a[i] = b[i] = (uint8_t)i;
    useHardware() = false;
    ctr(aes, key, 0x0123456789abcdefull, 37, a, sizeof(a));
    uint8_t m1[32], m2[32];
    Hmac(key, 32).mac(a, sizeof(a), m1);
    useHardware() = true;
    ctr(aes, key, 0x0123456789abcdefull, 37, b, sizeof(b));
    Hmac(key, 32).mac(b, sizeof(b), m2);
    useHardware() = memcmp(a, b, sizeof(a)) == 0 && memcmp(m1, m2, 32) == 0;
    return useHardware();
}
#endif

// ---------------------------------------------------------------- a locked folder's items

// What a locked folder holds, written by the PC (encrypted) and read by the
// Switch once unlocked. ids are 1-based within the folder; parent 0 is the
// locked folder itself. path is the file on the card (relative to the app's
// folder), thumb a row of locked_pictures, nonce what its file is encrypted with.
struct Item {
    int64_t id = 0, parent = 0;
    int32_t kind = 0, width = 0, height = 0, children = 0;
    std::string name, path;
    int64_t size = 0, mtime = 0, taken = 0, duration = 0, thumb = 0;
    uint64_t nonce = 0;
    std::vector<uint8_t> wave;
};

struct Writer {
    std::vector<uint8_t> out;
    void u64(uint64_t v) {
        for (int i = 0; i < 8; i++) out.push_back((uint8_t)(v >> (8 * i)));
    }
    void bytes(const void* p, size_t n) {
        u64(n);
        out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n);
    }
    void str(const std::string& s) { bytes(s.data(), s.size()); }
};

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    uint64_t u64() {
        if (at + 8 > n) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = v << 8 | p[at + i];
        at += 8;
        return v;
    }
    std::string str() {
        uint64_t len = u64();
        if (!ok || len > n - at) { ok = false; return ""; }
        std::string s((const char*)p + at, (size_t)len);
        at += (size_t)len;
        return s;
    }
};

constexpr uint64_t FORMAT = 0x4d434c4f434b3031ull;  // "MCLOCK01"

inline std::vector<uint8_t> encodeItems(const std::string& folderName, const std::vector<Item>& items) {
    Writer w;
    w.u64(FORMAT);
    w.str(folderName);
    w.u64(items.size());
    for (const Item& it : items) {
        w.u64((uint64_t)it.id);
        w.u64((uint64_t)it.parent);
        w.u64((uint64_t)(int64_t)it.kind);
        w.str(it.name);
        w.str(it.path);
        w.u64((uint64_t)it.size);
        w.u64((uint64_t)it.mtime);
        w.u64((uint64_t)it.taken);
        w.u64((uint64_t)it.duration);
        w.u64((uint64_t)(int64_t)it.width);
        w.u64((uint64_t)(int64_t)it.height);
        w.u64((uint64_t)(int64_t)it.children);
        w.u64((uint64_t)it.thumb);
        w.u64(it.nonce);
        w.bytes(it.wave.data(), it.wave.size());
    }
    return w.out;
}

inline bool decodeItems(const std::vector<uint8_t>& data, std::string& folderName, std::vector<Item>& items) {
    Reader r{data.data(), data.size()};
    if (r.u64() != FORMAT) return false;
    folderName = r.str();
    uint64_t count = r.u64();
    if (!r.ok || count > data.size()) return false;
    items.clear();
    for (uint64_t i = 0; i < count && r.ok; i++) {
        Item it;
        it.id = (int64_t)r.u64();
        it.parent = (int64_t)r.u64();
        it.kind = (int32_t)(int64_t)r.u64();
        it.name = r.str();
        it.path = r.str();
        it.size = (int64_t)r.u64();
        it.mtime = (int64_t)r.u64();
        it.taken = (int64_t)r.u64();
        it.duration = (int64_t)r.u64();
        it.width = (int32_t)(int64_t)r.u64();
        it.height = (int32_t)(int64_t)r.u64();
        it.children = (int32_t)(int64_t)r.u64();
        it.thumb = (int64_t)r.u64();
        it.nonce = r.u64();
        std::string wave = r.str();
        it.wave.assign(wave.begin(), wave.end());
        items.push_back(std::move(it));
    }
    return r.ok;
}

}  // namespace lockbox
