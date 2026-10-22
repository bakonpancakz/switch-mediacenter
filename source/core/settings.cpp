// settings.ini (flat key=value) and positions.txt (per-file reading/playback position).
#include "app.h"
#include <map>
#include <mutex>

static std::map<std::string, std::string> s_cfg;
static std::mutex s_cfgMutex;
static bool s_cfgDirty = false;
static uint32_t s_cfgDirtyAt = 0;

static const char* CFG_PATH = APP_DIR "/settings.ini";
static const char* POS_PATH = APP_DIR "/positions.txt";

static void parseLines(const std::string& data, const std::function<void(const std::string&)>& f) {
    size_t p = 0;
    while (p < data.size()) {
        size_t nl = data.find('\n', p);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(p, nl - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) f(line);
        p = nl + 1;
    }
}

void cfgLoad() {
    std::string data;
    if (!readFile(CFG_PATH, data)) return;
    std::lock_guard<std::mutex> lock(s_cfgMutex);
    parseLines(data, [](const std::string& line) {
        if (line[0] == '#' || line[0] == ';') return;
        size_t eq = line.find('=');
        if (eq == std::string::npos) return;
        s_cfg[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    });
}

void cfgFlush() {
    std::string out;
    {
        std::lock_guard<std::mutex> lock(s_cfgMutex);
        if (!s_cfgDirty) return;
        for (auto& kv : s_cfg) out += kv.first + "=" + kv.second + "\n";
        s_cfgDirty = false;
    }
    mkdirs(APP_DIR);
    writeFile(CFG_PATH, out);
}

void cfgTick() {
    if (s_cfgDirty && SDL_GetTicks() - s_cfgDirtyAt > 1500) cfgFlush();
}

std::string cfgStr(const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> lock(s_cfgMutex);
    auto it = s_cfg.find(key);
    return it == s_cfg.end() ? def : it->second;
}

int cfgInt(const std::string& key, int def) {
    std::string v = cfgStr(key, "");
    if (v.empty()) return def;
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    return end && *end == 0 ? (int)n : def;
}

bool cfgBool(const std::string& key, bool def) {
    std::string v = lower(cfgStr(key, ""));
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

void cfgSet(const std::string& key, const std::string& v) {
    std::lock_guard<std::mutex> lock(s_cfgMutex);
    auto it = s_cfg.find(key);
    if (it != s_cfg.end() && it->second == v) return;
    s_cfg[key] = v;
    if (!s_cfgDirty) s_cfgDirtyAt = SDL_GetTicks();
    s_cfgDirty = true;
}

void cfgSetInt(const std::string& key, int v) { cfgSet(key, std::to_string(v)); }
void cfgSetBool(const std::string& key, bool v) { cfgSet(key, v ? "1" : "0"); }

// ---------------------------------------------------------------- positions
// One entry per line: "pos<TAB>total<TAB>path".

struct Pos { int64_t pos, total; };
static std::map<std::string, Pos> s_pos;
static bool s_posLoaded = false, s_posDirty = false;
static std::mutex s_posMutex;

static void posLoad() {
    if (s_posLoaded) return;
    s_posLoaded = true;
    std::string data;
    if (!readFile(POS_PATH, data)) return;
    parseLines(data, [](const std::string& line) {
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) return;
        s_pos[line.substr(t2 + 1)] = {strtoll(line.c_str(), nullptr, 10), strtoll(line.c_str() + t1 + 1, nullptr, 10)};
    });
}

bool posGet(const std::string& path, int64_t& pos, int64_t& total) {
    std::lock_guard<std::mutex> lock(s_posMutex);
    posLoad();
    auto it = s_pos.find(path);
    if (it == s_pos.end()) return false;
    pos = it->second.pos;
    total = it->second.total;
    return true;
}

void posSet(const std::string& path, int64_t pos, int64_t total) {
    std::lock_guard<std::mutex> lock(s_posMutex);
    posLoad();
    Pos& p = s_pos[path];
    if (p.pos == pos && p.total == total) return;
    p.pos = pos;
    p.total = total;
    s_posDirty = true;
}

void posFlush() {
    std::string out;
    {
        std::lock_guard<std::mutex> lock(s_posMutex);
        if (!s_posDirty) return;
        char buf[64];
        for (auto& kv : s_pos) {
            snprintf(buf, sizeof(buf), "%lld\t%lld\t", (long long)kv.second.pos, (long long)kv.second.total);
            out += buf + kv.first + "\n";
        }
        s_posDirty = false;
    }
    writeFile(POS_PATH, out);
}
