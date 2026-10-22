// shared/lockbox.h against the published test vectors, plus its own counter
// layout and item format. The Switch runs the same code (or its CPU's crypto
// instructions, checked against this code when it starts).
#include <string>
#include <vector>

#include "lockbox.h"
#include "test.h"

using namespace lockbox;

static std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)std::stoi(s.substr(i, 2), nullptr, 16));
    return v;
}

TEST(sha256_vectors) {  // FIPS 180-2
    uint8_t d[32];
    sha256("abc", 3, d);
    CHECK_EQ(hex(d, 32), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256("", 0, d);
    CHECK_EQ(hex(d, 32), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    std::string m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // two blocks
    sha256(m.data(), m.size(), d);
    CHECK_EQ(hex(d, 32), std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST(hmac_vectors) {  // RFC 4231, test case 2
    uint8_t d[32];
    std::string msg = "what do ya want for nothing?";
    Hmac("Jefe", 4).mac(msg.data(), msg.size(), d);
    CHECK_EQ(hex(d, 32), std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

TEST(pbkdf2_vectors) {  // PBKDF2-HMAC-SHA256, "password" / "salt"
    uint8_t d[32];
    pbkdf2("password", (const uint8_t*)"salt", 4, 1, d, 32);
    CHECK_EQ(hex(d, 32), std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    pbkdf2("password", (const uint8_t*)"salt", 4, 2, d, 32);
    CHECK_EQ(hex(d, 32), std::string("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    pbkdf2("password", (const uint8_t*)"salt", 4, 4096, d, 32);
    CHECK_EQ(hex(d, 32), std::string("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
}

TEST(aes256_vectors) {
    // FIPS-197 appendix C.3
    std::vector<uint8_t> key = unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    std::vector<uint8_t> pt = unhex("00112233445566778899aabbccddeeff");
    uint8_t out[16];
    Aes256(key.data()).encryptBlock(pt.data(), out);
    CHECK_EQ(hex(out, 16), std::string("8ea2b7ca516745bfeafc49904b496089"));

    // NIST SP 800-38A F.5.5, CTR-AES256: each counter block encrypted, XORed with the plaintext
    key = unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    Aes256 aes(key.data());
    const char* ctrs[] = {"f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", "f0f1f2f3f4f5f6f7f8f9fafbfcfdff00",
                          "f0f1f2f3f4f5f6f7f8f9fafbfcfdff01", "f0f1f2f3f4f5f6f7f8f9fafbfcfdff02"};
    const char* pts[] = {"6bc1bee22e409f96e93d7e117393172a", "ae2d8a571e03ac9c9eb76fac45af8e51",
                         "30c81c46a35ce411e5fbc1191a0a52ef", "f69f2445df4f9b17ad2b417be66c3710"};
    const char* cts[] = {"601ec313775789a5b7a7f504bbf3d228", "f443e3ca4d62b59aca84e990cacaf5c5",
                         "2b0930daa23de94ce87017ba2d84988d", "dfc9c58db67aada613c2dd08457941a6"};
    for (int i = 0; i < 4; i++) {
        std::vector<uint8_t> c = unhex(ctrs[i]), p = unhex(pts[i]);
        uint8_t ks[16];
        aes.encryptBlock(c.data(), ks);
        for (int j = 0; j < 16; j++) p[j] ^= ks[j];
        CHECK_EQ(hex(p.data(), 16), std::string(cts[i]));
    }
}

TEST(counter_mode_layout_and_offsets) {
    std::vector<uint8_t> key(32, 7);
    Aes256 aes(key.data());
    const uint64_t nonce = 0x1122334455667788ull;
    // the keystream for byte offset o is AES(nonce || o / 16)
    uint8_t blk[16], ks[16];
    for (int i = 0; i < 8; i++) blk[i] = (uint8_t)(nonce >> (56 - 8 * i));
    uint64_t blockNo = 5;
    for (int i = 0; i < 8; i++) blk[8 + i] = (uint8_t)(blockNo >> (56 - 8 * i));
    aes.encryptBlock(blk, ks);
    uint8_t z[16] = {};
    ctr(aes, key.data(), nonce, 5 * 16, z, 16);
    CHECK(memcmp(z, ks, 16) == 0);
    // any slice decrypts on its own, wherever it starts
    std::vector<uint8_t> plain(1000), enc;
    for (size_t i = 0; i < plain.size(); i++) plain[i] = (uint8_t)(i * 31 + 5);
    enc = plain;
    ctr(aes, key.data(), nonce, 0, enc.data(), enc.size());
    CHECK(enc != plain);
    for (size_t at : {0, 1, 15, 16, 17, 333, 999}) {
        std::vector<uint8_t> part(enc.begin() + at, enc.end());
        ctr(aes, key.data(), nonce, at, part.data(), part.size());
        CHECK(std::equal(part.begin(), part.end(), plain.begin() + at));
    }
}

TEST(keys_and_item_format) {
    uint8_t salt[16] = {1, 2, 3};
    Keys a, b, c;
    a.derive("1234", salt, 16, 1000);
    b.derive("1234", salt, 16, 1000);
    c.derive("1235", salt, 16, 1000);
    CHECK_EQ(a.check(), b.check());
    CHECK(a.check() != c.check());  // a wrong PIN is told apart
    CHECK_EQ(a.tag("x/y.jpg", 24).size(), (size_t)24);
    CHECK(a.nonceFor("x/y.jpg|1") != a.nonceFor("x/y.jpg|2"));

    std::vector<Item> items(2);
    items[0].id = 1;
    items[0].kind = 0;
    items[0].name = "Holiday \xE2\x98\x86";
    items[1].id = 2;
    items[1].parent = 1;
    items[1].kind = 1;
    items[1].name = "a.jpg";
    items[1].path = ".media/files/Pictures/.locked/abc.jpg";
    items[1].size = 1234567;
    items[1].thumb = 9;
    items[1].nonce = 0xfedcba9876543210ull;
    items[1].wave = {1, 2, 3};
    std::vector<uint8_t> data = encodeItems("Private", items);
    std::string name;
    std::vector<Item> back;
    CHECK(decodeItems(data, name, back));
    CHECK_EQ(name, std::string("Private"));
    CHECK_EQ(back.size(), (size_t)2);
    CHECK_EQ(back[0].name, items[0].name);
    CHECK_EQ(back[1].path, items[1].path);
    CHECK_EQ(back[1].size, (int64_t)1234567);
    CHECK(back[1].nonce == items[1].nonce && back[1].wave == items[1].wave && back[1].parent == 1);
    data.resize(data.size() - 5);
    CHECK(!decodeItems(data, name, back));  // cut short: refused, not misread
}

int main() { return runTests(); }
