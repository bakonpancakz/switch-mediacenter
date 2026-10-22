// Locked folders on the Switch (the format and crypto: shared/lockbox.h).
//
// One locked folder is open at a time. Once its PIN is in, its items are held
// here with negative ids (-1, -2, ...) and their pictures with negative picture
// ids, so the Files tab browses them like any folder; leaving it, the HOME menu
// or sleep lock it again and everything decrypted is dropped.
//
// Files in it are read through LockedFile (photos, GIFs, mpv, the visualizer's
// decoder), which decrypts as it reads, at any offset.
#pragma once
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../shared/lockbox.h"
#include "core/library.h"

namespace locks {

struct State {
    int64_t folder = 0;  // the locked folder's item id
    int64_t root = 0;    // its mapping
    std::string name;    // its real name
    lockbox::Keys keys;
    std::vector<LibItem> items;              // ids -1, -2, ...; parent: `folder` or another negative id
    std::map<std::string, uint64_t> nonces;  // full path of each file -> what it's encrypted with
};

// The open locked folder, or nullptr. Safe from any thread: a reader keeps the
// keys it got even if the folder is locked meanwhile.
std::shared_ptr<const State> current();
bool isOpen(int64_t folder);

// Whether a locked folder's PIN is all digits (a number pad, then).
bool numericPin(int64_t folder);
enum Result { OPENED, WRONG_PIN, FAILED };
Result unlock(int64_t folder, const std::string& pin);
void lock();  // close it (and drop what was decrypted)

// Browsing an open locked folder (ids: the folder's own, or negative)
bool inside(int64_t id);
std::vector<LibItem> children(int64_t id);
bool item(int64_t id, LibItem& out);

// The open folder's file at path: true and its nonce (and the keys, kept alive).
bool fileNonce(const std::string& path, uint64_t& nonce, std::shared_ptr<const State>* keep = nullptr);
// A whole file (or its first maxBytes), decrypted when it's an open locked folder's.
bool readFile(const std::string& path, std::string& out, size_t maxBytes = SIZE_MAX);
// A locked picture (negative id), decrypted. False when its folder isn't open.
bool pictureData(int64_t id, std::string& out);

// A file read at any offset, decrypting when it's an open locked folder's.
class LockedFile {
public:
    ~LockedFile() { close(); }
    bool open(const std::string& path);
    void close();
    int64_t read(void* buf, size_t n);  // bytes read, 0 at the end, -1 on an error
    bool seek(uint64_t pos);
    uint64_t size() const { return m_size; }
    uint64_t pos() const { return m_pos; }

private:
    FILE* m_f = nullptr;
    uint64_t m_size = 0, m_pos = 0, m_nonce = 0;
    std::shared_ptr<const State> m_state;  // null: a plain file
};

}  // namespace locks
