// String, file and logging helpers.
#include "app.h"
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string ext(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return lower(path.substr(dot + 1));
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string dirName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string stem(const std::string& path) {
    std::string b = baseName(path);
    size_t dot = b.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
// Case-insensitive compare where digit runs compare numerically ("page2" < "page10").
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = a[i], cb = b[j];
        if (isdigit(ca) && isdigit(cb)) {
            size_t i0 = i, j0 = j;
            while (i < a.size() && a[i] == '0') i++;
            while (j < b.size() && b[j] == '0') j++;
            size_t si = i, sj = j;
            while (i < a.size() && isdigit((unsigned char)a[i])) i++;
            while (j < b.size() && isdigit((unsigned char)b[j])) j++;
            size_t li = i - si, lj = j - sj;
            if (li != lj) return li < lj;
            int c = a.compare(si, li, b, sj, lj);
            if (c != 0) return c < 0;
            if ((i - i0) != (j - j0)) return (i - i0) < (j - j0);
        } else {
            int la = tolower(ca), lb = tolower(cb);
            if (la != lb) return la < lb;
            i++;
            j++;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = n ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(got);
    return true;
}

// Writes via a temp file so a crash never leaves a half-written file behind.
bool writeFile(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp.c_str()); return false; }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t fileSize(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1;
}

void mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            std::string p = path.substr(0, i);
            if (!p.empty() && p.back() != ':') mkdir(p.c_str(), 0777);
        }
    }
}

std::string formatBytes(double b) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (b >= 1024 && u < 4) { b /= 1024; u++; }
    char buf[32];
    snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : b < 10 ? "%.1f %s" : "%.0f %s", b, units[u]);
    return buf;
}

std::string formatTime(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t s = ms / 1000;
    char buf[32];
    if (s >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(buf, sizeof(buf), "%d:%02d", (int)(s / 60), (int)(s % 60));
    return buf;
}

uint64_t fnv64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

uint32_t utf8Next(const std::string& s, size_t& i) {
    unsigned char c = s[i];
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { i++; return 0xFFFD; }
    if (i + n > s.size()) { i = s.size(); return 0xFFFD; }
    for (int k = 1; k < n; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    i += n;
    return cp;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}

uint64_t nowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ULL; }

bool isImageExt(const std::string& e) {
    return e == "jpg" || e == "jpeg" || e == "png" || e == "webp" || e == "gif" || e == "bmp" || e == "tga" ||
           e == "avif" || e == "jfif";
}
bool isAudioExt(const std::string& e) {
    return e == "mp3" || e == "m4a" || e == "aac" || e == "flac" || e == "ogg" || e == "oga" || e == "opus" ||
           e == "wav" || e == "wma" || e == "aiff" || e == "aif" || e == "alac" || e == "mka";
}
bool isComicExt(const std::string& e) {
    return e == "cbz" || e == "cbr" || e == "cb7" || e == "cbt" || e == "zip" || e == "rar" || e == "7z" || e == "tar";
}

// ---------------------------------------------------------------- log

static std::mutex s_logMutex;
static const char* LOG_PATH = APP_DIR "/log.txt";
static long s_logBytes = 0;

void appLogReset() {
    remove(APP_DIR "/log.prev.txt");
    rename(LOG_PATH, APP_DIR "/log.prev.txt");
}

void appLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    if (s_logBytes > 2 * 1024 * 1024) return;
    FILE* f = fopen(LOG_PATH, "ab");
    if (!f) return;
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    s_logBytes += fprintf(f, "[%02d:%02d:%02d] ", lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0, lt ? lt->tm_sec : 0);
    va_list ap;
    va_start(ap, fmt);
    s_logBytes += vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
