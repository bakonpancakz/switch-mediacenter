#include "config.h"

#include <windows.h>

#include <random>

#include "library.h"
#include "util.h"

std::string Mapping::switchDir() const { return std::string(lib::categoryDir(category)) + "/" + cardSafe(name); }

std::string configPath() { return dataDir() + "/config.ini"; }

std::string cleanMappingName(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) {
            if (!o.empty() && o.back() != ' ') o += ' ';
            continue;
        }
        o += (char)c;
    }
    o = trim(o);
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    return o.empty() ? "Folder" : o;
}

std::string newMappingId() {
    std::random_device rd;
    char b[20];
    snprintf(b, sizeof(b), "m%08x%04x", rd(), rd() & 0xFFFF);
    return b;
}

// One setting per line, "key=value". A mapping is
// "map=<id>|<category>|1|<name>|<folder>" (the 1 is a field that's no longer used).
bool Config::load(const std::string& file) {
    std::vector<uint8_t> data;
    if (!readWhole(file, data)) return false;
    std::string txt(data.begin(), data.end());
    mappings.clear();
    size_t p = 0;
    while (p < txt.size()) {
        size_t e = txt.find('\n', p);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "map") {
            std::vector<std::string> f;
            size_t q = 0;
            for (int i = 0; i < 4; i++) {
                size_t bar = v.find('|', q);
                if (bar == std::string::npos) break;
                f.push_back(v.substr(q, bar - q));
                q = bar + 1;
            }
            if (f.size() != 4) continue;
            Mapping m;
            m.id = f[0];
            m.category = atoi(f[1].c_str());
            if (m.category < 0 || m.category >= lib::C_COUNT) m.category = 0;
            m.name = cleanMappingName(f[3]);
            m.folder = v.substr(q);
            if (!m.id.empty() && !m.folder.empty()) mappings.push_back(m);
        } else if (k == "tmdb_token") tmdbToken = trim(v);
        else if (k == "language") language = trim(v).empty() ? "en-US" : trim(v);
        else if (k == "ffmpeg_dir") ffmpegDir = trim(v);
        else if (k == "auto_sync") autoSync = v != "0";
        else if (k == "close_to_tray") closeToTray = v != "0";
    }
    return true;
}

bool Config::save(const std::string& file) const {
    std::string t = "# Media Center Sync settings\r\n";
    for (auto& m : mappings)
        t += "map=" + m.id + "|" + std::to_string(m.category) + "|1|" + m.name + "|" + m.folder + "\r\n";
    t += "tmdb_token=" + tmdbToken + "\r\n";
    t += "language=" + language + "\r\n";
    t += "ffmpeg_dir=" + ffmpegDir + "\r\n";
    t += std::string("auto_sync=") + (autoSync ? "1" : "0") + "\r\n";
    t += std::string("close_to_tray=") + (closeToTray ? "1" : "0") + "\r\n";
    return writeWhole(file, t.data(), t.size());
}
