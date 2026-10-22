// Spotify tab: a Spotify Connect receiver (cspot) with a now-playing screen.
// If the Switchify sysmodule is running, the tab remote-controls it instead
// of starting a second receiver.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <atomic>
#include <cstdarg>
#include <condition_variable>
#include <cmath>
#include <memory>
#include <mutex>
#include "core/http.h"
#include "core/thread.h"

#include "app.h"
#include "BellLogger.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "LoginBlob.h"
#include "SpircHandler.h"
#include "sp_audio.hpp"
#include "sp_eq_ui.hpp"
#include "sp_player.hpp"
#include "sp_visual.hpp"
#include "sp_zeroconf.hpp"
#include "switchify_ipc.h"

namespace {

const std::string CRED_PATH = std::string(APP_DIR) + "/spotify-login.json";
const char* SWITCHIFY_CREDS = "sdmc:/config/switchify/credentials.json";

// Routes cspot/bell logging into our log file.
class AppBellLogger : public bell::AbstractLogger {
public:
    void debug(std::string, int, std::string, const char*, ...) override {}
    void error(std::string, int, std::string, const char* fmt, ...) override {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        appLog("cspot E %s", buf);
    }
    void info(std::string, int, std::string, const char* fmt, ...) override {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        appLog("cspot I %s", buf);
    }
};

bool serviceRunning(const char* name) {
    u8 out = 0;
    SmServiceName n = smEncodeName(name);
    Result rc;
    // Atmosphere extension: sm "HasService" (65100) answers without blocking
    if (hosversionAtLeast(12, 0, 0)) rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, n, out);
    else rc = serviceDispatchInOut(smGetServiceSession(), 65100, n, out);
    return R_SUCCEEDED(rc) && out;
}

std::string deviceName() {
    std::string custom = trim(cfgStr("spotify.name", ""));
    if (!custom.empty()) return custom;
    SetSysDeviceNickName nick = {};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0]) return nick.nickname;
    return "Nintendo Switch";
}

// ---------------------------------------------------------------- backends

struct Backend {
    virtual ~Backend() {}
    virtual void status(SwitchifyStatus& s) = 0;
    virtual std::string imageUrl() = 0;
    virtual void playPause() = 0;
    virtual void next() = 0;
    virtual void prev() = 0;
    virtual void seek(u32 ms) = 0;
    virtual void setVolume(u32 v) = 0;
    virtual void previewVolume(u32) {}  // local only, while a slider is dragged
    virtual void logout() = 0;
    virtual bool external() const = 0;
    virtual void stop() {}
    virtual AudioOutput* audio() { return nullptr; }  // our own output (EQ, visualizer)
};

class Embedded : public Backend {
public:
    Embedded() {
        if (!fileExists(CRED_PATH)) {
            // reuse a pairing made with the Switchify sysmodule
            std::string c;
            if (readFile(SWITCHIFY_CREDS, c) && !c.empty()) writeFile(CRED_PATH, c);
        }
        if (!m_audio.start()) appLog("spotify: audio failed to start");
        m_player = std::make_unique<Player>(m_audio);
        m_thread.start([this] { run(); m_done = true; }, 1024 * 1024);
    }
    ~Embedded() override { stop(); }

    void stop() override {
        if (m_stopped) return;
        m_stopped = true;
        m_quit = true;
        m_cancel = true;
        // The session loop wakes every 200 ms, then disconnect() stops cspot's own
        // threads. A connect/login in progress can take up to its 10 s socket timeout.
        // Every thread must be gone before we return: an NRO shares its process
        // with the launcher, so a leftover thread crashes it after we exit.
        for (int i = 0; i < 300 && !m_done; i++) svcSleepThread(50'000'000ULL);
        m_audio.stop();
        if (m_done) m_thread.join();
        else {
            appLog("spotify: session thread didn't stop in time");
            m_thread.waitAndJoin(0);  // detaches: its stack leaks, logged
            m_leaked = true;
        }
    }
    bool leaked() const { return m_leaked; }

    void status(SwitchifyStatus& s) override { m_player->getStatus(s); }
    std::string imageUrl() override { return m_player->getImageUrl(); }
    void playPause() override { m_player->playPause(); }
    void next() override { m_player->next(); }
    void prev() override { m_player->prev(); }
    void seek(u32 ms) override { m_player->seek(ms); }
    void setVolume(u32 v) override { m_player->setVolume(v); }
    void previewVolume(u32 v) override { m_player->previewVolume(v); }
    void logout() override { m_logout = true; m_cancel = true; }
    bool external() const override { return false; }
    AudioOutput* audio() override { return &m_audio; }

private:
    AudioOutput m_audio;
    std::unique_ptr<Player> m_player;
    WorkerThread m_thread;
    std::atomic<bool> m_quit{false}, m_cancel{false}, m_logout{false}, m_done{false};
    bool m_stopped = false, m_leaked = false;

    void sleepMs(int ms) {
        for (int i = 0; i < ms / 100 && !m_quit; i++) svcSleepThread(100'000'000ULL);
    }

    u32 waitForNetwork() {
        bool announced = false;
        while (!m_quit) {
            NifmInternetConnectionType type;
            u32 strength;
            NifmInternetConnectionStatus st;
            if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &st)) && st == NifmInternetConnectionStatus_Connected) {
                u32 ip = 0;
                if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip) return ip;
            }
            if (!announced) { m_player->setState(SwitchifyState_NoNetwork, "Waiting for an internet connection"); announced = true; }
            sleepMs(2000);
        }
        return 0;
    }

    void run() {
        const std::string name = deviceName();
        while (!m_quit) {
            m_cancel = false;
            u32 ip = waitForNetwork();
            if (m_quit) break;
            char ipStr[16];
            inet_ntop(AF_INET, &ip, ipStr, sizeof(ipStr));
            m_player->setDeviceInfo(name, ipStr);
            auto blob = std::make_shared<cspot::LoginBlob>(name);
            std::string creds;
            bool fromFile = false;
            if (readFile(CRED_PATH, creds) && !creds.empty()) {
                try {
                    blob->loadJson(creds);
                    fromFile = !blob->authData.empty();
                } catch (...) { fromFile = false; }
            }
            if (!fromFile) {
                m_player->setState(SwitchifyState_WaitingPairing, "Open Spotify on the same Wi-Fi and pick this device");
                Zeroconf zc;
                if (!zc.waitForLogin(blob, ip, m_cancel)) {
                    if (m_logout.exchange(false)) continue;
                    sleepMs(1000);
                    continue;
                }
            }
            m_player->setState(SwitchifyState_Connecting, "Signing in to Spotify");
            try {
                auto ctx = cspot::Context::createFromBlob(blob);
                ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;
                ctx->session->connectWithRandomAp();
                ctx->config.authData = ctx->session->authenticate(blob);
                if (ctx->config.authData.empty()) {
                    if (fromFile) remove(CRED_PATH.c_str());
                    m_player->setState(SwitchifyState_Error, "Spotify login failed (Premium is required)");
                    sleepMs(5000);
                    continue;
                }
                writeFile(CRED_PATH, ctx->getCredentialsJson());
                m_player->setUsername(ctx->config.username);
                auto handler = std::make_shared<cspot::SpircHandler>(ctx);
                ctx->session->startTask();
                m_player->attach(handler);
                m_player->setState(SwitchifyState_Idle, "Ready - pick this device in Spotify");
                while (!m_cancel) {
                    ctx->session->handlePacket();
                    m_player->tick();
                }
                m_player->detach();
                handler->disconnect();
                if (m_logout.exchange(false)) {
                    remove(CRED_PATH.c_str());
                    m_player->setUsername("");
                }
            } catch (const std::exception& e) {
                appLog("spotify: session error: %s", e.what());
                m_player->detach();
                m_player->setState(SwitchifyState_Error, std::string("Connection error: ") + e.what());
                sleepMs(5000);
            }
        }
    }
};

class Remote : public Backend {
public:
    ~Remote() override { if (m_open) serviceClose(&m_srv); }
    void status(SwitchifyStatus& s) override {
        if (!connect()) { s = {}; s.state = SwitchifyState_Error; strcpy(s.message, "Switchify sysmodule not reachable"); return; }
        Result rc = serviceDispatch(&m_srv, SwitchifyCmd_GetStatus, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                                    .buffers = {{&s, sizeof(s)}});
        if (R_FAILED(rc)) { serviceClose(&m_srv); m_open = false; }
    }
    std::string imageUrl() override {
        if (!connect() || m_version < 3) return "";
        char url[SWITCHIFY_URL_LEN] = {};
        serviceDispatch(&m_srv, SwitchifyCmd_GetImageUrl, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
                        .buffers = {{url, sizeof(url)}});
        url[sizeof(url) - 1] = 0;
        return url;
    }
    void playPause() override { cmd(SwitchifyCmd_PlayPause); }
    void next() override { cmd(SwitchifyCmd_Next); }
    void prev() override { cmd(SwitchifyCmd_Prev); }
    void seek(u32 ms) override { if (connect()) serviceDispatchIn(&m_srv, SwitchifyCmd_Seek, ms); }
    void setVolume(u32 v) override { if (connect()) serviceDispatchIn(&m_srv, SwitchifyCmd_SetVolume, v); }
    void logout() override { cmd(SwitchifyCmd_Logout); }
    bool external() const override { return true; }

private:
    Service m_srv;
    bool m_open = false;
    u32 m_version = 0;
    bool connect() {
        if (m_open) return true;
        if (R_FAILED(smGetService(&m_srv, SWITCHIFY_SERVICE_NAME))) return false;
        m_open = true;
        serviceDispatchOut(&m_srv, SwitchifyCmd_GetVersion, m_version);
        return true;
    }
    void cmd(u32 id) { if (connect()) serviceDispatch(&m_srv, id); }
};

// ---------------------------------------------------------------- cover art

class CoverFetcher {
public:
    CoverFetcher() { m_thread.start([this] { run(); }, 512 * 1024); }
    ~CoverFetcher() {
        {
            std::lock_guard<std::mutex> l(m_mx);
            m_quit = true;
        }
        m_cancel = true;  // ends a fetch in flight
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
        if (m_result) SDL_FreeSurface(m_result);
    }
    void want(const std::string& url) {
        std::lock_guard<std::mutex> l(m_mx);
        if (url == m_want) return;
        m_want = url;
        m_cv.notify_one();
    }
    // Returns a freshly fetched cover (caller owns it) and the URL it belongs to.
    SDL_Surface* take(std::string& url) {
        std::lock_guard<std::mutex> l(m_mx);
        SDL_Surface* s = m_result;
        m_result = nullptr;
        url = m_resultUrl;
        return s;
    }

private:
    WorkerThread m_thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    std::string m_want, m_done, m_resultUrl;
    SDL_Surface* m_result = nullptr;
    bool m_quit = false;
    std::atomic<bool> m_cancel{false};

    void run() {
        for (;;) {
            std::string url;
            {
                std::unique_lock<std::mutex> l(m_mx);
                m_cv.wait(l, [this] { return m_quit || m_want != m_done; });
                if (m_quit) return;
                url = m_done = m_want;
            }
            if (url.empty()) continue;
            SDL_Surface* s = nullptr;
            // curl, not bell's HTTP client: it has time limits and can be
            // cancelled, so exit never waits on a dead connection
            HttpResult r = httpGet(url, {}, 15, &m_cancel);
            if (r.status == 200 && !r.body.empty()) s = loadImageMem((const uint8_t*)r.body.data(), r.body.size(), 640, 640);
            else if (!m_cancel) appLog("spotify: cover fetch failed: %s", r.error.empty() ? "bad status" : r.error.c_str());
            std::lock_guard<std::mutex> l(m_mx);
            if (m_result) SDL_FreeSurface(m_result);
            m_result = s;
            m_resultUrl = url;
        }
    }
};

// ---------------------------------------------------------------- tab

class SpotifyTab;
SpotifyTab* g_spotify = nullptr;

class SpotifyTab : public Tab {
public:
    SpotifyTab() {
        static AppBellLogger logger;
        bell::bellGlobalLogger = &logger;
        g_spotify = this;
        if (!cfgBool("spotify.enabled", true)) return;
        startBackend();
    }
    ~SpotifyTab() override {
        g_spotify = nullptr;
        if (m_cover) SDL_DestroyTexture(m_cover);
    }

    const char* name() override { return "Spotify"; }
    int icon() override { return IC_MUSIC; }
    SDL_Color accent() override { return rgb(0x1ED760); }
    bool keepAwake() override { return playing(); }
    std::string badge() override { return playing() ? "playing" : ""; }

    void shutdown() override {
        m_fetcher.reset();
        appLog("spotify: cover thread stopped");
        if (!m_backend) return;
        uint32_t t0 = SDL_GetTicks();
        m_backend->stop();
        appLog("spotify: session stopped (%u ms)", SDL_GetTicks() - t0);
        // never free state a still-running thread uses
        auto* e = dynamic_cast<Embedded*>(m_backend.get());
        if (e && e->leaked()) m_backend.release();
        else m_backend.reset();
    }

    bool playing() const { return m_status.state == SwitchifyState_Playing; }
    void pauseIfPlaying() {
        if (m_backend && playing()) {
            m_backend->playPause();
            m_status.state = SwitchifyState_Paused;
        }
    }

    void background() override {
        if (!m_backend) return;
        // the status is cheap; refresh a few times a second
        if (g_ticks - m_polledAt > 250) {
            m_polledAt = g_ticks;
            m_backend->status(m_status);
            std::string url = m_backend->imageUrl();
            if (m_fetcher) m_fetcher->want(url);
            if (url.empty() && !m_coverUrl.empty()) dropCover();
        }
        if (m_fetcher) {
            std::string url;
            if (SDL_Surface* s = m_fetcher->take(url)) {
                if (m_cover) SDL_DestroyTexture(m_cover);
                m_cover = textureFromSurface(s);
                m_ublur.setCover(s);  // crossfades from the previous cover
                m_coverUrl = url;
                SDL_FreeSurface(s);
            }
        }
    }

    void update(const SDL_Rect&) override {}

    void draw(const SDL_Rect& area) override {
        if (!m_backend) { drawDisabled(area); return; }
        const SwitchifyStatus& s = m_status;
        bool loaded = s.state == SwitchifyState_Playing || s.state == SwitchifyState_Paused;
        AudioOutput* audio = m_backend->audio();
        int vis = audio ? m_visStyle : VIS_OFF;
        if (loaded && vis != VIS_OFF) m_vis.update(audio->tap, AudioOutput::queuedSamples() - 1024);
        if (m_ublur.ready() && loaded) {
            m_ublur.draw(area, vis != VIS_OFF ? m_vis.level() * 0.7f : 0);
            fillGradientV(area, SDL_Color{0, 0, 0, 60}, SDL_Color{0, 0, 0, 175});
        }
        if (m_eq.open) { m_eq.draw(area, audio); return; }
        SDL_Color visColor = m_ublur.ready() ? m_ublur.accent() : g_theme.accent;
        if (loaded && vis != VIS_OFF) m_vis.drawBackground(area, vis, visColor);

        int x0 = area.x + 48;
        drawText("Spotify", x0, 26, 36, g_theme.fg, 0, 0, FS_BOLD);
        std::string sub = s.device_name[0] ? std::string(s.device_name) : deviceName();
        if (s.username[0]) sub += "  \xC2\xB7  " + std::string(s.username);
        if (m_backend->external()) sub += "  \xC2\xB7  via Switchify";
        drawText(sub, x0, 72, 20, g_theme.dim, 0, area.w - 380);
        if (iconButton(area.x + area.w - 56.0f, 52, 30, IC_GEAR, g_theme.fg, SDL_Color{0, 0, 0, 0})) m_menu = true;
        SDL_Color eqC = m_eq.enabled() && audio ? g_theme.accent : g_theme.fg;
        if (iconButton(area.x + area.w - 128.0f, 52, 30, IC_EQ, eqC, SDL_Color{0, 0, 0, 0})) m_eq.open = true;
        SDL_Color visC = vis != VIS_OFF ? g_theme.accent : g_theme.fg;
        if (iconButton(area.x + area.w - 200.0f, 52, 30, IC_WAVE, visC, SDL_Color{0, 0, 0, 0})) {
            if (!audio) toast("The visualizer needs the built-in receiver (not the Switchify sysmodule)", 3500);
            else {
                m_visStyle = (m_visStyle + 1) % VIS_COUNT;
                cfgSetInt("spotify.visualizer", m_visStyle);
                toast(std::string("Visualizer: ") + visStyleName(m_visStyle));
            }
        }

        if (loaded) drawPlayer(area, vis, visColor);
        else drawWaiting(area);
        if (m_menu) drawMenu(area);
    }

    void startBackend() {
        if (serviceRunning(SWITCHIFY_SERVICE_NAME)) {
            appLog("spotify: Switchify sysmodule found, using it");
            m_backend = std::make_unique<Remote>();
        } else {
            m_backend = std::make_unique<Embedded>();
        }
        m_fetcher = std::make_unique<CoverFetcher>();
        m_eq.apply(m_backend->audio());
    }

private:
    std::unique_ptr<Backend> m_backend;
    std::unique_ptr<CoverFetcher> m_fetcher;
    SwitchifyStatus m_status = {};
    uint32_t m_polledAt = 0, m_seekAt = 0, m_volAt = 0;
    SDL_Texture* m_cover = nullptr;
    Ultrablur m_ublur;
    Visualizer m_vis;
    EqSheet m_eq;
    int m_visStyle = SDL_clamp(cfgInt("spotify.visualizer", VIS_BARS), 0, VIS_COUNT - 1);
    std::string m_coverUrl;
    bool m_seeking = false, m_menu = false, m_volDrag = false;
    float m_seekV = 0, m_vol = 0.7f;
    u32 m_volLast = 1000, m_volSent = 1000;
    uint32_t m_volMovedAt = 0;

    void dropCover() {
        if (m_cover) SDL_DestroyTexture(m_cover);
        m_cover = nullptr;
        m_ublur.clear();
        m_coverUrl.clear();
    }

    void drawPlayer(const SDL_Rect& area, int vis, SDL_Color visColor) {
        const SwitchifyStatus& s = m_status;
        int size = std::min(440, area.h - 200);
        SDL_Rect art = {area.x + 64, 130, size, size};
        if (m_cover) {
            m_vis.drawCoverGlow(art, vis, visColor);
            float sc = m_vis.coverScale(vis);
            int grow = (int)(size * (sc - 1) / 2);
            SDL_Rect a = {art.x - grow, art.y - grow, art.w + grow * 2, art.h + grow * 2};
            fillRound({a.x + 6, a.y + 10, a.w, a.h}, 18, SDL_Color{0, 0, 0, 120});
            drawTexFit(m_cover, a, true, 255);
        } else {
            fillRound(art, 18, g_theme.panel2);
            drawIcon(IC_MUSIC, art.x + art.w / 2.0f, art.y + art.h / 2.0f, 120, g_theme.faint);
        }
        int tx = art.x + art.w + 56, tw = area.x + area.w - tx - 56;
        int ty = art.y + 30;
        ty += drawWrapped(s.title[0] ? s.title : "Unknown track", tx, ty, 40, g_theme.fg, tw, 2, 0, FS_BOLD) + 8;
        drawText(s.artist, tx, ty, 28, g_theme.fg, 0, tw);
        ty += 42;
        drawText(s.album, tx, ty, 22, g_theme.dim, 0, tw);

        // progress
        int py = art.y + art.h - 150;
        if (!m_seeking && g_ticks - m_seekAt > 1500) m_seekV = s.duration_ms ? s.position_ms / (float)s.duration_ms : 0;
        bool rel = false;
        if (slider({tx, py, tw, 8}, m_seekV, g_theme.fg, withAlpha(g_theme.fg, 60), &rel)) {
            m_seeking = !rel;
            if (rel && s.duration_ms) { m_backend->seek((u32)(m_seekV * s.duration_ms)); m_seekAt = g_ticks; }
        }
        u32 shownMs = (u32)(m_seekV * s.duration_ms);
        drawText(formatTime(shownMs), tx, py + 18, 20, g_theme.dim);
        drawText(formatTime(s.duration_ms), tx + tw, py + 18, 20, g_theme.dim, 2);

        // transport
        float cy = py + 100.0f, cx = tx + tw / 2.0f;
        if (iconButton(cx - 130, cy, 34, IC_PREV, g_theme.fg, SDL_Color{0, 0, 0, 0})) m_backend->prev();
        if (iconButton(cx, cy, 46, s.state == SwitchifyState_Playing ? IC_PAUSE : IC_PLAY, SDL_Color{10, 10, 12, 255}, g_theme.fg, 0.48f))
            m_backend->playPause();
        if (iconButton(cx + 130, cy, 34, IC_NEXT, g_theme.fg, SDL_Color{0, 0, 0, 0})) m_backend->next();
        // the buttons (see ui/input.cpp): X plays/pauses, R skips ahead, L goes back
        if (!g_in.consumed) {
            if (g_in.playPause) m_backend->playPause();
            if (g_in.buttonsDown & HidNpadButton_R) m_backend->next();
            if (g_in.buttonsDown & HidNpadButton_L) m_backend->prev();
        }

        // volume
        int vy = (int)cy + 90;
        if (vy + 20 < area.h) {
            drawIcon(IC_AUDIO, tx + 16.0f, vy + 4.0f, 30, g_theme.dim);
            if (!m_volDrag && g_ticks - m_volAt > 1500) m_vol = s.volume / 100.0f;
            bool vrel = false;
            if (slider({tx + 52, vy, tw - 52, 6}, m_vol, g_theme.accent, withAlpha(g_theme.fg, 60), &vrel)) {
                m_volDrag = !vrel;
                // Each Spotify volume change is pushed to every device: change ours live,
                // tell Spotify on release or when the finger rests (debounced below).
                u32 v = (u32)lroundf(m_vol * 100);
                if (vrel) { m_backend->setVolume(v); m_volSent = v; }
                else m_backend->previewVolume(v);
                if (v != m_volLast) { m_volLast = v; m_volMovedAt = g_ticks; }
                m_volAt = g_ticks;
            }
            // debounce: while dragging, tell Spotify once the finger rests a moment
            if (m_volDrag && m_volLast != m_volSent && g_ticks - m_volMovedAt > 400) {
                m_backend->setVolume(m_volLast);
                m_volSent = m_volLast;
            }
        }
    }

    void drawWaiting(const SDL_Rect& area) {
        const SwitchifyStatus& s = m_status;
        int cx = area.x + area.w / 2, cy = area.h / 2;
        std::string title, body = s.message;
        int icon = IC_MUSIC;
        bool spin = false;
        switch (s.state) {
            case SwitchifyState_NoNetwork: title = "No internet connection"; icon = IC_CLOUD; break;
            case SwitchifyState_WaitingPairing:
                title = "Pair with your phone";
                body = "Open Spotify on a phone or computer on the same Wi-Fi, tap the Devices icon and pick \"" +
                       std::string(s.device_name[0] ? s.device_name : deviceName().c_str()) + "\". You only need to do this once.";
                icon = IC_DEVICE;
                break;
            case SwitchifyState_Connecting: title = "Signing in\xE2\x80\xA6"; spin = true; break;
            case SwitchifyState_Idle:
                title = "Ready to play";
                body = "Pick \"" + std::string(s.device_name) + "\" from Spotify's device list on your phone.";
                break;
            case SwitchifyState_Error: title = "Something went wrong"; icon = IC_WARN; break;
            default: title = "Starting\xE2\x80\xA6"; spin = true; break;
        }
        fillCircle((float)cx, cy - 110.0f, 70, withAlpha(g_theme.accent, 40));
        if (spin) spinner((float)cx, cy - 110.0f, 40, g_theme.accent);
        else drawIcon(icon, (float)cx, cy - 110.0f, 70, g_theme.accent);
        drawText(title, cx, cy - 10, 34, g_theme.fg, 1, area.w - 120, FS_BOLD);
        drawWrapped(body, area.x + 120, cy + 44, 24, g_theme.dim, area.w - 240, 4, 1);
        drawText("Spotify Premium is required for Connect devices.", cx, area.h - 60, 18, g_theme.faint, 1);
    }

    void drawMenu(const SDL_Rect& area) {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        fillRect(area, SDL_Color{0, 0, 0, 160});
        int w = 640, h = 420;
        SDL_Rect r = {area.x + (area.w - w) / 2, (area.h - h) / 2, w, h};
        fillRound(r, 24, g_theme.panel);
        strokeRound(r, 24, 2, g_theme.line);
        drawText("Spotify settings", r.x + 32, r.y + 26, 28, g_theme.fg, 0, 0, FS_BOLD);
        if (iconButton(r.x + w - 48.0f, r.y + 46.0f, 26, IC_CLOSE, g_theme.fg, SDL_Color{0, 0, 0, 0})) m_menu = false;
        int y = r.y + 90;
        drawText("Device name", r.x + 32, y, 20, g_theme.dim);
        drawText(deviceName(), r.x + 32, y + 28, 26, g_theme.fg, 0, w - 260);
        if (button({r.x + w - 200, y + 6, 168, 56}, "Rename", IC_EDIT, BTN_NORMAL, 20)) {
            std::string n;
            if (keyboard("Spotify device name", deviceName(), n, 40) && !trim(n).empty()) {
                cfgSet("spotify.name", trim(n));
                toast("Restart the app to use the new name");
            }
        }
        y += 96;
        drawText(m_backend && m_backend->external() ? "Using the Switchify sysmodule (keeps playing outside this app)."
                                                    : "Built-in receiver (plays while this app is open).",
                 r.x + 32, y, 20, g_theme.dim, 0, w - 64);
        y += 50;
        if (button({r.x + 32, y, w - 64, 64}, "Log out and pair again", IC_LOGOUT, BTN_DANGER))
            dialog("Log out of Spotify?", "You'll need to pick this device from the Spotify app again.", {"Cancel", "Log out"},
                   [this](int b) { if (b == 1 && m_backend) { m_backend->logout(); m_menu = false; } });
        y += 84;
        bool en = cfgBool("spotify.enabled", true);
        drawText("Enable Spotify receiver", r.x + 32, y + 18, 22, g_theme.fg);
        if (toggle({r.x + 32, y, w - 64, 60}, en)) {
            cfgSetBool("spotify.enabled", en);
            toast(en ? "Spotify will start next launch" : "Spotify turns off next launch");
        }
        if (g_in.back || (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r))) m_menu = false;
        g_in.consumed = true;
    }

    void drawDisabled(const SDL_Rect& area) {
        drawText("Spotify", area.x + 48, 26, 36, g_theme.fg, 0, 0, FS_BOLD);
        drawText("The Spotify receiver is turned off.", area.x + area.w / 2, area.h / 2 - 40, 28, g_theme.fg, 1);
        if (button({area.x + area.w / 2 - 140, area.h / 2 + 10, 280, 64}, "Turn on", IC_POWER, BTN_ACCENT)) {
            cfgSetBool("spotify.enabled", true);
            startBackend();
        }
    }
};

}  // namespace

Tab* makeSpotifyTab() { return new SpotifyTab(); }
void spotifyPauseForVideo() { if (g_spotify) g_spotify->pauseIfPlaying(); }
