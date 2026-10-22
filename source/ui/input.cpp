// Touch gestures plus a controller cursor (left stick + A) that produces the
// same events, so every screen works docked too.
#include "app.h"
#include <algorithm>
#include <cmath>

Input g_in;

static PadState s_pad;
static uint32_t s_downAt = 0;
static int s_lastX = 0, s_lastY = 0;
static bool s_longFired = false;
static uint32_t s_lastTapAt = 0;
static int s_lastTapX = -999, s_lastTapY = -999;
static bool s_tapPending = false;
static bool s_tapUsed = false;  // a widget took the pending tap: no singleTap for it
static void layersFrameStart();  // input layers, below
static float s_pinchDist = 0;
static float s_curX = 640, s_curY = 360;  // controller cursor, screen space
static uint32_t s_cursorUsedAt = 0;
static bool s_cursorDown = false;
struct Sample { uint32_t t; int x, y; };
static Sample s_hist[8];
static int s_histN = 0;

void inputInit() {
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    padInitializeAny(&s_pad);
    hidInitializeTouchScreen();
}

// Screen (1280x720) -> canvas coordinates for the current rotation.
static void toCanvas(float sx, float sy, int rotation, float& cx, float& cy) {
    float x = sx - 640, y = sy - 360, ox, oy;
    if (rotation == 90) { ox = y; oy = -x; }
    else if (rotation == 270) { ox = -y; oy = x; }
    else if (rotation == 180) { ox = -x; oy = -y; }
    else { ox = x; oy = y; }
    cx = ox + g_W / 2.0f;
    cy = oy + g_H / 2.0f;
}

bool inRect(int x, int y, const SDL_Rect& r) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

bool tapped(const SDL_Rect& r) {
    if (!g_in.tap || g_in.consumed) return false;
    if (!inRect(g_in.sx, g_in.sy, r) || !inRect(g_in.x, g_in.y, r)) return false;
    g_in.consumed = true;
    s_tapUsed = true;
    return true;
}

// (not when something above already has the touch: it lights up, not what's under it)
bool pressing(const SDL_Rect& r) {
    return g_in.down && !g_in.moved && !g_in.multi && !g_in.consumed && inRect(g_in.sx, g_in.sy, r) && inRect(g_in.x, g_in.y, r);
}
bool startedIn(const SDL_Rect& r) { return (g_in.down || g_in.released) && inRect(g_in.sx, g_in.sy, r); }
void consumeInput() {
    g_in.consumed = true;
    if (g_in.tap) s_tapUsed = true;
}

static bool s_media = false, s_mediaNext = false;
void dpadForMedia() { s_mediaNext = true; }

// The buttons, the same everywhere (the usual Switch habits where there are any):
//   left stick, d-pad  the cursor
//   A                  tap (hold to drag)          B   back
//   right stick        scroll; in the viewer, move around a zoomed-in photo
//   L / R              the item before / after (photo, episode, page, track)
//   d-pad left / right in a player, viewer or book: back / ahead within it
//                      (10 s, or a photo or page); elsewhere they move the cursor
//   ZL / ZR            in a player, viewer or book: zoom out / in at the cursor;
//                      on lists, scroll up / down
//   X                  play / pause                Y   show or hide the controls
//   +                  quit
void inputUpdate(int rotation) {
    s_media = s_mediaNext;
    s_mediaNext = false;
    padUpdate(&s_pad);
    u64 held = padGetButtons(&s_pad), downB = padGetButtonsDown(&s_pad);
    g_in.buttons = held;
    g_in.buttonsDown = downB;
    g_in.back = downB & HidNpadButton_B;
    g_in.quit = downB & HidNpadButton_Plus;
    g_in.playPause = downB & HidNpadButton_X;
    g_in.toggleUi = downB & HidNpadButton_Y;

    Input& in = g_in;
    in.pressed = in.released = in.tap = in.doubleTap = in.longPress = in.singleTap = false;
    in.swipeX = in.swipeY = in.seekX = 0;
    in.dx = in.dy = 0;
    in.pinchScale = 1.0f;
    in.pdx = in.pdy = 0;
    in.consumed = false;
    in.scroll = 0;
    uint32_t now = SDL_GetTicks();

    // ---- controller cursor
    HidAnalogStickState ls = padGetStickPos(&s_pad, 0), rs = padGetStickPos(&s_pad, 1);
    float lx = ls.x / 32767.0f, ly = -ls.y / 32767.0f;
    if (lx * lx + ly * ly < 0.03f) lx = ly = 0;
    if (s_media) {
        if (downB & HidNpadButton_Left) in.seekX = -1;
        if (downB & HidNpadButton_Right) in.seekX = 1;
    } else {
        if (held & HidNpadButton_Left) lx = -0.6f;
        if (held & HidNpadButton_Right) lx = 0.6f;
    }
    if (held & HidNpadButton_Up) ly = -0.6f;
    if (held & HidNpadButton_Down) ly = 0.6f;
    // A while the cursor is hidden only brings it up: a tap where you can't see
    // the cursor could flip a setting nobody meant to (Auto-rotate was turned
    // off that way). The next A taps.
    static bool s_aSwallowed = false;
    bool cursorWasUp = appletGetOperationMode() == AppletOperationMode_Console || (s_cursorUsedAt && now - s_cursorUsedAt < 6000);
    if ((downB & HidNpadButton_A) && !cursorWasUp) s_aSwallowed = true;
    if (!(held & HidNpadButton_A)) s_aSwallowed = false;
    if (lx || ly || (downB & HidNpadButton_A)) s_cursorUsedAt = now ? now : 1;
    float speed = 900 * g_dt;
    float mag = sqrtf(lx * lx + ly * ly);
    s_curX = SDL_clamp(s_curX + lx * speed * (0.4f + mag), 0.0f, 1279.0f);
    s_curY = SDL_clamp(s_curY + ly * speed * (0.4f + mag), 0.0f, 719.0f);
    float rx = rs.x / 32767.0f, ry = -rs.y / 32767.0f;
    in.stickX = fabsf(rx) > 0.2f ? rx : 0;
    in.stickY = fabsf(ry) > 0.2f ? ry : 0;
    if (fabsf(ry) > 0.2f) {
        float rx2, ry2, ox, oy;
        toCanvas(640, 360 + ry * 1400 * g_dt, rotation, rx2, ry2);
        toCanvas(640, 360, rotation, ox, oy);
        in.scroll = (ry2 - oy) + (rx2 - ox);  // scroll along the canvas' vertical axis
    }
    // Docked there's no touchscreen: keep the cursor up, and say how it works once per docking.
    static bool s_docked = false;
    bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    if (docked && !s_docked) toast("Docked: the left stick moves the cursor, A taps (hold to drag), B goes back, the right stick scrolls", 5000);
    s_docked = docked;
    in.cursor = docked || (s_cursorUsedAt && now - s_cursorUsedAt < 6000);
    // L / R: the item before / after
    if (downB & HidNpadButton_R) in.swipeX = -1;
    if (downB & HidNpadButton_L) in.swipeX = 1;
    if (!s_media) {
        // on lists the triggers scroll, picking up speed the longer they're held
        static float s_scrollRamp = 0;
        int dir = ((held & HidNpadButton_ZR) ? 1 : 0) - ((held & HidNpadButton_ZL) ? 1 : 0);
        s_scrollRamp = dir ? std::min(1.0f, s_scrollRamp + g_dt / 0.8f) : 0;
        if (dir) in.scroll += dir * (700 + 2300 * s_scrollRamp * s_scrollRamp) * g_dt;
    }

    // ---- touch
    HidTouchScreenState ts = {};
    int count = 0;
    float fx[2] = {0, 0}, fy[2] = {0, 0};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        count = ts.count;
        for (int i = 0; i < 2 && i < ts.count; i++) toCanvas(ts.touches[i].x, ts.touches[i].y, rotation, fx[i], fy[i]);
        s_cursorUsedAt = 0;
        in.cursor = false;
    } else if ((held & HidNpadButton_A) && !s_aSwallowed) {
        count = 1;
        toCanvas(s_curX, s_curY, rotation, fx[0], fy[0]);
        s_cursorDown = true;
    } else {
        s_cursorDown = false;
    }

    // ZL/ZR in a player, viewer or book: zoom (a pinch) around the cursor, or the middle
    if (s_media && !ts.count && (held & (HidNpadButton_ZL | HidNpadButton_ZR))) {
        float cx, cy;
        toCanvas(in.cursor ? s_curX : 640, in.cursor ? s_curY : 360, rotation, cx, cy);
        in.pinchScale = (held & HidNpadButton_ZR) ? 1.025f : 1 / 1.025f;
        in.px = cx;
        in.py = cy;
    }

    bool down = count > 0;
    in.fingers = count;
    if (down) {
        int x = (int)fx[0], y = (int)fy[0];
        if (!in.down) {
            in.pressed = true;
            in.sx = in.x = s_lastX = x;
            in.sy = in.y = s_lastY = y;
            s_downAt = now;
            in.moved = false;
            in.multi = false;
            s_longFired = false;
            s_histN = 0;
        }
        if (count >= 2) {
            float d = sqrtf((fx[1] - fx[0]) * (fx[1] - fx[0]) + (fy[1] - fy[0]) * (fy[1] - fy[0]));
            float mx = (fx[0] + fx[1]) / 2, my = (fy[0] + fy[1]) / 2;
            if (in.pinch && s_pinchDist > 10) {
                in.pinchScale = d / s_pinchDist;
                in.pdx = mx - in.px;
                in.pdy = my - in.py;
            }
            in.pinch = true;
            in.multi = true;
            in.moved = true;
            s_pinchDist = d;
            in.px = mx;
            in.py = my;
            // keep the primary finger from jumping when a finger lifts
            s_lastX = x;
            s_lastY = y;
        } else {
            in.pinch = false;
            in.dx = x - s_lastX;
            in.dy = y - s_lastY;
            s_lastX = x;
            s_lastY = y;
        }
        in.x = x;
        in.y = y;
        int mx = in.x - in.sx, my = in.y - in.sy;
        if (mx * mx + my * my > 22 * 22) in.moved = true;
        in.heldMs = now - s_downAt;
        if (!in.moved && !in.multi && !s_longFired && in.heldMs > 550) {
            in.longPress = true;
            s_longFired = true;
        }
        s_hist[s_histN % 8] = {now, x, y};
        s_histN++;
    } else if (in.down) {
        in.released = true;
        in.pinch = false;
        uint32_t dur = now - s_downAt;
        // release velocity from samples in the last ~90ms
        in.vx = in.vy = 0;
        int n = std::min(s_histN, 8);
        for (int k = 1; k < n; k++) {
            const Sample& a = s_hist[(s_histN - 1 - k) % 8];
            const Sample& b = s_hist[(s_histN - 1) % 8];
            if (now - a.t > 90 || k == n - 1) {
                float dtS = std::max(1u, b.t - a.t) / 1000.0f;
                if (now - b.t < 60) { in.vx = (b.x - a.x) / dtS; in.vy = (b.y - a.y) / dtS; }
                break;
            }
        }
        int mx = in.x - in.sx, my = in.y - in.sy;
        if (!in.moved && !in.multi && dur < 500 && !s_longFired) {
            in.tap = true;
            int ddx = in.x - s_lastTapX, ddy = in.y - s_lastTapY;
            if (now - s_lastTapAt < 320 && ddx * ddx + ddy * ddy < 60 * 60) {
                in.doubleTap = true;
                in.tapX = in.x;
                in.tapY = in.y;
                s_tapPending = false;
                s_lastTapAt = 0;
            } else {
                s_lastTapAt = now;
                s_lastTapX = in.x;
                s_lastTapY = in.y;
                s_tapPending = true;
                s_tapUsed = false;
            }
        } else if (!in.multi) {
            bool fast = fabsf(in.vx) > 500 || (abs(mx) > 160 && dur < 700);
            if (abs(mx) > 70 && abs(mx) > 1.5f * abs(my) && fast) in.swipeX = mx < 0 ? -1 : 1;
            bool fastY = fabsf(in.vy) > 500 || (abs(my) > 160 && dur < 700);
            if (abs(my) > 70 && abs(my) > 1.5f * abs(mx) && fastY) in.swipeY = my < 0 ? -1 : 1;
        }
    }
    in.down = down;
    if (s_tapPending && now - s_lastTapAt > 280) {
        s_tapPending = false;
        in.singleTap = !s_tapUsed;
        in.tapX = s_lastTapX;
        in.tapY = s_lastTapY;
    }
    layersFrameStart();
}

// ---------------------------------------------------------------- layers

static Input s_real;          // the real input while a layer below the owner draws
static bool s_hidden = false;
static int s_layerCount = 0;  // layers begun this frame
static int s_owner = 0;       // the layer (1-based order) that gets the input; 0 = the screen itself
static std::vector<int> s_layers;

static void hideInput() {
    if (s_hidden) return;
    s_real = g_in;
    Input none;
    none.x = g_in.x;
    none.y = g_in.y;
    none.sx = g_in.sx;
    none.sy = g_in.sy;
    none.cursor = g_in.cursor;
    none.consumed = true;
    g_in = none;
    s_hidden = true;
}

static void showInput() {
    if (!s_hidden) return;
    g_in = s_real;
    s_hidden = false;
}

static void layersFrameStart() {
    s_layerCount = 0;
    s_layers.clear();
    if (s_owner > 0) hideInput();  // the screen under the owner gets nothing
}

void layerBegin() {
    int idx = ++s_layerCount;
    s_layers.push_back(idx);
    if (idx == s_owner) showInput();
}

void layerEnd() {
    if (s_layers.empty()) return;
    int idx = s_layers.back();
    s_layers.pop_back();
    if (idx == s_owner && !s_hidden) {
        // a tap on the layer must not come back later as a singleTap underneath
        if (g_in.tap) s_tapUsed = true;
        hideInput();  // keeps the layer's changes in s_real; whatever is drawn after sees nothing
    }
}

void inputFrameEnd() {
    showInput();  // inputUpdate builds on the real state
    s_owner = s_layerCount;
}

void inputLayersReset() {
    showInput();
    s_owner = 0;
    s_layerCount = 0;
    s_layers.clear();
}

void drawCursor() {
    if (!g_in.cursor) return;
    // drawn in screen space after the canvas is composited
    float x = s_curX, y = s_curY;
    fillCircle(x, y, 13, SDL_Color{0, 0, 0, 140});
    strokeCircle(x, y, 10, 3, s_cursorDown ? g_theme.accent : SDL_Color{255, 255, 255, 230});
    fillCircle(x, y, 3, SDL_Color{255, 255, 255, 255});
}
