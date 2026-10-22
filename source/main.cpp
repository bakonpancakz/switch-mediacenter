// Entry point: services, the tab rail and the main loop.
#include "app.h"
#include "core/http.h"
#include "core/library.h"
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <cmath>
#include <ctime>

bool g_quitRequested = false;

// More parallel SD card access: thumbnails, video, books, the folder listers
// all read at once (libnx defaults to 3 sessions).
extern "C" {
u32 __nx_fs_num_sessions = 8;
}

static const int RAIL_W = 112;
static bool s_appSettings = false;

// App-wide settings sheet (opened from the rail).
static void drawAppSettings() {
    InputLayer layer;  // on top: gets the input, the screen under it doesn't
    fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 160});
    int w = std::min(860, g_W - 40), h = 700;  // (narrower on a portrait screen, like Books)
    SDL_Rect r = {(g_W - w) / 2, (g_H - h) / 2, w, h};
    fillRound(r, 24, g_theme.panel);
    strokeRound(r, 24, 2, g_theme.line);
    drawText("Settings", r.x + 32, r.y + 24, 30, g_theme.fg, 0, 0, FS_BOLD);
    if (iconButton(r.x + w - 48.0f, r.y + 44.0f, 26, IC_CLOSE, g_theme.fg, SDL_Color{0, 0, 0, 0})) s_appSettings = false;
    int x = r.x + 32, rw = w - 64, y = r.y + 82;
    int face = uiFace();
    if (int d = stepper({x, y, rw, 70}, "Interface font", fontFaceName(face)))
        cfgSetInt("app.font", (face + d + fontFaceCount()) % fontFaceCount());
    y += 72;
    drawText("The quick brown fox jumps over the lazy dog  \xC2\xB7  book fonts are in a book's Aa menu", x, y, 20,
             g_theme.dim, 0, rw);
    y += 44;

    sectionTitle("LIBRARY", x, y);
    y += 34;
    std::string lib = Library::get().problem();
    if (Library::get().ready()) {
        time_t at = (time_t)Library::get().builtAt();
        char when[64] = "";
        if (struct tm* lt = localtime(&at)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", lt);
        lib = "From " + (Library::get().builtOn().empty() ? std::string("your PC") : Library::get().builtOn()) + ", " + when +
              "  \xC2\xB7  " + std::to_string(Library::get().count(0)) + " files, " + std::to_string(Library::get().count(1)) +
              " videos, " + std::to_string(Library::get().count(2)) + " books";
    }
    drawText(lib, x, y + 4, 22, g_theme.fg, 0, rw);
    drawWrapped("To change it: put this SD card in your PC (a card reader, or hekate's USB mass storage) and sync with "
                "Media Center Sync.",
                x, y + 40, 18, g_theme.dim, rw, 3);
    y += 110;

    sectionTitle("DISPLAY", x, y);
    y += 34;
    bool turn = cfgBool("display.autorotate", true);
    drawText("Auto-rotate photos, clips and books", x, y + 14, 22, g_theme.fg);
    if (toggle({x, y, rw, 52}, turn)) cfgSetBool("display.autorotate", turn);
    y += 56;
    drawWrapped("Uses the Joy-Cons on the console's rails (they turn with the screen), in handheld mode.", x, y, 18,
                g_theme.dim, rw, 2);
    y += 60;
    if (g_in.back || (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r))) s_appSettings = false;
    g_in.consumed = true;
}

static void initSockets() {
    // Several threads block in recv at once (Spotify, its pairing server, cover
    // art, TMDB), so ask for more BSD sessions than default.
    static const SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x10000,
        .tcp_rx_buf_size = 0x20000,
        .tcp_tx_buf_max_size = 0x80000,
        .tcp_rx_buf_max_size = 0x100000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 4,
        .num_bsd_sessions = 16,
        .bsd_service_type = BsdServiceType_Auto,
    };
    Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        appLog("socketInitialize(custom) failed 0x%x, using defaults", rc);
        socketInitializeDefault();
    }
}

static void drawRail(std::vector<Tab*>& tabs, int& active) {
    fillRect(0, 0, RAIL_W, g_H, g_theme.panel);
    fillRect(RAIL_W - 1, 0, 1, g_H, g_theme.line);
    // clock + battery
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0);
    drawText(clock, RAIL_W / 2, 18, 24, g_theme.fg, 1, 0, FS_BOLD);
    u32 batt = 0;
    psmGetBatteryChargePercentage(&batt);
    PsmChargerType charger = PsmChargerType_Unconnected;
    psmGetChargerType(&charger);
    char b[16];
    snprintf(b, sizeof(b), "%s%u%%", charger != PsmChargerType_Unconnected ? "+" : "", batt);
    drawText(b, RAIL_W / 2, 48, 18, g_theme.dim, 1);

    int n = (int)tabs.size(), itemH = n > 4 ? 96 : 112;  // five tabs must clear the gear and power buttons
    int top = (g_H - n * itemH) / 2 - 30;
    for (int i = 0; i < n; i++) {
        SDL_Rect r = {8, top + i * itemH + 6, RAIL_W - 16, itemH - 12};
        bool sel = i == active;
        SDL_Color ac = tabs[i]->accent();
        if (sel) fillRound(r, 22, withAlpha(ac, 46));
        else if (pressing(r)) fillRound(r, 22, g_theme.panel2);
        SDL_Color c = sel ? ac : g_theme.dim;
        drawIcon(tabs[i]->icon(), r.x + r.w / 2.0f, r.y + r.h * 0.38f, 40, c);
        drawText(tabs[i]->name(), r.x + r.w / 2, r.y + r.h - 34, 18, sel ? g_theme.fg : g_theme.dim, 1, r.w - 4, sel ? FS_BOLD : 0);
        std::string badge = tabs[i]->badge();
        if (!badge.empty()) fillCircle(r.x + r.w - 20.0f, r.y + 18.0f, 6, ac);
        if (tapped(r) && i != active) {
            tabs[active]->leave();
            active = i;
            uint32_t e0 = SDL_GetTicks();
            tabs[active]->enter();
            if (SDL_GetTicks() - e0 > 200) appLog("slow: opening %s took %u ms", tabs[active]->name(), SDL_GetTicks() - e0);
            cfgSetInt("app.tab", active);
        }
    }
    if (iconButton(RAIL_W / 2.0f, g_H - 120.0f, 30, IC_GEAR, g_theme.dim, SDL_Color{0, 0, 0, 0})) s_appSettings = true;
    if (iconButton(RAIL_W / 2.0f, g_H - 52.0f, 32, IC_POWER, g_theme.dim, SDL_Color{0, 0, 0, 0}))
        dialog("Quit Media Center?", "Spotify stops while the app is closed.", {"Cancel", "Quit"},
               [](int b) { if (b == 1) g_quitRequested = true; });
}

// After exit the launcher (sphaira's hbl) reads the next NRO into the heap we
// used, with one IPC transfer; the kernel refuses it (0xD401, a crash in hbl)
// if any heap page is still borrowed, IPC-mapped, device-mapped or uncached.
// Logs every region still in such a state, so a leak shows up in log.txt.
static void logMemoryLeftovers(const char* when) {
    u64 hs = 0, he = 0;
    if (envHasHeapOverride()) {
        hs = (u64)envGetHeapOverrideAddr();
        he = hs + envGetHeapOverrideSize();
    }
    int found = 0;
    u64 addr = 0;
    for (int guard = 0; guard < 4096; guard++) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
        bool inHeap = mi.addr < he && mi.addr + mi.size > hs;
        u32 type = mi.type & 0xFF;
        bool odd = mi.attr != 0 || type == MemType_TransferMem || type == MemType_TransferMemIsolated ||
                   (inHeap && type != MemType_Heap && type != MemType_Unmapped);
        if (odd && found++ < 40)
            appLog("%s: %s region %lx +%lx type %x attr %x perm %x ipc %u dev %u", when, inHeap ? "heap" : "other",
                   (unsigned long)mi.addr, (unsigned long)mi.size, type, mi.attr, mi.perm, mi.ipc_refcount, mi.device_refcount);
        u64 next = mi.addr + mi.size;
        if (next <= addr) break;
        addr = next;
    }
    appLog("%s: %d unusual memory regions (heap %lx-%lx)", when, found, (unsigned long)hs, (unsigned long)he);
}

// Shown while the app closes: a sleepy mochi saying bye, with a dot per
// shutdown step filling in (stopping network threads can take a few seconds).
static void drawClosing(int done, int steps) {
    const float PI = 3.14159265f;
    g_W = 1280;
    g_H = 720;
    SDL_SetRenderTarget(g_ren, nullptr);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    float cx = 640, cy = 290;
    SDL_Color body = {255, 216, 232, 255}, ink = {70, 40, 56, 255}, blush = {255, 140, 175, 170};
    fillRound({(int)cx - 86, (int)cy + 62, 172, 14}, 7, SDL_Color{255, 255, 255, 18});  // shadow
    fillRound({(int)cx - 84, (int)cy - 64, 168, 132}, 60, body);                       // mochi
    // happy closed eyes (^ ^), blush and a small smile
    drawArc(cx - 30, cy + 4, 11, PI * 1.12f, PI * 1.88f, 4.5f, ink);
    drawArc(cx + 30, cy + 4, 11, PI * 1.12f, PI * 1.88f, 4.5f, ink);
    fillCircle(cx - 52, cy + 20, 12, blush);
    fillCircle(cx + 52, cy + 20, 12, blush);
    drawArc(cx, cy + 14, 8, PI * 0.15f, PI * 0.85f, 3.5f, ink);
    // sleepy z's drifting up as it goes
    float lift = done * 6.0f;
    drawText("z", (int)(cx + 92), (int)(cy - 62 - lift), 26, withAlpha(g_theme.fg, 150), 1, 0, FS_BOLD);
    drawText("z", (int)(cx + 116), (int)(cy - 96 - lift), 32, withAlpha(g_theme.fg, 190), 1, 0, FS_BOLD);
    drawText("Z", (int)(cx + 146), (int)(cy - 138 - lift), 40, withAlpha(g_theme.fg, 230), 1, 0, FS_BOLD);
    drawText("Bye bye! See you soon", (int)cx, (int)cy + 104, 32, g_theme.fg, 1, 0, FS_BOLD);
    drawText("Saving your place and tucking everything in", (int)cx, (int)cy + 148, 20, g_theme.dim, 1);
    // progress
    int dot = 14, gap = 12, w = steps * dot + (steps - 1) * gap, x = (int)cx - w / 2, y = (int)cy + 196;
    for (int i = 0; i < steps; i++)
        fillCircle(x + i * (dot + gap) + dot / 2.0f, y + dot / 2.0f, dot / 2.0f, i < done ? body : withAlpha(g_theme.fg, 40));
    SDL_RenderPresent(g_ren);
}

// Borrowed heap pages (lent to a service as transfer memory, attr bit 0): the
// kind that's left over at exit. Logs each one appearing or going away, with a
// label for what just happened, so the owner can be found.
static std::vector<std::pair<u64, u64>> s_borrowed;
static void trackBorrowed(const char* label) {
    if (!envHasHeapOverride()) return;
    u64 hs = (u64)envGetHeapOverrideAddr(), he = hs + envGetHeapOverrideSize();
    std::vector<std::pair<u64, u64>> now;
    u64 addr = hs;
    for (int guard = 0; guard < 4096 && addr < he; guard++) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
        if (mi.attr & MemAttr_IsBorrowed) now.push_back({mi.addr, mi.size});
        u64 next = mi.addr + mi.size;
        if (next <= addr) break;
        addr = next;
    }
    for (auto& r : now)
        if (std::find(s_borrowed.begin(), s_borrowed.end(), r) == s_borrowed.end())
            appLog("borrowed + %lx +%lx  (%s)", (unsigned long)r.first, (unsigned long)r.second, label);
    for (auto& r : s_borrowed)
        if (std::find(now.begin(), now.end(), r) == now.end())
            appLog("borrowed - %lx +%lx  (%s)", (unsigned long)r.first, (unsigned long)r.second, label);
    s_borrowed.swap(now);
}

// The Spotify login and EQ presets used to live in a spotify/ folder; they're
// plain files at the top of the app's folder now.
static void moveOldFiles() {
    struct Move { const char* from; const char* to; };
    for (Move m : {Move{"/spotify/credentials.json", "/spotify-login.json"}, Move{"/spotify/eq_presets.txt", "/spotify-eq.txt"}}) {
        std::string from = std::string(APP_DIR) + m.from, to = std::string(APP_DIR) + m.to;
        if (fileExists(from) && !fileExists(to)) rename(from.c_str(), to.c_str());
    }
    rmdir(APP_DIR "/spotify");  // only if that emptied it
}

int main(int, char**) {
    romfsInit();
    srand((unsigned)time(nullptr));
    mkdirs(APP_DIR);
    moveOldFiles();
    appLogReset();
    appLog("Media Center starting");
    trackBorrowed("start");
    cfgLoad();
    initSockets();
    httpInit();
    trackBorrowed("sockets");
    nifmInitialize(NifmServiceType_User);
    psmInitialize();
    setsysInitialize();
    plInitialize(PlServiceType_User);
    trackBorrowed("nifm/psm/setsys/pl");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { appLog("SDL_Init: %s", SDL_GetError()); return 1; }
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    SDL_Window* win = SDL_CreateWindow("Media Center", 0, 0, 1280, 720, SDL_WINDOW_OPENGL);
    g_ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!win || !g_ren) { appLog("window/renderer: %s", SDL_GetError()); SDL_Quit(); return 1; }
    trackBorrowed("SDL window and renderer");
    SDL_RendererInfo ri;
    SDL_GetRendererInfo(g_ren, &ri);
    appLog("renderer %s, max texture %dx%d", ri.name, ri.max_texture_width, ri.max_texture_height);
    if (!gfxInit()) { appLog("font init failed: %s", SDL_GetError()); SDL_Quit(); return 1; }
    trackBorrowed("fonts");
    inputInit();
    orientationInit();
    trackBorrowed("input");
    Library::get().open();
    picturesInit();
    trackBorrowed("library and pictures");

    std::vector<Tab*> tabs;
    tabs.push_back(makeFilesTab());
    tabs.push_back(makeMediaTab());
    trackBorrowed("Files and Media tabs");
    tabs.push_back(makeBooksTab());
    trackBorrowed("Books tab");
    tabs.push_back(makeSpotifyTab());
    trackBorrowed("Spotify tab");
    uint32_t trackedAt = 0;
    int active = SDL_clamp(cfgInt("app.tab", 0), 0, (int)tabs.size() - 1);
    tabs[active]->enter();

    // ARGB8888: the GLES renderer can't use RGBA8888 as a render target.
    SDL_Texture* canvasL = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 1280, 720);
    SDL_Texture* canvasP = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 720, 1280);
    bool keepAwake = false;
    uint32_t last = SDL_GetTicks();
    float shownAngle = 0;  // the canvas's angle on screen: follows the tab's rotation with a turn
    // Keep running on the HOME menu so Spotify carries on (applications
    // are suspended there by default). We stop drawing while out of focus.
    bool background = false;
    AppletFocusState lastFocus = AppletFocusState_InFocus;
    if (appletGetAppletType() == AppletType_Application) appletSetFocusHandlingMode(AppletFocusHandlingMode_NoSuspend);

    while (!g_quitRequested && appletMainLoop()) {
        uint32_t now = SDL_GetTicks();
        g_dt = SDL_clamp((now - last) / 1000.0f, 0.001f, 0.1f);
        last = now;
        g_ticks = now;
        if (now - trackedAt > 1000) {  // borrowed-memory tracking (see trackBorrowed)
            trackedAt = now;
            trackBorrowed(tabs[active]->name());
        }
        // Only stop drawing when we're hidden (HOME menu, sleep). Out of focus
        // means something sits on top of the still-visible app (a system applet,
        // the dock/TV-output switch): the system keeps waiting for our frames
        // then, and when they stopped the whole console hung (docking, and the
        // system keyboard before).
        AppletFocusState fs = appletGetFocusState();
        if (fs != lastFocus) {
            appLog("focus state %d -> %d", (int)lastFocus, (int)fs);
            lastFocus = fs;
        }
        if (fs == AppletFocusState_Background) {
            if (!background) { background = true; tabs[active]->leave(); appLog("in the background"); }
            for (Tab* t : tabs) t->background();
            cfgTick();
            svcSleepThread(50'000'000ULL);
            continue;
        }
        if (background) {
            background = false;
            orientationResume();
            uint32_t e0 = SDL_GetTicks();
            tabs[active]->enter();
            appLog("back from the background (%s took %u ms to resume)", tabs[active]->name(), SDL_GetTicks() - e0);
        }
        orientationTick();
        Tab* tab = tabs[active];
        g_theme.accent = tab->accent();
        int rot = (canvasP && canvasL) ? tab->rotation() : 0;
        g_W = rot % 180 ? 720 : 1280;  // turned a quarter: portrait
        g_H = rot % 180 ? 1280 : 720;
        uint32_t t0 = SDL_GetTicks();  // frame timing: slow frames are logged with where the time went
        inputUpdate(rot);
        uint32_t tInput = SDL_GetTicks();
        picturesTick();
        for (Tab* t : tabs) t->background();

        bool full = tab->fullscreen();
        SDL_Rect area = full ? SDL_Rect{0, 0, g_W, g_H} : SDL_Rect{RAIL_W, 0, g_W - RAIL_W, g_H};
        if (dialogActive()) dialogUpdate();
        else if (s_appSettings) {
        } else {
            if (g_in.quit)
                dialog("Quit Media Center?", "Spotify stops while the app is closed.", {"Cancel", "Quit"},
                       [](int b) { if (b == 1) g_quitRequested = true; });
            tab->update(area);
        }

        bool awake = false;
        for (Tab* t : tabs) awake |= t->keepAwake();
        if (awake != keepAwake) { appletSetMediaPlaybackState(awake); keepAwake = awake; }

        // ---- draw
        uint32_t tUpdate = SDL_GetTicks();
        SDL_RenderFlush(g_ren);
        tab = tabs[active];  // the rail may have switched tabs during update
        tab->preRender();
        rot = (canvasP && canvasL) ? tab->rotation() : 0;
        g_W = rot % 180 ? 720 : 1280;  // turned a quarter: portrait
        g_H = rot % 180 ? 1280 : 720;
        full = tab->fullscreen();
        area = full ? SDL_Rect{0, 0, g_W, g_H} : SDL_Rect{RAIL_W, 0, g_W - RAIL_W, g_H};
        SDL_Texture* canvas = rot % 180 ? canvasP : canvasL;
        SDL_SetRenderTarget(g_ren, canvas);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        {
            // Screens handle taps while drawing (immediate mode); a dialog on
            // top owns the input, so hide it from everything underneath.
            bool wasDialog = dialogActive() || (s_appSettings && !full), c = g_in.consumed;
            if (wasDialog) g_in.consumed = true;
            tab->draw(area);
            if (wasDialog) g_in.consumed = c;
        }
        if (!full && !dialogActive()) drawRail(tabs, active);
        else if (!full) {
            // keep the rail visible (but inert) behind dialogs
            bool c = g_in.consumed;
            g_in.consumed = true;
            drawRail(tabs, active);
            g_in.consumed = c;
        }
        if (s_appSettings && !full) drawAppSettings();
        dialogDraw();
        drawToasts();

        SDL_SetRenderTarget(g_ren, nullptr);
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_ren);
        if (canvas) {
            // A new rotation turns into place: the canvas swings the short way
            // round from the old angle, shrunk on the way so it stays on screen.
            float diff = fmodf(rot - shownAngle + 540.0f, 360.0f) - 180.0f;
            shownAngle += fabsf(diff) < 0.5f ? diff : diff * std::min(1.0f, g_dt * 10);
            shownAngle = fmodf(shownAngle + 360.0f, 360.0f);
            if (!rot && shownAngle == 0) SDL_RenderCopy(g_ren, canvas, nullptr, nullptr);
            else {
                float cw = rot % 180 ? 720.0f : 1280.0f, ch = rot % 180 ? 1280.0f : 720.0f, a = shownAngle * (float)M_PI / 180;
                float bw = cw * fabsf(cosf(a)) + ch * fabsf(sinf(a)), bh = cw * fabsf(sinf(a)) + ch * fabsf(cosf(a));
                float sc = std::min({1.0f, 1280 / bw, 720 / bh});
                SDL_Rect dst = {(int)lroundf(640 - cw * sc / 2), (int)lroundf(360 - ch * sc / 2), (int)lroundf(cw * sc),
                                (int)lroundf(ch * sc)};
                SDL_RenderCopyEx(g_ren, canvas, nullptr, &dst, shownAngle, nullptr, SDL_FLIP_NONE);
            }
        }
        int bright = tab->brightness();
        if (bright < 100) fillRect(0, 0, 1280, 720, SDL_Color{0, 0, 0, (Uint8)(255 * (100 - bright) / 100)});
        g_W = 1280;
        g_H = 720;
        drawCursor();
        uint32_t tDraw = SDL_GetTicks();
        SDL_RenderPresent(g_ren);
        uint32_t tPresent = SDL_GetTicks();
        if (tPresent - t0 > 200)
            appLog("slow frame: %u ms (input %u, update %u, draw %u, present %u) on %s", tPresent - t0, tInput - t0,
                   tUpdate - tInput, tDraw - tUpdate, tPresent - tDraw, tabs[active]->name());
        gfxTrimCache();
        cfgTick();
        inputFrameEnd();  // decides which input layer owns the next frame
    }

    appLog("shutting down");
    const int closeSteps = (int)tabs.size() + 1;
    int closeDone = 0;
    drawClosing(closeDone, closeSteps);
    // each step is logged, so a crash on exit shows in log.txt where it happened
    for (Tab* t : tabs) {
        appLog("exit: stopping %s", t->name());
        t->shutdown();
        trackBorrowed(t->name());
        drawClosing(++closeDone, closeSteps);
    }
    appLog("exit: tabs stopped");
    picturesShutdown();
    orientationExit();
    appLog("exit: pictures stopped");
    drawClosing(++closeDone, closeSteps);
    logMemoryLeftovers("after tabs");
    posFlush();
    cfgFlush();
    if (keepAwake) appletSetMediaPlaybackState(false);
    // hand the launcher back the default focus mode (we set NoSuspend at startup)
    if (appletGetAppletType() == AppletType_Application) appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
    for (Tab* t : tabs) {
        appLog("exit: deleting %s", t->name());
        delete t;
    }
    Library::get().close();  // after the tabs and pictures, which read it
    if (canvasL) SDL_DestroyTexture(canvasL);
    if (canvasP) SDL_DestroyTexture(canvasP);
    appLog("exit: graphics");
    gfxQuit();
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    appLog("exit: services");
    trackBorrowed("graphics shut down");
    logMemoryLeftovers("after graphics");
    plExit();
    setsysExit();
    psmExit();
    nifmExit();
    httpExit();
    socketExit();
    romfsExit();
    trackBorrowed("services closed");
    logMemoryLeftovers("exit memory");
    appLog("exit: done");
    return 0;
}
