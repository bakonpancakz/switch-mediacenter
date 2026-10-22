#include "vname.h"

#include <cctype>
#include <regex>
#include <vector>

namespace {

const std::regex::flag_type RX = std::regex::ECMAScript | std::regex::icase;

std::string trimSep(std::string s) {
    auto sep = [](char c) { return c == ' ' || c == '-' || c == '.' || c == '_' || c == ',' || c == '~' || c == ':'; };
    while (!s.empty() && sep(s.back())) s.pop_back();
    size_t a = 0;
    while (a < s.size() && sep(s[a])) a++;
    return s.substr(a);
}

// Drops release-group and hash tags ([SubsPlease], [ABCD1234], (1080p)),
// keeps a "(2019)" year, turns dots/underscores into spaces.
std::string clean(const std::string& in) {
    std::string s;
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        char close = c == '[' ? ']' : c == '(' ? ')' : c == '{' ? '}' : 0;
        if (close) {
            size_t e = in.find(close, i + 1);
            if (e != std::string::npos) {
                std::string inner = in.substr(i + 1, e - i - 1);
                bool year = inner.size() == 4 && (inner.compare(0, 2, "19") == 0 || inner.compare(0, 2, "20") == 0) &&
                            isdigit((unsigned char)inner[2]) && isdigit((unsigned char)inner[3]);
                if (year && c == '(') s += " (" + inner + ") ";
                else s += ' ';
                i = e;
                continue;
            }
        }
        s += c;
    }
    bool spaces = s.find(' ') != std::string::npos && s.find(' ') < s.size() - 1;
    for (char& c : s) {
        if (c == '_') c = ' ';
        else if (c == '.' && !spaces) c = ' ';
    }
    // a scene name with real spaces may still use dots as separators ("Show Name.S01E02")
    std::string out;
    for (char c : s) {
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out += c;
    }
    return trimSep(out);
}

// Everything from the first technical token on (1080p, x265, WEB-DL...) is noise.
std::string cutJunk(const std::string& s) {
    static const std::regex junk(
        "(^|[ .\\-])(2160p|1440p|1080[pi]|720p|576p|480p|4k|uhd|hdr10?\\+?|dv|x26[45]|h ?26[45]|hevc|avc|xvid|divx|"
        "web(-?dl|-?rip)?|blu-?ray|bdrip|brrip|bdremux|remux|dvd(rip)?|hdtv|hdrip|aac[0-9. ]*|ac3|e-?ac-?3|dts(-hd)?|"
        "truehd|atmos|flac|opus|10-?bit|8-?bit|proper|repack|extended|unrated|remastered|multi|dual[ -]?audio|"
        "subbed|dubbed|complete|batch|nf|amzn|dsnp|hmax|cr|funi)([ .\\-]|$)",
        RX);
    std::smatch m;
    if (std::regex_search(s, m, junk)) return trimSep(s.substr(0, m.position(0)));
    return trimSep(s);
}

int seasonOfFolder(const std::string& d) {
    static const std::regex rx("^(season|series|staffel|saison|s)[ ._-]*(\\d{1,2})$", RX);
    std::string c = clean(d);
    std::smatch m;
    if (std::regex_match(c, m, rx)) return std::stoi(m[2].str());
    std::string low;
    for (char ch : c) low += (char)tolower((unsigned char)ch);
    if (low == "specials" || low == "special" || low == "extras" || low == "ova" || low == "ovas") return 0;
    return -1;
}

// "Show Name (2019)" -> "Show Name", 2019
std::string splitYear(const std::string& s, int& year) {
    static const std::regex rx("^(.*?)[ (]*((19|20)\\d{2})\\)?$", RX);
    std::smatch m;
    if (std::regex_match(s, m, rx) && !trimSep(m[1].str()).empty()) {
        year = std::stoi(m[2].str());
        return trimSep(m[1].str());
    }
    return s;
}

}  // namespace

std::string nameKey(const std::string& s) {
    std::string k;
    for (unsigned char c : s)
        if (isalnum(c)) k += (char)tolower(c);
    return k;
}

VideoName parseVideoName(const std::string& rel) {
    VideoName v;
    std::vector<std::string> parts;
    size_t p = 0;
    while (true) {
        size_t s = rel.find('/', p);
        if (s == std::string::npos) { parts.push_back(rel.substr(p)); break; }
        if (s > p) parts.push_back(rel.substr(p, s - p));
        p = s + 1;
    }
    std::string file = parts.back();
    parts.pop_back();
    size_t dot = file.rfind('.');
    std::string stem = clean(dot == std::string::npos ? file : file.substr(0, dot));

    // folders: the deepest that isn't a season folder names the show or movie
    int folderSeason = -1;
    std::string folder;
    for (int i = (int)parts.size() - 1; i >= 0; i--) {
        int s = seasonOfFolder(parts[i]);
        if (s >= 0) {
            if (folderSeason < 0) folderSeason = s;
            continue;
        }
        folder = clean(parts[i]);
        break;
    }

    std::smatch m;
    auto episodeTitle = [](const std::string& after) {
        std::string t = cutJunk(trimSep(after));
        return t;
    };
    auto finishShow = [&](std::string show) {
        show = trimSep(show);
        if (show.empty()) show = folder;
        int y = 0;
        show = splitYear(show, y);
        v.show = show;
        v.year = y;
        v.type = VideoName::EPISODE;
    };

    // S01E02, S01 E02, S01.E02 (first episode of S01E01E02)
    static const std::regex sxe("(^|[^a-z0-9])s(\\d{1,2})[ ._-]*e(\\d{1,4})", RX);
    // 1x02
    static const std::regex nxn("(^|[^0-9])(\\d{1,2})x(\\d{2,3})([^0-9]|$)", RX);
    // Season 1 Episode 2
    static const std::regex words("season[ ._-]*(\\d{1,2})[ ._-]*(episode|ep)[ ._-]*(\\d{1,4})", RX);
    // Episode 5, Ep 05, E05, Session 05 (season from the folder)
    static const std::regex epOnly("(^|[ ._-])(episode|session|ep|e)[ ._-]*(\\d{1,4})([ ._-]|v\\d|$)", RX);
    // Anime: "Show - 05", "Show - 05v2", "Show - 105"
    static const std::regex dash("^(.*?)[ ]+-[ ]+(\\d{1,4})(v\\d)?([ ._-].*)?$", RX);
    // a name that starts with the number: "05 - Title", "05", "05. Title"
    static const std::regex lead("^(\\d{1,4})([ ._-].*)?$", RX);

    if (std::regex_search(stem, m, sxe)) {
        finishShow(stem.substr(0, m.position(0) + m[1].length()));
        v.season = std::stoi(m[2].str());
        v.episode = std::stoi(m[3].str());
        v.title = episodeTitle(m.suffix().str());
    } else if (std::regex_search(stem, m, words)) {
        finishShow(stem.substr(0, m.position(0)));
        v.season = std::stoi(m[1].str());
        v.episode = std::stoi(m[3].str());
        v.title = episodeTitle(m.suffix().str());
    } else if (std::regex_search(stem, m, nxn)) {
        finishShow(stem.substr(0, m.position(0) + m[1].length()));
        v.season = std::stoi(m[2].str());
        v.episode = std::stoi(m[3].str());
        v.title = episodeTitle(m[4].str() + m.suffix().str());
    } else if (std::regex_match(stem, m, dash) && !cutJunk(m[1].str()).empty()) {
        finishShow(cutJunk(m[1].str()));
        v.season = folderSeason;
        v.episode = std::stoi(m[2].str());
        v.title = episodeTitle(m[4].str());
    } else if (std::regex_search(stem, m, epOnly) && (!folder.empty() || m.position(0) > 0)) {
        finishShow(stem.substr(0, m.position(0)));
        v.season = folderSeason >= 0 ? folderSeason : (folder.empty() ? -1 : -1);
        v.episode = std::stoi(m[3].str());
        v.title = episodeTitle(m.suffix().str());
    } else if (!folder.empty() && std::regex_match(stem, m, lead) && (m[1].length() < 4 || folderSeason >= 0)) {
        // a bare number inside a show's folder
        finishShow("");
        v.season = folderSeason;
        v.episode = std::stoi(m[1].str());
        v.title = episodeTitle(m[2].str());
    } else {
        // a movie: "Title (2019)", "Title.2019.1080p", or named by its own folder
        v.type = VideoName::MOVIE;
        std::string t = cutJunk(stem);
        // the last year in the name: "Blade Runner 2049 (2017)" is from 2017
        static const std::regex yr("^(.+)[ (]+((19|20)\\d{2})\\)?([ ._-].*)?$", RX);
        int year = 0;
        if (std::regex_match(t, m, yr) && !trimSep(m[1].str()).empty()) {
            year = std::stoi(m[2].str());
            t = trimSep(m[1].str());
        }
        if (!folder.empty()) {
            int fy = 0;
            std::string ft = splitYear(cutJunk(folder), fy);
            // a "Title (Year)" folder holding a badly named file
            if (fy && (!year || t.empty())) { t = ft; year = fy; }
        }
        if (t.empty()) t = folder;
        v.title = t;
        v.year = year;
    }
    if (v.type == VideoName::EPISODE && v.season < 0 && folderSeason >= 0) v.season = folderSeason;
    return v;
}
