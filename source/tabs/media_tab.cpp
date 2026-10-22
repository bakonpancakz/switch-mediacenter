// Media tab: shows and movies (matched and illustrated on the PC), the plain
// folders they came from, and the player: subtitles, track selection,
// zoom/rotate, resume where you left off, Continue watching, next episode.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#include "media/video.h"
#include "tabs/common.h"

namespace {

struct PlayItem {
    std::string path, title;
};

// Most recently played videos, newest first (APP_DIR/recent.txt).
const char* RECENT_FILE = APP_DIR "/recent.txt";

std::vector<std::string> recentVideos() {
    std::vector<std::string> r;
    std::string txt;
    if (!readFile(RECENT_FILE, txt)) return r;
    size_t p = 0;
    while (p < txt.size()) {
        size_t e = txt.find('\n', p);
        if (e == std::string::npos) e = txt.size();
        if (e > p) r.push_back(txt.substr(p, e - p));
        p = e + 1;
    }
    return r;
}

void saveRecent(const std::vector<std::string>& r) {
    std::string txt;
    for (auto& p : r) txt += p + "\n";
    writeFile(RECENT_FILE, txt);
}

void notePlayed(const std::string& path) {
    std::vector<std::string> r = recentVideos();
    r.erase(std::remove(r.begin(), r.end(), path), r.end());
    r.insert(r.begin(), path);
    if (r.size() > 40) r.resize(40);
    saveRecent(r);
}

// Off Continue watching (where it was left is kept: it still resumes there).
void forgetPlayed(const std::string& path) {
    std::vector<std::string> r = recentVideos();
    r.erase(std::remove(r.begin(), r.end(), path), r.end());
    saveRecent(r);
}

std::string epLabel(const LibEpisode& e) {
    if (e.seasonNumber >= 0) return "S" + std::to_string(e.seasonNumber) + " E" + std::to_string(e.number);
    return "Episode " + std::to_string(e.number);
}

std::string epTitle(const LibShow& s, const LibEpisode& e) {
    return s.title + "  \xC2\xB7  " + epLabel(e) + (e.title.empty() ? "" : "  \xC2\xB7  " + e.title);
}

// ---------------------------------------------------------------- the player

class VideoView {
public:
    std::vector<PlayItem> playlist;
    int index = 0;
    bool closed = false;

    void open(const std::vector<PlayItem>& list, int i) {
        playlist = list;
        index = i;
        closed = false;
        start();
    }

    void shutdown() { savePos(); }

    void draw() {
        Video& v = Video::get();
        dpadForMedia();
        fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 255});
        if (v.texture() && v.loaded() && v.frameReady()) drawTexFit(v.texture(), SDL_Rect{0, 0, g_W, g_H}, false);
        else if (v.active()) spinner(g_W / 2.0f, g_H / 2.0f, 30, g_theme.dim);
        if (!v.error().empty() && !v.active()) {
            drawIcon(IC_WARN, g_W / 2.0f, g_H / 2.0f - 50, 64, g_theme.danger);
            drawText(v.error(), g_W / 2, g_H / 2, 24, g_theme.fg, 1, g_W - 80);
        }
        if (g_ticks - m_savedAt > 5000) savePos();
        bool ended = v.ended() && v.loaded();
        if (ended && !m_wasEnded) { m_endedAt = g_ticks; m_controls = true; savePos(true); }
        m_wasEnded = ended;

        if (!m_flash.empty() && g_ticks - m_flashAt < 700) {
            float a = 1 - (g_ticks - m_flashAt) / 700.0f;
            float cx = m_flashSide < 0 ? g_W * 0.2f : g_W * 0.8f;
            fillCircle(cx, g_H / 2.0f, 90, SDL_Color{255, 255, 255, (Uint8)(40 * a)});
            drawText(m_flash, (int)cx, g_H / 2 - 16, 28, withAlpha(g_theme.fg, (Uint8)(255 * a)), 1, 0, FS_BOLD);
        }
        // controls first: taps they use must not also count as gestures on the video
        // (they fade in and out; on the way out they're only drawn, not used)
        approach(m_controlsA, m_controls && !m_tracks && !m_adjust ? 1.0f : 0.0f, 7);
        if (m_controlsA > 0) {
            FadeScope fadeControls(m_controlsA);
            bool showing = m_controls && !m_tracks && !m_adjust, c = g_in.consumed;
            if (!showing) g_in.consumed = true;
            drawControls(ended);
            if (!showing) g_in.consumed = c;
        }
        if (m_tracks) drawTracks();
        if (m_adjust) drawAdjust();
        if (closed) return;
        bool live = !g_in.consumed && !m_tracks && !m_adjust;

        if (live) {
            if (g_in.back) { if (m_controls && !ended) m_controls = false; else exit(); return; }
            if (g_in.pinchScale != 1.0f) {
                m_zoom = SDL_clamp(m_zoom * g_in.pinchScale, 1.0f, 6.0f);
                v.setDouble("video-zoom", log2(m_zoom));
                if (m_zoom <= 1.0f) { m_panX = m_panY = 0; v.setDouble("video-pan-x", 0); v.setDouble("video-pan-y", 0); }
            }
            if (g_in.down && !g_in.multi && g_in.moved && m_zoom > 1.01f) {
                m_panX += g_in.dx / (g_W * m_zoom);
                m_panY += g_in.dy / (g_H * m_zoom);
                float lim = 0.5f - 0.5f / m_zoom;
                m_panX = SDL_clamp(m_panX, -lim, lim);
                m_panY = SDL_clamp(m_panY, -lim, lim);
                v.setDouble("video-pan-x", m_panX);
                v.setDouble("video-pan-y", m_panY);
            }
            if (g_in.doubleTap) {
                float fx = g_in.tapX / (float)g_W;
                if (fx < 0.35f) { v.seek(-10, true, false); flash("\xE2\x88\x92" "10 s", -1); }
                else if (fx > 0.65f) { v.seek(10, true, false); flash("+10 s", 1); }
                else togglePause();
            } else if (g_in.singleTap) {
                m_controls = !m_controls;
                m_touchedAt = g_ticks;
            }
            // the buttons (see ui/input.cpp): R the next episode, L the one before;
            // the d-pad's left/right skip 10 s; ZL/ZR zoom (as a pinch); X pauses;
            // Y shows or hides the controls
            if (g_in.swipeX && g_in.buttonsDown) {
                int to = index + (g_in.swipeX < 0 ? 1 : -1);
                if (to >= 0 && to < (int)playlist.size()) {
                    savePos();
                    index = to;
                    start();
                    return;
                }
            }
            if (g_in.toggleUi) {
                m_controls = !m_controls;
                m_touchedAt = g_ticks;
            }
            if (g_in.playPause) togglePause();
            if (g_in.seekX) {
                v.seek(g_in.seekX * 10, true, false);
                flash(g_in.seekX < 0 ? "\xE2\x88\x92" "10 s" : "+10 s", g_in.seekX);
                // the controls up a second (long enough to read where it went), then fading
                if (!v.paused()) {
                    m_controls = true;
                    m_touchedAt = g_ticks - 3000;  // (they tuck away 4 s after a touch)
                }
            }
        }
        if (m_controls && !v.paused() && !ended && g_ticks - m_touchedAt > 4000 && !m_seeking) m_controls = false;
        if (g_in.down) m_touchedAt = g_ticks;
    }

    void exit() {
        savePos();
        Video::get().stop();
        closed = true;
    }

    // auto-advance at the end of a video
    void tick() {
        Video& v = Video::get();
        if (closed || !v.loaded() || !v.ended()) return;
        if (cfgBool("media.autonext", true) && index + 1 < (int)playlist.size() && g_ticks - m_endedAt > 4000 && m_wasEnded) {
            index++;
            start();
        }
    }

private:
    bool m_controls = true, m_tracks = false, m_adjust = false, m_seeking = false, m_wasEnded = false;
    uint32_t m_touchedAt = 0, m_savedAt = 0, m_flashAt = 0, m_endedAt = 0;
    float m_controlsA = 1;  // the controls' fade (0..1)
    float m_seekV = 0, m_zoom = 1, m_panX = 0, m_panY = 0;
    int m_rotate = 0, m_flashSide = 1, m_aspect = 0;
    std::string m_flash;
    Scroller m_trackScroll;

    void flash(const std::string& s, int side) { m_flash = s; m_flashAt = g_ticks; m_flashSide = side; }

    void start() {
        Video& v = Video::get();
        const std::string& path = playlist[index].path;
        int64_t pos = 0, total = 0;
        double startSec = 0;
        if (posGet(path, pos, total) && total > 0 && pos > 10000 && pos < total * 95 / 100) startSec = pos / 1000.0;
        spotifyPauseForVideo();
        notePlayed(path);
        v.load(path, startSec, Video::MEDIA);
        if (startSec > 0) toast("Resuming at " + formatTime(pos));
        // the controls flash up and fade straight away (a tap brings them back)
        m_controls = false;
        m_controlsA = 1;
        m_touchedAt = g_ticks;
        m_zoom = 1;
        m_panX = m_panY = 0;
        m_rotate = 0;
        m_wasEnded = false;
        m_savedAt = g_ticks;
    }

    void savePos(bool finished = false) {
        Video& v = Video::get();
        m_savedAt = g_ticks;
        if (!v.loaded() || playlist.empty() || v.path() != playlist[index].path) return;
        int64_t dur = (int64_t)(v.duration() * 1000), pos = (int64_t)(v.position() * 1000);
        if (dur <= 0) return;
        posSet(playlist[index].path, finished ? dur : pos, dur);
    }

    void togglePause() {
        Video& v = Video::get();
        if (v.ended()) { v.seek(0, false); v.setPause(false); return; }
        v.setPause(!v.paused());
        if (v.paused()) savePos();
        // paused: the controls come up; playing again: they fade away
        m_controls = v.paused();
        m_touchedAt = g_ticks;
    }

    void drawControls(bool ended) {
        Video& v = Video::get();
        SDL_Rect top = {0, 0, g_W, 110}, bottom = {0, g_H - 150, g_W, 150};
        fillGradientV(top, SDL_Color{0, 0, 0, 140}, SDL_Color{0, 0, 0, 0});
        fillGradientV(bottom, SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 160});
        if (iconButton(48, 46, 30, IC_BACK, g_theme.fg, SDL_Color{0, 0, 0, 0})) { exit(); return; }
        drawText(playlist[index].title, 96, 30, 26, g_theme.fg, 0, g_W - 96 - 260, FS_BOLD);
        if (iconButton(g_W - 120.0f, 46, 30, IC_SUBS, g_theme.fg, SDL_Color{0, 0, 0, 0})) { m_tracks = true; m_trackScroll = Scroller(); }
        if (iconButton(g_W - 48.0f, 46, 30, IC_GEAR, g_theme.fg, SDL_Color{0, 0, 0, 0})) m_adjust = true;

        float cy = g_H / 2.0f;
        if (iconButton(g_W / 2.0f - 170, cy, 40, IC_RWD, g_theme.fg, SDL_Color{0, 0, 0, 80})) { v.seek(-10, true, false); m_touchedAt = g_ticks; }
        if (iconButton(g_W / 2.0f, cy, 56, ended ? IC_REFRESH : v.paused() ? IC_PLAY : IC_PAUSE, g_theme.fg, SDL_Color{0, 0, 0, 100}, 0.5f)) {
            togglePause();
            m_touchedAt = g_ticks;
        }
        if (iconButton(g_W / 2.0f + 170, cy, 40, IC_FWD, g_theme.fg, SDL_Color{0, 0, 0, 80})) { v.seek(10, true, false); m_touchedAt = g_ticks; }
        if (playlist.size() > 1) {
            if (index > 0 && iconButton(g_W / 2.0f - 300, cy, 30, IC_PREV, g_theme.fg, SDL_Color{0, 0, 0, 90})) { savePos(); index--; start(); }
            if (index + 1 < (int)playlist.size() && iconButton(g_W / 2.0f + 300, cy, 30, IC_NEXT, g_theme.fg, SDL_Color{0, 0, 0, 90})) {
                savePos();
                index++;
                start();
            }
        }
        if (ended && index + 1 < (int)playlist.size()) drawText("Up next: " + playlist[index + 1].title, g_W / 2, (int)cy + 80, 22, g_theme.fg, 1, g_W - 200);

        double dur = v.duration();
        if (!m_seeking) m_seekV = dur > 0 ? (float)(v.position() / dur) : 0;
        SDL_Rect bar = {150, g_H - 62, g_W - 300, 8};
        bool rel = false;
        bool ch = slider(bar, m_seekV, g_theme.accent, withAlpha(g_theme.fg, 70), &rel);
        if (ch && dur > 0) {
            m_seeking = !rel;
            v.seek(m_seekV * dur, false, rel);  // keyframe seeks while dragging, exact on release
            m_touchedAt = g_ticks;
            if (rel && !v.paused()) m_touchedAt = g_ticks - 3000;  // seeked: away in a second (still up while paused)
        }
        drawText(formatTime((int64_t)((m_seeking ? m_seekV * dur : v.position()) * 1000)), 130, g_H - 72, 22, g_theme.fg, 2);
        drawText(formatTime((int64_t)(dur * 1000)), g_W - 130, g_H - 72, 22, g_theme.fg, 0);
        if (g_in.tap && !g_in.consumed && (inRect(g_in.x, g_in.y, top) || inRect(g_in.x, g_in.y, bottom))) g_in.consumed = true;
    }

    void drawTracks() {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        Video& v = Video::get();
        SDL_Rect r = {g_W - 560, 0, 560, g_H};
        fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 30});
        fillRect(r, SDL_Color{18, 18, 22, 175});
        drawText("Subtitles & audio", r.x + 32, 28, 28, g_theme.fg, 0, 0, FS_BOLD);
        if (iconButton(r.x + r.w - 46.0f, 46, 26, IC_CLOSE, g_theme.fg, SDL_Color{0, 0, 0, 0})) m_tracks = false;
        auto tr = v.tracks();
        SDL_Rect area = {r.x, 90, r.w, g_H - 90 - 170};
        int rowH = 60;
        struct Row { bool head; std::string text, type; int64_t id; bool sel; };
        std::vector<Row> rows;
        bool anySub = false;
        for (auto& t : tr)
            if (t.type == "sub" && t.selected) anySub = true;
        rows.push_back({true, "Subtitles", "", 0, false});
        rows.push_back({false, "Off", "sub", 0, !anySub});
        for (auto& t : tr)
            if (t.type == "sub") rows.push_back({false, t.label(), "sub", t.id, t.selected});
        rows.push_back({true, "Audio", "", 0, false});
        for (auto& t : tr)
            if (t.type == "audio") rows.push_back({false, t.label(), "audio", t.id, t.selected});
        float contentH = rows.size() * (float)rowH;
        m_trackScroll.update(area, contentH);
        pushClip(area);
        for (size_t i = 0; i < rows.size(); i++) {
            int y = area.y + (int)i * rowH - (int)m_trackScroll.pos;
            if (y + rowH < area.y || y > area.y + area.h) continue;
            const Row& row = rows[i];
            if (row.head) { drawText(row.text, r.x + 32, y + 26, 20, g_theme.dim, 0, 0, FS_BOLD); continue; }
            SDL_Rect rr = {r.x + 16, y + 4, r.w - 32, rowH - 8};
            if (row.sel) fillRound(rr, 12, withAlpha(g_theme.accent, 50));
            else if (pressing(rr)) fillRound(rr, 12, g_theme.panel2);
            if (row.sel) drawIcon(IC_CHECK, rr.x + 26.0f, rr.y + rr.h / 2.0f, 24, g_theme.accent);
            drawText(row.text, rr.x + 56, rr.y + 14, 22, g_theme.fg, 0, rr.w - 70);
            if (tapped(rr)) v.selectTrack(row.type, row.id);
        }
        popClip();
        int y = g_H - 160;
        int scale = cfgInt("media.sub_scale", 100);
        int d = stepper({r.x + 32, y, r.w - 64, 70}, "Subtitle size", std::to_string(scale) + "%");
        if (d) {
            scale = SDL_clamp(scale + d * 10, 50, 250);
            cfgSetInt("media.sub_scale", scale);
            v.setDouble("sub-scale", scale / 100.0);
        }
        double delay = v.getDouble("sub-delay");
        char buf[32];
        snprintf(buf, sizeof(buf), "%+.1f s", delay);
        d = stepper({r.x + 32, y + 76, r.w - 64, 70}, "Subtitle delay", buf);
        if (d) v.setDouble("sub-delay", delay + d * 0.1);
        if (g_in.back || (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r))) m_tracks = false;
        g_in.consumed = true;
        m_touchedAt = g_ticks;
    }

    void drawAdjust() {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        Video& v = Video::get();
        SDL_Rect r = {g_W - 560, 0, 560, g_H};
        fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 30});
        fillRect(r, SDL_Color{18, 18, 22, 175});
        drawText("Picture", r.x + 32, 28, 28, g_theme.fg, 0, 0, FS_BOLD);
        if (iconButton(r.x + r.w - 46.0f, 46, 26, IC_CLOSE, g_theme.fg, SDL_Color{0, 0, 0, 0})) m_adjust = false;
        int x = r.x + 32, y = 100, w = r.w - 64;
        drawText("Rotate", x, y + 16, 22, g_theme.fg);
        if (button({x + w - 200, y, 90, 56}, "", IC_ROTL)) { m_rotate = (m_rotate + 270) % 360; v.setString("video-rotate", std::to_string(m_rotate)); }
        if (button({x + w - 90, y, 90, 56}, "", IC_ROTR)) { m_rotate = (m_rotate + 90) % 360; v.setString("video-rotate", std::to_string(m_rotate)); }
        y += 80;
        drawText("Size", x, y + 16, 22, g_theme.fg);
        double pan = v.getDouble("panscan");
        if (chip({x + w - 330, y, 100, 52}, "Fit", pan < 0.5 && m_zoom <= 1.01f, 20)) {
            v.setDouble("panscan", 0);
            m_zoom = 1;
            m_panX = m_panY = 0;
            v.setDouble("video-zoom", 0);
            v.setDouble("video-pan-x", 0);
            v.setDouble("video-pan-y", 0);
        }
        if (chip({x + w - 220, y, 100, 52}, "Fill", pan >= 0.5, 20)) v.setDouble("panscan", 1.0);
        if (chip({x + w - 110, y, 110, 52}, "Zoom+", m_zoom > 1.01f, 20)) { m_zoom = std::min(6.0f, m_zoom * 1.25f); v.setDouble("video-zoom", log2(m_zoom)); }
        y += 80;
        drawText("Aspect", x, y + 16, 22, g_theme.fg);
        y += 52;
        static const char* aspects[] = {"no", "16:9", "4:3", "2.35:1", "1:1"};
        static const char* labels[] = {"Auto", "16:9", "4:3", "2.35", "1:1"};
        int ax = x;
        for (int i = 0; i < 5; i++) {
            if (chip({ax, y, 92, 48}, labels[i], m_aspect == i, 20)) { m_aspect = i; v.setString("video-aspect-override", aspects[i]); }
            ax += 100;
        }
        y += 76;
        drawText("Speed", x, y + 16, 22, g_theme.fg);
        y += 52;
        static const double speeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
        double sp = v.getDouble("speed", 1.0);
        int sx = x;
        for (double s : speeds) {
            char b[16];
            snprintf(b, sizeof(b), "%gx", s);
            if (chip({sx, y, 76, 48}, b, fabs(sp - s) < 0.01, 20)) v.setDouble("speed", s);
            sx += 82;
        }
        y += 80;
        drawText("Volume", x, y + 16, 22, g_theme.fg);
        float vol = (float)(v.getDouble("volume", 100) / 100.0);
        bool volDone = false;
        if (slider({x + 140, y + 24, w - 160, 8}, vol, g_theme.accent, g_theme.faint, &volDone)) v.setDouble("volume", vol * 100);
        if (volDone) cfgSetInt("media.volume", (int)(vol * 100 + 0.5));  // the next video starts at it too
        y += 80;
        bool autoNext = cfgBool("media.autonext", true);
        drawText("Play next video automatically", x, y + 16, 22, g_theme.fg);
        if (toggle({x, y, w, 60}, autoNext)) cfgSetBool("media.autonext", autoNext);
        if (g_in.back || (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r))) m_adjust = false;
        g_in.consumed = true;
        m_touchedAt = g_ticks;
    }
};

// ---------------------------------------------------------------- tab

class MediaTab : public Tab {
public:
    const char* name() override { return "Media"; }
    int icon() override { return IC_VIDEO; }
    SDL_Color accent() override { return rgb(0xFF8A65); }
    bool fullscreen() override { return m_playing; }
    bool keepAwake() override { return m_playing; }
    std::string badge() override { return m_playing && !Video::get().paused() ? "playing" : ""; }
    void enter() override { refresh(); }
    void leave() override {
        if (m_playing) Video::get().setPause(true);
    }
    void shutdown() override {
        m_video.shutdown();
        Video::get().shutdown();
    }
    void background() override { Video::get().poll(); }
    void preRender() override {
        if (m_playing) Video::get().render();
    }
    void update(const SDL_Rect&) override {
        if (m_playing) {
            m_video.tick();
            if (m_video.closed) { m_playing = false; posFlush(); }
        }
    }
    void draw(const SDL_Rect& area) override {
        refresh();
        if (m_playing) {
            m_video.draw();
            if (m_video.closed) { m_playing = false; posFlush(); }
            return;
        }
        if (!Library::get().ready() || (m_shows.empty() && m_movies.empty() && m_roots.empty())) {
            drawText(name(), area.x + 32, 26, 36, g_theme.fg, 0, 0, FS_BOLD);
            drawNoLibrary(area, IC_VIDEO, "Media");
            return;
        }
        if (m_page == SHOW && m_show >= 0 && m_show < (int)m_shows.size()) drawShow(area, m_shows[m_show]);
        else if (m_page == MOVIE && m_movie >= 0 && m_movie < (int)m_movies.size()) drawMovie(area, m_movies[m_movie]);
        else {
            m_page = HOME;
            drawHomeHeader(area);
            if (folderMode()) drawFolders({area.x, 96, area.w, area.h - 96});
            else drawHome({area.x, 96, area.w, area.h - 96});
        }
    }

private:
    enum Page { HOME, SHOW, MOVIE };
    Page m_page = HOME;
    int m_show = -1, m_movie = -1, m_season = 0;
    bool m_playing = false;
    VideoView m_video;
    Scroller m_homeScroll, m_pageScroll, m_folderScroll;
    uint32_t m_version = 0;
    std::vector<LibShow> m_shows;
    std::vector<LibMovie> m_movies;
    std::vector<LibRoot> m_roots;
    std::map<std::string, std::pair<int, int>> m_epAt;  // path -> show, episode
    std::map<std::string, int> m_movieAt;               // path -> movie
    // folders view
    int64_t m_dir = 0;
    LibItem m_dirItem;
    std::string m_dirPath;  // the open folder, by path: ids change with every library
    std::vector<LibItem> m_entries;

    bool folderMode() const { return cfgBool("videos.folders", false); }

    void refresh() {
        uint32_t v = Library::get().version();
        if (v == m_version) return;
        m_version = v;
        // the page on screen, found again in the new library by its title
        std::string showTitle = m_page == SHOW && m_show >= 0 && m_show < (int)m_shows.size() ? m_shows[m_show].title : "";
        std::string moviePath = m_page == MOVIE && m_movie >= 0 && m_movie < (int)m_movies.size() ? m_movies[m_movie].path : "";
        m_shows = Library::get().shows();
        m_movies = Library::get().movies();
        m_roots = Library::get().roots(lib::C_MEDIA);
        m_epAt.clear();
        m_movieAt.clear();
        for (int s = 0; s < (int)m_shows.size(); s++)
            for (int e = 0; e < (int)m_shows[s].episodes.size(); e++) m_epAt[m_shows[s].episodes[e].path] = {s, e};
        for (int m = 0; m < (int)m_movies.size(); m++) m_movieAt[m_movies[m].path] = m;
        m_show = m_movie = -1;
        for (int s = 0; s < (int)m_shows.size(); s++)
            if (m_shows[s].title == showTitle) m_show = s;
        if (!moviePath.empty() && m_movieAt.count(moviePath)) m_movie = m_movieAt[moviePath];
        if ((m_page == SHOW && m_show < 0) || (m_page == MOVIE && m_movie < 0)) m_page = HOME;
        LibItem it;
        if (!m_dirPath.empty() && Library::get().itemByPath(m_dirPath, it) && it.kind == lib::K_DIR) openDir(it.id, true);
        else openDir(home(), true);
    }

    std::vector<PlayItem> showPlaylist(const LibShow& s) {
        std::vector<PlayItem> v;
        for (auto& e : s.episodes) v.push_back({e.path, epTitle(s, e)});
        return v;
    }

    void play(const std::vector<PlayItem>& list, int index) {
        if (list.empty() || index < 0 || index >= (int)list.size()) return;
        m_video.open(list, index);
        m_playing = true;
    }

    // The episode to watch next: one in progress, else the first not finished.
    int nextUp(const LibShow& s, float* progress) {
        for (int i = 0; i < (int)s.episodes.size(); i++) {
            if (s.episodes[i].seasonNumber == 0) continue;  // specials don't drive "next"
            float f = progressOf(s.episodes[i].path);
            if (f < 0.95f) {
                if (progress) *progress = f;
                return i;
            }
        }
        if (progress) *progress = -1;
        return s.episodes.empty() ? -1 : 0;
    }

    void drawHomeHeader(const SDL_Rect& area) {
        int x0 = area.x + 32, right = area.x + area.w - 32;
        bool sub = folderMode() && m_dir != home();
        if (sub && (iconButton(x0 + 22.0f, 46, 28, IC_BACK, g_theme.fg, g_theme.panel2) || g_in.back)) {
            openDir(m_dirItem.parent ? m_dirItem.parent : home());
            return;
        }
        int tx = sub ? x0 + 64 : x0;
        drawText(sub ? m_dirItem.name : name(), tx, 26, 36, g_theme.fg, 0, area.w - 420 - (tx - x0), FS_BOLD);
        bool folders = folderMode();
        if (chip({right - 150, 26, 150, 44}, "Folders", folders, 20) && !folders) cfgSetBool("videos.folders", true);
        if (chip({right - 306, 26, 146, 44}, "Library", !folders, 20) && folders) cfgSetBool("videos.folders", false);
    }

    void drawHome(const SDL_Rect& area) {
        int x0 = area.x + 24, w = area.w - 48;
        pushClip(area);
        int y = area.y + 8 - (int)m_homeScroll.pos;
        bool fresh = false;  // a tap opened something: stop handling taps this frame

        // continue watching: recently played and unfinished, or the next episode after a finished one
        struct Card { int64_t art; std::string title, sub; float progress; std::vector<PlayItem> list; int index; std::string recent; };
        std::vector<Card> cards;
        std::vector<int> seenShows;
        for (const std::string& p : recentVideos()) {
            if ((int)cards.size() >= 8) break;
            float pr = progressOf(p);
            auto ep = m_epAt.find(p);
            if (ep != m_epAt.end()) {
                int si = ep->second.first, ei = ep->second.second;
                if (std::find(seenShows.begin(), seenShows.end(), si) != seenShows.end()) continue;
                seenShows.push_back(si);
                const LibShow& s = m_shows[si];
                if (pr >= 0.95f) {  // finished: offer the next one
                    if (ei + 1 >= (int)s.episodes.size()) continue;
                    ei++;
                    pr = progressOf(s.episodes[ei].path);
                }
                const LibEpisode& e = s.episodes[ei];
                cards.push_back({e.still ? e.still : s.backdrop, s.title, epLabel(e) + (e.title.empty() ? "" : "  \xC2\xB7  " + e.title), pr,
                                 showPlaylist(s), ei, p});
                continue;
            }
            auto mv = m_movieAt.find(p);
            if (mv == m_movieAt.end() || pr < 0 || pr >= 0.95f) continue;
            const LibMovie& m = m_movies[mv->second];
            cards.push_back({m.backdrop ? m.backdrop : m.thumb, m.title, "Movie", pr, {{m.path, m.title}}, 0, p});
        }
        if (!cards.empty()) {
            drawText("Continue watching", x0, y, 26, g_theme.fg, 0, 0, FS_BOLD);
            if (chip({x0 + w - 100, y - 4, 100, 40}, "Clear", false, 18) && !fresh) {
                fresh = true;
                dialog("Clear Continue watching?", "Where you left each one is kept.", {"Cancel", "Clear"},
                       [](int b) { if (b == 1) saveRecent({}); });
            }
            y += 44;
            int cw = 262, ch = cw * 9 / 16, gap = 16, cols = std::max(1, (w + gap) / (cw + gap));
            for (int i = 0; i < (int)cards.size() && i < cols * 2; i++) {
                int cx = x0 + (i % cols) * (cw + gap), cy = y + (i / cols) * (ch + 74);
                SDL_Rect r = {cx, cy, cw, ch};
                if (!drawPicture(cards[i].art, r, 12, pressing(r) ? 170 : 255)) fillRound(r, 12, g_theme.panel2);
                fillCircle(cx + 30.0f, cy + ch - 30.0f, 20, SDL_Color{0, 0, 0, 150});
                drawIcon(IC_PLAY, cx + 31.0f, cy + ch - 30.0f, 20, g_theme.fg);
                progressBar({cx + 10, cy + ch - 8, cw - 20, 4}, cards[i].progress);
                // off the row (handled before the card, so the tap doesn't also play it)
                if (iconButton(cx + cw - 22.0f, cy + 22.0f, 16, IC_CLOSE, g_theme.fg, SDL_Color{0, 0, 0, 150}) && !fresh) {
                    forgetPlayed(cards[i].recent);
                    fresh = true;
                }
                drawText(cards[i].title, cx + 2, cy + ch + 8, 20, g_theme.fg, 0, cw - 4, FS_BOLD);
                drawText(cards[i].sub, cx + 2, cy + ch + 36, 17, g_theme.dim, 0, cw - 4);
                if (tapped(r) && !fresh) { play(cards[i].list, cards[i].index); fresh = true; }
            }
            int rows = ((int)std::min<size_t>(cards.size(), cols * 2) + cols - 1) / cols;
            y += rows * (ch + 74) + 16;
        }

        // poster grids
        auto grid = [&](const char* title, int count, const std::function<void(int, const SDL_Rect&)>& tile) {
            if (!count) return;
            drawText(title, x0, y, 26, g_theme.fg, 0, 0, FS_BOLD);
            drawText(std::to_string(count), x0 + textWidth(title, 26, FS_BOLD) + 12, y + 6, 20, g_theme.dim);
            y += 44;
            int tw = 168, gap = 18, cols = std::max(1, (w + gap) / (tw + gap));
            tw = (w - (cols - 1) * gap) / cols;
            int th = tw * 3 / 2, tileH = th + 62;
            for (int i = 0; i < count; i++) {
                int ty = y + (i / cols) * (tileH + 10);
                if (ty + tileH < area.y || ty > area.y + area.h) continue;
                tile(i, {x0 + (i % cols) * (tw + gap), ty, tw, th});
            }
            y += ((count + cols - 1) / cols) * (tileH + 10) + 16;
        };
        // no poster: a frame from it, dimmed, with the name on it
        auto posterOr = [](int64_t poster, int64_t frame, const std::string& title, const SDL_Rect& r, bool down) {
            if (drawPicture(poster, r, 12, down ? 170 : 255)) return;
            fillRound(r, 12, g_theme.panel2);
            drawPicture(frame, r, 12, down ? 110 : 150);
            drawWrapped(title, r.x + 12, r.y + r.h / 2 - 30, 20, g_theme.fg, r.w - 24, 3, 1, FS_BOLD);
        };
        grid("Shows", (int)m_shows.size(), [&](int i, const SDL_Rect& r) {
            const LibShow& s = m_shows[i];
            bool down = pressing(r);
            posterOr(s.poster, s.backdrop ? s.backdrop : s.episodes.empty() ? 0 : s.episodes[0].still, s.title, r, down);
            drawText(s.title, r.x + 2, r.y + r.h + 8, 19, g_theme.fg, 0, r.w - 4, FS_BOLD);
            int eps = s.episodeCount;
            drawText(std::to_string(eps) + (eps == 1 ? " episode" : " episodes") + (s.year ? "  \xC2\xB7  " + std::to_string(s.year) : ""),
                     r.x + 2, r.y + r.h + 34, 16, g_theme.dim, 0, r.w - 4);
            if (tapped(r) && !fresh) {
                m_page = SHOW;
                m_show = i;
                m_season = 0;
                m_pageScroll = Scroller();
                fresh = true;
            }
        });
        grid("Movies", (int)m_movies.size(), [&](int i, const SDL_Rect& r) {
            const LibMovie& m = m_movies[i];
            bool down = pressing(r);
            posterOr(m.poster, m.backdrop ? m.backdrop : m.thumb, m.title, r, down);
            progressBar({r.x + 10, r.y + r.h - 10, r.w - 20, 4}, progressOf(m.path));
            drawText(m.title, r.x + 2, r.y + r.h + 8, 19, g_theme.fg, 0, r.w - 4, FS_BOLD);
            if (m.year) drawText(std::to_string(m.year), r.x + 2, r.y + r.h + 34, 16, g_theme.dim);
            if (tapped(r) && !fresh) {
                m_page = MOVIE;
                m_movie = i;
                m_pageScroll = Scroller();
                fresh = true;
            }
        });
        float contentH = (float)(y + (int)m_homeScroll.pos - area.y) + 24;
        popClip();
        m_homeScroll.update(area, contentH);
        drawScrollbar(area, contentH, m_homeScroll.pos);
    }

    // The top of a show or movie page: backdrop, poster, title, details, overview. Returns the y below it.
    int hero(const SDL_Rect& area, int y, int64_t backdrop, int64_t poster, const std::string& title, const std::string& meta,
             const std::string& overview) {
        SDL_Rect bd = {area.x, y, area.w, 340};
        if (!drawPicture(backdrop, bd, 0, 150)) fillRect(bd, g_theme.panel);
        fillGradientV({bd.x, bd.y + bd.h - 200, bd.w, 200}, withAlpha(g_theme.bg, 0), g_theme.bg);
        fillGradientV({bd.x, bd.y, bd.w, 110}, SDL_Color{0, 0, 0, 120}, SDL_Color{0, 0, 0, 0});
        int px = area.x + 32, pw = 180, ph = 270, py = y + 96;
        if (!drawPicture(poster, {px, py, pw, ph}, 0)) {
            fillRect({px, py, pw, ph}, g_theme.panel2);
            drawIcon(IC_VIDEO, px + pw / 2.0f, py + ph / 2.0f, 56, g_theme.faint);
        }
        int tx = px + pw + 28, tw = area.x + area.w - tx - 32;
        int ty = y + 120;
        ty += drawWrapped(title, tx, ty, 36, g_theme.fg, tw, 2, 0, FS_BOLD) + 4;
        drawText(meta, tx, ty, 20, g_theme.dim, 0, tw);
        ty += 34;
        if (!overview.empty()) ty += drawWrapped(overview, tx, ty, 19, g_theme.fg, tw, 5);
        return std::max(py + ph, ty) + 20;
    }

    // The back button floats over the page, which scrolls under it: its tap is
    // claimed before the page is drawn (the episode or Resume button under it
    // used to take it). The button itself is drawn last, on top.
    bool backClaimed(const SDL_Rect& area) {
        SDL_Rect r = {area.x + 50 - 34, area.y + 46 - 34, 68, 68};
        if (g_in.consumed || !inRect(g_in.sx, g_in.sy, r)) return false;
        if (g_in.tap && inRect(g_in.x, g_in.y, r)) {
            consumeInput();
            m_page = HOME;
            return true;
        }
        // a finger resting on it: nothing under it lights up
        if (g_in.down && !g_in.moved) m_backHeld = true;
        return false;
    }
    bool m_backHeld = false;  // this frame: the back button has the touch (drawn pressed, last)

    void drawShow(const SDL_Rect& area, const LibShow& s) {
        if (backClaimed(area)) return;
        bool backHeld = m_backHeld, wasConsumed = g_in.consumed;
        m_backHeld = false;
        if (backHeld) g_in.consumed = true;  // the page under it neither lights up nor takes it
        pushClip(area);
        int y = area.y - (int)m_pageScroll.pos;
        int count = s.episodeCount;
        std::string meta = (s.year ? std::to_string(s.year) + "  \xC2\xB7  " : "") + std::to_string(count) + (count == 1 ? " episode" : " episodes");
        y = hero(area, y, s.backdrop, s.poster, s.title, meta, s.overview);
        int x0 = area.x + 32, w = area.w - 64;
        float pr = -1;
        int next = nextUp(s, &pr);
        bool fresh = false;
        if (next >= 0) {
            std::string label = (pr > 0 ? "Resume " : "Play ") + epLabel(s.episodes[next]);
            if (button({x0, y, 300, 60}, label, IC_PLAY, BTN_ACCENT, 20)) {
                play(showPlaylist(s), next);
                fresh = true;
            }
        }
        y += 80;
        // seasons
        m_season = SDL_clamp(m_season, 0, std::max(0, (int)s.seasons.size() - 1));
        if (s.seasons.size() > 1) {
            int cx = x0;
            for (int i = 0; i < (int)s.seasons.size(); i++) {
                std::string n = s.seasons[i].title;
                int cw = textWidth(n, 20) + 40;
                if (cx + cw > x0 + w) { cx = x0; y += 58; }
                if (chip({cx, y, cw, 46}, n, i == m_season, 20)) m_season = i;
                cx += cw + 10;
            }
            y += 66;
        }
        // episodes of the chosen season
        if (!s.seasons.empty()) {
            int64_t seasonId = s.seasons[m_season].id;
            const int rowH = 196;
            for (int i = 0; i < (int)s.episodes.size(); i++) {
                const LibEpisode& e = s.episodes[i];
                if (e.season != seasonId) continue;
                SDL_Rect row = {x0, y, w, rowH - 10};
                if (y + rowH > area.y && y < area.y + area.h) {
                    if (pressing(row)) fillRound(row, 14, g_theme.panel2);
                    SDL_Rect th = {row.x + 6, row.y + 6, 304, 171};
                    if (!drawPicture(e.still, th, 10)) fillRound(th, 10, g_theme.panel2);
                    float fp = progressOf(e.path);
                    progressBar({th.x + 8, th.y + th.h - 8, th.w - 16, 4}, fp);
                    int tx = th.x + th.w + 20, tw = row.x + row.w - tx - 12;
                    std::string head = std::to_string(e.number) + ". " + (e.title.empty() ? fileStem(baseName(e.path)) : e.title);
                    drawText(head, tx, row.y + 10, 21, fp >= 0.95f ? g_theme.dim : g_theme.fg, 0, tw, FS_BOLD);
                    std::string sub = epLabel(e) + (e.duration > 0 ? "  \xC2\xB7  " + formatTime(e.duration) : "") +
                                      (fp >= 0.95f ? "  \xC2\xB7  watched" : fp > 0 ? "  \xC2\xB7  " + std::to_string((int)(fp * 100)) + "%" : "");
                    drawText(sub, tx, row.y + 40, 16, g_theme.dim, 0, tw);
                    if (!e.overview.empty()) drawWrapped(e.overview, tx, row.y + 66, 16, g_theme.dim, tw, 2);
                    if (tapped(row) && !fresh) {
                        play(showPlaylist(s), i);
                        fresh = true;
                    }
                }
                y += rowH;
            }
        }
        float contentH = (float)(y + (int)m_pageScroll.pos - area.y) + 24;
        popClip();
        m_pageScroll.update(area, contentH);
        drawScrollbar(area, contentH, m_pageScroll.pos);
        if (backHeld) g_in.consumed = wasConsumed;
        if (iconButton(area.x + 50.0f, area.y + 46.0f, 28, IC_BACK, g_theme.fg, SDL_Color{0, 0, 0, 110}) || g_in.back) m_page = HOME;
    }

    void drawMovie(const SDL_Rect& area, const LibMovie& m) {
        if (backClaimed(area)) return;
        bool backHeld = m_backHeld, wasConsumed = g_in.consumed;
        m_backHeld = false;
        if (backHeld) g_in.consumed = true;  // the page under it neither lights up nor takes it
        pushClip(area);
        int y = area.y - (int)m_pageScroll.pos;
        std::string meta;
        if (m.year) meta += std::to_string(m.year);
        int mins = m.runtime ? m.runtime : (int)(m.duration / 60000);
        if (mins) meta += (meta.empty() ? "" : "  \xC2\xB7  ") + std::to_string(mins / 60) + "h " + std::to_string(mins % 60) + "m";
        if (meta.empty()) meta = baseName(m.path);
        y = hero(area, y, m.backdrop, m.poster, m.title, meta, m.overview);
        int x0 = area.x + 32;
        float pr = progressOf(m.path);
        if (button({x0, y, 260, 60}, pr > 0 && pr < 0.95f ? "Resume" : "Play", IC_PLAY, BTN_ACCENT, 20)) play({{m.path, m.title}}, 0);
        progressBar({x0, y + 76, 516, 6}, pr);
        y += 110;
        drawText(m.path.substr(std::string(APP_DIR).size() + 1), x0, y, 16, g_theme.faint, 0, area.w - 64);
        y += 40;
        float contentH = (float)(y + (int)m_pageScroll.pos - area.y);
        popClip();
        m_pageScroll.update(area, contentH);
        if (backHeld) g_in.consumed = wasConsumed;
        if (iconButton(area.x + 50.0f, area.y + 46.0f, 28, IC_BACK, g_theme.fg, SDL_Color{0, 0, 0, 110}) || g_in.back) m_page = HOME;
    }

    // ---------------------------------------------------------------- folders

    int64_t home() const { return m_roots.size() == 1 ? m_roots[0].item : 0; }

    void openDir(int64_t dir, bool keepScroll = false) {
        m_dir = dir;
        m_dirItem = LibItem();
        m_entries.clear();
        if (dir && Library::get().item(dir, m_dirItem)) m_entries = Library::get().children(dir);
        else {
            m_dir = 0;
            for (auto& r : m_roots) {
                LibItem it;
                if (Library::get().item(r.item, it)) m_entries.push_back(it);
            }
        }
        if (!keepScroll) m_folderScroll = Scroller();
        if (Library::get().ready()) m_dirPath = m_dir ? m_dirItem.path : "";  // kept while a sync has the library closed
    }

    // The title a video gets in the player: its episode or movie name when it has one.
    std::string titleFor(const LibItem& e) {
        auto ep = m_epAt.find(e.path);
        if (ep != m_epAt.end()) return epTitle(m_shows[ep->second.first], m_shows[ep->second.first].episodes[ep->second.second]);
        auto mv = m_movieAt.find(e.path);
        if (mv != m_movieAt.end()) return m_movies[mv->second].title;
        return fileStem(e.name);
    }

    // List of the folder: names, durations and watch progress.
    void drawFolders(const SDL_Rect& area) {
        const int rowH = 116;
        float contentH = m_entries.size() * (float)rowH + 24;
        m_folderScroll.update(area, contentH);
        pushClip(area);
        for (int i = 0; i < (int)m_entries.size(); i++) {
            int y = area.y + 4 + i * rowH - (int)m_folderScroll.pos;
            if (y + rowH < area.y || y > area.y + area.h) continue;
            const LibItem& e = m_entries[i];
            SDL_Rect row = {area.x + 24, y, area.w - 48, rowH - 8};
            fillRound(row, 14, pressing(row) ? g_theme.line : g_theme.panel);
            SDL_Rect th = {row.x + 8, row.y + 8, 124, row.h - 16};
            if (!drawPicture(e.thumb, th, 10, e.kind == lib::K_DIR ? 150 : 255)) fillRound(th, 10, g_theme.panel2);
            if (e.kind == lib::K_DIR) drawIcon(IC_FOLDER, th.x + th.w / 2.0f, th.y + th.h / 2.0f, 40, g_theme.accent);
            int tx = th.x + th.w + 20, tw = row.x + row.w - tx - 20;
            std::string sub;
            float watched = -1;
            if (e.kind == lib::K_DIR) sub = std::to_string(e.children) + (e.children == 1 ? " item" : " items");
            else {
                if (e.duration > 0) sub = formatTime(e.duration) + "  \xC2\xB7  ";
                sub += formatBytes((double)e.size);
                watched = progressOf(e.path);
                if (watched >= 0) sub += watched > 0.95f ? "  \xC2\xB7  watched" : "  \xC2\xB7  " + std::to_string((int)(watched * 100)) + "% watched";
            }
            std::string nm = e.kind == lib::K_DIR ? e.name : fileStem(e.name);
            int lines = std::min(2, (int)wrapText(nm, 22, tw).size());
            int blockH = lines * fontHeight(22) + 4 + fontHeight(18) + (watched >= 0 ? 16 : 0);
            int ty = row.y + std::max(6, (row.h - blockH) / 2);
            ty += drawWrapped(nm, tx, ty, 22, g_theme.fg, tw, 2);
            drawText(sub, tx, ty + 4, 18, g_theme.dim, 0, tw);
            if (watched >= 0) progressBar({tx, ty + 4 + fontHeight(18) + 12, tw, 4}, watched);
            if (tapped(row)) {
                if (e.kind == lib::K_DIR) { openDir(e.id); popClip(); return; }
                // videos play on into the next one in the folder
                std::vector<PlayItem> list;
                int at = 0;
                for (auto& o : m_entries) {
                    if (o.kind != lib::K_VIDEO) continue;
                    if (o.id == e.id) at = (int)list.size();
                    list.push_back({o.path, titleFor(o)});
                }
                play(list, at);
                popClip();
                return;
            }
        }
        popClip();
        drawScrollbar(area, contentH, m_folderScroll.pos);
        if (m_entries.empty()) drawText("This folder is empty.", area.x + area.w / 2, area.y + area.h / 2, 24, g_theme.dim, 1);
    }
};

}  // namespace

Tab* makeMediaTab() { return new MediaTab(); }
