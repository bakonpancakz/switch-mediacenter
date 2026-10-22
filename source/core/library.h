// The library the PC wrote to the card (system.db, see shared/library.h).
// Every screen asks it what to show: folders, shows, movies, books, and the
// pictures themselves, all from the one file. version() changes when it's
// (re)opened, so screens know to ask again.
//
// One SQLite connection behind one lock: safe from any thread (the screens on
// the main thread, the picture workers on theirs). The app never writes to it;
// it's opened read-write only so SQLite can undo a write the PC didn't finish
// (a card pulled out mid-sync leaves a journal behind).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../../shared/library.h"

struct LibRoot {
    int64_t id = 0, item = 0;
    int category = 0;
    std::string name, dir;
};

struct LibItem {
    int64_t id = 0, parent = 0, root = 0;
    int kind = 0;
    std::string name, path;  // path: the full sdmc:/ path
    int64_t size = 0, mtime = 0, taken = 0, duration = 0;  // duration in ms
    int width = 0, height = 0, children = 0;
    int64_t thumb = 0;  // picture id
    std::vector<uint8_t> wave;    // audio: lib::WAVE_PEAKS loudness values
};

struct LibEpisode {
    int64_t id = 0, season = 0, item = 0, still = 0, duration = 0;
    int seasonNumber = -1, number = -1;
    std::string title, overview, path;
};

struct LibSeason {
    int64_t id = 0;
    int number = -1;  // 0 = specials, -1 = unknown
    std::string title;
};

struct LibShow {
    int64_t id = 0, root = 0, poster = 0, backdrop = 0, added = 0;
    int year = 0, tmdb = 0, episodeCount = 0;
    std::string title, overview;
    std::vector<LibSeason> seasons;
    std::vector<LibEpisode> episodes;  // all of them, in watching order
};

struct LibMovie {
    int64_t id = 0, root = 0, item = 0, poster = 0, backdrop = 0, added = 0, duration = 0, thumb = 0;
    int year = 0, runtime = 0, tmdb = 0;
    std::string title, overview, path;
};

struct LibBook {
    int64_t id = 0, root = 0, item = 0, cover = 0, added = 0, size = 0;
    int format = 0, pages = 0;
    std::string title, author, path;
};

// A locked folder's row (see shared/lockbox.h): what unlocking it needs.
struct LibLock {
    std::string lock;  // names its folder on the card
    std::vector<uint8_t> salt, data;
    int iterations = 0;
    std::string check;
    bool numeric = true;  // the PIN is all digits
    uint64_t nonce = 0;
};

class Library {
public:
    static Library& get();

    void open();    // APP_DIR/library.db, if there is one
    void close();
    bool ready();
    std::string problem();  // why it isn't ready, for the screens to show
    uint32_t version();     // changes whenever the library does
    std::string builtOn();  // "the PC's name"
    int64_t builtAt();      // unix seconds

    std::vector<LibRoot> roots(int category);
    bool item(int64_t id, LibItem& out);
    bool itemByPath(const std::string& path, LibItem& out);  // full sdmc:/ path
    // Folders first, then by name (sort keys made on the PC).
    std::vector<LibItem> children(int64_t parent);
    bool pictureData(int64_t id, std::string& out);  // the JPEG bytes
    bool lockRow(int64_t item, LibLock& out);         // a locked folder's (false for a library without them)
    bool lockedPicture(int64_t id, std::string& data, uint64_t& nonce);  // still encrypted

    std::vector<LibShow> shows();  // with seasons and episodes
    std::vector<LibMovie> movies();
    std::vector<LibBook> books();
    int count(int category);  // files (not folders) in a category

    static std::string fullPath(const std::string& rel);  // "files/a.jpg" -> "sdmc:/switch/mediacenter/files/a.jpg"
};
