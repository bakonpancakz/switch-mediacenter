// The Switch's library reader (source/core/library.cpp) on the Switch's own
// SQLite file layer (source/core/sqlite_vfs.cpp, SQLITE_OS_OTHER), against a
// system.db made with the shared schema: every query the tabs make, the
// read-only "immutable" open, and the schema check.
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include "app.h"
#include "core/library.h"
#include "sqlite3.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        printf("FAIL sql: %s\n  in: %s\n", err, sql.c_str());
        g_fail++;
        sqlite3_free(err);
    }
}

static void copyFile(const char* from, const char* to) {
    FILE* a = fopen(from, "rb");
    FILE* b = fopen(to, "wb");
    char buf[65536];
    size_t n;
    while (a && b && (n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static long fileSize(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void makeLibrary(int schema) {
    remove(APP_DIR "/system.db");
    sqlite3* db = nullptr;
    sqlite3_open(APP_DIR "/system.db", &db);
    exec(db, lib::SCHEMA_SQL);
    exec(db, "INSERT INTO meta VALUES('schema', '" + std::to_string(schema) + "'), ('pc', 'DESKTOP'), ('built', '1700000000')");
    exec(db,
         "INSERT INTO roots VALUES(1, 0, 'Pictures', '.media/files/Pictures', 1), (2, 1, 'Shows', '.media/media/Shows', 10), (3, 2, 'Reading', '.media/books/Reading', 20)");
    // Files: Pictures/{Trip/{b.jpg}, a.jpg, song.mp3} ; sort keys as the PC makes them
    exec(db, "INSERT INTO items(id, parent, root, kind, name, sort, path, children, thumb) VALUES"
             "(1, 0, 1, 0, 'Pictures', 'remote', '.media/files/Pictures', 3, 1),"
             "(2, 1, 1, 0, 'Trip', 'trip', '.media/files/Pictures/Trip', 1, 1)");
    exec(db, "INSERT INTO items(id, parent, root, kind, name, sort, path, size, taken, width, height, thumb) VALUES"
             "(3, 2, 1, 1, 'b.jpg', 'b.jpg', '.media/files/Pictures/Trip/b.jpg', 1000, 1600000000, 4000, 3000, 1),"
             "(4, 1, 1, 1, 'a 10.jpg', 'a 0000000010.jpg', '.media/files/Pictures/a 10.jpg', 2000, 0, 800, 600, 2),"
             "(5, 1, 1, 1, 'a 9.jpg', 'a 0000000009.jpg', '.media/files/Pictures/a 9.jpg', 2000, 0, 800, 600, 2)");
    exec(db, "INSERT INTO items(id, parent, root, kind, name, sort, path, duration, wave) VALUES"
             "(6, 1, 1, 3, 'song.mp3', 'song.mp3', '.media/files/Pictures/song.mp3', 300, x'00FF80')");
    // Media: a show with two seasons, and a movie
    exec(db, "INSERT INTO items(id, parent, root, kind, name, sort, path, duration, thumb) VALUES"
             "(10, 0, 2, 0, 'Shows', 'shows', '.media/media/Shows', 0, 0),"
             "(11, 10, 2, 2, 'ep1.mkv', 'ep1.mkv', '.media/media/Shows/ep1.mkv', 1440000, 3),"
             "(12, 10, 2, 2, 'ep2.mkv', 'ep2.mkv', '.media/media/Shows/ep2.mkv', 1500000, 3),"
             "(13, 10, 2, 2, 'sp.mkv', 'sp.mkv', '.media/media/Shows/sp.mkv', 600000, 3),"
             "(14, 10, 2, 2, 'movie.mp4', 'movie.mp4', '.media/media/Shows/movie.mp4', 6000000, 3)");
    exec(db, "INSERT INTO shows(id, root, title, sort, year, overview, poster, backdrop, tmdb, added) VALUES"
             "(1, 2, 'Cool Show', 'cool show', 2020, 'About things', 2, 3, 99, 5)");
    exec(db, "INSERT INTO seasons(id, show, number, title, ord) VALUES(1, 1, 1, 'Season 1', 0), (2, 1, 0, 'Specials', 1)");
    exec(db, "INSERT INTO episodes(show, season, number, title, overview, still, item, ord) VALUES"
             "(1, 1, 1, 'Pilot', 'It starts', 1, 11, 0), (1, 1, 2, 'Second', '', 0, 12, 1), (1, 2, 1, 'Extra', '', 0, 13, 2)");
    exec(db, "INSERT INTO movies(root, title, sort, year, overview, poster, backdrop, runtime, tmdb, item, added) VALUES"
             "(2, 'A Movie', 'a movie', 2019, 'Plot', 1, 2, 101, 7, 14, 3)");
    // Books
    exec(db, "INSERT INTO items(id, parent, root, kind, name, sort, path, size, thumb) VALUES"
             "(20, 0, 3, 0, 'Reading', 'reading', '.media/books/Reading', 0, 0),"
             "(21, 20, 3, 4, 'b.epub', 'b.epub', '.media/books/Reading/b.epub', 5000, 2)");
    exec(db, "INSERT INTO books(root, item, title, author, sort, cover, format, pages, added) VALUES(3, 21, 'The Book', 'Someone', 'the book', 2, 1, 0, 1)");
    exec(db, "INSERT INTO pictures(id, hash, data, width, height) VALUES(1, 'a', x'FFD8FF01', 360, 270), (2, 'b', x'FFD8FF02', 342, 513),"
             " (3, 'c', x'FFD8FF03', 300, 169)");
    exec(db, "INSERT INTO synced VALUES('.media/files/Pictures/a 9.jpg', 2000, 1)");
    sqlite3_close(db);
}

int main() {
#ifdef _WIN32
    mkdir(APP_DIR);
#else
    mkdir(APP_DIR, 0777);
#endif
    makeLibrary(lib::SCHEMA);
    Library& L = Library::get();
    L.open();
    CHECK(L.ready());
    CHECK(L.problem().empty());
    CHECK(L.builtAt() == 1700000000);
    CHECK(L.builtOn() == "DESKTOP");

    auto roots = L.roots(lib::C_FILES);
    CHECK(roots.size() == 1 && roots[0].item == 1 && roots[0].dir == ".media/files/Pictures");
    auto kids = L.children(1);
    CHECK(kids.size() == 4);
    if (kids.size() == 4) {
        CHECK(kids[0].name == "Trip");      // folders first
        CHECK(kids[1].name == "a 9.jpg");   // then natural order: 9 before 10
        CHECK(kids[2].name == "a 10.jpg");
        CHECK(kids[3].name == "song.mp3");
        CHECK(kids[3].wave.size() == 3 && kids[3].wave[1] == 0xFF);
        CHECK(kids[1].path == std::string(APP_DIR) + "/.media/files/Pictures/a 9.jpg");
    }
    LibItem it;
    CHECK(L.itemByPath(std::string(APP_DIR) + "/.media/files/Pictures/Trip/b.jpg", it) && it.id == 3 && it.width == 4000 && it.taken == 1600000000);
    CHECK(!L.itemByPath("sdmc:/elsewhere/b.jpg", it));
    CHECK(L.item(2, it) && it.children == 1 && it.kind == lib::K_DIR);
    std::string data;
    CHECK(L.pictureData(2, data) && data == std::string("\xFF\xD8\xFF\x02", 4));
    CHECK(!L.pictureData(0, data));
    CHECK(!L.pictureData(99, data));

    auto shows = L.shows();
    CHECK(shows.size() == 1);
    if (!shows.empty()) {
        const LibShow& s = shows[0];
        CHECK(s.title == "Cool Show" && s.year == 2020 && s.poster == 2 && s.episodeCount == 3);
        CHECK(s.seasons.size() == 2 && s.seasons[0].number == 1 && s.seasons[1].title == "Specials");
        CHECK(s.episodes.size() == 3);
        if (s.episodes.size() == 3) {
            CHECK(s.episodes[0].title == "Pilot" && s.episodes[0].seasonNumber == 1 && s.episodes[0].duration == 1440000);
            CHECK(s.episodes[0].path == std::string(APP_DIR) + "/.media/media/Shows/ep1.mkv");
            CHECK(s.episodes[2].seasonNumber == 0 && s.episodes[2].season == 2);
        }
    }
    auto movies = L.movies();
    CHECK(movies.size() == 1 && movies[0].title == "A Movie" && movies[0].runtime == 101 && movies[0].duration == 6000000 &&
          movies[0].thumb == 3);
    auto books = L.books();
    CHECK(books.size() == 1 && books[0].author == "Someone" && books[0].size == 5000 && books[0].format == lib::B_EPUB);
    CHECK(L.count(lib::C_FILES) == 4);
    CHECK(L.count(lib::C_MEDIA) == 4);

    // a sync swaps the file: close, replace, reopen
    uint32_t v = L.version();
    L.close();
    CHECK(!L.ready());
    CHECK(L.children(1).empty());  // asking while it's closed is harmless
    makeLibrary(lib::SCHEMA + 1);
    L.open();
    CHECK(!L.ready());
    CHECK(L.problem().find("newer") != std::string::npos);
    CHECK(L.version() != v);
    remove(APP_DIR "/system.db");
    L.open();
    CHECK(!L.ready() && L.problem() == "No library yet");

    // The PC was writing when the card came out: system.db is half-changed and
    // its journal is still there. The Switch's open must roll it back.
    makeLibrary(lib::SCHEMA);
    {
        sqlite3* pc = nullptr;
        sqlite3_open(APP_DIR "/system.db", &pc);
        exec(pc, "PRAGMA cache_size=1");  // so the changes reach the file before a commit
        exec(pc, "BEGIN");
        exec(pc, "UPDATE items SET name = 'half-written'");
        for (int i = 0; i < 200; i++)
            exec(pc, "INSERT INTO pictures(hash, data) VALUES('x" + std::to_string(i) + "', zeroblob(20000))");
        // "pull the card": keep the files as they are right now
        copyFile(APP_DIR "/system.db", APP_DIR "/pulled.db");
        copyFile(APP_DIR "/system.db-journal", APP_DIR "/pulled.db-journal");
        CHECK(fileExists(APP_DIR "/pulled.db-journal"));
        exec(pc, "ROLLBACK");
        sqlite3_close(pc);
    }
    remove(APP_DIR "/system.db");
    rename(APP_DIR "/pulled.db", APP_DIR "/system.db");
    rename(APP_DIR "/pulled.db-journal", APP_DIR "/system.db-journal");
    L.open();
    CHECK(L.ready());
    CHECK(L.item(4, it) && it.name == "a 10.jpg");  // as it was before the PC's write
    CHECK(L.children(1).size() == 4);
    CHECK(!fileExists(APP_DIR "/system.db-journal") || fileSize(APP_DIR "/system.db-journal") == 0);

    printf(g_fail ? "LIBRARY: %d failed\n" : "LIBRARY: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
