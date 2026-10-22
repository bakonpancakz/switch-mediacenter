// Builds the library from the mapped PC folders: scans them, makes what's
// missing (thumbnails, waveforms, display copies, book covers, TMDB details)
// and returns the Model. Everything made is kept in work.db, keyed by the
// file's size and date, so a rebuild only redoes changed files.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "config.h"
#include "media.h"
#include "model.h"
#include "sql.h"

struct BuildProgress {
    std::string stage;  // "Scanning", "Making thumbnails", "Looking up shows and movies"
    std::string item;
    int done = 0, total = 0;
};

// Creates or updates work.db's tables (the builder's, TMDB's and the mirror's).
bool openWorkDb(Sql& db, const std::string& path);

class Builder {
public:
    Builder(Sql& work, const Config& cfg, const FfTools& ff, const std::atomic<bool>& cancel,
            std::function<void(const BuildProgress&)> progress);
    // False when cancelled or a mapped folder is missing (see error()):
    // a missing folder must not read as "everything was deleted".
    bool build(Model& out);
    const std::string& error() const { return m_error; }
    const std::vector<std::string>& warnings() const { return m_warnings; }
    // Shows and movies TMDB had nothing for (with a key set).
    const std::vector<std::string>& unmatched() const { return m_unmatched; }
    int threads = 0;  // analysis threads (0 = from the CPU count)

    static constexpr int ANALYSIS_VERSION = 2;  // bump to redo every file's analysis

private:
    Sql& m_db;
    const Config& m_cfg;
    FfTools m_ff;
    const std::atomic<bool>& m_cancel;
    std::function<void(const BuildProgress&)> m_progress;
    std::string m_error;
    std::vector<std::string> m_warnings, m_unmatched;

    bool scan(const Mapping& m, int64_t stamp);
    bool analyze();
    void report(const std::string& stage, const std::string& item, int done, int total);
};
