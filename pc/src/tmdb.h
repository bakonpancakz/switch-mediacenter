// TMDB (themoviedb.org) lookups for the Media category: matching names to
// shows and movies, their details and pictures. Answers and pictures are
// cached in work.db, so a sync only asks TMDB about new things.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "sql.h"

struct cJSON;

struct TmdbHit {
    int id = 0;
    std::string name, overview;
    int year = 0;
};

class Tmdb {
public:
    Tmdb(Sql& db, std::string token, std::string language, const std::atomic<bool>* cancel = nullptr);
    bool enabled() const { return !m_token.empty(); }
    std::string lastError() const { return m_error; }

    // The TMDB id a name matches (0 = none): a hand-picked match, a remembered
    // one, or a search. Misses are retried after three days.
    int match(bool tv, const std::string& key, const std::string& name, int year);
    // Pins a match (0 = "not on TMDB").
    void setMatch(bool tv, const std::string& key, int tmdbId);
    std::vector<TmdbHit> search(bool tv, const std::string& query, int year = 0);

    // A cached answer, refreshed after maxAgeDays (the stale one when offline).
    // The caller frees it with cJSON_Delete.
    cJSON* get(const std::string& cacheKey, const std::string& pathQuery, int maxAgeDays);
    // A TMDB picture ("/abc.jpg") at a size ("w342"): its hash in the blobs table, "" if unavailable.
    std::string image(const std::string& remotePath, const char* size);

    static std::string jstr(const cJSON* o, const char* k);
    static int jint(const cJSON* o, const char* k);
    static int yearOf(const std::string& date);

private:
    Sql& m_db;
    std::string m_token, m_lang, m_error;
    const std::atomic<bool>* m_cancel;
    std::mutex m_mx;
    bool fetch(const std::string& pathQuery, std::string& out);
};

// Schema for the tables Tmdb uses (part of work.db).
extern const char* TMDB_SCHEMA;
