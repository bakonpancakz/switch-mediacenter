// The Media category: grouping video files into shows (seasons, episodes)
// and movies by their names, then filling in TMDB details.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "model.h"
#include "vname.h"

class Tmdb;

struct VideoFile {
    int item = 0;          // MItem id
    std::string rel;       // inside the mapping
    VideoName name;
    int64_t mtime = 0;     // unix seconds
    std::string frame;     // the file's own thumbnail hash (fallback picture)
};

// Shows and movies from one mapping's videos, ordered the way they're shown.
void groupVideos(int root, std::vector<VideoFile> files, std::vector<MShow>& shows, std::vector<MMovie>& movies);

// TMDB titles, overviews and pictures. status(text) reports what it's doing.
void lookupMedia(Tmdb& tmdb, std::vector<MShow>& shows, std::vector<MMovie>& movies, const std::atomic<bool>& cancel,
                 const std::function<void(const std::string&, int, int)>& status);

// Exposed for the tests.
std::string stripExtra(const std::string& title, int& num);
void placeAbsolute(MShow& s, const std::vector<std::pair<int, int>>& counts);
