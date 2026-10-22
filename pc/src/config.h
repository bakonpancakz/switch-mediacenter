// The PC tool's settings: folder mappings and options, in
// %LOCALAPPDATA%\MediaCenterSync\config.ini.
#pragma once
#include <string>
#include <vector>

struct Mapping {
    std::string id;      // stable, so renaming or moving the folder keeps its history
    std::string folder;  // PC folder, '/'-separated UTF-8
    std::string name;    // on the Switch: <category>/<name>
    int category = 0;    // lib::Category
    std::string switchDir() const;  // ".media/files/Remote"
};

struct Config {
    std::vector<Mapping> mappings;
    std::string tmdbToken;          // a TMDB API key or read access token
    std::string language = "en-US"; // for TMDB titles and overviews
    std::string ffmpegDir;          // "" = find it
    bool autoSync = true;           // sync when the Switch shows up
    bool closeToTray = true;

    bool load(const std::string& file);
    bool save(const std::string& file) const;
};

std::string configPath();
// A name that works as a folder on the Switch ("My: Stuff?" -> "My Stuff").
std::string cleanMappingName(const std::string& s);
std::string newMappingId();
