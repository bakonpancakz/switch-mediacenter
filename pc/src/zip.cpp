#include "zip.h"

#include <zlib.h>

#include <algorithm>

#include "fsx.h"

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

ZipReader::~ZipReader() {
    if (m_f) fclose(m_f);
}

static bool readAt(FILE* f, uint64_t off, void* buf, size_t n) {
    return _fseeki64(f, (int64_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

bool ZipReader::open(const std::string& path) {
    m_f = fsx::open(path, "rb");
    if (!m_f) return false;
    _fseeki64(m_f, 0, SEEK_END);
    int64_t size = _ftelli64(m_f);
    if (size < 22) return false;
    // the end-of-central-directory record is in the last 64 KiB + 22 bytes
    size_t tail = (size_t)std::min<int64_t>(size, 65557);
    std::vector<uint8_t> t(tail);
    if (!readAt(m_f, size - tail, t.data(), tail)) return false;
    int64_t eocd = -1;
    for (int64_t i = (int64_t)tail - 22; i >= 0; i--)
        if (rd32(&t[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return false;
    uint64_t count = rd16(&t[eocd + 10]);
    uint64_t cdSize = rd32(&t[eocd + 12]), cdOff = rd32(&t[eocd + 16]);
    // ZIP64: a locator just before it points at the 64-bit record
    if (eocd >= 20 && rd32(&t[eocd - 20]) == 0x07064b50) {
        uint8_t z[56];
        if (readAt(m_f, rd64(&t[eocd - 20 + 8]), z, sizeof(z)) && rd32(z) == 0x06064b50) {
            count = rd64(z + 32);
            cdSize = rd64(z + 40);
            cdOff = rd64(z + 48);
        }
    }
    if (cdSize > (256u << 20) || cdOff + cdSize > (uint64_t)size) return false;
    std::vector<uint8_t> cd(cdSize);
    if (!readAt(m_f, cdOff, cd.data(), cd.size())) return false;
    size_t p = 0;
    for (uint64_t i = 0; i < count && p + 46 <= cd.size(); i++) {
        if (rd32(&cd[p]) != 0x02014b50) break;
        Entry e;
        e.method = rd16(&cd[p + 10]);
        e.compressed = rd32(&cd[p + 20]);
        e.size = rd32(&cd[p + 24]);
        size_t nameLen = rd16(&cd[p + 28]), extraLen = rd16(&cd[p + 30]), commentLen = rd16(&cd[p + 32]);
        e.offset = rd32(&cd[p + 42]);
        if (p + 46 + nameLen + extraLen > cd.size()) break;
        e.name.assign((const char*)&cd[p + 46], nameLen);
        // ZIP64 extra field: the 64-bit values, in order, for the fields that are 0xFFFFFFFF
        size_t x = p + 46 + nameLen, xe = x + extraLen;
        while (x + 4 <= xe) {
            uint16_t id = rd16(&cd[x]), len = rd16(&cd[x + 2]);
            if (id == 1) {
                size_t q = x + 4;
                if (e.size == 0xFFFFFFFF && q + 8 <= x + 4 + len) { e.size = rd64(&cd[q]); q += 8; }
                if (e.compressed == 0xFFFFFFFF && q + 8 <= x + 4 + len) { e.compressed = rd64(&cd[q]); q += 8; }
                if (e.offset == 0xFFFFFFFF && q + 8 <= x + 4 + len) { e.offset = rd64(&cd[q]); q += 8; }
            }
            x += 4 + len;
        }
        if (!e.name.empty() && e.name.back() != '/') m_entries.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipReader::Entry* ZipReader::find(const std::string& name) const {
    for (auto& e : m_entries)
        if (e.name == name) return &e;
    return nullptr;
}

bool ZipReader::read(const Entry& e, std::vector<uint8_t>& out, uint64_t maxSize) {
    if (e.size > maxSize || e.compressed > (1ull << 32)) return false;
    uint8_t lh[30];
    if (!readAt(m_f, e.offset, lh, sizeof(lh)) || rd32(lh) != 0x04034b50) return false;
    uint64_t data = e.offset + 30 + rd16(lh + 26) + rd16(lh + 28);
    std::vector<uint8_t> comp(e.compressed);
    if (e.compressed && !readAt(m_f, data, comp.data(), comp.size())) return false;
    if (e.method == 0) {
        out = std::move(comp);
        return out.size() == e.size;
    }
    if (e.method != 8) return false;
    out.assign(e.size, 0);
    z_stream z = {};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = comp.data();
    z.avail_in = (uInt)comp.size();
    z.next_out = out.data();
    z.avail_out = (uInt)out.size();
    int rc = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return (rc == Z_STREAM_END || rc == Z_OK || rc == Z_BUF_ERROR) && z.total_out == e.size;
}
