// Small pieces the Files, Media and Books tabs share.
#pragma once
#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

#include "app.h"
#include "core/library.h"

// An audio file's loudness as bars across r (the peaks the PC measured). With
// `played` (0..1), each bar turns playedC as the playhead crosses it.
inline void drawWave(const std::vector<uint8_t>& peaks, const SDL_Rect& r, SDL_Color c, float played = -1, SDL_Color playedC = {}) {
    if (peaks.empty()) return;
    int n = (int)peaks.size();
    float step = r.w / (float)n, bw = std::max(1.0f, step * 0.62f);
    for (int i = 0; i < n; i++) {
        float t = played < 0 ? 0 : SDL_clamp(played * n - i, 0.0f, 1.0f);  // how far the playhead is through this bar
        float h = std::max(3.0f, r.h * (peaks[i] / 255.0f));
        float x = r.x + i * step + (step - bw) / 2, y = r.y + (r.h - h) / 2;
        SDL_Color col = t > 0 ? mix(c, playedC, t) : c;
        fillRound({(int)x, (int)y, (int)std::ceil(bw), (int)h}, (int)(bw / 2), col);
    }
}

// 0..1 of the way through, or -1 if never started.
inline float progressOf(const std::string& path) {
    int64_t pos = 0, total = 0;
    if (!posGet(path, pos, total) || total <= 0 || pos <= 0) return -1;
    return SDL_clamp(pos / (float)total, 0.0f, 1.0f);
}

inline void progressBar(const SDL_Rect& r, float f) {
    if (f < 0) return;
    fillRound(r, r.h / 2, SDL_Color{255, 255, 255, 60});
    fillRound({r.x, r.y, std::max(r.h, (int)(r.w * f)), r.h}, r.h / 2, f > 0.95f ? g_theme.ok : g_theme.accent);
}

// A picture from the packs fitted into r; false (nothing drawn) until it's loaded.
inline bool drawPicture(int64_t id, const SDL_Rect& r, int radius, Uint8 alpha = 255, bool cover = true) {
    SDL_Texture* t = pictureGet(id);
    if (!t) return false;
    drawTexFit(t, r, cover, alpha, radius);
    return true;
}

// The whole tab when there's no library to show yet.
inline void drawNoLibrary(const SDL_Rect& area, int icon, const std::string& category) {
    std::string problem = Library::get().problem();
    float cx = area.x + area.w / 2.0f, cy = area.y + area.h / 2.0f;
    drawIcon(icon, cx, cy - 70, 72, g_theme.faint);
    drawText(problem.empty() ? "Nothing in " + category + " yet" : problem, (int)cx, (int)cy - 10, 28, g_theme.fg, 1, area.w - 80);
    drawText("On your PC, open Media Center Sync and add a folder as " + category +
                 ". Then put this SD card in the PC (a card reader, or hekate's USB mass storage) and sync.",
             (int)cx, (int)cy + 34, 20, g_theme.dim, 1, area.w - 120);
}

// A length: "0.3 s" for short sounds (they'd all read 0:00), "4:05" otherwise.
inline std::string durationText(int64_t ms) {
    if (ms >= 10000) return formatTime(ms);
    char b[16];
    snprintf(b, sizeof(b), "%.1f s", ms / 1000.0);
    return b;
}

inline std::string fileStem(const std::string& name) {
    size_t d = name.rfind('.');
    return d == std::string::npos || d == 0 ? name : name.substr(0, d);
}

// wallClock: the time was recorded as the camera's local clock (EXIF dates),
// stored as if it were UTC, so it's shown as it is.
inline std::string dateText(int64_t unixSecs, bool wallClock = false) {
    if (unixSecs <= 0) return "";
    time_t t = (time_t)unixSecs;
    struct tm* lt = wallClock ? gmtime(&t) : localtime(&t);
    char buf[64] = "";
    if (lt) strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", lt);
    return buf;
}
