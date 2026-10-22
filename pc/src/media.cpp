#include "media.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "cJSON.h"
#include "fsx.h"
#include "util.h"

FfTools findFfmpeg(const std::string& dir) {
    FfTools t;
    auto tryDir = [&](const std::string& dir) {
        if (dir.empty() || t.ok()) return;
        std::string d = forwardSlashes(dir);
        if (fsx::size(d + "/ffmpeg.exe") > 0 && fsx::size(d + "/ffprobe.exe") > 0) {
            t.ffmpeg = wide(d + "/ffmpeg.exe");
            t.ffprobe = wide(d + "/ffprobe.exe");
        }
    };
    tryDir(dir);
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH)) {
        std::string s = utf8(self);
        tryDir(s.substr(0, s.find_last_of("\\/")));
    }
    if (!t.ok()) {
        wchar_t a[MAX_PATH], b[MAX_PATH];
        if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, a, nullptr) && SearchPathW(nullptr, L"ffprobe.exe", nullptr, MAX_PATH, b, nullptr)) {
            t.ffmpeg = a;
            t.ffprobe = b;
        }
    }
    return t;
}

static std::string jstr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

// "2023-05-01T12:34:56.000000Z" -> unix seconds
static int64_t isoDate(const std::string& s) {
    int Y, M, D, h = 0, m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 3 || Y < 1971) return 0;
    tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = sec;
    return (int64_t)_mkgmtime(&t);
}

bool probeMedia(const FfTools& t, const std::string& path, MediaInfo& out, const std::atomic<bool>* cancel) {
    std::vector<uint8_t> o;
    int rc = runProcess(t.ffprobe, {L"-v", L"error", L"-print_format", L"json", L"-show_format", L"-show_streams", winPath(path)}, o,
                        60000, cancel);
    if (rc != 0 || o.empty()) return false;
    o.push_back(0);
    cJSON* root = cJSON_Parse((const char*)o.data());
    if (!root) return false;
    const cJSON* fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    double dur = atof(jstr(fmt, "duration").c_str());
    out.taken = isoDate(jstr(cJSON_GetObjectItemCaseSensitive(fmt, "tags"), "creation_time"));
    const cJSON* s;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(root, "streams")) {
        std::string type = jstr(s, "codec_type");
        const cJSON* disp = cJSON_GetObjectItemCaseSensitive(s, "disposition");
        const cJSON* pic = cJSON_GetObjectItemCaseSensitive(disp, "attached_pic");
        if (type == "video" && !out.video && !(cJSON_IsNumber(pic) && pic->valueint)) {  // cover art isn't video
            out.video = true;
            const cJSON* w = cJSON_GetObjectItemCaseSensitive(s, "width");
            const cJSON* h = cJSON_GetObjectItemCaseSensitive(s, "height");
            out.width = cJSON_IsNumber(w) ? w->valueint : 0;
            out.height = cJSON_IsNumber(h) ? h->valueint : 0;
            // phones record portrait video as landscape plus a rotation
            int rot = 0;
            const cJSON* sd;
            cJSON_ArrayForEach(sd, cJSON_GetObjectItemCaseSensitive(s, "side_data_list")) {
                const cJSON* r = cJSON_GetObjectItemCaseSensitive(sd, "rotation");
                if (cJSON_IsNumber(r)) rot = r->valueint;
            }
            if (!rot) rot = atoi(jstr(cJSON_GetObjectItemCaseSensitive(s, "tags"), "rotate").c_str());
            if (std::abs(rot) % 180 == 90) std::swap(out.width, out.height);
            if (dur <= 0) dur = atof(jstr(s, "duration").c_str());
        } else if (type == "audio") {
            out.audio = true;
            if (dur <= 0) dur = atof(jstr(s, "duration").c_str());
        }
    }
    cJSON_Delete(root);
    out.durationMs = dur > 0 ? (int64_t)std::llround(dur * 1000) : 0;
    return out.video || out.audio;
}

static bool grabFrame(const FfTools& t, const std::string& path, double atSec, int maxSide, Image& out, const std::atomic<bool>* cancel) {
    char ss[32], vf[160];
    snprintf(ss, sizeof(ss), "%.3f", atSec);
    // square the pixels first (anamorphic DVDs), then fit inside maxSide
    snprintf(vf, sizeof(vf), "scale=iw*sar:ih,setsar=1,scale=w=%d:h=%d:force_original_aspect_ratio=decrease", maxSide, maxSide);
    std::vector<uint8_t> o;
    std::vector<std::wstring> args = {L"-nostdin", L"-v", L"error"};
    if (atSec > 0) { args.push_back(L"-ss"); args.push_back(wide(ss)); }
    for (const wchar_t* a : {L"-i"}) args.push_back(a);
    args.push_back(winPath(path));
    for (const wchar_t* a : {L"-map", L"0:v:0", L"-frames:v", L"1", L"-vf"}) args.push_back(a);
    args.push_back(wide(vf));
    for (const wchar_t* a : {L"-f", L"image2pipe", L"-c:v", L"bmp", L"-pix_fmt", L"bgr24", L"-"}) args.push_back(a);
    int rc = runProcess(t.ffmpeg, args, o, 90000, cancel);
    return rc == 0 && decodeBmp(o, out);
}

bool videoFrame(const FfTools& t, const std::string& path, const MediaInfo& info, int maxSide, Image& out, const std::atomic<bool>* cancel) {
    double dur = info.durationMs / 1000.0;
    std::vector<double> at;
    if (dur > 4) at = {std::max(1.0, dur * 0.1), dur * 0.25, dur * 0.5, dur * 0.75};
    else at = {dur > 1 ? dur / 2 : 0.0};
    Image best;
    double bestScore = -1;
    for (double s : at) {
        if (cancel && *cancel) return false;
        Image img;
        if (!grabFrame(t, path, s, maxSide, img, cancel)) continue;
        double mean, sd;
        imageStats(img, mean, sd);
        if (mean > 24 && sd > 8) {  // a real picture
            out = std::move(img);
            return true;
        }
        double score = mean + sd;  // otherwise the least black one
        if (score > bestScore) { bestScore = score; best = std::move(img); }
    }
    // a seek past the end of a broken file: the very first frame
    if (bestScore < 0 && !(cancel && *cancel)) {
        Image img;
        if (grabFrame(t, path, 0, maxSide, img, cancel)) { out = std::move(img); return true; }
    }
    if (bestScore < 0) return false;
    out = std::move(best);
    return true;
}

bool audioPeaks(const FfTools& t, const std::string& path, int count, std::vector<uint8_t>& out, const std::atomic<bool>* cancel) {
    std::vector<uint8_t> o;
    // 2000 samples a second, mono: plenty for a picture of loudness, small for an hour-long file
    int rc = runProcess(t.ffmpeg, {L"-nostdin", L"-v", L"error", L"-i", winPath(path), L"-map", L"0:a:0", L"-ac", L"1", L"-ar", L"2000",
                                   L"-f", L"s16le", L"-"},
                        o, 300000, cancel);
    size_t n = o.size() / 2;
    if (rc != 0 || n == 0) return false;
    const int16_t* s = (const int16_t*)o.data();
    std::vector<double> v(count, 0.0);
    for (int i = 0; i < count; i++) {
        size_t a = n * i / count, b = std::max(a + 1, n * (i + 1) / count);
        double sum = 0;
        for (size_t j = a; j < b && j < n; j++) sum += (double)s[j] * s[j];
        v[i] = std::sqrt(sum / (b - a));  // loudness (RMS) of the slice
    }
    double peak = *std::max_element(v.begin(), v.end());
    out.resize(count);
    for (int i = 0; i < count; i++) out[i] = peak > 0 ? (uint8_t)std::lround(255 * v[i] / peak) : 0;
    return true;
}

bool ffmpegImage(const FfTools& t, const std::string& path, int maxSide, Image& out, const std::atomic<bool>* cancel) {
    return grabFrame(t, path, 0, maxSide > 0 ? maxSide : 16384, out, cancel);
}
