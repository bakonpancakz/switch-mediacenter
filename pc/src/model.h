// What the builder makes: the whole library as it should be on the Switch,
// before it's written out as system.db. Pictures are referred to by hash.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct MItem {
    int id = 0, parent = 0, root = 0, kind = 0;
    std::string name, sort, path;
    int64_t size = 0, mtime = 0, taken = 0, duration = 0;
    int width = 0, height = 0, children = 0;
    std::string thumb;  // picture hash
    std::vector<uint8_t> wave;
};

struct MRoot {
    int id = 0, category = 0, item = 0;
    std::string name, dir;
};

struct MEpisode {
    int season = -1, number = -1, item = 0;
    std::string title, overview, still;
    std::string rel;  // inside the mapping (for sorting and the tests)
};

struct MSeason {
    int number = -1;  // 0 = specials, -1 = unknown season
    std::string title;
    std::vector<MEpisode> episodes;
};

struct MShow {
    int root = 0, year = 0, tmdb = 0;
    std::string key, title, overview, poster, backdrop;
    int64_t added = 0;
    std::vector<MSeason> seasons;
};

struct MMovie {
    int root = 0, year = 0, tmdb = 0, runtime = 0, item = 0;
    std::string key;  // the name it was matched by (nameKey of the file's title), kept when TMDB renames it
    std::string title, overview, poster, backdrop;
    int64_t added = 0;
};

struct MBook {
    int root = 0, item = 0, format = 0, pages = 0;
    std::string title, author, cover;
    int64_t added = 0;
};

// A file that has to be on the Switch.
struct MFile {
    std::string remote;  // ".media/media/Shows/x.mkv"
    std::string local;   // PC path, '/'-separated UTF-8
    std::string blob;    // when set, the file is this picture from work.db instead (a photo converted for the Switch)
    uint64_t size = 0;
    int64_t mtime = 0;   // PC last write (FILETIME ticks): a change means "send it again"
};

struct Model {
    std::vector<MRoot> roots;
    std::vector<MItem> items;  // ids are 1-based positions: items[id - 1]
    std::vector<MShow> shows;
    std::vector<MMovie> movies;
    std::vector<MBook> books;
    std::vector<MFile> files;
    std::vector<std::string> pictures() const;  // every picture hash used, once
};
