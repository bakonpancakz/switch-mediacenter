#include "medialib.h"

#include <algorithm>
#include <map>
#include <regex>

#include "cJSON.h"
#include "library.h"
#include "tmdb.h"
#include "util.h"

namespace {

int seasonRank(int n) { return n > 0 ? n : n < 0 ? 1000 : 1001; }  // 1, 2, ... then unknown, specials last

void sortSeasons(MShow& s) {
    for (auto& se : s.seasons)
        std::stable_sort(se.episodes.begin(), se.episodes.end(), [](const MEpisode& a, const MEpisode& b) {
            if (a.number != b.number) return a.number < b.number;
            return lib::sortKey(a.rel) < lib::sortKey(b.rel);
        });
    std::sort(s.seasons.begin(), s.seasons.end(), [](const MSeason& a, const MSeason& b) { return seasonRank(a.number) < seasonRank(b.number); });
}

MSeason& seasonOf(MShow& s, int number) {
    for (auto& se : s.seasons)
        if (se.number == number) return se;
    MSeason ns;
    ns.number = number;
    ns.title = number < 0 ? "Episodes" : number == 0 ? "Specials" : "Season " + std::to_string(number);
    s.seasons.push_back(ns);
    return s.seasons.back();
}

}  // namespace

// "Lucky Star OVA" -> "Lucky Star" (and "OVA 2" -> 2 in num): OVAs, OADs and
// specials are usually listed under their show, not as movies. Returns the
// title unchanged when it isn't one.
std::string stripExtra(const std::string& title, int& num) {
    static const std::regex rx("^(.*?)[ ._-]*\\b(ova|oad|ona|specials?|sp)\\b[ ._-]*(\\d{1,3})?.*$", std::regex::icase);
    std::smatch m;
    num = 0;
    if (!std::regex_match(title, m, rx) || m[1].length() == 0) return title;
    if (m[3].matched) num = atoi(m[3].str().c_str());
    return m[1].str();
}

void groupVideos(int root, std::vector<VideoFile> files, std::vector<MShow>& shows, std::vector<MMovie>& movies) {
    std::sort(files.begin(), files.end(), [](const VideoFile& a, const VideoFile& b) { return lib::sortKey(a.rel) < lib::sortKey(b.rel); });
    std::map<std::string, size_t> showIdx;
    size_t firstShow = shows.size(), firstMovie = movies.size();
    for (const VideoFile& f : files) {
        if (f.name.type == VideoName::MOVIE) {
            MMovie m;
            m.root = root;
            m.item = f.item;
            m.title = f.name.title.empty() ? stemOf(f.rel) : f.name.title;
            m.key = nameKey(m.title);
            m.year = f.name.year;
            m.added = f.mtime;
            m.backdrop = f.frame;  // until TMDB has a better one
            movies.push_back(m);
            continue;
        }
        std::string key = nameKey(f.name.show);
        auto it = showIdx.find(key);
        if (it == showIdx.end()) {
            MShow s;
            s.root = root;
            s.key = key;
            s.title = f.name.show;
            it = showIdx.emplace(key, shows.size()).first;
            shows.push_back(s);
        }
        MShow& s = shows[it->second];
        if (f.name.year && !s.year) s.year = f.name.year;
        s.added = std::max(s.added, f.mtime);
        MEpisode e;
        e.item = f.item;
        e.season = f.name.season;
        e.number = f.name.episode;
        e.title = f.name.title;
        e.still = f.frame;
        e.rel = f.rel;
        seasonOf(s, f.name.season).episodes.push_back(e);
    }
    // an OVA or special named like a show we have joins that show's specials
    for (size_t i = firstMovie; i < movies.size();) {
        int num = 0;
        std::string base = stripExtra(movies[i].title, num);
        MShow* s = nullptr;
        if (base != movies[i].title)
            for (size_t j = firstShow; j < shows.size(); j++)
                if (shows[j].key == nameKey(base)) s = &shows[j];
        if (!s) { i++; continue; }
        MSeason& sp = seasonOf(*s, 0);
        MEpisode e;
        e.item = movies[i].item;
        e.season = 0;
        e.number = num ? num : (int)sp.episodes.size() + 1;
        e.title = movies[i].title;
        e.still = movies[i].backdrop;
        sp.episodes.push_back(e);
        movies.erase(movies.begin() + i);
    }
    for (size_t j = firstShow; j < shows.size(); j++) sortSeasons(shows[j]);
}

// Puts absolute-numbered episodes (anime "- 28") into TMDB's seasons using
// each season's episode count, then merges them into those seasons.
void placeAbsolute(MShow& s, const std::vector<std::pair<int, int>>& counts) {
    auto unknown = std::find_if(s.seasons.begin(), s.seasons.end(), [](const MSeason& x) { return x.number < 0; });
    if (unknown == s.seasons.end() || counts.empty()) return;
    std::vector<MEpisode> eps = std::move(unknown->episodes);
    s.seasons.erase(unknown);
    for (MEpisode& e : eps) {
        int n = e.number, season = -1, before = 0;
        for (auto& c : counts) {
            if (n <= before + c.second) { season = c.first; n -= before; break; }
            before += c.second;
        }
        if (season < 0) { season = counts.back().first; n = e.number - (before - counts.back().second); }  // past the known episodes
        e.season = season;
        e.number = n;
        seasonOf(s, season).episodes.push_back(e);
    }
    sortSeasons(s);
}

void lookupMedia(Tmdb& tmdb, std::vector<MShow>& shows, std::vector<MMovie>& movies, const std::atomic<bool>& cancel,
                 const std::function<void(const std::string&, int, int)>& status) {
    if (!tmdb.enabled()) return;
    int total = (int)(shows.size() + movies.size()), done = 0;
    for (MShow& s : shows) {
        if (cancel) return;
        status(s.title, ++done, total);
        s.tmdb = tmdb.match(true, s.key, s.title, s.year);
        if (!s.tmdb) continue;
        std::string id = std::to_string(s.tmdb);
        cJSON* tv = tmdb.get("tv" + id, "/tv/" + id, 7);
        if (!tv) continue;
        if (!Tmdb::jstr(tv, "name").empty()) s.title = Tmdb::jstr(tv, "name");
        s.overview = Tmdb::jstr(tv, "overview");
        s.year = Tmdb::yearOf(Tmdb::jstr(tv, "first_air_date"));
        s.poster = tmdb.image(Tmdb::jstr(tv, "poster_path"), "w342");
        std::string bd = tmdb.image(Tmdb::jstr(tv, "backdrop_path"), "w780");
        if (!bd.empty()) s.backdrop = bd;
        std::vector<std::pair<int, int>> counts;  // regular seasons: number, episodes
        std::map<int, std::string> seasonNames;
        const cJSON* it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(tv, "seasons")) {
            int n = Tmdb::jint(it, "season_number");
            seasonNames[n] = Tmdb::jstr(it, "name");
            if (n > 0 && Tmdb::jint(it, "episode_count") > 0) counts.push_back({n, Tmdb::jint(it, "episode_count")});
        }
        cJSON_Delete(tv);
        std::sort(counts.begin(), counts.end());
        placeAbsolute(s, counts);
        for (MSeason& se : s.seasons) {
            if (se.number < 0) continue;
            if (!seasonNames[se.number].empty()) se.title = seasonNames[se.number];
            std::string sn = std::to_string(se.number);
            cJSON* sj = tmdb.get("tv" + id + "_s" + sn, "/tv/" + id + "/season/" + sn, 7);
            if (!sj) continue;
            std::map<int, const cJSON*> byNum;
            cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(sj, "episodes")) byNum[Tmdb::jint(it, "episode_number")] = it;
            for (MEpisode& e : se.episodes) {
                auto f = byNum.find(e.number);
                if (f == byNum.end()) continue;
                if (!Tmdb::jstr(f->second, "name").empty()) e.title = Tmdb::jstr(f->second, "name");
                e.overview = Tmdb::jstr(f->second, "overview");
                std::string still = tmdb.image(Tmdb::jstr(f->second, "still_path"), "w300");
                if (!still.empty()) e.still = still;
                if (cancel) break;
            }
            cJSON_Delete(sj);
        }
    }
    for (MMovie& m : movies) {
        if (cancel) return;
        status(m.title, ++done, total);
        m.tmdb = tmdb.match(false, m.key, m.title, m.year);
        if (!m.tmdb) {
            // no such movie: an OVA or special of a show we don't have; use the show's artwork
            int num = 0;
            std::string base = stripExtra(m.title, num);
            if (base == m.title) continue;
            int tvId = tmdb.match(true, nameKey(base), base, 0);
            if (!tvId) continue;
            std::string id = std::to_string(tvId);
            cJSON* tv = tmdb.get("tv" + id, "/tv/" + id, 7);
            if (!tv) continue;
            m.overview = Tmdb::jstr(tv, "overview");
            m.poster = tmdb.image(Tmdb::jstr(tv, "poster_path"), "w342");
            std::string bd = tmdb.image(Tmdb::jstr(tv, "backdrop_path"), "w780");
            if (!bd.empty()) m.backdrop = bd;
            cJSON_Delete(tv);
            continue;
        }
        std::string id = std::to_string(m.tmdb);
        cJSON* mj = tmdb.get("movie" + id, "/movie/" + id, 30);
        if (!mj) continue;
        if (!Tmdb::jstr(mj, "title").empty()) m.title = Tmdb::jstr(mj, "title");
        m.overview = Tmdb::jstr(mj, "overview");
        m.year = Tmdb::yearOf(Tmdb::jstr(mj, "release_date"));
        m.runtime = Tmdb::jint(mj, "runtime");
        m.poster = tmdb.image(Tmdb::jstr(mj, "poster_path"), "w342");
        std::string bd = tmdb.image(Tmdb::jstr(mj, "backdrop_path"), "w780");
        if (!bd.empty()) m.backdrop = bd;
        cJSON_Delete(mj);
    }
}
