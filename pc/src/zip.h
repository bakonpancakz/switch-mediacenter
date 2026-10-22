// Reads entries out of a ZIP file (EPUB and CBZ books): stored or deflated,
// ZIP64 included.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class ZipReader {
public:
    struct Entry {
        std::string name;
        uint64_t offset = 0, compressed = 0, size = 0;
        int method = 0;
    };
    ~ZipReader();
    bool open(const std::string& path);
    const std::vector<Entry>& entries() const { return m_entries; }
    const Entry* find(const std::string& name) const;  // exact name
    // At most maxSize bytes uncompressed (bigger entries fail).
    bool read(const Entry& e, std::vector<uint8_t>& out, uint64_t maxSize = 64 << 20);

private:
    FILE* m_f = nullptr;
    std::vector<Entry> m_entries;
};
