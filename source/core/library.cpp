#include "core/library.h"

#include <atomic>
#include <mutex>

#include "app.h"
#include "sqlite3.h"

namespace {

std::mutex s_mx;  // SQLite is built single-threaded: this is the only way in
sqlite3* s_db = nullptr;
std::string s_problem = "No library yet";
std::atomic<uint32_t> s_version{1};
std::string s_builtOn;
int64_t s_builtAt = 0;

// A prepared statement for one call, finalized on the way out.
struct Q {
    sqlite3_stmt* s = nullptr;
    explicit Q(const char* sql) {
        if (s_db && sqlite3_prepare_v2(s_db, sql, -1, &s, nullptr) != SQLITE_OK) {
            appLog("library: %s in %s", sqlite3_errmsg(s_db), sql);
            s = nullptr;
        }
    }
    ~Q() { if (s) sqlite3_finalize(s); }
    Q& bind(int i, int64_t v) { if (s) sqlite3_bind_int64(s, i, v); return *this; }
    Q& bind(int i, const std::string& v) { if (s) sqlite3_bind_text(s, i, v.data(), (int)v.size(), SQLITE_TRANSIENT); return *this; }
    bool step() { return s && sqlite3_step(s) == SQLITE_ROW; }
    int64_t i64(int c) { return sqlite3_column_int64(s, c); }
    int i32(int c) { return sqlite3_column_int(s, c); }
    std::string str(int c) {
        const unsigned char* t = sqlite3_column_text(s, c);
        return t ? std::string((const char*)t, sqlite3_column_bytes(s, c)) : "";
    }
    std::vector<uint8_t> blob(int c) {
        const uint8_t* p = (const uint8_t*)sqlite3_column_blob(s, c);
        return p ? std::vector<uint8_t>(p, p + sqlite3_column_bytes(s, c)) : std::vector<uint8_t>();
    }
};

std::string metaLocked(const char* key) {
    Q q("SELECT value FROM meta WHERE key = ?");
    q.bind(1, std::string(key));
    return q.step() ? q.str(0) : "";
}

void closeLocked() {
    if (s_db) sqlite3_close(s_db);
    s_db = nullptr;
}

const char* ITEM_COLS = "id, parent, root, kind, name, path, size, mtime, taken, duration, width, height, children, thumb, wave";

LibItem readItem(Q& q) {
    LibItem it;
    it.id = q.i64(0);
    it.parent = q.i64(1);
    it.root = q.i64(2);
    it.kind = q.i32(3);
    it.name = q.str(4);
    it.path = Library::fullPath(q.str(5));
    it.size = q.i64(6);
    it.mtime = q.i64(7);
    it.taken = q.i64(8);
    it.duration = q.i64(9);
    it.width = q.i32(10);
    it.height = q.i32(11);
    it.children = q.i32(12);
    it.thumb = q.i64(13);
    it.wave = q.blob(14);
    return it;
}

}  // namespace

Library& Library::get() {
    static Library l;
    return l;
}

std::string Library::fullPath(const std::string& rel) { return std::string(APP_DIR) + "/" + rel; }

void Library::open() {
    std::lock_guard<std::mutex> l(s_mx);
    closeLocked();
    std::string path = fullPath(lib::DB_FILE);
    s_builtOn.clear();
    s_builtAt = 0;
    if (!fileExists(path)) {
        s_problem = "No library yet";
        s_version++;
        return;
    }
    // read-write only so a journal the PC left (a card pulled out mid-sync) can be rolled back
    if (sqlite3_open_v2(path.c_str(), &s_db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        appLog("library: can't open %s: %s", path.c_str(), s_db ? sqlite3_errmsg(s_db) : "?");
        closeLocked();
        s_problem = "The library couldn't be opened";
        s_version++;
        return;
    }
    sqlite3_exec(s_db, "PRAGMA cache_size=-8192", nullptr, nullptr, nullptr);  // 8 MiB of pages
    int schema = atoi(metaLocked("schema").c_str());
    if (schema != lib::SCHEMA) {
        appLog("library: schema %d, this app reads %d", schema, lib::SCHEMA);
        closeLocked();
        s_problem = schema > lib::SCHEMA ? "This library needs a newer Media Center: update the app" : "The library couldn't be read: sync again";
        s_version++;
        return;
    }
    s_builtOn = metaLocked("pc");
    s_builtAt = atoll(metaLocked("built").c_str());
    s_problem.clear();
    s_version++;
    appLog("library: from %s, built %lld", s_builtOn.c_str(), (long long)s_builtAt);
}

void Library::close() {
    std::lock_guard<std::mutex> l(s_mx);
    closeLocked();
    s_problem = "No library yet";
    s_version++;
}

bool Library::ready() {
    std::lock_guard<std::mutex> l(s_mx);
    return s_db != nullptr;
}

std::string Library::problem() {
    std::lock_guard<std::mutex> l(s_mx);
    return s_problem;
}

uint32_t Library::version() { return s_version; }

std::string Library::builtOn() {
    std::lock_guard<std::mutex> l(s_mx);
    return s_builtOn;
}

int64_t Library::builtAt() {
    std::lock_guard<std::mutex> l(s_mx);
    return s_builtAt;
}

std::vector<LibRoot> Library::roots(int category) {
    std::lock_guard<std::mutex> l(s_mx);
    std::vector<LibRoot> v;
    Q q("SELECT id, item, category, name, dir FROM roots WHERE category = ? ORDER BY name");
    q.bind(1, (int64_t)category);
    while (q.step()) v.push_back({q.i64(0), q.i64(1), q.i32(2), q.str(3), q.str(4)});
    return v;
}

bool Library::item(int64_t id, LibItem& out) {
    std::lock_guard<std::mutex> l(s_mx);
    Q q((std::string("SELECT ") + ITEM_COLS + " FROM items WHERE id = ?").c_str());
    q.bind(1, id);
    if (!q.step()) return false;
    out = readItem(q);
    return true;
}

bool Library::itemByPath(const std::string& path, LibItem& out) {
    std::string prefix = std::string(APP_DIR) + "/";
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    std::lock_guard<std::mutex> l(s_mx);
    Q q((std::string("SELECT ") + ITEM_COLS + " FROM items WHERE path = ?").c_str());
    q.bind(1, path.substr(prefix.size()));
    if (!q.step()) return false;
    out = readItem(q);
    return true;
}

std::vector<LibItem> Library::children(int64_t parent) {
    std::lock_guard<std::mutex> l(s_mx);
    std::vector<LibItem> v;
    // the same expression as the index, so SQLite walks it in order
    Q q((std::string("SELECT ") + ITEM_COLS + " FROM items WHERE parent = ? ORDER BY kind <> 0, sort").c_str());
    q.bind(1, parent);
    while (q.step()) v.push_back(readItem(q));
    return v;
}

bool Library::pictureData(int64_t id, std::string& out) {
    if (!id) return false;
    std::lock_guard<std::mutex> l(s_mx);
    Q q("SELECT data FROM pictures WHERE id = ?");
    q.bind(1, id);
    if (!q.step()) return false;
    const void* p = sqlite3_column_blob(q.s, 0);
    int n = sqlite3_column_bytes(q.s, 0);
    out.assign(p ? (const char*)p : "", p ? n : 0);
    return !out.empty();
}

bool Library::lockRow(int64_t item, LibLock& out) {
    std::lock_guard<std::mutex> l(s_mx);
    Q q("SELECT lock, salt, iterations, check_tag, numeric, nonce, data FROM locks WHERE item = ?");
    if (!q.s) return false;  // a library from before locked folders
    q.bind(1, item);
    if (!q.step()) return false;
    out.lock = q.str(0);
    out.salt = q.blob(1);
    out.iterations = q.i32(2);
    out.check = q.str(3);
    out.numeric = q.i32(4) != 0;
    out.nonce = (uint64_t)q.i64(5);
    out.data = q.blob(6);
    return true;
}

bool Library::lockedPicture(int64_t id, std::string& data, uint64_t& nonce) {
    std::lock_guard<std::mutex> l(s_mx);
    Q q("SELECT nonce, data FROM locked_pictures WHERE id = ?");
    if (!q.s) return false;
    q.bind(1, id);
    if (!q.step()) return false;
    nonce = (uint64_t)q.i64(0);
    const void* p = sqlite3_column_blob(q.s, 1);
    int n = sqlite3_column_bytes(q.s, 1);
    data.assign(p ? (const char*)p : "", p ? n : 0);
    return !data.empty();
}

std::vector<LibShow> Library::shows() {
    std::lock_guard<std::mutex> l(s_mx);
    std::vector<LibShow> v;
    {
        Q q("SELECT id, root, title, year, overview, poster, backdrop, tmdb, added FROM shows ORDER BY sort");
        while (q.step()) {
            LibShow s;
            s.id = q.i64(0);
            s.root = q.i64(1);
            s.title = q.str(2);
            s.year = q.i32(3);
            s.overview = q.str(4);
            s.poster = q.i64(5);
            s.backdrop = q.i64(6);
            s.tmdb = q.i32(7);
            s.added = q.i64(8);
            v.push_back(std::move(s));
        }
    }
    if (v.empty()) return v;
    std::vector<size_t> byId;  // show id -> index (ids are dense from 1)
    for (size_t i = 0; i < v.size(); i++) {
        if ((size_t)v[i].id >= byId.size()) byId.resize(v[i].id + 1, SIZE_MAX);
        byId[v[i].id] = i;
    }
    auto showOf = [&](int64_t id) -> LibShow* {
        return id >= 0 && (size_t)id < byId.size() && byId[id] != SIZE_MAX ? &v[byId[id]] : nullptr;
    };
    {
        Q q("SELECT id, show, number, title FROM seasons ORDER BY show, ord");
        while (q.step())
            if (LibShow* s = showOf(q.i64(1))) s->seasons.push_back({q.i64(0), q.i32(2), q.str(3)});
    }
    {
        Q q("SELECT e.id, e.show, e.season, s.number, e.number, e.title, e.overview, e.still, e.item, i.path, i.duration"
            " FROM episodes e JOIN seasons s ON s.id = e.season JOIN items i ON i.id = e.item ORDER BY e.show, e.ord");
        while (q.step()) {
            LibShow* s = showOf(q.i64(1));
            if (!s) continue;
            LibEpisode e;
            e.id = q.i64(0);
            e.season = q.i64(2);
            e.seasonNumber = q.i32(3);
            e.number = q.i32(4);
            e.title = q.str(5);
            e.overview = q.str(6);
            e.still = q.i64(7);
            e.item = q.i64(8);
            e.path = fullPath(q.str(9));
            e.duration = q.i64(10);
            s->episodes.push_back(std::move(e));
        }
    }
    for (auto& s : v) s.episodeCount = (int)s.episodes.size();
    return v;
}

std::vector<LibMovie> Library::movies() {
    std::lock_guard<std::mutex> l(s_mx);
    std::vector<LibMovie> v;
    Q q("SELECT m.id, m.root, m.item, m.title, m.year, m.overview, m.poster, m.backdrop, m.runtime, m.tmdb, m.added, i.path,"
        " i.duration, i.thumb FROM movies m JOIN items i ON i.id = m.item ORDER BY m.sort");
    while (q.step()) {
        LibMovie m;
        m.id = q.i64(0);
        m.root = q.i64(1);
        m.item = q.i64(2);
        m.title = q.str(3);
        m.year = q.i32(4);
        m.overview = q.str(5);
        m.poster = q.i64(6);
        m.backdrop = q.i64(7);
        m.runtime = q.i32(8);
        m.tmdb = q.i32(9);
        m.added = q.i64(10);
        m.path = fullPath(q.str(11));
        m.duration = q.i64(12);
        m.thumb = q.i64(13);
        v.push_back(std::move(m));
    }
    return v;
}

std::vector<LibBook> Library::books() {
    std::lock_guard<std::mutex> l(s_mx);
    std::vector<LibBook> v;
    Q q("SELECT b.id, b.root, b.item, b.title, b.author, b.cover, b.format, b.pages, b.added, i.path, i.size FROM books b"
        " JOIN items i ON i.id = b.item ORDER BY b.sort");
    while (q.step()) {
        LibBook b;
        b.id = q.i64(0);
        b.root = q.i64(1);
        b.item = q.i64(2);
        b.title = q.str(3);
        b.author = q.str(4);
        b.cover = q.i64(5);
        b.format = q.i32(6);
        b.pages = q.i32(7);
        b.added = q.i64(8);
        b.path = fullPath(q.str(9));
        b.size = q.i64(10);
        v.push_back(std::move(b));
    }
    return v;
}

int Library::count(int category) {
    std::lock_guard<std::mutex> l(s_mx);
    Q q("SELECT COUNT(*) FROM items i JOIN roots r ON r.id = i.root WHERE r.category = ? AND i.kind <> 0");
    q.bind(1, (int64_t)category);
    return q.step() ? q.i32(0) : 0;
}
