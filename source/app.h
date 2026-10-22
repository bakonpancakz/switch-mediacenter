// Shared declarations for the media center: graphics, touch input, widgets,
// settings and small helpers. Everything is drawn in logical canvas
// coordinates (1280x720, or 720x1280 when the reader is rotated).
#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The app's folder: the app, system.db and .media from the PC (see
// shared/library.h), and the app's own small text files (settings, positions, log).
#define APP_DIR "sdmc:/switch/mediacenter"

// ---------------------------------------------------------------- colors
SDL_Color rgb(uint32_t hex, Uint8 a = 255);
SDL_Color withAlpha(SDL_Color c, Uint8 a);
SDL_Color mix(SDL_Color a, SDL_Color b, float t);

// Everything drawn is this see-through (0..1): fades whole groups of controls.
extern float g_fade;
struct FadeScope {
    float saved;
    explicit FadeScope(float f) : saved(g_fade) { g_fade *= SDL_clamp(f, 0.0f, 1.0f); }
    ~FadeScope() { g_fade = saved; }
};

struct Theme {
    SDL_Color bg, panel, panel2, line, fg, dim, faint, accent, danger, ok;
};
extern Theme g_theme;  // accent follows the active tab

// ---------------------------------------------------------------- settings
// Flat key=value store in APP_DIR/settings.ini. Modules namespace their keys
// ("books.font_size"). Saves are batched; cfgFlush() writes immediately.
std::string cfgStr(const std::string& key, const std::string& def = "");
int  cfgInt(const std::string& key, int def);
bool cfgBool(const std::string& key, bool def);
void cfgSet(const std::string& key, const std::string& v);
void cfgSetInt(const std::string& key, int v);
void cfgSetBool(const std::string& key, bool v);
void cfgLoad();
void cfgFlush();
void cfgTick();  // writes pending changes after a short delay

// Per-file positions (book offsets, video milliseconds): "pos total" by path.
bool posGet(const std::string& path, int64_t& pos, int64_t& total);
void posSet(const std::string& path, int64_t pos, int64_t total);
void posFlush();

// ---------------------------------------------------------------- util
std::string lower(std::string s);
std::string ext(const std::string& path);
std::string baseName(const std::string& path);
std::string dirName(const std::string& path);
std::string stem(const std::string& path);
std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& p);
bool naturalLess(const std::string& a, const std::string& b);
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);
bool fileExists(const std::string& path);
bool isDir(const std::string& path);
int64_t fileSize(const std::string& path);
void mkdirs(const std::string& path);
std::string formatBytes(double bytes);
std::string formatTime(int64_t ms);  // h:mm:ss or m:ss
uint64_t fnv64(const std::string& s);
uint32_t utf8Next(const std::string& s, size_t& i);
void appendUtf8(std::string& out, uint32_t cp);
uint64_t nowMs();  // monotonic
void appLogReset();
void appLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

bool isImageExt(const std::string& e);
bool isAudioExt(const std::string& e);
bool isComicExt(const std::string& e);

// ---------------------------------------------------------------- graphics
extern SDL_Renderer* g_ren;
extern int g_W, g_H;      // logical canvas size
extern uint32_t g_ticks;  // SDL_GetTicks() at frame start
extern float g_dt;        // seconds since the previous frame
// Moves v toward target (0 or 1) at `speed` per second; for fades.
inline void approach(float& v, float target, float speed) {
    float d = target - v, step = speed * g_dt;
    v = fabsf(d) <= step ? target : v + (d > 0 ? step : -step);
}

enum { FS_REGULAR = 0, FS_BOLD = 1, FS_ITALIC = 2 };
bool gfxInit();
void gfxQuit();
// Font faces: 0 is the Switch system font, the rest are bundled in romfs.
// face -1 means the UI face (setting "app.font").
int fontFaceCount();
const char* fontFaceName(int face);
int uiFace();
TTF_Font* font(int size, int style = FS_REGULAR, int face = -1);
// The face's font, or the system font if the face lacks a character in s.
TTF_Font* fontFor(const std::string& s, int size, int style = FS_REGULAR, int face = -1);
int  fontHeight(int size, int style = FS_REGULAR, int face = -1);
// align: 0 left, 1 center, 2 right. maxW > 0 ellipsizes. Returns drawn width.
int  drawText(const std::string& s, int x, int y, int size, SDL_Color c, int align = 0, int maxW = 0, int style = FS_REGULAR, int face = -1);
int  textWidth(const std::string& s, int size, int style = FS_REGULAR, int face = -1);
// Word-wraps into lines no wider than maxW.
std::vector<std::string> wrapText(const std::string& s, int size, int maxW, int style = FS_REGULAR);
int  drawWrapped(const std::string& s, int x, int y, int size, SDL_Color c, int maxW, int maxLines = 0, int align = 0, int style = FS_REGULAR);
void gfxTrimCache();
void textGamma(SDL_Surface* s);  // smooths glyph edges of a TTF_Render*_Blended surface

void fillRect(int x, int y, int w, int h, SDL_Color c);
void fillRect(const SDL_Rect& r, SDL_Color c);
void fillRound(const SDL_Rect& r, int radius, SDL_Color c);
void strokeRound(const SDL_Rect& r, int radius, int thick, SDL_Color c);
void fillCircle(float cx, float cy, float r, SDL_Color c);
void strokeCircle(float cx, float cy, float r, float thick, SDL_Color c);
void drawLine(float x1, float y1, float x2, float y2, float thick, SDL_Color c, bool roundCaps = true);
void drawArc(float cx, float cy, float r, float a0, float a1, float thick, SDL_Color c);
void fillTri(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c);
void fillPoly(const SDL_FPoint* pts, int n, SDL_Color c);  // convex
void fillGradientV(const SDL_Rect& r, SDL_Color top, SDL_Color bottom);
void drawTex(SDL_Texture* t, const SDL_Rect& dst, Uint8 alpha = 255);
void drawTexFit(SDL_Texture* t, const SDL_Rect& box, bool cover, Uint8 alpha = 255, int radius = 0);
void pushClip(const SDL_Rect& r);
void popClip();
void spinner(float cx, float cy, float r, SDL_Color c);
SDL_Texture* textureFromSurface(SDL_Surface* s);  // frees nothing

enum Icon {
    IC_NONE = -1, IC_SYNC, IC_MEDIA, IC_MUSIC, IC_BOOK, IC_FOLDER, IC_IMAGE, IC_VIDEO, IC_PLAY, IC_PAUSE,
    IC_NEXT, IC_PREV, IC_BACK, IC_CLOSE, IC_PLUS, IC_MINUS, IC_CHECK, IC_GEAR, IC_ROTL, IC_ROTR, IC_LIST,
    IC_SUBS, IC_AUDIO, IC_FWD, IC_RWD, IC_SLIDES, IC_INFO, IC_DEVICE, IC_REFRESH, IC_QR, IC_ZOOM, IC_POWER,
    IC_TEXT, IC_FILE, IC_LINK, IC_UP, IC_DOWN, IC_SHUFFLE, IC_FIT, IC_MOON, IC_COMIC, IC_HOME, IC_LOGOUT,
    IC_WARN, IC_CLOUD, IC_PAUSECIRCLE, IC_LEFT, IC_RIGHT, IC_TRASH, IC_EDIT, IC_BOOKMARK, IC_GRID,
    IC_EQ, IC_WAVE, IC_LOCK,
};
void drawIcon(int icon, float cx, float cy, float size, SDL_Color c);

// ---------------------------------------------------------------- input
// One pointer: the touchscreen, or a controller cursor (left stick + A) for
// when the console is docked. Coordinates are in canvas space.
struct Input {
    bool down = false, pressed = false, released = false;
    int x = 0, y = 0, sx = 0, sy = 0, dx = 0, dy = 0;
    bool moved = false;        // left the tap slop
    bool tap = false, doubleTap = false, longPress = false;
    bool singleTap = false;      // a tap with no second tap following (fires ~280ms later)
    int tapX = 0, tapY = 0;      // where singleTap/doubleTap happened
    int swipeX = 0, swipeY = 0;  // on release: -1/+1
    int seekX = 0;               // west/east pressed while a player or viewer asked for them (dpadForMedia): -1/+1
    bool playPause = false;      // X
    bool toggleUi = false;       // Y: show or hide a player's or viewer's controls
    float stickX = 0, stickY = 0;  // the right stick, -1..1 (it also scrolls lists: g_in.scroll)
    float vx = 0, vy = 0;      // px/s, on release
    uint32_t heldMs = 0;
    // two fingers
    int fingers = 0;
    bool pinch = false;          // two fingers down
    float pinchScale = 1.0f;     // change this frame
    float px = 0, py = 0;        // midpoint
    float pdx = 0, pdy = 0;      // midpoint movement this frame
    bool multi = false;          // this gesture used two fingers
    // misc
    bool consumed = false;       // a widget took this frame's tap/drag
    float scroll = 0;            // right stick, px this frame (positive = content up)
    bool back = false;           // B
    bool quit = false;           // +
    bool cursor = false;         // controller cursor visible
    u64 buttons = 0, buttonsDown = 0;
};
extern Input g_in;
void inputInit();
void inputUpdate(int rotation);
// A player, viewer or book calls this every frame it's showing: from the next
// frame the d-pad's left and right go back and ahead (g_in.seekX) instead of
// moving the cursor, and ZL/ZR zoom instead of scrolling. See the button map
// in ui/input.cpp.
void dpadForMedia();
// The way the console is held (ui/orientation.cpp): 0 (landscape), 90 (turned
// left), 180 (upside down) or 270 (turned right); always 0 when it can't tell
// or Auto-rotate is off.
void orientationInit();
void orientationExit();
void orientationResume();  // back in the foreground: restart the sensor
void orientationTick();
int deviceRotation();
void drawCursor();
bool inRect(int x, int y, const SDL_Rect& r);
bool tapped(const SDL_Rect& r);     // tap landed and started in r (consumes it)
bool pressing(const SDL_Rect& r);   // finger resting on r, not dragging
bool startedIn(const SDL_Rect& r);
void consumeInput();

// Input layers. Anything drawn on top of a screen (a sheet, panel, menu or
// dialog) declares itself with an InputLayer for the length of its drawing.
// Each frame only the topmost layer of the previous frame gets the input;
// everything else, drawn before or after it, sees no input at all (no taps,
// presses, back button or scrolling). This is what stops taps going through
// a panel to the screen underneath, for every screen at once.
void layerBegin();
void layerEnd();
struct InputLayer {
    InputLayer() { layerBegin(); }
    ~InputLayer() { layerEnd(); }
    InputLayer(const InputLayer&) = delete;
    InputLayer& operator=(const InputLayer&) = delete;
};
void inputFrameEnd();     // end of every main loop frame
void inputLayersReset();  // for a modal loop with its own input (the keyboard)

// ---------------------------------------------------------------- widgets
enum { BTN_NORMAL, BTN_ACCENT, BTN_GHOST, BTN_DANGER, BTN_SOLID_LIGHT };
bool button(const SDL_Rect& r, const std::string& label, int icon = IC_NONE, int style = BTN_NORMAL, int size = 24);
bool iconButton(float cx, float cy, float r, int icon, SDL_Color fg, SDL_Color bg, float iconScale = 0.55f);
bool chip(const SDL_Rect& r, const std::string& label, bool active, int size = 22);
// Horizontal slider; returns true while the value changes. `id` keeps drag state.
bool slider(const SDL_Rect& r, float& v01, SDL_Color fill, SDL_Color track, bool* releasedOut = nullptr);
bool toggle(const SDL_Rect& r, bool& v);
// +/- stepper row: draws label, value and buttons; returns -1/0/+1.
int  stepper(const SDL_Rect& r, const std::string& label, const std::string& value);
void sectionTitle(const std::string& s, int x, int y);

struct Scroller {
    float pos = 0, vel = 0;
    bool dragging = false;
    bool thumb = false;      // the scrollbar's thumb is held: the finger sets the place directly
    float thumbGrab = 0;     // where on the thumb it was taken
    float lastPos = 0;       // pos a frame ago: how fast it's going
    // Applies drags/flings inside area; content taller than area scrolls. The
    // scrollbar's thumb (drawScrollbar) can be dragged to go through it fast.
    void update(const SDL_Rect& area, float contentH);
    void clamp(const SDL_Rect& area, float contentH);
    void scrollTo(float y, const SDL_Rect& area, float contentH);
};
// While its thumb is held, `where` (if given) shows in a bubble beside it: what's
// at this point of the list ("N", "May 2026").
void drawScrollbar(const SDL_Rect& area, float contentH, float pos, const std::string& where = "");

void toast(const std::string& s, int ms = 2500);
void drawToasts();

// Modal dialog with up to three buttons. cb receives the button index (or -1 when dismissed).
void dialog(const std::string& title, const std::string& msg, std::vector<std::string> buttons,
            std::function<void(int)> cb);
bool dialogActive();
void dialogUpdate();
void dialogDraw();

// System keyboard (swkbd). Returns false when cancelled.
bool keyboard(const std::string& header, const std::string& initial, std::string& out, int maxLen = 256,
              bool numeric = false, bool password = false);  // password: dots, not the text

// ---------------------------------------------------------------- images
// Decodes with SDL_image, applies JPEG EXIF orientation and downscales so it
// fits in maxW x maxH (0 = no limit). origW/H receive the oriented full size.
SDL_Surface* loadImage(const std::string& path, int maxW, int maxH, int* origW = nullptr, int* origH = nullptr);
SDL_Surface* loadImageMem(const void* data, size_t len, int maxW, int maxH, int* origW = nullptr, int* origH = nullptr);
SDL_Surface* scaleSurface(SDL_Surface* src, int w, int h);  // RGBA32 box/bilinear
int jpegExifOrientation(const uint8_t* d, size_t n);

// ---------------------------------------------------------------- pictures
// Thumbnails, posters and covers the PC made, read out of system.db by
// picture id (see core/library.h). A texture once loaded, nullptr until then:
// screens ask every frame for what they show, and only that gets loaded.
SDL_Texture* pictureGet(int64_t id);
bool pictureFailed(int64_t id);  // no such picture, or it couldn't be read
// A book's cover by its file path (the readers' front pages).
SDL_Texture* bookCover(const std::string& path);
void picturesInit();
void picturesTick();      // uploads finished pictures (main thread)
void picturesHoldOff();   // a list is flying past: for a moment, pictureGet starts nothing new
void picturesReset();     // the library was reopened: picture ids mean other pictures now
void picturesShutdown();

// ---------------------------------------------------------------- tabs
struct Tab {
    virtual ~Tab() {}
    virtual const char* name() = 0;
    virtual int icon() = 0;
    virtual SDL_Color accent() = 0;
    virtual void enter() {}
    virtual void leave() {}
    virtual void update(const SDL_Rect& area) = 0;
    virtual void draw(const SDL_Rect& area) = 0;
    virtual void background() {}           // every frame, active or not
    virtual void preRender() {}            // before canvas drawing (raw GL work)
    virtual bool fullscreen() { return false; }
    virtual bool keepAwake() { return false; }
    virtual int  rotation() { return 0; }  // canvas rotation (0/90/270)
    virtual int  brightness() { return 100; }
    virtual std::string badge() { return ""; }  // small status text on the rail
    virtual void shutdown() {}
};
Tab* makeFilesTab();
Tab* makeMediaTab();
Tab* makeSpotifyTab();
Tab* makeBooksTab();

// Lets one tab pause another's playback (e.g. a video pauses Spotify).
void spotifyPauseForVideo();
extern bool g_quitRequested;
