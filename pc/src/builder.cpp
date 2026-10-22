#include "builder.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "books.h"
#include "fsx.h"
#include "images.h"
#include "library.h"
#include "medialib.h"
#include "winmedia.h"
#include "tmdb.h"
#include "util.h"

static const char* WORK_SCHEMA =
    // every file in the mapped folders; done = the analysis version that made the rest
    "CREATE TABLE IF NOT EXISTS files(mapping TEXT NOT NULL, rel TEXT NOT NULL, size INTEGER NOT NULL, mtime INTEGER NOT NULL,"
    " kind INTEGER NOT NULL, seen INTEGER NOT NULL DEFAULT 0, done INTEGER NOT NULL DEFAULT 0, failed INTEGER NOT NULL DEFAULT 0,"
    " width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0, duration INTEGER NOT NULL DEFAULT 0,"
    " taken INTEGER NOT NULL DEFAULT 0, thumb TEXT, view TEXT, wave BLOB, title TEXT, author TEXT, pages INTEGER NOT NULL DEFAULT 0,"
    " PRIMARY KEY(mapping, rel));"
    // pictures made here (thumbnails, covers, converted photos) and TMDB's, by hash
    "CREATE TABLE IF NOT EXISTS blobs(hash TEXT PRIMARY KEY, data BLOB NOT NULL, width INTEGER NOT NULL DEFAULT 0,"
    " height INTEGER NOT NULL DEFAULT 0);"
    // each locked folder's salt, kept so its names on the card stay the same from sync to sync
    "CREATE TABLE IF NOT EXISTS lock_salts(mapping TEXT NOT NULL, rel TEXT NOT NULL, salt BLOB NOT NULL,"
    " PRIMARY KEY(mapping, rel));"
    // the mirror of the USB days, no longer used (the card's system.db knows what's on it)
    "DROP TABLE IF EXISTS sw_files; DROP TABLE IF EXISTS sw_packs; DROP TABLE IF EXISTS sw_blobs; DROP TABLE IF EXISTS sw_meta;";

bool openWorkDb(Sql& db, const std::string& path) {
    if (!db.open(path)) return false;
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA synchronous=NORMAL");
    return db.exec(WORK_SCHEMA) && db.exec(TMDB_SCHEMA);
}

Builder::Builder(Sql& work, const Config& cfg, const FfTools& ff, const std::atomic<bool>& cancel,
                 std::function<void(const BuildProgress&)> progress)
    : m_db(work), m_cfg(cfg), m_ff(ff), m_cancel(cancel), m_progress(std::move(progress)) {}

void Builder::report(const std::string& stage, const std::string& item, int done, int total) {
    if (!m_progress) return;
    BuildProgress p;
    p.stage = stage;
    p.item = item;
    p.done = done;
    p.total = total;
    m_progress(p);
}

// Besides lib::Kind, what a scanned file can be (files.kind in work.db):
constexpr int SIDECAR = -1;  // copied next to its video, not listed (subtitles)
constexpr int SKIPPED = -2;  // not synced at all

static int kindFor(int category, const std::string& ext) {
    using namespace lib;
    if (isSidecarExt(ext)) return category == C_BOOKS ? SKIPPED : SIDECAR;
    switch (category) {
    case C_FILES:
        if (isImageExt(ext)) return K_IMAGE;
        if (isVideoExt(ext)) return K_VIDEO;
        if (isAudioExt(ext)) return K_AUDIO;
        if (bookFormat(ext)) return K_BOOK;
        return SKIPPED;
    case C_MEDIA: return isVideoExt(ext) ? K_VIDEO : SKIPPED;
    case C_BOOKS: return bookFormat(ext) ? K_BOOK : SKIPPED;
    }
    return SKIPPED;
}

static int64_t unixFromFiletime(int64_t ft) { return ft > 0 ? ft / 10000000 - 11644473600LL : 0; }

// A name the card's file system (FAT32 or exFAT, same rules as Windows for
// what a name may hold) keeps as it is: anything Windows allowed, except parts
// ending in a dot or a space, which FAT trims (two names would become one).
static bool storableOnCard(const std::string& rel) {
    size_t start = 0;
    while (start <= rel.size()) {
        size_t end = rel.find('/', start);
        if (end == std::string::npos) end = rel.size();
        if (end == start || rel[end - 1] == '.' || rel[end - 1] == ' ') return false;
        start = end + 1;
    }
    return rel.size() < 700;
}

// ---------------------------------------------------------------- scanning

bool Builder::scan(const Mapping& m, int64_t stamp) {
    std::string root = m.folder;
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    if (!fsx::isDir(root)) {
        m_error = "Can't find the folder for " + m.name + ": " + root;
        return false;
    }
    Stmt up = m_db.prepare(
        "INSERT INTO files(mapping, rel, size, mtime, kind, seen) VALUES(?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(mapping, rel) DO UPDATE SET seen = excluded.seen,"
        " done = CASE WHEN size <> excluded.size OR mtime <> excluded.mtime OR kind <> excluded.kind THEN 0 ELSE done END,"
        " size = excluded.size, mtime = excluded.mtime, kind = excluded.kind");
    Tx tx(m_db);
    int found = 0;
    std::deque<std::string> todo = {""};
    std::wstring base = winPath(root, true);  // long paths work too
    while (!todo.empty() && !m_cancel) {
        std::string rel = todo.back();
        todo.pop_back();
        std::wstring dir = base;
        if (!rel.empty()) dir += L"\\" + winPath(rel);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            if (!rel.empty()) m_warnings.push_back("Couldn't read " + root + "/" + rel);
            continue;
        }
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || n[0] == L'.') continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            std::string name = utf8(n);
            std::string r = rel.empty() ? name : rel + "/" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) todo.push_back(r);  // no junction loops
                continue;
            }
            int kind = kindFor(m.category, extOf(name));
            if (kind == SKIPPED) continue;
            if (!storableOnCard(r)) {
                m_warnings.push_back("Skipped (name the Switch can't store): " + root + "/" + r);
                continue;
            }
            int64_t size = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            int64_t mtime = ((int64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            up.bind(1, m.id).bind(2, r).bind(3, size).bind(4, mtime).bind(5, kind).bind(6, stamp).run();
            if (++found % 500 == 0) report("Scanning " + m.name, r, found, 0);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (m_cancel) return false;
    m_db.prepare("DELETE FROM files WHERE mapping = ? AND seen <> ?").bind(1, m.id).bind(2, stamp).run();
    tx.commit();
    logLine("scanned " + m.name + ": " + std::to_string(found) + " files");
    return true;
}

// ---------------------------------------------------------------- analysis

namespace {

struct Job {
    std::string mapping, rel, path;
    int kind = 0, category = 0;
    int64_t size = 0, mtime = 0;
};

struct Pic {
    std::vector<uint8_t> data;
    int w = 0, h = 0;
    std::string hash;
};

struct Result {
    Job job;
    bool failed = false;
    int width = 0, height = 0;
    int64_t duration = 0, taken = 0;
    Pic thumb, view;
    std::vector<uint8_t> wave;
    std::string title, author;
    int pages = 0;
};

bool toJpeg(const Image& img, float q, Pic& out) {
    if (img.bgra.empty() || !encodeJpeg(img, q, out.data)) return false;
    out.w = img.w;
    out.h = img.h;
    out.hash = hashHex(out.data.data(), out.data.size());
    return true;
}

// A picture worth showing: not a black frame or one flat colour.
bool lively(const Image& img) {
    if (img.bgra.empty()) return false;
    double mean, sd;
    imageStats(img, mean, sd);
    return mean > 24 && sd > 8;
}

// Each kind is done with what Windows has first (WIC, the Shell's thumbnail
// handlers such as Icaros, Media Foundation, the PDF renderer), ffmpeg after.
void analyzeOne(const FfTools& ff, Result& r, const std::atomic<bool>& cancel) {
    const Job& j = r.job;
    std::string ext = extOf(j.rel);
    switch (j.kind) {
    case lib::K_IMAGE: {
        Image img;
        ImageInfo info;
        bool ok = loadImageFile(j.path, lib::THUMB, img, &info);
        if (!ok && shellThumbnail(j.path, lib::THUMB, img)) ok = true;
        if (!ok && ff.ok() && ffmpegImage(ff, j.path, lib::THUMB, img, &cancel)) ok = true;
        if (!ok) { r.failed = true; return; }
        if (!info.width) {
            MediaInfo mi;
            if (ff.ok() && probeMedia(ff, j.path, mi, &cancel)) { info.width = mi.width; info.height = mi.height; }
        }
        r.width = info.width;
        r.height = info.height;
        r.taken = info.taken;
        toJpeg(img, 0.85f, r.thumb);
        // a picture the Switch can't open goes over as a JPEG instead
        if (!lib::switchDecodesImage(ext)) {
            Image big;
            if (loadImageFile(j.path, lib::VIEW, big) || (ff.ok() && ffmpegImage(ff, j.path, lib::VIEW, big, &cancel))) toJpeg(big, 0.9f, r.view);
        }
        break;
    }
    case lib::K_VIDEO: {
        MediaInfo mi;
        if (!mfProbe(j.path, mi) && !(ff.ok() && probeMedia(ff, j.path, mi, &cancel))) {
            r.failed = true;
            return;
        }
        r.width = mi.width;
        r.height = mi.height;
        r.duration = mi.durationMs;
        r.taken = mi.taken;
        // the Shell's thumbnail, unless it's a black frame and ffmpeg can find a better one
        Image frame;
        bool shell = shellThumbnail(j.path, lib::THUMB, frame);
        if (!(shell && lively(frame)) && ff.ok()) {
            Image other;
            if (videoFrame(ff, j.path, mi, lib::THUMB, other, &cancel)) frame = std::move(other);
        }
        toJpeg(frame, 0.85f, r.thumb);
        break;
    }
    case lib::K_AUDIO: {
        int64_t dur = 0;
        if (mfAudioPeaks(j.path, lib::WAVE_PEAKS, r.wave, &dur, &cancel)) r.duration = dur;
        else {
            MediaInfo mi;
            if (!ff.ok() || !probeMedia(ff, j.path, mi, &cancel)) { r.failed = true; return; }
            r.duration = mi.durationMs;
            if (!audioPeaks(ff, j.path, lib::WAVE_PEAKS, r.wave, &cancel)) r.wave.clear();
        }
        // album art, where the file has some (the Shell gives none otherwise)
        Image art;
        if (shellThumbnail(j.path, lib::THUMB, art) && lively(art)) toJpeg(art, 0.85f, r.thumb);
        break;
    }
    case lib::K_BOOK: {
        BookInfo b;
        if (!readBook(j.path, lib::bookFormat(ext), lib::COVER_H, b)) r.failed = true;
        r.title = b.title;
        r.author = b.author;
        r.pages = b.pages;
        if (b.cover.bgra.empty()) {  // the Shell may know the format (comic archives, with Icaros)
            Image c;
            if (shellThumbnail(j.path, lib::COVER_H, c)) b.cover = c;
        }
        toJpeg(b.cover, 0.88f, r.thumb);
        break;
    }
    }
}

}  // namespace

bool Builder::analyze() {
    std::map<std::string, const Mapping*> maps;
    for (auto& m : m_cfg.mappings) maps[m.id] = &m;
    std::vector<Job> jobs;
    {
        Stmt q = m_db.prepare("SELECT mapping, rel, kind, size, mtime FROM files WHERE kind >= 0 AND done <> ?");
        q.bind(1, ANALYSIS_VERSION);
        while (q.step()) {
            auto it = maps.find(q.str(0));
            if (it == maps.end()) continue;
            Job j;
            j.mapping = q.str(0);
            j.rel = q.str(1);
            j.kind = q.i32(2);
            j.size = q.i64(3);
            j.mtime = q.i64(4);
            j.category = it->second->category;
            std::string root = it->second->folder;
            while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
            j.path = root + "/" + j.rel;
            jobs.push_back(std::move(j));
        }
    }
    if (jobs.empty()) return true;
    // pictures first (fast and plentiful), then the ffmpeg work
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return (a.kind == lib::K_IMAGE) > (b.kind == lib::K_IMAGE); });
    logLine("analysing " + std::to_string(jobs.size()) + " files");

    int n = threads > 0 ? threads : (int)std::clamp(std::thread::hardware_concurrency() / 2u, 1u, 6u);
    std::atomic<size_t> next{0};
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Result> done;
    int running = n;
    std::vector<std::thread> pool;
    for (int i = 0; i < n; i++)
        pool.emplace_back([&] {
            imagesThreadInit();
            winMediaThreadInit();
            for (;;) {
                size_t k = next++;
                if (k >= jobs.size() || m_cancel) break;
                Result r;
                r.job = jobs[k];
                analyzeOne(m_ff, r, m_cancel);
                std::lock_guard<std::mutex> l(mx);
                done.push_back(std::move(r));
                cv.notify_all();
            }
            winMediaThreadExit();
            imagesThreadExit();
            std::lock_guard<std::mutex> l(mx);
            running--;
            cv.notify_all();
        });

    Stmt blob = m_db.prepare("INSERT OR IGNORE INTO blobs(hash, data, width, height) VALUES(?, ?, ?, ?)");
    // only if the file is still what was analysed (it may have changed meanwhile)
    Stmt upd = m_db.prepare(
        "UPDATE files SET done = ?, failed = ?, width = ?, height = ?, duration = ?, taken = ?, thumb = ?, view = ?, wave = ?,"
        " title = ?, author = ?, pages = ? WHERE mapping = ? AND rel = ? AND size = ? AND mtime = ?");
    size_t finished = 0, failed = 0;
    uint64_t lastReport = 0;
    for (;;) {
        std::vector<Result> batch;
        {
            std::unique_lock<std::mutex> l(mx);
            cv.wait_for(l, std::chrono::milliseconds(1000), [&] { return done.size() >= 16 || running == 0; });
            while (!done.empty()) {
                batch.push_back(std::move(done.front()));
                done.pop_front();
            }
            if (batch.empty() && running == 0) break;
        }
        Tx tx(m_db);
        for (Result& r : batch) {
            for (Pic* p : {&r.thumb, &r.view})
                if (!p->hash.empty()) blob.bind(1, p->hash).bind(2, p->data).bind(3, p->w).bind(4, p->h).run();
            upd.bind(1, ANALYSIS_VERSION).bind(2, r.failed ? 1 : 0).bind(3, r.width).bind(4, r.height).bind(5, r.duration).bind(6, r.taken);
            if (r.thumb.hash.empty()) upd.bindNull(7); else upd.bind(7, r.thumb.hash);
            if (r.view.hash.empty()) upd.bindNull(8); else upd.bind(8, r.view.hash);
            upd.bind(9, r.wave).bind(10, r.title).bind(11, r.author).bind(12, r.pages);
            upd.bind(13, r.job.mapping).bind(14, r.job.rel).bind(15, r.job.size).bind(16, r.job.mtime).run();
            if (r.failed) {
                failed++;
                logLine("couldn't read " + r.job.path);
            }
        }
        tx.commit();
        finished += batch.size();
        if (!batch.empty() && (monoMs() - lastReport > 250 || finished == jobs.size())) {
            lastReport = monoMs();
            report("Making thumbnails", baseOf(batch.back().job.rel), (int)finished, (int)jobs.size());
        }
    }
    for (auto& t : pool) t.join();
    if (failed) m_warnings.push_back(std::to_string(failed) + " file(s) couldn't be read (see the log)");
    return !m_cancel;
}

// ---------------------------------------------------------------- the model

std::vector<std::string> Model::pictures() const {
    std::set<std::string> s;
    auto add = [&](const std::string& h) { if (!h.empty()) s.insert(h); };
    for (auto& i : items)
        if (!i.lock) add(i.thumb);  // (a locked folder's go encrypted, on their own)
    for (auto& sh : shows) {
        add(sh.poster);
        add(sh.backdrop);
        for (auto& se : sh.seasons)
            for (auto& e : se.episodes) add(e.still);
    }
    for (auto& m : movies) { add(m.poster); add(m.backdrop); }
    for (auto& b : books) add(b.cover);
    return std::vector<std::string>(s.begin(), s.end());
}

bool Builder::build(Model& out) {
    out = Model();
    m_error.clear();
    m_warnings.clear();
    m_unmatched.clear();
    std::set<std::string> dirs;
    for (auto& m : m_cfg.mappings) {
        if (!dirs.insert(lower(m.switchDir())).second) {
            m_error = "Two folders are both called " + m.name + " in " + lib::categoryLabel(m.category) + ": rename one";
            return false;
        }
    }

    // 1. scan. Each scan marks what it saw with a new number; rows without it are gone.
    int64_t stamp = 1;
    {
        Stmt q = m_db.prepare("SELECT COALESCE(MAX(seen), 0) + 1 FROM files");
        if (q.step()) stamp = q.i64(0);
    }
    for (auto& m : m_cfg.mappings) {
        report("Scanning", m.name, 0, 0);
        if (!scan(m, stamp)) return false;
    }
    {
        // forget folders that aren't mapped any more
        std::string ids;
        for (auto& m : m_cfg.mappings) ids += (ids.empty() ? "'" : ",'") + m.id + "'";
        m_db.exec("DELETE FROM files WHERE mapping NOT IN (" + (ids.empty() ? std::string("''") : ids) + ")");
    }

    // 2. thumbnails and the rest
    if (!m_ff.ok()) m_warnings.push_back("ffmpeg.exe wasn't found: videos and audio get no thumbnails or lengths");
    if (!analyze()) return false;

    // 3. the tree of items, mapping by mapping
    Tmdb tmdb(m_db, m_cfg.tmdbToken, m_cfg.language, &m_cancel);
    for (size_t mi = 0; mi < m_cfg.mappings.size(); mi++) {
        const Mapping& m = m_cfg.mappings[mi];
        std::string root = m.folder;
        while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
        MRoot r;
        r.id = (int)mi + 1;
        r.category = m.category;
        r.name = m.name;
        r.dir = m.switchDir();
        auto newItem = [&](int parent, int kind, const std::string& name, const std::string& path) -> MItem& {
            MItem it;
            it.id = (int)out.items.size() + 1;
            it.parent = parent;
            it.root = r.id;
            it.kind = kind;
            it.name = name;
            it.sort = lib::sortKey(name);
            it.path = path;
            out.items.push_back(it);
            return out.items.back();
        };
        r.item = newItem(0, lib::K_DIR, m.name, r.dir).id;
        out.roots.push_back(r);

        std::map<std::string, int> dirIds = {{"", r.item}};
        std::function<int(const std::string&)> dirFor = [&](const std::string& rel) -> int {
            auto it = dirIds.find(rel);
            if (it != dirIds.end()) return it->second;
            size_t s = rel.find_last_of('/');
            int parent = dirFor(s == std::string::npos ? "" : rel.substr(0, s));
            int id = newItem(parent, lib::K_DIR, s == std::string::npos ? rel : rel.substr(s + 1), r.dir + "/" + cardSafe(rel)).id;
            dirIds[rel] = id;
            return id;
        };

        // Locked folders: the outermost folder holding a PIN file locks all in it.
        // (Files mappings only, and not the mapping's own top folder.)
        std::map<std::string, int> lockOfDir;  // folder -> its lock (1-based in out.locks), 0 = none
        std::function<int(const std::string&)> lockFor = [&](const std::string& relDir) -> int {
            if (m.category != lib::C_FILES || relDir.empty()) return 0;
            auto known = lockOfDir.find(relDir);
            if (known != lockOfDir.end()) return known->second;
            size_t s = relDir.find_last_of('/');
            int lock = s == std::string::npos ? 0 : lockFor(relDir.substr(0, s));
            if (!lock) {
                std::vector<uint8_t> pinData;
                if (readWhole(root + "/" + relDir + "/" + lockbox::PIN_FILE, pinData)) {
                    std::string pin = trim(std::string(pinData.begin(), pinData.end()));
                    if (pin.compare(0, 3, "\xEF\xBB\xBF") == 0) pin = trim(pin.substr(3));  // a UTF-8 BOM (Notepad)
                    if (pin.empty()) logLine("the PIN file in " + m.name + "/" + relDir + " is empty: that folder isn't locked");
                    else {
                        MLock L;
                        Stmt get = m_db.prepare("SELECT salt FROM lock_salts WHERE mapping = ? AND rel = ?");
                        get.bind(1, m.id).bind(2, relDir);
                        if (get.step()) L.salt = get.blob(0);
                        if (L.salt.size() != 16) {
                            L.salt.assign(16, 0);
                            BCryptGenRandom(nullptr, L.salt.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
                            m_db.prepare("INSERT OR REPLACE INTO lock_salts(mapping, rel, salt) VALUES(?, ?, ?)")
                                .bind(1, m.id).bind(2, relDir).bind(3, L.salt).run();
                        }
                        L.keys.derive(pin, L.salt.data(), L.salt.size(), lockbox::ITERATIONS);
                        L.numeric = pin.find_first_not_of("0123456789") == std::string::npos;
                        L.id = L.keys.tag("folder", 16);
                        L.name = s == std::string::npos ? relDir : relDir.substr(s + 1);
                        MItem& folder = out.items[dirFor(relDir) - 1];
                        folder.kind = lib::K_LOCKED;
                        folder.name = "Locked folder";  // the real one is encrypted
                        folder.sort = lib::sortKey(folder.name);  // (not by the real name: its place would hint at it)
                        folder.path = r.dir + "/" + lib::LOCKED_DIR + "/" + L.id;
                        L.item = folder.id;
                        out.locks.push_back(std::move(L));
                        lock = (int)out.locks.size();
                    }
                }
            }
            lockOfDir[relDir] = lock;
            return lock;
        };

        std::vector<VideoFile> videos;
        int skipped = 0;
        Stmt blobSize = m_db.prepare("SELECT length(data) FROM blobs WHERE hash = ?");
        Stmt q = m_db.prepare(
            "SELECT rel, kind, size, mtime, width, height, duration, taken, thumb, view, wave, title, author, pages, failed FROM files"
            " WHERE mapping = ?");
        q.bind(1, m.id);
        while (q.step()) {
            std::string rel = q.str(0);
            int kind = q.i32(1);
            // a file nothing here could read (damaged, or a format the PC can't open either)
            // would only be a dead tile on the Switch: it stays off the card
            if (kind != SIDECAR && q.i32(14)) {
                logLine("left off the Switch (couldn't be read): " + root + "/" + rel);
                skipped++;
                continue;
            }
            MFile f;
            f.remote = r.dir + "/" + cardSafe(rel);
            f.local = root + "/" + rel;
            f.size = (uint64_t)q.i64(2);
            f.mtime = q.i64(3);
            // a picture the Switch can't open goes over as the JPEG made from it ("x.heic.jpg")
            std::string view = q.str(9);
            if (kind == lib::K_IMAGE && !view.empty() && !lib::switchDecodesImage(extOf(rel))) {
                blobSize.bind(1, view);
                if (blobSize.step()) {
                    f.remote += ".jpg";
                    f.blob = view;
                    f.size = (uint64_t)blobSize.i64(0);
                }
                blobSize.reset();
            }
            // in a locked folder: encrypted, under a name that says nothing (the extension stays)
            {
                size_t slash = rel.find_last_of('/');
                f.lock = lockFor(slash == std::string::npos ? "" : rel.substr(0, slash));
                if (f.lock) {
                    const MLock& L = out.locks[f.lock - 1];
                    std::string ext = extOf(f.remote);
                    f.nonce = L.keys.nonceFor(rel + "|" + std::to_string(f.size) + "|" + std::to_string(f.mtime));
                    f.remote = r.dir + "/" + lib::LOCKED_DIR + "/" + L.id + "/" + L.keys.tag(rel, 24) + (ext.empty() ? "" : "." + ext);
                }
            }
            out.files.push_back(f);
            if (kind == SIDECAR) continue;  // copied, not listed
            size_t s = rel.find_last_of('/');
            int parent = dirFor(s == std::string::npos ? "" : rel.substr(0, s));
            MItem& it = newItem(parent, kind, s == std::string::npos ? rel : rel.substr(s + 1), f.remote);
            it.size = q.i64(2);
            it.mtime = unixFromFiletime(q.i64(3));
            it.width = q.i32(4);
            it.height = q.i32(5);
            it.duration = q.i64(6);
            it.taken = q.i64(7);
            it.thumb = q.str(8);
            it.wave = q.blob(10);
            if (m.category == lib::C_MEDIA && kind == lib::K_VIDEO) {
                VideoFile v;
                v.item = it.id;
                v.rel = rel;
                v.name = parseVideoName(rel);
                v.mtime = it.mtime;
                v.frame = it.thumb;
                videos.push_back(v);
            } else if (m.category == lib::C_BOOKS || kind == lib::K_BOOK) {
                MBook b;
                b.root = r.id;
                b.item = it.id;
                b.format = lib::bookFormat(extOf(rel));
                b.title = q.str(11).empty() ? titleFromFileName(stemOf(rel)) : q.str(11);
                b.author = q.str(12);
                b.pages = q.i32(13);
                b.cover = it.thumb;
                b.added = it.mtime;
                if (m.category == lib::C_BOOKS) out.books.push_back(b);
            }
        }
        if (m.category == lib::C_MEDIA) groupVideos(r.id, videos, out.shows, out.movies);
        if (skipped) m_warnings.push_back(std::to_string(skipped) + " file(s) in " + m.name + " couldn't be read, so they're left off the Switch");
    }

    // everything inside a locked folder belongs to it (parents come before their children)
    for (size_t li = 0; li < out.locks.size(); li++) out.items[out.locks[li].item - 1].lock = 0;
    std::vector<int> lockAt(out.items.size() + 1, 0);
    for (size_t li = 0; li < out.locks.size(); li++) lockAt[out.locks[li].item] = (int)li + 1;
    for (auto& it : out.items) {
        if (!it.parent) continue;
        const MItem& p = out.items[it.parent - 1];
        int l = p.kind == lib::K_LOCKED ? lockAt[p.id] : p.lock;
        if (l) it.lock = l;
    }

    // folders: how many entries, and a picture from inside them
    std::vector<std::vector<int>> kids(out.items.size() + 1);
    for (auto& it : out.items)
        if (it.parent) kids[it.parent].push_back(it.id);
    std::function<std::string(int)> folderThumb = [&](int id) -> std::string {
        MItem& d = out.items[id - 1];
        auto& k = kids[id];
        d.children = (int)k.size();
        std::sort(k.begin(), k.end(), [&](int a, int b) {
            const MItem &x = out.items[a - 1], &y = out.items[b - 1];
            if ((x.kind != lib::K_DIR) != (y.kind != lib::K_DIR)) return x.kind == lib::K_DIR;
            return x.sort < y.sort;
        });
        std::string fromFile, fromDir;
        for (int c : k) {
            MItem& ci = out.items[c - 1];
            if (ci.kind == lib::K_DIR) {
                std::string t = folderThumb(c);
                if (fromDir.empty()) fromDir = t;
            } else if (fromFile.empty() && !ci.thumb.empty() && (ci.kind == lib::K_IMAGE || ci.kind == lib::K_VIDEO))
                fromFile = ci.thumb;
        }
        d.thumb = fromFile.empty() ? fromDir : fromFile;
        if (d.kind == lib::K_LOCKED) d.thumb.clear();  // a padlock: nothing from inside shows outside
        return d.thumb;
    };
    for (auto& r : out.roots) folderThumb(r.item);

    // 4. TMDB
    if (!out.shows.empty() || !out.movies.empty()) {
        if (m_cfg.tmdbToken.empty()) m_warnings.push_back("No TMDB key set: shows and movies get no posters or descriptions");
        lookupMedia(tmdb, out.shows, out.movies, m_cancel, [&](const std::string& what, int d, int t) {
            report("Looking up shows and movies", what, d, t);
        });
        if (!tmdb.lastError().empty()) m_warnings.push_back(tmdb.lastError());
        if (tmdb.enabled()) {
            for (auto& s : out.shows)
                if (!s.tmdb) m_unmatched.push_back(s.title);
            for (auto& mv : out.movies)
                if (!mv.tmdb) m_unmatched.push_back(mv.title);
            for (auto& u : m_unmatched) m_warnings.push_back("Not found on TMDB: " + u + " (Fix shows/movies can pick the right one)");
        }
    }
    if (m_cancel) return false;
    std::sort(out.shows.begin(), out.shows.end(), [](const MShow& a, const MShow& b) { return lib::sortKey(a.title) < lib::sortKey(b.title); });
    std::sort(out.movies.begin(), out.movies.end(), [](const MMovie& a, const MMovie& b) { return lib::sortKey(a.title) < lib::sortKey(b.title); });
    std::sort(out.books.begin(), out.books.end(), [](const MBook& a, const MBook& b) { return lib::sortKey(a.title) < lib::sortKey(b.title); });

    // pictures nothing uses any more
    m_db.exec(
        "DELETE FROM blobs WHERE hash NOT IN (SELECT thumb FROM files WHERE thumb IS NOT NULL UNION SELECT view FROM files WHERE view IS NOT"
        " NULL UNION SELECT hash FROM art)");
    report("Ready", "", 0, 0);
    return true;
}
