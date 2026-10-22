#include "tmdb.h"

#include <windows.h>

#include "cJSON.h"
#include "http.h"
#include "images.h"
#include "util.h"
#include "vname.h"

const char* TMDB_SCHEMA =
    "CREATE TABLE IF NOT EXISTS tmdb(key TEXT PRIMARY KEY, json TEXT NOT NULL, fetched INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS art(url TEXT PRIMARY KEY, hash TEXT NOT NULL);"
    // what a name was matched to; pinned = picked by hand
    "CREATE TABLE IF NOT EXISTS matches(kind TEXT NOT NULL, key TEXT NOT NULL, year INTEGER NOT NULL, tmdb INTEGER NOT NULL,"
    " pinned INTEGER NOT NULL DEFAULT 0, at INTEGER NOT NULL, PRIMARY KEY(kind, key, year));";

static const char* IMG = "https://image.tmdb.org/t/p/";

Tmdb::Tmdb(Sql& db, std::string token, std::string language, const std::atomic<bool>* cancel)
    : m_db(db), m_token(trim(token)), m_lang(language.empty() ? "en-US" : language), m_cancel(cancel) {}

std::string Tmdb::jstr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
int Tmdb::jint(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}
int Tmdb::yearOf(const std::string& date) { return date.size() >= 4 ? atoi(date.substr(0, 4).c_str()) : 0; }

// GET /3<pathQuery>. v4 read-access tokens ("eyJ...") go in a header, v3 keys in the query.
bool Tmdb::fetch(const std::string& pathQuery, std::string& out) {
    if (m_token.empty()) return false;
    std::string url = "https://api.themoviedb.org/3" + pathQuery;
    url += pathQuery.find('?') == std::string::npos ? "?" : "&";
    url += "language=" + urlEncode(m_lang);
    std::vector<std::string> hdr = {"Accept: application/json"};
    if (m_token.compare(0, 3, "eyJ") == 0) hdr.push_back("Authorization: Bearer " + m_token);
    else url += "&api_key=" + urlEncode(m_token);
    for (int attempt = 0; attempt < 4; attempt++) {
        if (m_cancel && *m_cancel) return false;
        HttpResult r = httpGet(url, hdr, 30, m_cancel);
        if (r.status == 200) { out = std::move(r.body); return true; }
        if (r.status == 429 || r.status >= 500 || r.status == 0) {  // busy or a network blip: wait and retry
            Sleep(1500 * (attempt + 1));
            continue;
        }
        m_error = r.status == 401 ? "TMDB didn't accept the API key" : "TMDB answered " + std::to_string(r.status);
        logLine("tmdb " + pathQuery + ": " + std::to_string(r.status) + " " + r.error);
        return false;
    }
    m_error = "Couldn't reach TMDB";
    return false;
}

cJSON* Tmdb::get(const std::string& key, const std::string& pathQuery, int maxAgeDays) {
    std::string txt;
    int64_t at = 0;
    {
        std::lock_guard<std::mutex> l(m_mx);
        Stmt q = m_db.prepare("SELECT json, fetched FROM tmdb WHERE key = ?");
        q.bind(1, key);
        if (q.step()) { txt = q.str(0); at = q.i64(1); }
    }
    if (txt.empty() || unixNow() - at > (int64_t)maxAgeDays * 86400) {
        std::string got;
        if (fetch(pathQuery, got)) {
            std::lock_guard<std::mutex> l(m_mx);
            m_db.prepare("INSERT OR REPLACE INTO tmdb(key, json, fetched) VALUES(?, ?, ?)").bind(1, key).bind(2, got).bind(3, unixNow()).run();
            txt = std::move(got);
        }
    }
    return txt.empty() ? nullptr : cJSON_Parse(txt.c_str());
}

std::string Tmdb::image(const std::string& remote, const char* size) {
    if (remote.empty()) return "";
    std::string url = std::string(IMG) + size + remote;
    {
        std::lock_guard<std::mutex> l(m_mx);
        Stmt q = m_db.prepare("SELECT hash FROM art WHERE url = ?");
        q.bind(1, url);
        if (q.step()) return q.str(0);
    }
    if (m_cancel && *m_cancel) return "";
    HttpResult r = httpGet(url, {}, 30, m_cancel);
    if (r.status != 200 || r.body.empty()) return "";
    Image probe;
    if (!loadImageMemory(r.body.data(), r.body.size(), 0, probe)) return "";  // not a picture after all
    std::vector<uint8_t> data(r.body.begin(), r.body.end());
    std::string hash = hashHex(data.data(), data.size());
    std::lock_guard<std::mutex> l(m_mx);
    Tx tx(m_db);
    m_db.prepare("INSERT OR IGNORE INTO blobs(hash, data, width, height) VALUES(?, ?, ?, ?)")
        .bind(1, hash).bind(2, data).bind(3, probe.w).bind(4, probe.h).run();
    m_db.prepare("INSERT OR REPLACE INTO art(url, hash) VALUES(?, ?)").bind(1, url).bind(2, hash).run();
    tx.commit();
    return hash;
}

std::vector<TmdbHit> Tmdb::search(bool tv, const std::string& query, int year) {
    std::vector<TmdbHit> hits;
    std::string q = std::string(tv ? "/search/tv" : "/search/movie") + "?query=" + urlEncode(query);
    if (year) q += (tv ? "&first_air_date_year=" : "&year=") + std::to_string(year);
    std::string txt;
    if (!fetch(q, txt)) return hits;
    cJSON* root = cJSON_Parse(txt.c_str());
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "results")) {
        TmdbHit h;
        h.id = jint(it, "id");
        h.name = jstr(it, tv ? "name" : "title");
        h.overview = jstr(it, "overview");
        h.year = yearOf(jstr(it, tv ? "first_air_date" : "release_date"));
        if (h.id) hits.push_back(h);
    }
    cJSON_Delete(root);
    return hits;
}

int Tmdb::match(bool tv, const std::string& key, const std::string& name, int year) {
    const char* kind = tv ? "tv" : "movie";
    {
        std::lock_guard<std::mutex> l(m_mx);
        // a pin for this name, whatever the year
        Stmt p = m_db.prepare("SELECT tmdb FROM matches WHERE kind = ? AND key = ? AND pinned = 1");
        p.bind(1, kind).bind(2, key);
        if (p.step()) return p.i32(0);
        Stmt q = m_db.prepare("SELECT tmdb, at FROM matches WHERE kind = ? AND key = ? AND year = ?");
        q.bind(1, kind).bind(2, key).bind(3, year);
        // nothing found before: try again after three days (TMDB grows)
        if (q.step() && (q.i32(0) || unixNow() - q.i64(1) < 3 * 86400)) return q.i32(0);
    }
    if (m_token.empty()) return 0;
    m_error.clear();
    std::vector<TmdbHit> hits = search(tv, name, year);
    if (hits.empty() && year && m_error.empty()) hits = search(tv, name, 0);
    // a failed request isn't "not on TMDB": don't remember it
    if ((m_cancel && *m_cancel) || (hits.empty() && !m_error.empty())) return 0;
    int id = 0;
    if (!hits.empty()) {
        // an exact (letters and digits) name match first, then TMDB's own ranking
        id = hits[0].id;
        std::string k = nameKey(name);
        for (auto& h : hits)
            if (nameKey(h.name) == k && (!year || !h.year || h.year == year)) { id = h.id; break; }
    }
    std::lock_guard<std::mutex> l(m_mx);
    m_db.prepare("INSERT OR REPLACE INTO matches(kind, key, year, tmdb, pinned, at) VALUES(?, ?, ?, ?, 0, ?)")
        .bind(1, kind).bind(2, key).bind(3, year).bind(4, id).bind(5, unixNow()).run();
    return id;
}

void Tmdb::setMatch(bool tv, const std::string& key, int tmdbId) {
    std::lock_guard<std::mutex> l(m_mx);
    const char* kind = tv ? "tv" : "movie";
    Tx tx(m_db);
    m_db.prepare("DELETE FROM matches WHERE kind = ? AND key = ?").bind(1, kind).bind(2, key).run();
    m_db.prepare("INSERT INTO matches(kind, key, year, tmdb, pinned, at) VALUES(?, ?, -1, ?, 1, ?)")
        .bind(1, kind).bind(2, key).bind(3, tmdbId).bind(4, unixNow()).run();
    tx.commit();
}
