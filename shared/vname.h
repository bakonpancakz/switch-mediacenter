// Works out what a video file is from its path inside a Media folder: an
// episode of a show (season/episode, or an absolute number as anime uses) or
// a movie (title and year). No dependencies: the PC builder uses it, and its tests.
#pragma once
#include <string>

struct VideoName {
    enum Type { MOVIE, EPISODE } type = MOVIE;
    std::string show;     // episodes
    int season = -1;      // -1 = unknown (absolute numbering)
    int episode = -1;     // in the season, or the absolute number when season < 0
    std::string title;    // movies: the title; episodes: an episode title if the name has one
    int year = 0;         // 0 = unknown
};

// rel: path relative to the Media folder, '/'-separated, with extension
VideoName parseVideoName(const std::string& rel);

// Lowercase letters and digits only: "Bocchi the Rock!" -> "bocchitherock"
std::string nameKey(const std::string& s);
