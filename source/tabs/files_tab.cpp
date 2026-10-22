// Files tab: the folders synced as Files, as the PC laid them out, with the
// photo viewer (pinch zoom, rotate, filmstrip, slideshow, animations). Videos
// and music among the photos play right in the viewer like animated photos:
// they start by themselves and have the same play/pause and seek bar. Nothing
// here remembers where a video was left; that's the Media tab's job.
//
// Everything comes from the library: a folder is one query, thumbnails and
// waveforms were made on the PC, and pictures the Switch can't open (HEIC,
// huge photos) have a display copy in the packs.
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>

#include "core/thread.h"
#include "core/locks.h"
#include "media/audio_tap.h"
#include "media/video.h"
#include "spotify/sp_visual.hpp"
#include "tabs/common.h"
#include "ui/anim.h"

namespace {

bool playsInPlace(const LibItem& e) { return e.kind == lib::K_VIDEO || e.kind == lib::K_AUDIO; }

// ---------------------------------------------------------------- photo loader

struct Photo { SDL_Texture* tex = nullptr; int ow = 0, oh = 0; bool failed = false; };

// Decodes the photos around the one on screen (full size up to 2560 px; a
// display copy from the packs where the PC made one).
class PhotoLoader {
public:
    PhotoLoader() { m_thread.start([this] { run(); }, 512 * 1024); }
    ~PhotoLoader() {
        {
            std::lock_guard<std::mutex> l(m_mx);
            m_quit = true;
        }
        m_cv.notify_all();
        m_thread.join();
        for (auto& r : m_done)
            if (r.surf) SDL_FreeSurface(r.surf);
        clear();
    }
    // Keeps `want` (current first) and drops everything else.
    void request(const std::vector<const LibItem*>& want) {
        std::vector<std::string> keys;
        for (auto* w : want) keys.push_back(w->path);
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (std::find(keys.begin(), keys.end(), it->first) == keys.end()) {
                if (it->second.tex) SDL_DestroyTexture(it->second.tex);
                it = m_cache.erase(it);
            } else ++it;
        }
        std::lock_guard<std::mutex> l(m_mx);
        m_queue.clear();
        for (auto* w : want) {
            if (m_cache.count(w->path) || w->path == m_busy) continue;
            Job j;
            j.path = w->path;
            j.ow = w->width;
            j.oh = w->height;
            m_queue.push_back(j);
        }
        m_cv.notify_one();
    }
    const Photo* get(const std::string& path) {
        auto it = m_cache.find(path);
        return it == m_cache.end() ? nullptr : &it->second;
    }
    void collect() {
        std::vector<Result> got;
        {
            std::lock_guard<std::mutex> l(m_mx);
            got.swap(m_done);
        }
        for (auto& r : got) {
            Photo p;
            p.ow = r.ow;
            p.oh = r.oh;
            p.failed = !r.surf;
            if (r.surf) {
                p.tex = textureFromSurface(r.surf);
                SDL_FreeSurface(r.surf);
            }
            auto old = m_cache.find(r.path);
            if (old != m_cache.end() && old->second.tex) SDL_DestroyTexture(old->second.tex);
            m_cache[r.path] = p;
        }
    }
    void clear() {
        for (auto& kv : m_cache)
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        m_cache.clear();
    }
    // Everything, including what's queued or decoded and not yet collected (a locked folder closing).
    void dropAll() {
        {
            std::lock_guard<std::mutex> l(m_mx);
            m_queue.clear();
            for (auto& r : m_done)
                if (r.surf) SDL_FreeSurface(r.surf);
            m_done.clear();
        }
        clear();
    }

private:
    struct Job { std::string path; int ow = 0, oh = 0; };
    struct Result { std::string path; SDL_Surface* surf; int ow, oh; };
    WorkerThread m_thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    bool m_quit = false;
    std::vector<Job> m_queue;
    std::string m_busy;
    std::vector<Result> m_done;
    std::map<std::string, Photo> m_cache;

    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_mx);
                m_cv.wait(l, [this] { return m_quit || !m_queue.empty(); });
                if (m_quit) return;
                j = m_queue.front();
                m_queue.erase(m_queue.begin());
                m_busy = j.path;
            }
            int ow = j.ow, oh = j.oh;
            SDL_Surface* s;
            uint64_t nonce;
            if (locks::fileNonce(j.path, nonce)) {  // a locked folder's: decrypted in memory
                std::string data;
                s = locks::readFile(j.path, data) ? loadImageMem(data.data(), data.size(), 2560, 2560, &ow, &oh) : nullptr;
            } else
                s = loadImage(j.path, 2560, 2560, &ow, &oh);
            if (s && (ow <= 0 || oh <= 0)) { ow = s->w; oh = s->h; }
            std::lock_guard<std::mutex> l(m_mx);
            m_busy.clear();
            m_done.push_back({j.path, s, ow, oh});
        }
    }
};

// ---------------------------------------------------------------- photo viewer

class PhotoViewer {
public:
    std::vector<LibItem> items;
    int index = 0;
    bool closed = false;

    void open(const std::vector<LibItem>& list, int i, bool slideshow) {
        items = list;
        index = SDL_clamp(i, 0, (int)items.size() - 1);
        closed = false;
        m_slideshow = slideshow;
        // opened on purpose: the controls flash up and fade straight away, so
        // you know they're there (a tap brings them back)
        m_overlay = false;
        m_overlayA = slideshow ? 0 : 1;
        m_slideAt = g_ticks;
        resetView();
        m_offset = 0;
        m_other = -1;
        m_fadeFrom = -1;
        m_arrived.clear();
        stopVideo();
        m_stripCentered = false;
        m_info = false;
        if (slideshow && cfgBool("media.shuffle", false)) shuffle();
    }
    bool slideshow() const { return m_slideshow; }
    const LibItem& cur() const { return items[index]; }

    void draw() {
        m_loader.collect();
        if (!m_vid.empty() && m_vid != cur().path) stopVideo();  // swiped away
        // a video starts by itself when you land on it, like an animated photo
        if (cur().path != m_arrived) {
            m_arrived = cur().path;
            if (playsInPlace(cur()) && (m_slideshow || cfgBool("photos.autoplay", true))) startVideo();
        }
        if (songPlaying() && visStyle() != VIS_OFF) {
            Video& v = Video::get();
            m_audio.follow(cur().path, v.position(), !v.paused() && !v.ended());
            m_vis.update(m_audio.tap(), 0);
        } else m_audio.stop();
        std::vector<const LibItem*> want;
        auto add = [&](int i) { if (i >= 0 && i < (int)items.size() && items[i].kind == lib::K_IMAGE) want.push_back(&items[i]); };
        add(index);
        add(index + 1);
        add(index - 1);
        if (m_slideshow && index + 1 >= (int)items.size()) add(0);
        if (m_fadeFrom >= 0) add(m_fadeFrom);
        m_loader.request(want);

        fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 255});
        const Photo* curPhoto = cur().kind == lib::K_IMAGE ? m_loader.get(cur().path) : nullptr;
        updateAnimation();
        bool live = !g_in.consumed && !m_info;
        bool zoomable = curPhoto && curPhoto->tex;
        // the buttons (see ui/input.cpp): the d-pad's left/right go back and
        // ahead, 10 s on a video or song and a photo otherwise; ZL/ZR zoom
        // (as a pinch); X plays/pauses; Y shows or hides the controls
        dpadForMedia();
        if (live && g_in.seekX) {
            if (playsInPlace(cur()) && videoActive()) {
                Video::get().seek(g_in.seekX * 10, true, false);
                showBriefly();  // the seek bar, long enough to read where it went
            } else if (m_zoom <= 1.01f && items.size() > 1) {
                go(g_in.seekX, 0);
                stopSlideshow();
            }
        }
        // the right stick moves around a zoomed-in photo
        if (live && m_zoom > 1.01f && (g_in.stickX || g_in.stickY)) {
            m_panX -= g_in.stickX * 1100 * g_dt;
            m_panY -= g_in.stickY * 1100 * g_dt;
        }
        if (live && g_in.toggleUi) {
            m_overlay = !m_overlay;
            m_touchedAt = g_ticks;
        }
        if (live && g_in.playPause) {
            if (playsInPlace(cur())) togglePlay();
            else if (m_anim && !m_anim->failed()) {  // an animated picture: same idea
                m_anim->setPaused(!m_anim->paused());
                m_overlay = m_anim->paused();
                m_touchedAt = g_ticks;
            }
        }

        // ---- gestures
        if (live) {
            if (g_in.back) { if (m_overlay && !m_slideshow) m_overlay = false; else close(); }
            if (g_in.pinchScale != 1.0f && zoomable) {
                zoomAt(m_zoom * g_in.pinchScale, g_in.px, g_in.py);
                if (g_in.pinch) { m_panX += g_in.pdx; m_panY += g_in.pdy; }
                stopSlideshow();
            }
            // drags that start on the controls (seek bar, filmstrip, panels) aren't swipes
            bool fromControls = m_overlay && (g_in.sy < 96 || g_in.sy > g_H - 220);
            if (g_in.down && !g_in.multi && g_in.moved && !fromControls) {
                if (m_zoom > 1.01f) { m_panX += g_in.dx; m_panY += g_in.dy; }
                else if (items.size() > 1) {
                    m_offset = (float)(g_in.x - g_in.sx);
                    m_other = m_offset < 0 ? next(1) : next(-1);
                    m_otherSide = m_offset < 0 ? 1 : -1;
                }
            }
            if (g_in.released && m_zoom <= 1.01f && !g_in.multi && g_in.moved && !fromControls && items.size() > 1) {
                if (g_in.swipeX || fabsf(m_offset) > g_W / 4.0f) {
                    int dir = (g_in.swipeX ? -g_in.swipeX : (m_offset < 0 ? 1 : -1));
                    go(dir, m_offset);
                    stopSlideshow();
                }
            } else if (g_in.swipeX && g_in.buttonsDown && m_zoom <= 1.01f && !g_in.down) {
                go(-g_in.swipeX, 0);  // L/R buttons (a finger flick is handled above, or is on the controls)
            }
            bool onControls = m_overlay && (g_in.tapY < 96 || g_in.tapY > g_H - 220);
            if (g_in.doubleTap && zoomable && !onControls) {
                if (m_zoom > 1.01f) resetView();
                else zoomAt(2.5f, (float)g_in.tapX, (float)g_in.tapY);
            } else if (g_in.singleTap) {
                m_overlay = !m_overlay;
            }
        }
        if (m_hideAt) {
            if (g_in.down) m_hideAt = 0;  // touched: they stay
            else if ((int32_t)(g_ticks - m_hideAt) > 0) {
                m_overlay = false;
                m_hideAt = 0;
            }
        }
        // over a playing video the controls tuck away after a few seconds, like the video player's
        if (g_in.down || !videoPlaying()) m_touchedAt = g_ticks;
        if (m_overlay && !m_slideshow && !m_info && !m_scrubbing && videoPlaying() && cur().kind == lib::K_VIDEO &&
            g_ticks - m_touchedAt > 3000)
            m_overlay = false;
        if (!g_in.down) m_offset -= m_offset * std::min(1.0f, g_dt * 14);
        if (fabsf(m_offset) < 1 && !g_in.down) { m_offset = 0; m_other = -1; }

        // ---- slideshow timer: photos stay for the set time, videos play to the end
        if (m_slideshow && !m_overlay && items.size() > 1) {
            int interval = cfgInt("media.slide_secs", 5) * 1000;
            int to = next(1);
            bool done;
            if (playsInPlace(cur())) {
                Video& v = Video::get();
                bool mine = m_vid == cur().path && v.path() == m_vid;
                done = mine && (v.ended() || (!v.active() && !v.error().empty()));
            } else done = g_ticks - m_slideAt > (uint32_t)interval;
            bool nextReady = items[to].kind != lib::K_IMAGE || m_loader.get(items[to].path);
            if (done && nextReady) {
                m_fadeFrom = index;
                m_fadeStart = g_ticks;
                index = to;
                resetView();
                m_slideAt = g_ticks;
            }
        } else m_slideAt = g_ticks;

        // ---- draw
        float fade = m_fadeFrom < 0 ? 1.0f : SDL_clamp((g_ticks - m_fadeStart) / 700.0f, 0.0f, 1.0f);
        if (m_fadeFrom >= 0) {
            if (m_fadeFrom < (int)items.size())
                if (const Photo* f = m_loader.get(items[m_fadeFrom].path)) drawPhoto(*f, 0, 1.0f, 0, 0, 0, (Uint8)(255 * (1 - fade)));
            if (fade >= 1.0f) m_fadeFrom = -1;
        }
        drawItem(index, m_offset, (Uint8)(255 * fade), true);
        if (m_other >= 0 && m_offset != 0) drawItem(m_other, m_offset + m_otherSide * (float)g_W, 255, false);
        clampPan(curPhoto);

        // the controls fade in and out; on the way out they're only drawn, not used
        approach(m_overlayA, m_overlay ? 1.0f : 0.0f, 7);
        if (m_overlayA > 0) {
            FadeScope fadeOverlay(m_overlayA);
            bool c = g_in.consumed;
            if (!m_overlay) g_in.consumed = true;
            drawOverlay();
            if (!m_overlay) g_in.consumed = c;
        }
        if (m_info) drawInfo(curPhoto);
    }

    void shutdown() {
        m_anim.reset();
        m_loader.clear();
    }

    bool videoActive() const { return !m_vid.empty(); }
    bool videoPlaying() const { return !m_vid.empty() && !Video::get().paused() && !Video::get().ended(); }
    void pauseVideo() { if (!m_vid.empty()) Video::get().setPause(true); }
    void stopVideo() {
        if (m_vid.empty()) return;
        Video::get().stop();
        m_vid.clear();
    }
    void close() {
        stopVideo();
        closed = true;
        m_slideshow = false;
        m_anim.reset();
    }
    // A locked folder closed: nothing decrypted stays (pictures, animations, the visualizer's decoder).
    void forget() {
        close();
        m_loader.dropAll();
        m_animated.clear();
        m_audio.stop();
        items.clear();
        index = 0;
    }

private:
    PhotoLoader m_loader;
    std::unique_ptr<Animation> m_anim;  // for the photo on screen, if it's animated
    std::map<std::string, bool> m_animated;
    std::string m_vid;      // the video playing in place (like an animated photo)
    std::string m_arrived;  // the item last landed on (videos start on arrival)
    float m_zoom = 1, m_panX = 0, m_panY = 0, m_offset = 0;
    int m_rot = 0, m_other = -1, m_otherSide = 1, m_fadeFrom = -1;
    bool m_overlay = true, m_slideshow = false, m_info = false, m_stripCentered = false;
    bool m_scrubbing = false;
    float m_scrubV = 0;
    uint32_t m_scrubSentAt = 0, m_touchedAt = 0;
    float m_overlayA = 0;  // the controls' fade (0..1)
    uint32_t m_hideAt = 0;  // the controls fade at this time (after a seek), unless touched first

    // the controls up for a second (long enough to read), then fading
    void showBriefly() {
        m_overlay = true;
        m_hideAt = g_ticks + 1000;
    }
    uint32_t m_slideAt = 0, m_fadeStart = 0;
    Scroller m_strip;
    // music: the Spotify tab's visualizer, fed by a second decode of the song
    AudioTap m_audio;
    Visualizer m_vis;
    static int visStyle() { return SDL_clamp(cfgInt("files.visualizer", VIS_BARS), 0, VIS_COUNT - 1); }
    bool songPlaying() const {
        return cur().kind == lib::K_AUDIO && !m_vid.empty() && m_vid == cur().path && Video::get().path() == m_vid;
    }

    void startVideo() {
        spotifyPauseForVideo();
        Video::get().load(cur().path, 0, Video::CLIP);
        m_vid = cur().path;
    }

    void updateAnimation() {
        const LibItem& it = cur();
        if (it.kind != lib::K_IMAGE) {
            m_anim.reset();
            return;
        }
        if (m_anim && m_anim->path() == it.path) {
            // found to be a single frame: remembered, so it isn't opened again
            if (m_anim->still()) {
                m_animated[it.path] = false;
                m_anim.reset();
            }
            return;
        }
        m_anim.reset();
        std::string e = ext(it.path);
        if (e != "gif" && e != "png" && e != "webp") return;
        // whether it moves is found out on the animation's own thread (reading
        // the file here held up the swipe onto it)
        auto f = m_animated.find(it.path);
        if (f != m_animated.end() && !f->second) return;
        m_anim = std::make_unique<Animation>(it.path, 1280, 1280);
    }

    // One item at a horizontal offset: a photo, a video (its still, then the
    // picture once it plays) or music (its waveform).
    void drawItem(int i, float offX, Uint8 alpha, bool current) {
        const LibItem& it = items[i];
        if (it.kind == lib::K_IMAGE) {
            const Photo* p = m_loader.get(it.path);
            if (p && p->tex) {
                Photo shown = *p;
                if (current && m_anim)
                    if (SDL_Texture* t = m_anim->texture()) shown.tex = t;
                drawPhoto(shown, offX, current ? m_zoom : 1.0f, current ? m_rot : 0, current ? m_panX : 0, current ? m_panY : 0, alpha);
            } else if (p && p->failed) {
                drawIcon(IC_WARN, g_W / 2.0f + offX, g_H / 2.0f - 30, 64, g_theme.dim);
                drawText("Can't open this picture", g_W / 2 + (int)offX, g_H / 2 + 20, 24, g_theme.dim, 1);
            } else {
                // the thumbnail, blown up, until the full picture is decoded
                if (SDL_Texture* t = pictureGet(it.thumb)) drawTexFit(t, {(int)offX, 0, g_W, g_H}, false, alpha);
                spinner(g_W / 2.0f + offX, g_H / 2.0f, 28, g_theme.dim);
            }
            return;
        }
        Video& v = Video::get();
        bool mine = current && !m_vid.empty() && v.path() == m_vid && m_vid == it.path;
        if (it.kind == lib::K_VIDEO) {
            if (mine && v.texture() && v.frameReady()) drawTexFit(v.texture(), SDL_Rect{(int)offX, 0, g_W, g_H}, false);
            else if (SDL_Texture* t = pictureGet(it.thumb)) drawTexFit(t, {(int)offX, 0, g_W, g_H}, false, alpha);
            else drawIcon(IC_VIDEO, g_W / 2.0f + offX, g_H / 2.0f, 96, g_theme.faint);
        } else {
            // music: its cover (or a note) with the visualizer around it, or
            // with the visualizer off, its waveform filling in as it plays
            int style = current && mine ? visStyle() : VIS_OFF;
            SDL_Color c = g_theme.accent;
            if (style != VIS_OFF) m_vis.drawBackground({(int)offX, 0, g_W, g_H}, style, c);
            int side = std::min(g_W, g_H) * 34 / 100;
            float sc = style != VIS_OFF ? m_vis.coverScale(style) : 1.0f;
            int s = (int)(side * sc);
            SDL_Rect art = {g_W / 2 + (int)offX - s / 2, g_H * 2 / 5 - s / 2, s, s};
            if (style != VIS_OFF) m_vis.drawCoverGlow(art, style, c);
            if (!drawPicture(it.thumb, art, 18, alpha)) {
                fillRound(art, 18, g_theme.panel2);
                drawIcon(IC_MUSIC, art.x + art.w / 2.0f, art.y + art.h / 2.0f, art.w * 0.4f, c);
            }
            int ty = g_H * 2 / 5 + side / 2 + 28;
            drawText(fileStem(it.name), g_W / 2 + (int)offX, ty, 28, g_theme.fg, 1, g_W - 160, FS_BOLD);
            if (style == VIS_OFF && !it.wave.empty()) {
                float played = mine && v.duration() > 0 ? (float)(v.ended() ? 1.0 : v.position() / v.duration()) : -1;
                int ww = std::min(g_W - 240, 900);
                drawWave(it.wave, {g_W / 2 + (int)offX - ww / 2, ty + 60, ww, 90}, withAlpha(g_theme.fg, 70), played, c);
            }
        }
        if (!current) return;
        if (mine && !v.error().empty() && !v.active()) {
            drawIcon(IC_WARN, g_W / 2.0f + offX, g_H / 2.0f - 30, 56, g_theme.danger);
            drawText(v.error(), g_W / 2 + (int)offX, g_H / 2 + 20, 22, g_theme.fg, 1, g_W - 80);
        } else if (mine && v.active() && !v.loaded()) {
            spinner(g_W / 2.0f + offX, g_H / 2.0f, 26, g_theme.fg);
        }
    }

    // Play or pause the video or song: paused, the controls come up; playing
    // again, they fade away.
    void togglePlay() {
        Video& v = Video::get();
        bool mine = m_vid == cur().path && v.path() == m_vid;
        if (!mine) startVideo();  // not started (autoplay off, or another tab took the player)
        else if (v.ended()) { v.seek(0, false); v.setPause(false); }
        else v.setPause(!v.paused());
        bool paused = mine && v.paused() && !v.ended();
        m_overlay = paused;
        m_touchedAt = g_ticks;
    }

    void drawVideoBar() {
        Video& v = Video::get();
        bool mine = m_vid == cur().path && v.path() == m_vid;
        int y = g_H - 196;
        bool stopped = !mine || v.paused() || v.ended();
        if (iconButton(56, y + 22.0f, 26, stopped ? IC_PLAY : IC_PAUSE, g_theme.fg, SDL_Color{0, 0, 0, 90})) togglePlay();
        double dur = mine ? v.duration() : 0;
        if (dur <= 0) dur = cur().duration / 1000.0;
        if (dur <= 0) return;
        SDL_Rect bar = {110, y + 18, g_W - 110 - 230, 8};
        if (!m_scrubbing) m_scrubV = !mine ? 0.0f : v.ended() ? 1.0f : (float)std::min(1.0, v.position() / dur);
        bool released = false;
        if (slider(bar, m_scrubV, g_theme.accent, withAlpha(g_theme.fg, 70), &released) && mine) {
            m_scrubbing = !released;
            if (released || g_ticks - m_scrubSentAt > 120) {
                v.seek(m_scrubV * dur, false, released);  // keyframes while dragging, exact on release
                m_scrubSentAt = g_ticks;
            }
        }
        if (released) {
            m_scrubbing = false;
            if (!v.paused()) showBriefly();  // seeked: out of the way soon (still up while paused)
        }
        double pos = !mine ? 0 : m_scrubbing || v.ended() ? m_scrubV * dur : std::min(v.position(), dur);
        char t[48];
        if (dur < 10)  // short sounds: tenths, or it would read 0:00 / 0:00
            snprintf(t, sizeof(t), "%.1f / %.1f s", pos, dur);
        else snprintf(t, sizeof(t), "%s / %s", formatTime((int64_t)(pos * 1000)).c_str(), formatTime((int64_t)(dur * 1000)).c_str());
        drawText(t, g_W - 40, y + 10, 20, g_theme.fg, 2);
    }

    void stopSlideshow() { if (m_slideshow) { m_slideshow = false; m_overlay = true; } }
    void resetView() { m_zoom = 1; m_panX = m_panY = 0; m_rot = 0; }

    void shuffle() {
        std::string at = cur().path;
        std::mt19937 rng((unsigned)time(nullptr));
        std::shuffle(items.begin(), items.end(), rng);
        for (int i = 0; i < (int)items.size(); i++)
            if (items[i].path == at) index = i;
    }

    int next(int d) const {
        int n = (int)items.size();
        int i = index + d;
        if (m_slideshow) return (i % n + n) % n;
        return SDL_clamp(i, 0, n - 1);
    }

    void go(int dir, float fromOffset) {
        int ni = next(dir);
        if (ni == index) { m_other = -1; return; }
        m_other = index;
        m_otherSide = -dir;
        index = ni;
        m_offset = fromOffset + dir * (float)g_W;
        resetView();
        m_stripCentered = false;
    }

    void fitSize(const Photo& p, int rot, float& w, float& h) const {
        int ow = p.ow, oh = p.oh;
        if (ow <= 0 || oh <= 0) { SDL_QueryTexture(p.tex, nullptr, nullptr, &ow, &oh); }
        if (rot == 90 || rot == 270) std::swap(ow, oh);
        float s = std::min(g_W / (float)ow, g_H / (float)oh);
        w = ow * s;
        h = oh * s;
    }

    void drawPhoto(const Photo& p, float offX, float zoom, int rot, float panX, float panY, Uint8 alpha) {
        float w, h;
        fitSize(p, rot, w, h);
        w *= zoom;
        h *= zoom;
        bool side = rot == 90 || rot == 270;
        float dw = side ? h : w, dh = side ? w : h;  // unrotated texture size on screen
        SDL_FRect dst = {g_W / 2.0f + offX + panX - dw / 2, g_H / 2.0f + panY - dh / 2, dw, dh};
        SDL_SetTextureAlphaMod(p.tex, alpha);
        SDL_SetTextureColorMod(p.tex, 255, 255, 255);
        SDL_RenderCopyExF(g_ren, p.tex, nullptr, &dst, rot, nullptr, SDL_FLIP_NONE);
    }

    void zoomAt(float nz, float px, float py) {
        nz = SDL_clamp(nz, 1.0f, 8.0f);
        float k = nz / m_zoom;
        float cx = g_W / 2.0f, cy = g_H / 2.0f;
        m_panX = px - cx - (px - cx - m_panX) * k;
        m_panY = py - cy - (py - cy - m_panY) * k;
        m_zoom = nz;
        if (nz <= 1.0f) m_panX = m_panY = 0;
    }

    void clampPan(const Photo* p) {
        if (!p || !p->tex) return;
        float w, h;
        fitSize(*p, m_rot, w, h);
        float mx = std::max(0.0f, (w * m_zoom - g_W) / 2), my = std::max(0.0f, (h * m_zoom - g_H) / 2);
        m_panX = SDL_clamp(m_panX, -mx, mx);
        m_panY = SDL_clamp(m_panY, -my, my);
    }

    void drawOverlay() {
        SDL_Rect top = {0, 0, g_W, 96};
        fillGradientV(top, SDL_Color{0, 0, 0, 150}, SDL_Color{0, 0, 0, 0});
        if (iconButton(48, 44, 30, IC_BACK, g_theme.fg, SDL_Color{0, 0, 0, 120})) close();
        drawText(cur().name, 96, 22, 24, g_theme.fg, 0, g_W - 96 - 420, FS_BOLD);
        drawText(std::to_string(index + 1) + " / " + std::to_string(items.size()), 96, 54, 18, g_theme.dim);
        float bx = g_W - 48.0f;
        // a running slideshow stops here (they're started from the browser)
        if (m_slideshow) {
            if (iconButton(bx, 44, 30, IC_PAUSE, g_theme.accent, SDL_Color{0, 0, 0, 90})) m_slideshow = false;
            bx -= 72;
        }
        if (iconButton(bx, 44, 30, IC_INFO, m_info ? g_theme.accent : g_theme.fg, SDL_Color{0, 0, 0, 90})) m_info = !m_info;
        if (cur().kind == lib::K_AUDIO) {
            // the visualizer's look, like the Spotify tab's button
            int vs = visStyle();
            if (iconButton(bx - 72, 44, 30, IC_WAVE, vs != VIS_OFF ? g_theme.accent : g_theme.fg, SDL_Color{0, 0, 0, 120})) {
                vs = (vs + 1) % VIS_COUNT;
                cfgSetInt("files.visualizer", vs);
                toast(std::string("Visualizer: ") + visStyleName(vs));
            }
        }
        if (cur().kind == lib::K_IMAGE) {
            if (iconButton(bx - 72, 44, 30, IC_ROTR, g_theme.fg, SDL_Color{0, 0, 0, 120})) { m_rot = (m_rot + 90) % 360; m_panX = m_panY = 0; }
            if (iconButton(bx - 144, 44, 30, IC_ROTL, g_theme.fg, SDL_Color{0, 0, 0, 120})) { m_rot = (m_rot + 270) % 360; m_panX = m_panY = 0; }
            if (m_zoom > 1.01f && iconButton(bx - 216, 44, 30, IC_FIT, g_theme.fg, SDL_Color{0, 0, 0, 120})) resetView();
        }

        // animated images, videos and music: play/pause and a seek bar
        if (playsInPlace(cur())) drawVideoBar();
        else if (m_anim && !m_anim->failed()) drawAnimBar();

        // filmstrip
        SDL_Rect strip = {0, g_H - 220, g_W, 220};
        fillGradientV(strip, SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 170});
        int tw = 120, th = 90, gap = 10;
        float contentW = items.size() * (float)(tw + gap) + gap;
        SDL_Rect area = {0, g_H - 120, g_W, 110};
        if (!m_stripCentered) {
            m_strip.pos = std::max(0.0f, std::min(index * (float)(tw + gap) - g_W / 2.0f + tw / 2.0f, contentW - g_W));
            m_stripCentered = true;
        }
        if (g_in.down && startedIn(area) && g_in.moved && !g_in.multi) { m_strip.pos -= g_in.dx; g_in.consumed = true; }
        m_strip.pos = SDL_clamp(m_strip.pos, 0.0f, std::max(0.0f, contentW - g_W));
        int first = std::max(0, (int)(m_strip.pos / (tw + gap)));
        for (int i = first; i < (int)items.size(); i++) {
            int x = gap + i * (tw + gap) - (int)m_strip.pos;
            if (x > g_W) break;
            SDL_Rect r = {x, area.y + 8, tw, th};
            if (!drawPicture(items[i].thumb, r, 0)) {
                fillRect(r, g_theme.panel2);
                if (items[i].kind == lib::K_AUDIO) drawWave(items[i].wave, {r.x + 8, r.y + 20, r.w - 16, r.h - 40}, g_theme.accent);
            }
            if (i == index) strokeRound(r, 6, 3, g_theme.accent);
            if (tapped(r)) { index = i; resetView(); m_offset = 0; m_other = -1; }
        }
        // overlay controls swallow taps so they don't toggle the overlay
        if (g_in.tap && (inRect(g_in.x, g_in.y, top) || g_in.y > g_H - 220)) g_in.consumed = true;
    }

    SDL_Rect panelRect() const { return {g_W - 440, 110, 410, 330}; }

    // Play/pause, seek bar and playhead for the animation on screen. Slow
    // animations (few frames a second) count frames, faster ones seconds.
    void drawAnimBar() {
        Animation& a = *m_anim;
        int total = a.durationMs(), count = a.frameCount();
        int y = g_H - 196;
        if (iconButton(56, y + 22.0f, 26, a.paused() ? IC_PLAY : IC_PAUSE, g_theme.fg, SDL_Color{0, 0, 0, 90})) a.setPaused(!a.paused());
        if (total <= 0 || count <= 1) return;  // still measuring
        bool byFrames = count * 1000.0f / total < 12.0f;
        SDL_Rect bar = {110, y + 18, g_W - 110 - 230, 8};
        if (!m_scrubbing) m_scrubV = a.positionMs() / (float)total;
        bool released = false;
        if (slider(bar, m_scrubV, g_theme.accent, withAlpha(g_theme.fg, 70), &released)) {
            m_scrubbing = !released;
            if (released || g_ticks - m_scrubSentAt > 120) {
                a.seek((int)(m_scrubV * total));
                m_scrubSentAt = g_ticks;
            }
        }
        if (released) m_scrubbing = false;
        char buf[64];
        if (byFrames) {
            int idx = m_scrubbing ? std::min(count - 1, (int)(m_scrubV * count)) : a.frameIndex();
            snprintf(buf, sizeof(buf), "Frame %d / %d", idx + 1, count);
        } else {
            float pos = (m_scrubbing ? m_scrubV * total : a.positionMs()) / 1000.0f;
            snprintf(buf, sizeof(buf), "%.1f / %.1f s", pos, total / 1000.0f);
        }
        drawText(buf, g_W - 40, y + 10, 20, g_theme.fg, 2);
    }

    void drawInfo(const Photo* p) {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        SDL_Rect r = panelRect();
        fillRound(r, 20, SDL_Color{20, 20, 24, 205});
        const LibItem& it = cur();
        int y = r.y + 24;
        drawText("Details", r.x + 24, y, 24, g_theme.fg, 0, 0, FS_BOLD);
        y += 44;
        auto row = [&](const std::string& k, const std::string& v) {
            if (v.empty()) return;
            drawText(k, r.x + 24, y, 18, g_theme.dim);
            drawText(v, r.x + 24, y + 22, 20, g_theme.fg, 0, r.w - 48);
            y += 54;
        };
        row("Name", it.name);
        int w = it.width ? it.width : p ? p->ow : 0, h = it.height ? it.height : p ? p->oh : 0;
        std::string size = formatBytes((double)it.size);
        if (w && h) size = std::to_string(w) + " \xC3\x97 " + std::to_string(h) + "  \xC2\xB7  " + size;
        if (it.duration > 0) size = durationText(it.duration) + "  \xC2\xB7  " + size;
        row("Size", size);
        if (it.taken) row(it.kind == lib::K_IMAGE ? "Taken" : "Recorded", dateText(it.taken, it.kind == lib::K_IMAGE));
        else row("Modified", dateText(it.mtime));
        uint64_t nonce;
        auto open = locks::current();
        if (open && locks::fileNonce(it.path, nonce)) row("Folder", open->name);  // (its path on the card says nothing)
        else row("Folder", dirName(it.path).substr(std::string(APP_DIR).size() + 1));
        // it takes all input while open, so any tap (on it or not) or B closes it
        if (g_in.back || (g_in.tap && !g_in.consumed)) m_info = false;
        g_in.consumed = true;
    }
};

// ---------------------------------------------------------------- tab

class FilesTab : public Tab {
public:
    const char* name() override { return "Files"; }
    int icon() override { return IC_FOLDER; }
    SDL_Color accent() override { return rgb(0x8C9EFF); }
    bool fullscreen() override { return m_viewing; }
    int rotation() override { return m_viewing ? deviceRotation() : 0; }  // the viewer turns with the console
    bool keepAwake() override { return m_viewing && (m_photo.slideshow() || m_photo.videoPlaying()); }
    void enter() override { refresh(); }
    void leave() override {
        if (m_viewing && m_photo.slideshow()) m_photo.close();
        if (m_viewing) m_photo.pauseVideo();
        if (locks::current()) leaveLocked();  // switching tabs, the HOME menu, sleep: it locks again
    }
    void shutdown() override {
        m_photo.stopVideo();
        m_photo.shutdown();
        Video::get().shutdown();
    }
    void background() override { Video::get().poll(); }
    void preRender() override {
        if (m_viewing && m_photo.videoActive()) Video::get().render();
    }
    void update(const SDL_Rect&) override {
        if (m_viewing && m_photo.closed) m_viewing = false;
    }
    void draw(const SDL_Rect& area) override {
        refresh();
        if (m_lockFolder && !locks::isOpen(m_lockFolder)) leaveLocked();  // locked again from elsewhere
        if (m_viewing) {
            m_photo.draw();
            if (m_photo.closed) m_viewing = false;
            return;
        }
        drawBrowser(area);
    }

private:
    bool m_viewing = false, m_slideMenu = false, m_sortMenu = false;
    int64_t m_dir = 0;          // the folder shown (an item id); 0 = the list of synced folders
    LibItem m_dirItem;
    std::vector<LibItem> m_entries;  // in the order shown
    struct Group { std::string label; int begin, end; };
    std::vector<Group> m_groups;     // runs of m_entries under a heading (one unnamed run when not grouping)
    std::vector<LibRoot> m_roots;
    uint32_t m_version = 0;
    Scroller m_scroll;
    PhotoViewer m_photo;
    int64_t m_lockFolder = 0;  // the locked folder we're in (open), or 0

    static bool isFolder(const LibItem& e) { return e.kind == lib::K_DIR || e.kind == lib::K_LOCKED; }

    // Out of an open locked folder: locked again, and back to where it sits.
    void leaveLocked() {
        int64_t folder = m_lockFolder ? m_lockFolder : (locks::current() ? locks::current()->folder : 0);
        bool inside = m_lockFolder != 0;
        m_lockFolder = 0;
        m_viewing = false;
        m_photo.forget();
        locks::lock();
        if (inside) {
            LibItem f;
            open(folder && Library::get().item(folder, f) && f.parent ? f.parent : home());
        }
    }

    // A locked folder tapped: its PIN, then in.
    void unlockFolder(const LibItem& e) {
        if (locks::isOpen(e.id)) {
            open(e.id);
            return;
        }
        std::string pin;
        if (!keyboard("PIN for this locked folder", "", pin, 64, locks::numericPin(e.id), true) || pin.empty()) return;
        switch (locks::unlock(e.id, pin)) {
        case locks::OPENED: open(e.id); break;
        case locks::WRONG_PIN: toast("Wrong PIN"); break;
        default: toast("Couldn't open this locked folder. Sync it again from the PC"); break;
        }
    }

    enum SortBy { BY_NAME, BY_DATE, BY_SIZE, BY_LENGTH, BY_TYPE };
    enum GroupBy { GROUP_NONE, GROUP_TYPE, GROUP_MONTH };
    static int sortBy() { return SDL_clamp(cfgInt("files.sort", BY_NAME), 0, 4); }
    static int groupBy() { return SDL_clamp(cfgInt("files.group", GROUP_NONE), 0, 2); }
    // Folders show a picture from inside them only when asked: some folders
    // are nobody else's business.
    static bool folderPictures() { return cfgBool("files.folder_pictures", true); }

    // The library changed (or this is the first look): find the folder again by its path.
    void refresh() {
        uint32_t v = Library::get().version();
        if (v == m_version) return;
        m_version = v;
        m_roots = Library::get().roots(lib::C_FILES);
        std::string last = cfgStr("files.last_dir", "");
        LibItem it;
        if (m_lockFolder) leaveLocked();  // a new library: ids mean other things now
        if (!last.empty() && Library::get().itemByPath(last, it) && it.kind == lib::K_DIR) open(it.id, true);
        else open(home(), true);
    }

    // With a single synced folder, it is the top level.
    int64_t home() const { return m_roots.size() == 1 ? m_roots[0].item : 0; }

    void open(int64_t dir, bool keepScroll = false) {
        // leaving an open locked folder locks it again
        if (m_lockFolder && dir != m_lockFolder && dir >= 0) {
            m_lockFolder = 0;
            m_photo.forget();
            locks::lock();
        }
        bool locked = dir && locks::inside(dir);
        if (locked) m_lockFolder = locks::current()->folder;
        m_dir = dir;
        m_dirItem = LibItem();
        if (dir < 0) locks::item(dir, m_dirItem);
        else if (dir) Library::get().item(dir, m_dirItem);
        if (locked) m_entries = locks::children(dir);
        else if (dir) m_entries = Library::get().children(dir);
        else {
            m_entries.clear();
            for (auto& r : m_roots) {
                LibItem it;
                if (Library::get().item(r.item, it)) m_entries.push_back(it);
            }
        }
        arrange();
        if (!keepScroll) m_scroll = Scroller();
        // (not while a sync has the library closed: the place to come back to is still the old one;
        // and never inside a locked folder: where it sits)
        if (Library::get().ready()) {
            std::string where = dir ? m_dirItem.path : "";
            LibItem f, p;
            if (locked && Library::get().item(m_lockFolder, f) && Library::get().item(f.parent, p)) where = p.path;
            if (!locked || !where.empty()) cfgSet("files.last_dir", where);
        }
    }

    void up() {
        if (m_dir == home()) return;
        int64_t parent = m_dirItem.parent;
        open(parent ? parent : home());
    }

    static int64_t when(const LibItem& e) { return e.taken ? e.taken : e.mtime; }

    // Which heading an entry goes under, as a rank (the order of the headings) and a label.
    static std::pair<int64_t, std::string> groupOf(const LibItem& e, int group, bool newestFirst) {
        if (isFolder(e)) return {INT64_MIN, "Folders"};
        if (group == GROUP_TYPE) {
            static const char* names[] = {"", "Photos", "Videos", "Music", "Books"};
            return {e.kind, names[SDL_clamp(e.kind, 0, 4)]};
        }
        int64_t t = when(e);
        if (t <= 0) return {INT64_MAX, "No date"};
        time_t tt = (time_t)t;
        struct tm* lt = e.taken && e.kind == lib::K_IMAGE ? gmtime(&tt) : localtime(&tt);  // EXIF dates are the camera's clock
        if (!lt) return {INT64_MAX, "No date"};
        static const char* months[] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
        int64_t key = (int64_t)(lt->tm_year + 1900) * 12 + lt->tm_mon;
        return {newestFirst ? -key : key, std::string(months[lt->tm_mon]) + " " + std::to_string(lt->tm_year + 1900)};
    }

    // Sorts m_entries (folders always first, by name) and splits it into headed groups.
    void arrange() {
        int by = sortBy(), group = groupBy();
        bool reverse = cfgBool("files.reverse", true);
        // the way each order usually reads: names A to Z, the rest newest, biggest or longest first
        bool descending = (by == BY_DATE || by == BY_SIZE || by == BY_LENGTH) != reverse;
        bool newestFirst = !(by == BY_DATE && !descending);
        auto key = [&](const LibItem& e) -> int64_t {
            switch (by) {
            case BY_DATE: return when(e);
            case BY_SIZE: return e.size;
            case BY_LENGTH: return e.duration;
            case BY_TYPE: return e.kind;
            default: return 0;
            }
        };
        std::vector<std::pair<int64_t, std::string>> groups(m_entries.size());
        for (size_t i = 0; i < m_entries.size(); i++)
            groups[i] = group == GROUP_NONE ? std::pair<int64_t, std::string>(isFolder(m_entries[i]) ? 0 : 1, "")
                                            : groupOf(m_entries[i], group, newestFirst);
        std::vector<size_t> order(m_entries.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        // The library hands them over by name (its sort keys), so the index is the
        // name order: it sorts by name, and breaks ties in the other orders.
        // (A group holds only folders or only files: folders get their own.)
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (groups[a].first != groups[b].first) return groups[a].first < groups[b].first;
            const LibItem &x = m_entries[a], &y = m_entries[b];
            if (isFolder(x)) return a < b;
            if (by == BY_NAME) return reverse ? a > b : a < b;
            int64_t kx = key(x), ky = key(y);
            if (kx != ky) return descending ? kx > ky : kx < ky;
            if (by == BY_TYPE) {
                std::string ex = ext(x.name), ey = ext(y.name);
                if (ex != ey) return ex < ey;
            }
            return a < b;
        });
        std::vector<LibItem> sorted;
        sorted.reserve(m_entries.size());
        m_groups.clear();
        for (size_t k = 0; k < order.size(); k++) {
            size_t i = order[k];
            if (m_groups.empty() || (group != GROUP_NONE && groups[i].second != m_groups.back().label))
                m_groups.push_back({group == GROUP_NONE ? "" : groups[i].second, (int)k, (int)k});
            m_groups.back().end = (int)k + 1;
            sorted.push_back(std::move(m_entries[i]));
        }
        m_entries = std::move(sorted);
    }

    // ---- layout: where each heading and entry goes, for the grid, masonry and the list

    enum Layout { L_GRID, L_MASONRY, L_LIST };
    static int layoutMode() { return SDL_clamp(cfgInt("files.layout", L_MASONRY), 0, 2); }
    static int tileSize() { return SDL_clamp(cfgInt("files.tile_size", 1), 0, 2); }  // small, medium, large
    static bool showExt() { return cfgBool("files.show_ext", false); }
    static std::string label(const LibItem& e) { return isFolder(e) || showExt() ? e.name : fileStem(e.name); }

    struct Placed { int entry; SDL_Rect r; };  // entry < 0: the heading of group -entry - 1
    std::vector<Placed> m_placed;
    std::set<std::string> m_collapsed;  // headings folded shut (by label, so it holds from folder to folder)

    bool collapsed(int g) const { return groupBy() != GROUP_NONE && m_collapsed.count(m_groups[g].label); }

    // Lays out m_entries across `width` (content coordinates: x from the area's
    // left, y from the top of the content). Returns the content height.
    float layout(int width) {
        m_placed.clear();
        const int mode = layoutMode(), size = tileSize(), gap = 16, side = 24, headH = 56;
        static const int tileWs[] = {170, 236, 330}, rowHs[] = {84, 116, 150};
        int inner = width - 2 * side;
        int cols = mode == L_LIST ? 1 : std::max(1, (inner + gap) / (tileWs[size] + gap));
        int colW = mode == L_LIST ? inner : (inner - (cols - 1) * gap) / cols;
        bool headings = groupBy() != GROUP_NONE;
        int y = 8;
        for (int g = 0; g < (int)m_groups.size(); g++) {
            const Group& gr = m_groups[g];
            if (headings) {
                m_placed.push_back({-g - 1, {side, y, inner, headH - 8}});
                y += headH;
            }
            if (collapsed(g)) continue;
            if (mode == L_LIST) {
                for (int i = gr.begin; i < gr.end; i++, y += rowHs[size]) m_placed.push_back({i, {side, y, inner, rowHs[size] - 8}});
            } else if (mode == L_GRID) {
                int tileH = colW * 3 / 4 + 66;
                for (int i = gr.begin; i < gr.end; i++) {
                    int k = i - gr.begin;
                    m_placed.push_back({i, {side + (k % cols) * (colW + gap), y + (k / cols) * (tileH + gap), colW, tileH}});
                }
                y += ((gr.end - gr.begin + cols - 1) / cols) * (tileH + gap);
            } else {
                // masonry: pictures at their own shape, each into the shortest
                // column (the viewer goes through them in the order they show:
                // see shownOrder)
                std::vector<int> colY(cols, y);
                for (int i = gr.begin; i < gr.end; i++) {
                    const LibItem& e = m_entries[i];
                    float aspect = 0.75f;  // height / width
                    if ((e.kind == lib::K_IMAGE || e.kind == lib::K_VIDEO) && e.width > 0 && e.height > 0)
                        aspect = SDL_clamp(e.height / (float)e.width, 0.4f, 2.2f);
                    int h = (int)(colW * aspect) + (captioned(e) ? 58 : 0);
                    int c = (int)(std::min_element(colY.begin(), colY.end()) - colY.begin());
                    m_placed.push_back({i, {side + c * (colW + gap), colY[c], colW, h}});
                    colY[c] += h + gap;
                }
                y = *std::max_element(colY.begin(), colY.end());
            }
        }
        return y + 24.0f;
    }

    // In masonry, pictures and videos go without a name under them.
    static bool captioned(const LibItem& e) { return layoutMode() != L_MASONRY || (e.kind != lib::K_IMAGE && e.kind != lib::K_VIDEO); }

    // A group's heading: tap to fold it shut or open.
    bool drawHeading(int g, const SDL_Rect& r) {
        bool shut = collapsed(g);
        bool down = pressing(r);
        if (down) fillRound(r, 12, g_theme.panel);
        drawIcon(shut ? IC_RIGHT : IC_DOWN, r.x + 16.0f, r.y + r.h / 2.0f, 22, g_theme.dim);
        int tx = r.x + 38;
        drawText(m_groups[g].label, tx, r.y + (r.h - fontHeight(22, FS_BOLD)) / 2, 22, g_theme.fg, 0, r.w - 120, FS_BOLD);
        int n = m_groups[g].end - m_groups[g].begin;
        drawText(std::to_string(n), tx + textWidth(m_groups[g].label, 22, FS_BOLD) + 12, r.y + (r.h - fontHeight(18)) / 2 + 2, 18, g_theme.dim);
        if (!tapped(r)) return false;
        if (shut) m_collapsed.erase(m_groups[g].label);
        else m_collapsed.insert(m_groups[g].label);
        return true;
    }

    // m_entries as they read on screen. In masonry that's by where each one
    // starts, top to bottom then left to right, not the sort's order (which
    // jumps between columns: "next" was often rows away); entries in folded
    // groups come after, in the sort's order.
    std::vector<const LibItem*> shownOrder() const {
        std::vector<const LibItem*> v;
        if (layoutMode() != L_MASONRY) {
            for (auto& e : m_entries) v.push_back(&e);
            return v;
        }
        std::vector<const Placed*> tiles;
        for (auto& p : m_placed)
            if (p.entry >= 0) tiles.push_back(&p);
        std::stable_sort(tiles.begin(), tiles.end(), [](const Placed* a, const Placed* b) {
            return a->r.y != b->r.y ? a->r.y < b->r.y : a->r.x < b->r.x;
        });
        std::vector<bool> seen(m_entries.size(), false);
        for (auto* t : tiles) {
            v.push_back(&m_entries[t->entry]);
            seen[t->entry] = true;
        }
        for (size_t i = 0; i < m_entries.size(); i++)
            if (!seen[i]) v.push_back(&m_entries[i]);
        return v;
    }

    // what the viewer steps through
    std::vector<LibItem> files() const {
        std::vector<LibItem> v;
        for (const LibItem* e : shownOrder())
            if (e->kind == lib::K_IMAGE || e->kind == lib::K_VIDEO || e->kind == lib::K_AUDIO) v.push_back(*e);
        return v;
    }

    void openEntry(const LibItem& e) {
        if (e.kind == lib::K_DIR) { open(e.id); return; }
        if (e.kind == lib::K_LOCKED) { unlockFolder(e); return; }
        if (e.kind == lib::K_BOOK) {
            toast("Books open in the Books tab");
            return;
        }
        auto v = files();
        int idx = 0;
        for (int i = 0; i < (int)v.size(); i++)
            if (v[i].id == e.id) idx = i;
        m_photo.open(v, idx, false);
        m_viewing = true;
    }

    // a folder: its icon, or (when folders may show pictures) one from inside with a folder badge
    void drawFolderThumb(const LibItem& e, const SDL_Rect& th, bool down, int radius, int iconSize) {
        if (e.kind == lib::K_LOCKED) {  // a padlock, never a picture
            fillRound(th, radius, down ? g_theme.line : g_theme.panel2);
            drawIcon(IC_LOCK, th.x + th.w / 2.0f, th.y + th.h / 2.0f, (float)iconSize, g_theme.accent);
            return;
        }
        if (folderPictures() && drawPicture(e.thumb, th, radius, down ? 120 : 170)) {
            fillCircle(th.x + 34.0f, th.y + th.h - 34.0f, 24, SDL_Color{0, 0, 0, 160});
            drawIcon(IC_FOLDER, th.x + 34.0f, th.y + th.h - 34.0f, 26, g_theme.accent);
        } else {
            fillRound(th, radius, down ? g_theme.line : g_theme.panel2);
            drawIcon(IC_FOLDER, th.x + th.w / 2.0f, th.y + th.h / 2.0f, (float)iconSize, g_theme.accent);
        }
    }

    static int kindIcon(const LibItem& e) {
        return e.kind == lib::K_AUDIO ? IC_MUSIC : e.kind == lib::K_VIDEO ? IC_VIDEO : e.kind == lib::K_BOOK ? IC_BOOK : IC_IMAGE;
    }

    // A list row: full name, size and (for videos and music) length.
    void drawRow(const LibItem& e, const SDL_Rect& row) {
        bool down = pressing(row);
        fillRound(row, 14, down ? g_theme.line : g_theme.panel);
        SDL_Rect th = {row.x + 8, row.y + 8, (row.h - 16) * 4 / 3, row.h - 16};
        if (isFolder(e)) drawFolderThumb(e, th, down, 10, th.h * 2 / 5);
        else if (!drawPicture(e.thumb, th, 10)) {
            fillRound(th, 10, g_theme.panel2);
            if (e.kind == lib::K_AUDIO && !e.wave.empty()) drawWave(e.wave, {th.x + 10, th.y + 14, th.w - 20, th.h - 28}, g_theme.accent);
            else drawIcon(kindIcon(e), th.x + th.w / 2.0f, th.y + th.h / 2.0f, th.h * 0.36f, g_theme.faint);
        }
        int tx = th.x + th.w + 20, tw = row.x + row.w - tx - 20;
        std::string sub;
        if (e.kind == lib::K_LOCKED) sub = "Locked";
        else if (e.kind == lib::K_DIR) sub = std::to_string(e.children) + (e.children == 1 ? " item" : " items");
        else {
            if (e.duration > 0) sub = durationText(e.duration) + "  \xC2\xB7  ";
            sub += formatBytes((double)e.size);
            if (when(e)) sub += "  \xC2\xB7  " + dateText(when(e), e.taken && e.kind == lib::K_IMAGE).substr(0, 10);
        }
        std::string nm = label(e);
        int maxLines = row.h > 90 ? 2 : 1;
        int lines = std::min(maxLines, (int)wrapText(nm, 22, tw).size());
        int blockH = lines * fontHeight(22) + 4 + fontHeight(18);
        int ty = row.y + std::max(6, (row.h - blockH) / 2);
        ty += drawWrapped(nm, tx, ty, 22, g_theme.fg, tw, maxLines);
        drawText(sub, tx, ty + 4, 18, g_theme.dim, 0, tw);
    }

    // A grid or masonry tile: the picture, then (if captioned) the name.
    void drawTile(const LibItem& e, const SDL_Rect& tile) {
        bool cap = captioned(e);
        int thumbH = cap ? (layoutMode() == L_GRID ? tile.w * 3 / 4 : tile.h - 58) : tile.h;
        SDL_Rect th = {tile.x, tile.y, tile.w, thumbH};
        bool down = pressing(tile);
        bool small = tile.w < 200;
        if (isFolder(e)) drawFolderThumb(e, th, down, 14, small ? 52 : 72);
        else {
            if (!drawPicture(e.thumb, th, 14, down ? 170 : 255)) {
                fillRound(th, 14, down ? g_theme.line : g_theme.panel2);
                if (e.kind == lib::K_AUDIO && !e.wave.empty())
                    drawWave(e.wave, {th.x + 18, th.y + 26, th.w - 36, th.h - 72}, g_theme.accent);
                else if (e.thumb && !pictureFailed(e.thumb)) spinner(th.x + th.w / 2.0f, th.y + th.h / 2.0f, 16, g_theme.faint);
                else drawIcon(kindIcon(e), th.x + th.w / 2.0f, th.y + th.h / 2.0f, small ? 40 : 56,
                              e.kind == lib::K_AUDIO ? g_theme.accent : g_theme.faint);
            }
            if (playsInPlace(e)) {
                float r = small ? 17 : 22, cx = th.x + r + 12, cy = th.y + th.h - r - 12;
                fillCircle(cx, cy, r, SDL_Color{0, 0, 0, 150});
                drawIcon(e.kind == lib::K_AUDIO ? IC_MUSIC : IC_PLAY, cx + 1, cy, r, g_theme.fg);
                if (e.duration > 0 && !small) {
                    std::string d = durationText(e.duration);
                    int w = textWidth(d, 18) + 16;
                    fillRound({th.x + th.w - w - 10, th.y + th.h - 38, w, 28}, 8, SDL_Color{0, 0, 0, 170});
                    drawText(d, th.x + th.w - 18, th.y + th.h - 35, 18, g_theme.fg, 2);
                }
            }
        }
        if (cap) drawWrapped(label(e), tile.x + 4, th.y + th.h + 8, small ? 18 : 20, g_theme.fg, tile.w - 8, 2);
    }

    void drawEntries(const SDL_Rect& area) {
        float contentH = layout(area.w);
        m_scroll.update(area, contentH);
        bool list = layoutMode() == L_LIST;
        pushClip(area);
        for (const Placed& p : m_placed) {
            SDL_Rect r = {area.x + p.r.x, area.y + p.r.y - (int)m_scroll.pos, p.r.w, p.r.h};
            if (r.y + r.h < area.y || r.y > area.y + area.h) continue;
            if (p.entry < 0) {
                if (drawHeading(-p.entry - 1, r)) break;  // the layout changed under us: drawn again next frame
                continue;
            }
            const LibItem& e = m_entries[p.entry];
            if (list) drawRow(e, r);
            else drawTile(e, r);
            if (tapped(r)) { openEntry(e); break; }
        }
        popClip();
        drawScrollbar(area, contentH, m_scroll.pos, m_scroll.thumb ? whereLabel() : "");
    }

    // What's at the top of the view, in the terms of the sort (for the
    // scrollbar's bubble): "N", "May 2026", "12 MB", "3:45", "Videos".
    std::string whereLabel() const {
        const LibItem* e = nullptr;
        for (const Placed& p : m_placed)
            if (p.entry >= 0 && p.r.y + p.r.h > m_scroll.pos) { e = &m_entries[p.entry]; break; }
        if (!e) return "";
        if (isFolder(*e)) return "Folders";
        switch (sortBy()) {
        case BY_DATE: return groupOf(*e, GROUP_MONTH, true).second;
        case BY_SIZE: return formatBytes((double)e->size);
        case BY_LENGTH: return e->duration > 0 ? durationText(e->duration) : "No length";
        case BY_TYPE: return groupOf(*e, GROUP_TYPE, true).second;
        default: {
            // the first letter (a whole character, whatever script); digits and signs as #
            const std::string& n = e->name;
            if (n.empty()) return "";
            unsigned char c = (unsigned char)n[0];
            if (c < 0x80) return isalpha(c) ? std::string(1, (char)toupper(c)) : "#";
            size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            return n.substr(0, len);
        }
        }
    }

    void drawBrowser(const SDL_Rect& area) {
        if (!Library::get().ready() || m_roots.empty()) {
            drawText(name(), area.x + 32, 26, 36, g_theme.fg, 0, 0, FS_BOLD);
            drawNoLibrary(area, IC_FOLDER, "Files");
            return;
        }
        int x0 = area.x + 32;
        bool sub = m_dir != home();
        if (sub && (iconButton(x0 + 22.0f, 46, 28, IC_BACK, g_theme.fg, g_theme.panel2) || g_in.back)) {
            up();
            return;
        }
        int tx = sub ? x0 + 64 : x0;
        std::string title = m_dir ? m_dirItem.name : name();
        if (m_lockFolder && m_dir == m_lockFolder && locks::current()) title = locks::current()->name;
        drawText(title, tx, 26, 36, g_theme.fg, 0, area.w - 560 - (tx - x0), FS_BOLD);
        int media = 0;
        for (auto& e : m_entries) media += e.kind == lib::K_IMAGE || e.kind == lib::K_VIDEO;
        int bx = area.x + area.w - 32;
        SDL_Rect viewBtn = {bx - 120, 22, 120, 52};
        if (button(viewBtn, "View", IC_NONE, BTN_NORMAL, 20)) { m_sortMenu = !m_sortMenu; m_slideMenu = false; }
        bx -= 132;
        SDL_Rect slideBtn = {bx - 180, 22, 180, 52};
        if (media > 0 && button(slideBtn, "Slideshow", IC_SLIDES, BTN_ACCENT, 20)) { m_slideMenu = !m_slideMenu; m_sortMenu = false; }
        if (media == 0) m_slideMenu = false;

        drawEntries({area.x, 96, area.w, area.h - 96});
        drawEmpty(area);
        if (m_slideMenu) drawSlideMenu(slideBtn);
        if (m_sortMenu) drawViewMenu(viewBtn);
    }

    // Under the View button: layout and size, sorting and grouping, names and folder pictures.
    void drawViewMenu(const SDL_Rect& under) {
        InputLayer layer;  // on top: gets the input, the grid under it doesn't
        SDL_Rect r = {std::min(under.x + under.w - 470, g_W - 500), under.y + under.h + 10, 470, 590};
        fillRound(r, 20, g_theme.panel2);
        int x = r.x + 24, y = r.y + 18, w = r.w - 48;
        bool changed = false;
        auto chips = [&](const char* title, std::initializer_list<const char*> names, int current, const char* key) {
            drawText(title, x, y, 18, g_theme.dim);
            y += 26;
            int cx = x, i = 0;
            for (const char* n : names) {
                int cw = textWidth(n, 18) + 34;
                if (chip({cx, y, cw, 44}, n, current == i, 18) && current != i) {
                    cfgSetInt(key, i);
                    changed = true;
                }
                cx += cw + 8;
                i++;
            }
            y += 54;
        };
        auto toggleRow = [&](const char* title, const char* key, bool def) {
            bool v = cfgBool(key, def);
            drawText(title, x, y + 14, 20, g_theme.fg);
            bool t = toggle({x, y, w, 52}, v);
            if (t) cfgSetBool(key, v);
            y += 56;
            return t;
        };
        chips("Layout", {"Grid", "Masonry", "List"}, layoutMode(), "files.layout");
        chips("Preview size", {"Small", "Medium", "Large"}, tileSize(), "files.tile_size");
        chips("Sort by", {"Name", "Date", "Size", "Length", "Type"}, sortBy(), "files.sort");
        if (toggleRow("Reverse", "files.reverse", true)) changed = true;
        chips("Group by", {"None", "Type", "Month"}, groupBy(), "files.group");
        toggleRow("File extensions", "files.show_ext", false);
        toggleRow("Folder thumbnails", "files.folder_pictures", true);
        drawText("Off: folders show an icon, not a picture from inside.", x, y, 16, g_theme.dim, 0, w);
        if (changed) open(m_dir, true);
        if (g_in.back || (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r))) m_sortMenu = false;
        g_in.consumed = true;
    }

    // Under the Slideshow button: time per photo, shuffle and Start. Videos in
    // the folder play to the end in a slideshow.
    void drawSlideMenu(const SDL_Rect& under) {
        InputLayer layer;  // on top: gets the input, the grid under it doesn't
        SDL_Rect r = {std::min(under.x, g_W - 440), under.y + under.h + 10, 410, 300};
        fillRound(r, 20, g_theme.panel2);
        int x = r.x + 24, y = r.y + 24;
        drawText("Slideshow", x, y, 24, g_theme.fg, 0, 0, FS_BOLD);
        y += 48;
        drawText("Each photo for", x, y, 18, g_theme.dim);
        y += 28;
        int secs = cfgInt("media.slide_secs", 5), cx = x;
        for (int sec : {3, 5, 10, 20, 30}) {
            if (chip({cx, y, 66, 44}, std::to_string(sec) + " s", secs == sec, 18)) cfgSetInt("media.slide_secs", sec);
            cx += 74;
        }
        y += 60;
        bool sh = cfgBool("media.shuffle", false);
        drawText("Shuffle", x, y + 14, 20, g_theme.fg);
        if (toggle({x, y, r.w - 48, 52}, sh)) cfgSetBool("media.shuffle", sh);
        y += 68;
        if (button({x, y, r.w - 48, 56}, "Start slideshow", IC_SLIDES, BTN_ACCENT, 20)) {
            m_slideMenu = false;
            std::vector<LibItem> show;
            for (const LibItem* e : shownOrder())
                if (e->kind == lib::K_IMAGE || e->kind == lib::K_VIDEO) show.push_back(*e);
            if (show.empty()) return;
            m_photo.open(show, 0, true);
            m_viewing = true;
            return;
        }
        if (g_in.back || (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r))) m_slideMenu = false;
        g_in.consumed = true;
    }

    // shown by both the grid and the list when the folder has nothing to show
    void drawEmpty(const SDL_Rect& area) {
        if (!m_entries.empty()) return;
        drawIcon(IC_FOLDER, area.x + area.w / 2.0f, area.h / 2.0f - 60, 72, g_theme.faint);
        drawText("Nothing here yet", area.x + area.w / 2, area.h / 2, 28, g_theme.fg, 1);
        drawText("This folder is empty on your PC.", area.x + area.w / 2, area.h / 2 + 44, 20, g_theme.dim, 1, area.w - 80);
    }
};

}  // namespace

Tab* makeFilesTab() { return new FilesTab(); }
