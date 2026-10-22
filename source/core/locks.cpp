#include "core/locks.h"

#include <algorithm>
#include <mutex>

#include "app.h"

namespace locks {

namespace {

std::mutex s_mx;
std::shared_ptr<const State> s_open;

void checkHardwareOnce() {
    static bool checked = false;
    if (checked) return;
    checked = true;
    bool hw = lockbox::checkHardware();
    appLog("locks: %s", hw ? "using the CPU's crypto instructions" : "the CPU's crypto instructions disagree: using the portable code");
}

}  // namespace

std::shared_ptr<const State> current() {
    std::lock_guard<std::mutex> l(s_mx);
    return s_open;
}

bool isOpen(int64_t folder) {
    auto st = current();
    return st && st->folder == folder;
}

bool numericPin(int64_t folder) {
    LibLock row;
    return !Library::get().lockRow(folder, row) || row.numeric;
}

Result unlock(int64_t folder, const std::string& pin) {
    checkHardwareOnce();
    LibLock row;
    LibItem folderItem;
    if (!Library::get().lockRow(folder, row) || !Library::get().item(folder, folderItem)) return FAILED;
    auto st = std::make_shared<State>();
    uint32_t t0 = SDL_GetTicks();
    st->keys.derive(pin, row.salt.data(), row.salt.size(), row.iterations);
    appLog("locks: PIN checked in %u ms", SDL_GetTicks() - t0);
    if (st->keys.check() != row.check) return WRONG_PIN;
    std::vector<uint8_t> data = row.data;
    st->keys.crypt(row.nonce, 0, data.data(), data.size());
    std::vector<lockbox::Item> items;
    if (!lockbox::decodeItems(data, st->name, items)) {
        appLog("locks: the folder's list didn't decode");
        return FAILED;
    }
    st->folder = folder;
    st->root = folderItem.root;
    for (const lockbox::Item& x : items) {
        LibItem it;
        it.id = -x.id;
        it.parent = x.parent ? -x.parent : folder;
        it.root = folderItem.root;
        it.kind = x.kind;
        it.name = x.name;
        it.path = x.path.empty() ? "" : Library::fullPath(x.path);
        it.size = x.size;
        it.mtime = x.mtime;
        it.taken = x.taken;
        it.duration = x.duration;
        it.width = x.width;
        it.height = x.height;
        it.children = x.children;
        it.thumb = x.thumb ? -x.thumb : 0;
        it.wave = x.wave;
        if (!it.path.empty()) st->nonces[it.path] = x.nonce;
        st->items.push_back(std::move(it));
    }
    {
        std::lock_guard<std::mutex> l(s_mx);
        s_open = st;
    }
    appLog("locks: a locked folder opened (%d items)", (int)items.size());
    return OPENED;
}

void lock() {
    std::shared_ptr<const State> was;
    {
        std::lock_guard<std::mutex> l(s_mx);
        was.swap(s_open);
    }
    if (!was) return;
    picturesReset();  // its pictures, decrypted, go too
    appLog("locks: locked again");
}

bool inside(int64_t id) {
    auto st = current();
    return st && (id == st->folder || id < 0);
}

std::vector<LibItem> children(int64_t id) {
    std::vector<LibItem> v;
    auto st = current();
    if (!st) return v;
    for (const LibItem& it : st->items)
        if (it.parent == id) v.push_back(it);
    // the library's order: folders first, then by name (the Files tab sorts from there)
    std::stable_sort(v.begin(), v.end(), [](const LibItem& a, const LibItem& b) {
        if ((a.kind == lib::K_DIR) != (b.kind == lib::K_DIR)) return a.kind == lib::K_DIR;
        return lib::sortKey(a.name) < lib::sortKey(b.name);
    });
    return v;
}

bool item(int64_t id, LibItem& out) {
    auto st = current();
    if (!st) return false;
    for (const LibItem& it : st->items)
        if (it.id == id) {
            out = it;
            return true;
        }
    return false;
}

bool fileNonce(const std::string& path, uint64_t& nonce, std::shared_ptr<const State>* keep) {
    auto st = current();
    if (!st) return false;
    auto f = st->nonces.find(path);
    if (f == st->nonces.end()) return false;
    nonce = f->second;
    if (keep) *keep = st;
    return true;
}

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    LockedFile f;
    if (!f.open(path)) return false;
    size_t n = (size_t)std::min<uint64_t>(f.size(), maxBytes);
    out.resize(n);
    size_t got = 0;
    while (got < n) {
        int64_t r = f.read(&out[got], n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    out.resize(got);
    return got == n;
}

bool pictureData(int64_t id, std::string& out) {
    auto st = current();
    uint64_t nonce = 0;
    if (!st || id >= 0 || !Library::get().lockedPicture(-id, out, nonce)) return false;
    st->keys.crypt(nonce, 0, (uint8_t*)&out[0], out.size());
    return true;
}

bool LockedFile::open(const std::string& path) {
    close();
    uint64_t nonce = 0;
    std::shared_ptr<const State> st;
    bool locked = fileNonce(path, nonce, &st);
    m_f = fopen(path.c_str(), "rb");
    if (!m_f) return false;
    fseeko(m_f, 0, SEEK_END);
    m_size = (uint64_t)ftello(m_f);
    fseeko(m_f, 0, SEEK_SET);
    m_pos = 0;
    m_nonce = nonce;
    if (locked) m_state = st;
    return true;
}

void LockedFile::close() {
    if (m_f) fclose(m_f);
    m_f = nullptr;
    m_state.reset();
    m_size = m_pos = 0;
}

int64_t LockedFile::read(void* buf, size_t n) {
    if (!m_f) return -1;
    size_t got = fread(buf, 1, n, m_f);
    if (got == 0) return ferror(m_f) ? -1 : 0;
    if (m_state) m_state->keys.crypt(m_nonce, m_pos, (uint8_t*)buf, got);
    m_pos += got;
    return (int64_t)got;
}

bool LockedFile::seek(uint64_t pos) {
    if (!m_f || fseeko(m_f, (off_t)pos, SEEK_SET) != 0) return false;
    m_pos = pos;
    return true;
}

}  // namespace locks
