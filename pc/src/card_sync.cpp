#include "card_sync.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>

#include "fsx.h"
#include "library.h"
#include "util.h"

// ---------------------------------------------------------------- finding the card

std::vector<Card> findCards() {
    std::vector<Card> out;
    UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);  // no "insert a disk" boxes for empty readers
    wchar_t drives[256];
    DWORD n = GetLogicalDriveStringsW(255, drives);
    for (wchar_t* d = drives; n && *d; d += wcslen(d) + 1) {
        UINT type = GetDriveTypeW(d);
        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;  // not network drives: slow, and never the card
        std::wstring dir = std::wstring(d) + L"switch\\mediacenter";
        DWORD a = GetFileAttributesW(dir.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        Card c;
        c.root = forwardSlashes(utf8(dir));
        wchar_t label[MAX_PATH + 1] = L"";
        GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        c.label = utf8(std::wstring(d, 2)) + (label[0] ? " (" + utf8(label) + ")" : "");
        ULARGE_INTEGER fr = {}, total = {};
        if (GetDiskFreeSpaceExW(d, &fr, &total, nullptr)) {
            c.freeBytes = fr.QuadPart;
            c.totalBytes = total.QuadPart;
        }
        c.hasApp = fsx::size(c.root + "/MediaCenter.nro") > 0;
        out.push_back(c);
    }
    SetErrorMode(oldMode);
    std::stable_sort(out.begin(), out.end(), [](const Card& a, const Card& b) { return a.hasApp > b.hasApp; });
    return out;
}

// ---------------------------------------------------------------- the card's database

namespace {

struct Synced {
    std::string path;
    int64_t size = 0, mtime = 0;
};

bool openCardDb(Sql& db, const std::string& path, bool& fresh, std::string& error) {
    fresh = false;
    if (fsx::size(path) > 0) {
        if (db.open(path)) {
            Stmt q = db.prepare("SELECT value FROM meta WHERE key = 'schema'");
            if (q.ok() && q.step() && atoi(q.str(0).c_str()) == lib::SCHEMA) {
                db.exec("PRAGMA journal_mode=DELETE");
                db.exec("PRAGMA synchronous=FULL");
                db.exec("PRAGMA cache_size=-32768");
                return true;
            }
        }
        // an older version's library, or not one at all: start a new one
        logLine("the card's system.db is from another version (or damaged): making a new one");
        db.close();
        fsx::removeAll(path + "-journal");
        if (!fsx::removeAll(path)) {
            error = "Couldn't replace " + path;
            return false;
        }
    }
    if (!db.open(path)) {
        error = "Couldn't create " + path;
        return false;
    }
    fresh = true;
    db.exec("PRAGMA page_size=8192");  // pictures are mostly 10-40 KB
    db.exec("PRAGMA auto_vacuum=INCREMENTAL");
    db.exec("PRAGMA journal_mode=DELETE");
    db.exec("PRAGMA synchronous=FULL");
    db.exec("PRAGMA cache_size=-32768");
    if (!db.exec(lib::SCHEMA_SQL)) {
        error = "Couldn't set up system.db: " + db.error();
        return false;
    }
    db.prepare("INSERT INTO meta(key, value) VALUES('schema', ?)").bind(1, std::to_string(lib::SCHEMA)).run();
    return true;
}

std::string parentOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "" : p.substr(0, s);
}

// Everything system.db is made of except the files themselves: when it and the
// files are the same as last time, the card isn't written at all.
std::string modelDigest(const Model& m) {
    std::string s;
    auto add = [&](const std::string& v) { s += v; s += '\x1f'; };
    auto num = [&](int64_t v) { add(std::to_string(v)); };
    for (auto& r : m.roots) { num(r.id); num(r.category); add(r.name); }
    for (auto& i : m.items) {
        num(i.id); num(i.parent); num(i.kind); add(i.name); add(i.path); num(i.size); num(i.mtime); num(i.taken);
        num(i.width); num(i.height); num(i.duration); add(i.thumb);
        s.append(i.wave.begin(), i.wave.end());
    }
    for (auto& sh : m.shows) {
        add(sh.title); num(sh.year); add(sh.overview); add(sh.poster); add(sh.backdrop);
        for (auto& se : sh.seasons) {
            num(se.number); add(se.title);
            for (auto& e : se.episodes) { num(e.number); num(e.item); add(e.title); add(e.overview); add(e.still); }
        }
    }
    for (auto& mv : m.movies) { add(mv.title); num(mv.year); add(mv.overview); add(mv.poster); add(mv.backdrop); num(mv.runtime); num(mv.item); }
    for (auto& b : m.books) { add(b.title); add(b.author); add(b.cover); num(b.item); num(b.pages); }
    return hashHex(s.data(), s.size());
}

struct CopyCtx {
    const std::atomic<bool>* cancel;
    std::function<void(uint64_t)> progress;
};

DWORD CALLBACK copyProgress(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID ctx) {
    auto* c = (CopyCtx*)ctx;
    if (*c->cancel) return PROGRESS_CANCEL;
    if (c->progress) c->progress((uint64_t)done.QuadPart);
    return PROGRESS_CONTINUE;
}

// Writes the library tables (not `synced`): the items whose files are on the
// card, and pictures by id. Inside the caller's transaction.
bool writeLibrary(Sql& card, Sql& work, const Model& model, const std::set<std::string>& present, const std::string& pcName,
                  std::string& error) {
    // pictures: keep the ones still used, add the new ones, drop the rest
    std::vector<std::string> needed = model.pictures();
    card.exec("CREATE TEMP TABLE IF NOT EXISTS needed(hash TEXT PRIMARY KEY) WITHOUT ROWID");
    card.exec("DELETE FROM needed");
    {
        Stmt ins = card.prepare("INSERT OR IGNORE INTO needed(hash) VALUES(?)");
        for (auto& h : needed) ins.bind(1, h).run();
    }
    card.exec("DELETE FROM pictures WHERE hash NOT IN (SELECT hash FROM needed)");
    std::map<std::string, int64_t> picId;
    {
        Stmt q = card.prepare("SELECT hash, id FROM pictures");
        while (q.step()) picId[q.str(0)] = q.i64(1);
    }
    Stmt blob = work.prepare("SELECT data, width, height FROM blobs WHERE hash = ?");
    Stmt addPic = card.prepare("INSERT INTO pictures(hash, data, width, height) VALUES(?, ?, ?, ?)");
    int added = 0;
    for (auto& h : needed) {
        if (picId.count(h)) continue;
        blob.bind(1, h);
        if (blob.step()) {
            addPic.bind(1, h).bind(2, blob.blob(0)).bind(3, blob.i32(1)).bind(4, blob.i32(2)).run();
            picId[h] = card.lastId();
            added++;
        }
        blob.reset();
    }
    if (added) logLine(std::to_string(added) + " new pictures");
    auto pic = [&](const std::string& h) -> int64_t {
        auto it = picId.find(h);
        return it == picId.end() ? 0 : it->second;
    };

    for (const char* t : {"roots", "items", "shows", "seasons", "episodes", "movies", "books"})
        if (!card.exec(std::string("DELETE FROM ") + t)) { error = "Couldn't clear " + std::string(t); return false; }
    Stmt m = card.prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    m.bind(1, "built").bind(2, std::to_string(unixNow())).run();
    m.bind(1, "pc").bind(2, pcName).run();

    // ids for what's on the card; folders always
    std::vector<int> newId(model.items.size() + 1, 0);
    int next = 0;
    for (const MItem& it : model.items)
        if (it.kind == lib::K_DIR || present.count(it.path)) newId[it.id] = ++next;
    std::vector<int> children(model.items.size() + 1, 0);
    for (const MItem& it : model.items)
        if (newId[it.id] && it.parent) children[it.parent]++;

    Stmt r = card.prepare("INSERT INTO roots(id, category, name, dir, item) VALUES(?, ?, ?, ?, ?)");
    for (auto& ro : model.roots) r.bind(1, ro.id).bind(2, ro.category).bind(3, ro.name).bind(4, ro.dir).bind(5, newId[ro.item]).run();
    Stmt ii = card.prepare(
        "INSERT INTO items(id, parent, root, kind, name, sort, path, size, mtime, taken, width, height, duration, thumb, wave, children)"
        " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    for (const MItem& it : model.items) {
        if (!newId[it.id]) continue;
        ii.bind(1, newId[it.id]).bind(2, it.parent ? newId[it.parent] : 0).bind(3, it.root).bind(4, it.kind).bind(5, it.name).bind(6, it.sort);
        ii.bind(7, it.path).bind(8, it.size).bind(9, it.mtime).bind(10, it.taken).bind(11, it.width).bind(12, it.height).bind(13, it.duration);
        ii.bind(14, pic(it.thumb)).bind(15, it.wave).bind(16, it.kind == lib::K_DIR ? children[it.id] : 0).run();
    }
    Stmt sh = card.prepare("INSERT INTO shows(root, title, sort, year, overview, poster, backdrop, tmdb, added) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
    Stmt se = card.prepare("INSERT INTO seasons(show, number, title, ord) VALUES(?, ?, ?, ?)");
    Stmt ep = card.prepare("INSERT INTO episodes(show, season, number, title, overview, still, item, ord) VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
    for (const MShow& s : model.shows) {
        int count = 0;
        for (auto& sea : s.seasons)
            for (auto& e : sea.episodes) count += newId[e.item] != 0;
        if (!count) continue;
        sh.bind(1, s.root).bind(2, s.title).bind(3, lib::sortKey(s.title)).bind(4, s.year).bind(5, s.overview).bind(6, pic(s.poster));
        sh.bind(7, pic(s.backdrop)).bind(8, s.tmdb).bind(9, s.added).run();
        int64_t showId = card.lastId();
        int ord = 0, sord = 0;
        for (auto& sea : s.seasons) {
            bool any = false;
            for (auto& e : sea.episodes) any |= newId[e.item] != 0;
            if (!any) continue;
            se.bind(1, showId).bind(2, sea.number).bind(3, sea.title).bind(4, sord++).run();
            int64_t seasonId = card.lastId();
            for (auto& e : sea.episodes) {
                if (!newId[e.item]) continue;
                ep.bind(1, showId).bind(2, seasonId).bind(3, e.number).bind(4, e.title).bind(5, e.overview).bind(6, pic(e.still));
                ep.bind(7, newId[e.item]).bind(8, ord++).run();
            }
        }
    }
    Stmt mo = card.prepare(
        "INSERT INTO movies(root, title, sort, year, overview, poster, backdrop, runtime, tmdb, item, added) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    for (const MMovie& mv : model.movies) {
        if (!newId[mv.item]) continue;
        mo.bind(1, mv.root).bind(2, mv.title).bind(3, lib::sortKey(mv.title)).bind(4, mv.year).bind(5, mv.overview).bind(6, pic(mv.poster));
        mo.bind(7, pic(mv.backdrop)).bind(8, mv.runtime).bind(9, mv.tmdb).bind(10, newId[mv.item]).bind(11, mv.added).run();
    }
    Stmt bo = card.prepare("INSERT INTO books(root, item, title, author, sort, cover, format, pages, added) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
    for (const MBook& b : model.books) {
        if (!newId[b.item]) continue;
        bo.bind(1, b.root).bind(2, newId[b.item]).bind(3, b.title).bind(4, b.author).bind(5, lib::sortKey(b.title)).bind(6, pic(b.cover));
        bo.bind(7, b.format).bind(8, b.pages).bind(9, b.added).run();
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------- a sync

SyncResult syncToCard(Sql& work, const Model& model, const std::string& root, const std::string& pcName, const std::atomic<bool>& cancel,
                      const std::function<void(const SyncProgress&)>& progress) {
    SyncResult res;
    SyncProgress pr;
    auto report = [&](const std::string& action, const std::string& item) {
        pr.action = action;
        pr.item = item;
        if (progress) progress(pr);
    };
    auto fail = [&](const std::string& why) {
        res.error = why;
        logLine("sync failed: " + why);
        return res;
    };
    if (!fsx::isDir(root)) return fail("The card isn't there any more (" + root + ")");

    report("Opening the card's library", "");
    Sql card;
    bool fresh = false;
    std::string err;
    if (!openCardDb(card, root + "/" + lib::DB_FILE, fresh, err)) return fail(err);

    std::map<std::string, const MFile*> target;  // by lowercase path: FAT doesn't tell cases apart
    for (auto& f : model.files) target[lower(f.remote)] = &f;
    if (fresh) {
        // a new library on a card that may already have the files: what's there
        // with the right size counts as copied, and in the mapped folders,
        // anything else is cleared away
        report("Looking at what's on the card", "");
        Tx tx(card);
        Stmt ins = card.prepare("INSERT OR REPLACE INTO synced(path, size, mtime) VALUES(?, ?, ?)");
        int kept = 0, cleared = 0;
        for (auto& r : model.roots) {
            fsx::walk(root + "/" + r.dir, [&](const std::string& rel, uint64_t size, bool dir) {
                if (dir) return;
                std::string path = r.dir + "/" + rel;
                auto t = target.find(lower(path));
                if (t != target.end() && t->second->remote == path && t->second->size == size) {
                    ins.bind(1, path).bind(2, (int64_t)size).bind(3, t->second->mtime).run();
                    kept++;
                } else if (fsx::removeAll(root + "/" + path)) cleared++;
            });
        }
        tx.commit();
        logLine("card: " + std::to_string(kept) + " files already there, " + std::to_string(cleared) + " cleared");
    }

    // what's there now, by lowercase path
    std::map<std::string, Synced> synced;
    {
        Stmt q = card.prepare("SELECT path, size, mtime FROM synced");
        while (q.step()) synced[lower(q.str(0))] = {q.str(0), q.i64(1), q.i64(2)};
    }
    std::vector<std::string> preDeletes, deletes;
    std::vector<const MFile*> copies;
    uint64_t copyBytes = 0, freed = 0;
    for (auto& [key, f] : target) {
        auto s = synced.find(key);
        if (s == synced.end()) copies.push_back(f);
        else if (s->second.path != f->remote) {  // the same file under another case: gone first
            preDeletes.push_back(s->second.path);
            copies.push_back(f);
        } else if ((uint64_t)s->second.size != f->size || s->second.mtime != f->mtime) copies.push_back(f);
    }
    for (auto& [key, s] : synced)
        if (!target.count(key)) {
            deletes.push_back(s.path);
            freed += (uint64_t)s.size;
        }
    for (auto* c : copies) copyBytes += c->size;

    std::string digest = modelDigest(model);
    std::string oldDigest;
    {
        Stmt q = card.prepare("SELECT value FROM meta WHERE key = 'digest'");
        if (q.step()) oldDigest = q.str(0);
    }
    if (copies.empty() && deletes.empty() && preDeletes.empty() && digest == oldDigest) {
        res.ok = true;
        res.nothingChanged = true;
        return res;
    }

    // space: if it's tight, the deletes come first (the library briefly lists what's gone)
    ULARGE_INTEGER fr = {};
    GetDiskFreeSpaceExW(winPath(root).c_str(), &fr, nullptr, nullptr);
    uint64_t margin = 64ull << 20;
    bool deletesFirst = copyBytes + margin > fr.QuadPart;
    if (deletesFirst && copyBytes + margin > fr.QuadPart + freed)
        return fail("Not enough space on the card: this sync needs " + formatBytes((double)copyBytes) + ", " +
                    formatBytes((double)fr.QuadPart) + " is free");

    pr.steps = (int)(preDeletes.size() + copies.size() + deletes.size() + 1);
    pr.bytes = copyBytes;
    Stmt addSynced = card.prepare("INSERT OR REPLACE INTO synced(path, size, mtime) VALUES(?, ?, ?)");
    Stmt dropSynced = card.prepare("DELETE FROM synced WHERE path = ?");

    auto deleteAll = [&](const std::vector<std::string>& list, int& count) -> bool {
        Tx tx(card);
        for (auto& d : list) {
            if (cancel) break;
            report("Deleting", d);
            if (!fsx::removeAll(root + "/" + d)) {
                logLine("couldn't delete " + d);
                continue;
            }
            // and the folders it leaves empty, up to (not including) .media/files and the like
            for (std::string dir = parentOf(d); dir.find('/') != dir.rfind('/'); dir = parentOf(dir))
                if (!fsx::removeEmptyDir(root + "/" + dir)) break;
            dropSynced.bind(1, d).run();
            count++;
            pr.stepsDone++;
        }
        return tx.commit();
    };

    int ignored = 0;
    if (!deleteAll(preDeletes, ignored)) return fail("Couldn't update system.db: " + card.error());
    if (deletesFirst && !deleteAll(deletes, res.deleted)) return fail("Couldn't update system.db: " + card.error());

    // 1. the files, through .incoming
    std::string incoming = root + "/" + lib::INCOMING_DIR;
    fsx::removeAll(incoming);
    if (!fsx::mkdirs(incoming)) return fail("Couldn't write to the card (is it read-only?)");
    Stmt blob = work.prepare("SELECT data FROM blobs WHERE hash = ?");
    int n = 0;
    uint64_t lastCommit = monoMs();
    card.exec("BEGIN");
    for (const MFile* f : copies) {
        if (cancel) break;
        report("Copying", f->remote);
        std::string tmp = incoming + "/" + std::to_string(++n);
        bool ok;
        DWORD code = 0;
        if (!f->blob.empty()) {
            blob.bind(1, f->blob);
            std::vector<uint8_t> data = blob.step() ? blob.blob(0) : std::vector<uint8_t>();
            blob.reset();
            ok = !data.empty() && writeWhole(tmp, data.data(), data.size());
            if (!ok) code = GetLastError();
        } else {
            uint64_t base = pr.bytesDone;
            uint64_t lastReport = 0;
            CopyCtx ctx{&cancel, [&](uint64_t done) {
                            pr.bytesDone = base + done;
                            if (progress && monoMs() - lastReport > 150) { lastReport = monoMs(); progress(pr); }
                        }};
            ok = CopyFileExW(winPath(f->local).c_str(), winPath(tmp).c_str(), copyProgress, &ctx, nullptr, 0) != 0;
            if (!ok) code = GetLastError();
            pr.bytesDone = base;
        }
        if (ok) {
            std::string dest = root + "/" + f->remote;
            fsx::mkdirs(parentOf(dest));
            ok = MoveFileExW(winPath(tmp).c_str(), winPath(dest).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!ok) code = GetLastError();
        }
        pr.bytesDone += f->size;
        pr.stepsDone++;
        if (!ok) {
            fsx::removeAll(tmp);
            if (cancel || code == ERROR_REQUEST_ABORTED) break;
            if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
                card.exec("COMMIT");
                return fail("The card is full");
            }
            if (fsx::isDir(root)) {  // the PC file, not the card: skip it
                res.failed++;
                logLine("couldn't copy " + f->local + " (error " + std::to_string(code) + ")");
                continue;
            }
            card.exec("ROLLBACK");
            return fail("The card was removed");
        }
        addSynced.bind(1, f->remote).bind(2, (int64_t)f->size).bind(3, f->mtime).run();
        res.copied++;
        res.bytes += f->size;
        // what's copied is recorded every couple of seconds, so a pulled card loses little
        if (monoMs() - lastCommit > 2000) {
            card.exec("COMMIT");
            card.exec("BEGIN");
            lastCommit = monoMs();
        }
    }
    if (!card.exec("COMMIT")) return fail("Couldn't update system.db: " + card.error());
    fsx::removeAll(incoming);
    if (cancel) return fail("Stopped");

    // 2. the library, from what's really on the card now
    report("Writing the library", "");
    std::set<std::string> present;
    {
        Stmt q = card.prepare("SELECT path FROM synced");
        while (q.step()) present.insert(q.str(0));
    }
    {
        Tx tx(card);
        if (!writeLibrary(card, work, model, present, pcName, err)) return fail(err);
        card.prepare("INSERT OR REPLACE INTO meta(key, value) VALUES('digest', ?)").bind(1, res.failed ? "" : digest).run();
        if (!tx.commit()) return fail("Couldn't write system.db: " + card.error());
    }
    pr.stepsDone++;

    // 3. what the library no longer has
    if (!deletesFirst && !deleteAll(deletes, res.deleted)) return fail("Couldn't update system.db: " + card.error());

    // give back the space of dropped pictures once there's a fair amount
    {
        Stmt q = card.prepare("SELECT (SELECT freelist_count FROM pragma_freelist_count), (SELECT page_count FROM pragma_page_count)");
        if (q.step() && q.i64(0) * 5 > q.i64(1)) card.exec("PRAGMA incremental_vacuum");
    }
    card.close();
    res.ok = true;
    report("Done", "");
    return res;
}
