// The whole PC side: mapped folders -> builder -> sync to a folder standing in
// for the SD card's switch/mediacenter. Then system.db is read the way the
// Switch reads it.
#include <windows.h>

#include <algorithm>

#include "builder.h"
#include "card_sync.h"
#include "fsx.h"
#include "images.h"
#include "library.h"
#include "lockbox.h"
#include "sqlite3.h"
#include "test.h"
#include "util.h"
#include "winmedia.h"

static std::string g_card;  // the card's switch/mediacenter
static std::string g_pc;    // the PC's folders
static FfTools g_ff;

static void copyFx(const char* fx, const std::string& to) {
    std::vector<uint8_t> d;
    readWhole(std::string("test/fixtures/") + fx, d);
    fsx::mkdirs(to.substr(0, to.find_last_of('/')));
    writeWhole(to, d.data(), d.size());
}

static void touch(const std::string& path, const std::string& text) {
    fsx::mkdirs(path.substr(0, path.find_last_of('/')));
    writeWhole(path, text.data(), text.size());
}

static Config makeConfig() {
    Config c;
    Mapping f;
    f.id = "mfiles";
    f.folder = g_pc + "/Pictures";
    f.name = "Pictures";
    f.category = lib::C_FILES;
    Mapping m;
    m.id = "mmedia";
    m.folder = g_pc + "/Videos";
    m.name = "Shows";
    m.category = lib::C_MEDIA;
    Mapping b;
    b.id = "mbooks";
    b.folder = g_pc + "/Reading";
    b.name = "Reading";
    b.category = lib::C_BOOKS;
    c.mappings = {f, m, b};
    return c;
}

struct Db {
    sqlite3* db = nullptr;
    explicit Db(const std::string& p) { sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READONLY, nullptr); }
    ~Db() { sqlite3_close(db); }
    int64_t num(const std::string& sql) {
        sqlite3_stmt* s;
        int64_t v = -1;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
        return v;
    }
    std::string text(const std::string& sql) {
        sqlite3_stmt* s;
        std::string v;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW && sqlite3_column_text(s, 0))
            v = (const char*)sqlite3_column_text(s, 0);
        sqlite3_finalize(s);
        return v;
    }
    std::vector<uint8_t> blob(const std::string& sql) {
        sqlite3_stmt* s;
        std::vector<uint8_t> v;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) {
            auto* p = (const uint8_t*)sqlite3_column_blob(s, 0);
            v.assign(p, p + sqlite3_column_bytes(s, 0));
        }
        sqlite3_finalize(s);
        return v;
    }
    // A picture, as the Switch reads it: a blob in the pictures table.
    bool pictureOk(int64_t id) {
        sqlite3_stmt* s;
        bool ok = false;
        std::string q = "SELECT data FROM pictures WHERE id = " + std::to_string(id);
        if (sqlite3_prepare_v2(db, q.c_str(), -1, &s, nullptr) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW) {
            Image img;
            ok = loadImageMemory(sqlite3_column_blob(s, 0), sqlite3_column_bytes(s, 0), 0, img) && img.w > 0;
        }
        sqlite3_finalize(s);
        return ok;
    }
};

static SyncResult syncOnce(const Config& cfg, Model* modelOut = nullptr, int stopAfterCopies = -1) {
    Sql work;
    openWorkDb(work, g_pc + "/work.db");
    std::atomic<bool> cancel{false};
    Builder b(work, cfg, g_ff, cancel, nullptr);
    Model model;
    if (!b.build(model)) {
        SyncResult r;
        r.error = "build: " + b.error();
        return r;
    }
    if (modelOut) *modelOut = model;
    int copies = 0;
    std::string last;
    return syncToCard(work, model, g_card, "TESTPC", cancel, [&](const SyncProgress& p) {
        // pulling the card: stop after a few files
        if (stopAfterCopies >= 0 && p.action == "Copying" && p.item != last) {
            last = p.item;
            if (copies++ == stopAfterCopies) cancel = true;
        }
    });
}

TEST(first_sync_builds_everything) {
    copyFx("big.png", g_pc + "/Pictures/big.png");
    copyFx("page.jpg", g_pc + "/Pictures/Trip/page.jpg");
    copyFx("photo.tif", g_pc + "/Pictures/Trip/photo.tif");
    copyFx("tone.wav", g_pc + "/Pictures/Trip/Sounds/tone.wav");
    copyFx("black_start.mp4", g_pc + "/Pictures/Trip/clip.mp4");
    touch(g_pc + "/Pictures/notes.exe", "not media");
    touch(g_pc + "/Pictures/broken.mp4", "not really a video");
    copyFx("black_start.mp4", g_pc + "/Videos/Cool Show/Season 1/Cool Show - S01E02.mp4");
    copyFx("black_start.mp4", g_pc + "/Videos/Cool Show/Season 1/Cool Show - S01E01.mp4");
    touch(g_pc + "/Videos/Cool Show/Season 1/Cool Show - S01E01.srt", "1\n00:00:01,000 --> 00:00:02,000\nHi\n");
    copyFx("rotated.mp4", g_pc + "/Videos/Some Movie (2020)/Some.Movie.2020.1080p.mp4");
    copyFx("book.epub", g_pc + "/Reading/book.epub");
    copyFx("comic.cbz", g_pc + "/Reading/Comics/comic.cbz");
    copyFx("page.pdf", g_pc + "/Reading/paper.pdf");
    touch(g_card + "/MediaCenter.nro", "the app");

    Model model;
    SyncResult r = syncOnce(makeConfig(), &model);
    CHECK(r.ok);
    if (!r.error.empty()) printf("  error: %s\n", r.error.c_str());
    CHECK_EQ(r.copied, 12);  // 5 files + 3 videos + 1 subtitle + 3 books; not the .exe
    CHECK_EQ(model.shows.size(), (size_t)1);
    CHECK_EQ(model.movies.size(), (size_t)1);
    CHECK_EQ(model.books.size(), (size_t)3);

    CHECK(fsx::size(g_card + "/.media/files/Pictures/Trip/Sounds/tone.wav") > 0);
    CHECK(fsx::size(g_card + "/.media/media/Shows/Cool Show/Season 1/Cool Show - S01E01.srt") > 0);
    CHECK(fsx::size(g_card + "/.media/files/Pictures/notes.exe") < 0);
    CHECK(fsx::size(g_card + "/.media/files/Pictures/broken.mp4") < 0);  // unreadable: left off
    // a TIFF goes over as the JPEG made from it
    CHECK(fsx::size(g_card + "/.media/files/Pictures/Trip/photo.tif") < 0);
    Image converted;
    CHECK(loadImageFile(g_card + "/.media/files/Pictures/Trip/photo.tif.jpg", 0, converted) && converted.w == 640);
    // the app's folder: the app, the library and .media (.incoming is gone again)
    std::vector<std::string> top;
    fsx::walk(g_card, [&](const std::string& rel, uint64_t, bool) {
        if (rel.find('/') == std::string::npos) top.push_back(rel);
    });
    std::sort(top.begin(), top.end());
    CHECK(top == (std::vector<std::string>{".media", "MediaCenter.nro", "system.db"}));

    Db lib(g_card + "/system.db");
    CHECK_EQ(lib.num("SELECT value FROM meta WHERE key = 'schema'"), (int64_t)lib::SCHEMA);
    CHECK_EQ(lib.num("SELECT COUNT(*) FROM items WHERE name = 'broken.mp4'"), 0ll);
    CHECK_EQ(lib.num("SELECT COUNT(*) FROM roots"), 3ll);
    CHECK_EQ(lib.num("SELECT COUNT(*) FROM synced"), 12ll);
    int64_t remote = lib.num("SELECT item FROM roots WHERE name = 'Pictures'");
    CHECK_EQ(lib.num("SELECT children FROM items WHERE id = " + std::to_string(remote)), 2ll);
    CHECK_EQ(lib.text("SELECT name FROM items WHERE parent = " + std::to_string(remote) + " ORDER BY kind <> 0, sort LIMIT 1"),
             std::string("Trip"));  // folders first
    CHECK(lib.num("SELECT thumb FROM items WHERE name = 'Trip'") > 0);  // a folder shows a picture from inside
    CHECK(lib.pictureOk(lib.num("SELECT thumb FROM items WHERE name = 'big.png'")));
    CHECK(lib.pictureOk(lib.num("SELECT thumb FROM items WHERE name = 'clip.mp4'")));
    CHECK_EQ(lib.text("SELECT path FROM items WHERE name = 'photo.tif'"), std::string(".media/files/Pictures/Trip/photo.tif.jpg"));
    CHECK(lib.pictureOk(lib.num("SELECT thumb FROM items WHERE name = 'photo.tif'")));
    CHECK(lib.num("SELECT duration FROM items WHERE name = 'clip.mp4'") > 7000);
    CHECK_EQ(lib.num("SELECT length(wave) FROM items WHERE name = 'tone.wav'"), (int64_t)lib::WAVE_PEAKS);
    CHECK_EQ(lib.num("SELECT width FROM items WHERE name = 'big.png'"), 1200ll);
    // media
    CHECK_EQ(lib.text("SELECT title FROM shows"), std::string("Cool Show"));
    CHECK_EQ(lib.num("SELECT COUNT(*) FROM episodes"), 2ll);
    CHECK_EQ(lib.num("SELECT number FROM episodes ORDER BY ord LIMIT 1"), 1ll);
    CHECK(lib.pictureOk(lib.num("SELECT still FROM episodes ORDER BY ord LIMIT 1")));  // the episode's own frame without TMDB
    CHECK_EQ(lib.text("SELECT title FROM movies"), std::string("Some Movie"));
    CHECK_EQ(lib.num("SELECT year FROM movies"), 2020ll);
    // books, PDF covers included
    CHECK_EQ(lib.text("SELECT author FROM books WHERE format = 1"), std::string("Dr. Seuss"));
    CHECK(lib.pictureOk(lib.num("SELECT cover FROM books WHERE format = 1")));
    CHECK(lib.pictureOk(lib.num("SELECT cover FROM books WHERE format = 2")));
    CHECK(lib.pictureOk(lib.num("SELECT cover FROM books WHERE format = 3")));
    CHECK_EQ(lib.num("SELECT pages FROM books WHERE format = 3"), 2ll);
}

TEST(second_sync_changes_nothing) {
    int64_t before = fsx::size(g_card + "/system.db");
    WIN32_FILE_ATTRIBUTE_DATA a1, a2;
    GetFileAttributesExW(wide(g_card + "/system.db").c_str(), GetFileExInfoStandard, &a1);
    SyncResult r = syncOnce(makeConfig());
    CHECK(r.ok);
    CHECK(r.nothingChanged);
    CHECK_EQ(r.copied, 0);
    GetFileAttributesExW(wide(g_card + "/system.db").c_str(), GetFileExInfoStandard, &a2);
    CHECK(CompareFileTime(&a1.ftLastWriteTime, &a2.ftLastWriteTime) == 0);  // the card wasn't written at all
    CHECK_EQ(fsx::size(g_card + "/system.db"), before);
}

TEST(changes_are_copied_and_removed_files_deleted) {
    fsx::removeAll(g_pc + "/Pictures/Trip/Sounds");
    copyFx("page.jpg", g_pc + "/Pictures/new.jpg");
    copyFx("big.png", g_pc + "/Pictures/Trip/page.jpg.png");
    Sleep(20);
    touch(g_pc + "/Reading/book.epub", "");
    copyFx("book.epub", g_pc + "/Reading/book.epub");  // same content, new date: sent again
    SyncResult r = syncOnce(makeConfig());
    CHECK(r.ok);
    CHECK_EQ(r.copied, 3);
    CHECK_EQ(r.deleted, 1);
    CHECK(fsx::size(g_card + "/.media/files/Pictures/Trip/Sounds/tone.wav") < 0);
    CHECK(!fsx::isDir(g_card + "/.media/files/Pictures/Trip/Sounds"));
    Db lib(g_card + "/system.db");
    CHECK_EQ(lib.num("SELECT COUNT(*) FROM items WHERE name = 'tone.wav'"), 0ll);
    CHECK_EQ(lib.num("SELECT COUNT(*) FROM synced WHERE path LIKE '%tone.wav'"), 0ll);
    CHECK(lib.pictureOk(lib.num("SELECT thumb FROM items WHERE name = 'new.jpg'")));
}

TEST(lost_library_is_rebuilt_without_copying_again) {
    fsx::removeAll(g_card + "/system.db");
    touch(g_card + "/.media/files/Pictures/stray.txt", "left by someone else");
    touch(g_card + "/.media/files/Unmapped/keep.jpg", "not ours");
    SyncResult r = syncOnce(makeConfig());
    CHECK(r.ok);
    CHECK_EQ(r.copied, 0);  // the files were all there already
    CHECK(fsx::size(g_card + "/.media/files/Pictures/stray.txt") < 0);   // inside a mapped folder: tidied away
    CHECK(fsx::size(g_card + "/.media/files/Unmapped/keep.jpg") > 0);  // outside: left alone
    Db lib(g_card + "/system.db");
    CHECK(lib.pictureOk(lib.num("SELECT thumb FROM items WHERE name = 'big.png'")));
}

TEST(old_library_is_replaced) {
    fsx::removeAll(g_card + "/system.db");
    sqlite3* db = nullptr;
    sqlite3_open((g_card + "/system.db").c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT); INSERT INTO meta VALUES('schema', '1');", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    SyncResult r = syncOnce(makeConfig());
    CHECK(r.ok);
    CHECK_EQ(r.copied, 0);
    Db lib(g_card + "/system.db");
    CHECK_EQ(lib.num("SELECT value FROM meta WHERE key = 'schema'"), (int64_t)lib::SCHEMA);
}

TEST(interrupted_sync_resumes) {
    for (int i = 0; i < 6; i++) copyFx("black_start.mp4", g_pc + "/Pictures/Batch/clip" + std::to_string(i) + ".mp4");
    SyncResult r = syncOnce(makeConfig(), nullptr, 3);  // the card comes out after three files
    CHECK(!r.ok);
    int there = 0;
    for (int i = 0; i < 6; i++) there += fsx::size(g_card + "/.media/files/Pictures/Batch/clip" + std::to_string(i) + ".mp4") > 0;
    CHECK(there >= 2 && there < 6);
    CHECK(!fsx::isDir(g_card + "/.incoming"));
    SyncResult r2 = syncOnce(makeConfig());
    CHECK(r2.ok);
    CHECK_EQ(r2.copied, 6 - there);  // only what didn't arrive
    Db lib(g_card + "/system.db");
    CHECK_EQ(lib.num("SELECT children FROM items WHERE name = 'Batch'"), 6ll);
}

TEST(removed_mapping_is_deleted_from_card) {
    Config cfg = makeConfig();
    cfg.mappings.pop_back();  // no more Reading
    SyncResult r = syncOnce(cfg);
    CHECK(r.ok);
    CHECK(!fsx::isDir(g_card + "/.media/books/Reading"));
    Db lib(g_card + "/system.db");
    CHECK_EQ(lib.num("SELECT COUNT(*) FROM books"), 0ll);
}

TEST(missing_folder_stops_before_deleting) {
    Config cfg = makeConfig();
    cfg.mappings[0].folder = g_pc + "/Unplugged Drive";
    SyncResult r = syncOnce(cfg);
    CHECK(!r.ok);
    CHECK(r.error.find("Can't find the folder") != std::string::npos);
    CHECK(fsx::size(g_card + "/.media/files/Pictures/big.png") > 0);
}

// A file too big for FAT32 goes over as the Switch's split file: a folder with
// the archive attribute holding parts 00, 01... (forced here with small parts,
// as the test card isn't FAT32). Later syncs and a lost library see it as the
// one file it is.
TEST(big_files_are_split_for_fat32) {
    splitForTests(100000, 40000);
    copyFx("tone.wav", g_pc + "/Pictures/Long/tone.wav");  // 176478 bytes: parts of 40000, 40000, 40000, 40000, 16478
    SyncResult r = syncOnce(makeConfig());
    CHECK(r.ok);
    CHECK(r.copied >= 1);
    std::string split = g_card + "/.media/files/Pictures/Long/tone.wav";
    DWORD attr = GetFileAttributesW(wide(split).c_str());
    CHECK(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && (attr & FILE_ATTRIBUTE_ARCHIVE));
    CHECK_EQ(fsx::size(split + "/00"), 40000ll);
    CHECK_EQ(fsx::size(split + "/03"), 40000ll);
    CHECK_EQ(fsx::size(split + "/04"), 16478ll);
    CHECK(fsx::size(split + "/05") < 0);
    CHECK_EQ(fsx::splitFileSize(split), 176478ll);
    std::vector<uint8_t> orig, joined, part;
    readWhole("test/fixtures/tone.wav", orig);
    for (const char* n : {"/00", "/01", "/02", "/03", "/04"}) {
        readWhole(split + n, part);
        joined.insert(joined.end(), part.begin(), part.end());
    }
    CHECK(joined == orig);
    {
        Db lib(g_card + "/system.db");
        CHECK_EQ(lib.num("SELECT size FROM items WHERE path = '.media/files/Pictures/Long/tone.wav'"), 176478ll);
    }

    CHECK(syncOnce(makeConfig()).nothingChanged);  // the next sync leaves it be
    fsx::removeAll(g_card + "/system.db");          // and a rebuilt library counts it as there
    SyncResult again = syncOnce(makeConfig());
    CHECK(again.ok);
    CHECK_EQ(again.copied, 0);
    CHECK_EQ(fsx::splitFileSize(split), 176478ll);
    splitForTests(0, 0);
}

// A small picture nothing else in the test has (so its thumbnail is its own).
static void writeBmp(const std::string& path, int seed) {
    const int w = 64, h = 48, row = w * 3;
    std::vector<uint8_t> f(54 + row * h, 0);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) f[at + i] = (uint8_t)(v >> (8 * i)); };
    f[0] = 'B';
    f[1] = 'M';
    put32(2, (uint32_t)f.size());
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    f[26] = 1;
    f[28] = 24;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* px = &f[54 + y * row + x * 3];
            px[0] = (uint8_t)(x * 4 + seed);
            px[1] = (uint8_t)(y * 5);
            px[2] = (uint8_t)((x ^ y) * 3 + seed * 7);
        }
    fsx::mkdirs(path.substr(0, path.find_last_of('/')));
    writeWhole(path, f.data(), f.size());
}

// What the Switch does to open a locked folder: the PIN against the check tag,
// then its items decrypted. False for a wrong PIN.
static bool unlock(Db& lib, const std::string& pin, lockbox::Keys& keys, std::string& name, std::vector<lockbox::Item>& items) {
    std::vector<uint8_t> salt = lib.blob("SELECT salt FROM locks");
    keys.derive(pin, salt.data(), salt.size(), (int)lib.num("SELECT iterations FROM locks"));
    if (keys.check() != lib.text("SELECT check_tag FROM locks")) return false;
    std::vector<uint8_t> data = lib.blob("SELECT data FROM locks");
    keys.crypt((uint64_t)lib.num("SELECT nonce FROM locks"), 0, data.data(), data.size());
    return lockbox::decodeItems(data, name, items);
}

TEST(locked_folder_goes_encrypted) {
    writeBmp(g_pc + "/Pictures/Private/secret.bmp", 11);
    copyFx("tone.wav", g_pc + "/Pictures/Private/Deeper/sound.wav");
    touch(g_pc + "/Pictures/Private/" + lockbox::PIN_FILE, "1234\r\n");
    SyncResult r = syncOnce(makeConfig());
    CHECK(r.ok);

    // nothing on the card or in its library says what's in there
    bool leak = false;
    fsx::walk(g_card + "/.media", [&](const std::string& rel, uint64_t, bool) {
        for (const char* w : {"Private", "secret", "Deeper", "sound", "mediacenter_pin"})
            if (rel.find(w) != std::string::npos) leak = true;
    });
    CHECK(!leak);
    std::string name;
    std::vector<lockbox::Item> items;
    lockbox::Keys keys;
    std::vector<uint8_t> secretPlain;
    readWhole(g_pc + "/Pictures/Private/secret.bmp", secretPlain);
    {
        Db lib(g_card + "/system.db");
        CHECK_EQ(lib.num("SELECT COUNT(*) FROM items WHERE name IN ('Private', 'secret.bmp', 'Deeper', 'sound.wav')"), 0ll);
        CHECK_EQ(lib.num("SELECT COUNT(*) FROM items WHERE kind = 5"), 1ll);
        CHECK_EQ(lib.text("SELECT name FROM items WHERE kind = 5"), std::string("Locked folder"));
        CHECK_EQ(lib.num("SELECT children + thumb FROM items WHERE kind = 5"), 0ll);
        CHECK_EQ(lib.num("SELECT COUNT(*) FROM locks"), 1ll);

        lockbox::Keys wrong;
        std::vector<lockbox::Item> none;
        CHECK(!unlock(lib, "0000", wrong, name, none));  // a wrong PIN is refused
        CHECK(unlock(lib, "1234", keys, name, items));
        CHECK_EQ(name, std::string("Private"));
        const lockbox::Item *secret = nullptr, *deeper = nullptr, *sound = nullptr;
        for (auto& it : items) {
            if (it.name == "secret.bmp") secret = &it;
            if (it.name == "Deeper") deeper = &it;
            if (it.name == "sound.wav") sound = &it;
        }
        CHECK(secret && deeper && sound);
        if (secret && deeper && sound) {
            CHECK(secret->parent == 0 && deeper->parent == 0 && sound->parent == deeper->id);
            CHECK_EQ(deeper->children, 1);
            // the file: encrypted on the card, the original once decrypted
            std::vector<uint8_t> onCard;
            CHECK(readWhole(g_card + "/" + secret->path, onCard));
            CHECK(onCard != secretPlain);
            keys.crypt(secret->nonce, 0, onCard.data(), onCard.size());
            CHECK(onCard == secretPlain);
            // its thumbnail: only in locked_pictures, and only encrypted
            std::vector<uint8_t> thumb = lib.blob("SELECT data FROM locked_pictures WHERE id = " + std::to_string(secret->thumb));
            CHECK(thumb.size() > 2);
            keys.crypt((uint64_t)lib.num("SELECT nonce FROM locked_pictures WHERE id = " + std::to_string(secret->thumb)), 0,
                       thumb.data(), thumb.size());
            CHECK(thumb.size() > 2 && thumb[0] == 0xFF && thumb[1] == 0xD8);  // a JPEG
            Image img;
            CHECK(loadImageMemory(thumb.data(), thumb.size(), 0, img) && img.w > 0);
            int64_t clash = 0;
            sqlite3_stmt* st;
            sqlite3_prepare_v2(lib.db, "SELECT COUNT(*) FROM pictures WHERE data = ?", -1, &st, nullptr);
            sqlite3_bind_blob(st, 1, thumb.data(), (int)thumb.size(), SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) clash = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
            CHECK_EQ(clash, 0ll);  // not among the plain pictures (so not any folder's either)
        }
    }
    CHECK(syncOnce(makeConfig()).nothingChanged);

    // a lost library is rebuilt without sending them again
    fsx::removeAll(g_card + "/system.db");
    CHECK_EQ(syncOnce(makeConfig()).copied, 0);

    // a new PIN: everything again under new names, the old ones gone
    std::string oldPath;
    for (auto& it : items)
        if (it.name == "secret.bmp") oldPath = it.path;
    touch(g_pc + "/Pictures/Private/" + lockbox::PIN_FILE, "5678");
    r = syncOnce(makeConfig());
    CHECK(r.ok);
    CHECK(fsx::size(g_card + "/" + oldPath) < 0);
    {
        Db lib(g_card + "/system.db");
        lockbox::Keys k;
        std::vector<lockbox::Item> now;
        CHECK(!unlock(lib, "1234", k, name, now));
        CHECK(unlock(lib, "5678", k, name, now));
        for (auto& it : now)
            if (it.name == "secret.bmp") {
                std::vector<uint8_t> onCard;
                CHECK(readWhole(g_card + "/" + it.path, onCard));
                k.crypt(it.nonce, 0, onCard.data(), onCard.size());
                CHECK(onCard == secretPlain);
            }
    }

    // unlocked again (the PIN file gone): plain, and the encrypted copies cleared
    fsx::removeAll(g_pc + "/Pictures/Private/" + std::string(lockbox::PIN_FILE));
    r = syncOnce(makeConfig());
    CHECK(r.ok);
    CHECK(!fsx::isDir(g_card + "/.media/files/Pictures/" + lib::LOCKED_DIR));
    CHECK(fsx::size(g_card + "/.media/files/Pictures/Private/secret.bmp") > 0);
    fsx::removeAll(g_pc + "/Pictures/Private");
    CHECK(syncOnce(makeConfig()).ok);
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::string t = utf8(tmp);
    for (auto& ch : t)
        if (ch == '\\') ch = '/';
    g_card = t + "mc_e2e_card/switch/mediacenter";
    g_pc = t + "mc_e2e_pc";
    fsx::removeAll(t + "mc_e2e_card");
    fsx::removeAll(g_pc);
    fsx::mkdirs(g_card);
    fsx::mkdirs(g_pc);
    imagesThreadInit();
    winMediaThreadInit();
    g_ff = findFfmpeg("");
    int rc = runTests();
    winMediaThreadExit();
    imagesThreadExit();
    fsx::removeAll(t + "mc_e2e_card");
    fsx::removeAll(g_pc);
    return rc;
}
