// Immediate-mode widgets: buttons, sliders, toggles, scrolling, toasts,
// modal dialogs and the system keyboard.
#include "app.h"
#include <cmath>
#include <cstring>
#include <deque>

// ---------------------------------------------------------------- buttons

bool button(const SDL_Rect& r, const std::string& label, int icon, int style, int size) {
    bool down = pressing(r);
    SDL_Color bg, fg;
    switch (style) {
        case BTN_ACCENT: bg = g_theme.accent; fg = SDL_Color{10, 10, 12, 255}; break;
        case BTN_DANGER: bg = withAlpha(g_theme.danger, 60); fg = g_theme.danger; break;
        case BTN_GHOST: bg = SDL_Color{0, 0, 0, 0}; fg = g_theme.fg; break;
        case BTN_SOLID_LIGHT: bg = g_theme.fg; fg = SDL_Color{10, 10, 12, 255}; break;
        default: bg = g_theme.panel2; fg = g_theme.fg; break;
    }
    if (down) bg = style == BTN_GHOST ? withAlpha(g_theme.fg, 30) : mix(bg, SDL_Color{255, 255, 255, bg.a}, 0.18f);
    fillRound(r, std::min(r.h / 2, 16), bg);
    int iconSize = std::min(r.h - 16, size + 6);
    int tw = label.empty() ? 0 : textWidth(label, size, FS_BOLD);
    int gap = (icon != IC_NONE && !label.empty()) ? 10 : 0;
    int total = (icon != IC_NONE ? iconSize : 0) + gap + tw;
    int x = r.x + (r.w - total) / 2;
    if (total > r.w - 24) x = r.x + 12;
    if (icon != IC_NONE) {
        drawIcon(icon, x + iconSize / 2.0f, r.y + r.h / 2.0f, (float)iconSize, fg);
        x += iconSize + gap;
    }
    if (!label.empty()) drawText(label, x, r.y + (r.h - fontHeight(size, FS_BOLD)) / 2, size, fg, 0, r.x + r.w - x - 10, FS_BOLD);
    return tapped(r);
}

bool iconButton(float cx, float cy, float rad, int icon, SDL_Color fg, SDL_Color bg, float iconScale) {
    SDL_Rect r = {(int)(cx - rad), (int)(cy - rad), (int)(2 * rad), (int)(2 * rad)};
    bool down = pressing(r);
    if (bg.a) fillCircle(cx, cy, rad, down ? mix(bg, SDL_Color{255, 255, 255, bg.a}, 0.2f) : bg);
    else if (down) fillCircle(cx, cy, rad, withAlpha(g_theme.fg, 40));
    drawIcon(icon, cx, cy, rad * 2 * iconScale, fg);
    return tapped(r);
}

bool chip(const SDL_Rect& r, const std::string& label, bool active, int size) {
    bool down = pressing(r);
    SDL_Color bg = active ? g_theme.accent : g_theme.panel2;
    if (down) bg = mix(bg, SDL_Color{255, 255, 255, 255}, 0.15f);
    fillRound(r, r.h / 2, bg);
    drawText(label, r.x + r.w / 2, r.y + (r.h - fontHeight(size)) / 2, size,
             active ? SDL_Color{10, 10, 12, 255} : g_theme.fg, 1, r.w - 16);
    return tapped(r);
}

static int s_sliderKey = -1;

bool slider(const SDL_Rect& r, float& v, SDL_Color fill, SDL_Color track, bool* releasedOut) {
    int key = r.x * 4099 + r.y;
    SDL_Rect hit = {r.x - 12, r.y + r.h / 2 - 26, r.w + 24, 52};
    bool changed = false;
    if (releasedOut) *releasedOut = false;
    if (g_in.pressed && !g_in.consumed && inRect(g_in.x, g_in.y, hit)) s_sliderKey = key;
    bool active = s_sliderKey == key;
    if (active) {
        if (g_in.down) {
            float nv = SDL_clamp((g_in.x - r.x) / (float)r.w, 0.0f, 1.0f);
            if (nv != v) { v = nv; changed = true; }
            g_in.consumed = true;
        } else {
            s_sliderKey = -1;
            if (g_in.released) {
                if (releasedOut) *releasedOut = true;
                g_in.consumed = true;
                changed = true;
            }
        }
    }
    int th = std::max(4, r.h);
    int cy = r.y + r.h / 2;
    fillRound(SDL_Rect{r.x, cy - th / 2, r.w, th}, th / 2, track);
    int fw = (int)(r.w * SDL_clamp(v, 0.0f, 1.0f));
    if (fw > 0) fillRound(SDL_Rect{r.x, cy - th / 2, std::max(fw, th), th}, th / 2, fill);
    float knob = active ? th * 1.6f + 6 : th + 6;
    fillCircle(r.x + fw, (float)cy, knob, g_theme.fg);
    return changed;
}

bool toggle(const SDL_Rect& r, bool& v) {
    SDL_Rect sw = {r.x + r.w - 72, r.y + (r.h - 38) / 2, 72, 38};
    fillRound(sw, 19, v ? g_theme.accent : g_theme.faint);
    fillCircle(v ? sw.x + sw.w - 19.0f : sw.x + 19.0f, sw.y + 19.0f, 14, g_theme.fg);
    if (tapped(r)) { v = !v; return true; }
    return false;
}

int stepper(const SDL_Rect& r, const std::string& label, const std::string& value) {
    drawText(label, r.x, r.y + (r.h - fontHeight(24)) / 2, 24, g_theme.fg, 0, r.w - 260);
    int bw = 64, by = r.y + (r.h - 52) / 2;
    SDL_Rect minus = {r.x + r.w - 2 * bw - 120, by, bw, 52}, plus = {r.x + r.w - bw, by, bw, 52};
    drawText(value, r.x + r.w - bw - 60, r.y + (r.h - fontHeight(24)) / 2, 24, g_theme.accent, 1, 112);
    int d = 0;
    if (button(minus, "", IC_MINUS)) d = -1;
    if (button(plus, "", IC_PLUS)) d = 1;
    return d;
}

void sectionTitle(const std::string& s, int x, int y) { drawText(s, x, y, 20, g_theme.dim, 0, 0, FS_BOLD); }

// ---------------------------------------------------------------- scrolling

void Scroller::clamp(const SDL_Rect& area, float contentH) {
    float maxPos = std::max(0.0f, contentH - area.h);
    if (pos < 0) { pos = 0; vel = 0; }
    if (pos > maxPos) { pos = maxPos; vel = 0; }
}

static SDL_Rect s_thumbArea;  // the area whose thumb is held (drawn wider)

// The scrollbar's thumb for content scrolled to pos.
static SDL_Rect thumbRect(const SDL_Rect& area, float contentH, float pos) {
    int h = std::max(40, (int)(area.h * (area.h / contentH)));
    int y = area.y + (int)((area.h - h) * SDL_clamp(pos / (contentH - area.h), 0.0f, 1.0f));
    return SDL_Rect{area.x + area.w - 7, y, 4, h};
}

void Scroller::update(const SDL_Rect& area, float contentH) {
    struct Speed {  // checked after this update, however it returns
        Scroller& s;
        ~Speed() {
            // flying past (a dragged thumb, a hard fling): what's on screen now is
            // gone next frame, so no pictures start loading until it slows down
            if (fabsf(s.pos - s.lastPos) / g_dt > 2500) picturesHoldOff();
            s.lastPos = s.pos;
        }
    } speed{*this};
    // the thumb: a press on it (generously: it's thin) or beside it at the edge
    if (g_in.pressed && !g_in.consumed && contentH > area.h + 1) {
        SDL_Rect t = thumbRect(area, contentH, pos);
        SDL_Rect hit = {area.x + area.w - 44, t.y - 30, 44, t.h + 60};
        if (inRect(g_in.x, g_in.y, hit)) {
            thumb = true;
            thumbGrab = (float)(g_in.y - t.y);
            vel = 0;
        }
    }
    if (thumb) {
        if (g_in.down) {
            // it follows the finger (the bubble beside it says where that is)
            SDL_Rect t = thumbRect(area, contentH, pos);
            float span = (float)(area.h - t.h);
            if (span > 0) pos = (g_in.y - thumbGrab - area.y) / span * (contentH - area.h);
            s_thumbArea = area;
        } else {
            thumb = false;
            s_thumbArea = SDL_Rect{};
        }
        consumeInput();  // not a drag or a tap on what's under it
        dragging = false;
        clamp(area, contentH);
        return;
    }
    if (g_in.pressed && inRect(g_in.x, g_in.y, area)) { vel = 0; dragging = true; }
    if (dragging) {
        if (g_in.down) {
            if (g_in.moved && !g_in.multi) pos -= g_in.dy;
        } else {
            dragging = false;
            if (g_in.moved) vel = -g_in.vy;
        }
    } else if (vel != 0) {
        pos += vel * g_dt;
        vel *= powf(0.05f, g_dt);
        if (fabsf(vel) < 20) vel = 0;
    }
    if (g_in.scroll != 0 && (g_in.cursor || true)) pos += g_in.scroll;
    clamp(area, contentH);
}

void Scroller::scrollTo(float y, const SDL_Rect& area, float contentH) {
    pos = y;
    vel = 0;
    clamp(area, contentH);
}

void drawScrollbar(const SDL_Rect& area, float contentH, float pos, const std::string& where) {
    if (contentH <= area.h + 1) return;
    SDL_Rect t = thumbRect(area, contentH, pos);
    bool held = s_thumbArea.w && s_thumbArea.x == area.x && s_thumbArea.y == area.y && s_thumbArea.w == area.w &&
                s_thumbArea.h == area.h;
    if (held) fillRound(SDL_Rect{t.x - 8, t.y, 12, t.h}, 6, g_theme.accent);  // wider while it's held
    else fillRound(t, 2, withAlpha(g_theme.fg, 70));
    if (held && !where.empty()) {
        // the bubble, pointing at the thumb
        int w = std::max(64, textWidth(where, 26, FS_BOLD) + 36), h = 56;
        float cy = t.y + t.h / 2.0f;
        SDL_Rect b = {t.x - 30 - w, (int)SDL_clamp(cy - h / 2.0f, (float)area.y, (float)(area.y + area.h - h)), w, h};
        fillRound(b, 28, g_theme.accent);
        float tipY = SDL_clamp(cy, b.y + 16.0f, b.y + h - 16.0f);
        fillTri(b.x + w - 2.0f, tipY - 12, b.x + w - 2.0f, tipY + 12, b.x + w + 14.0f, tipY, g_theme.accent);
        drawText(where, b.x + w / 2, b.y + (h - fontHeight(26, FS_BOLD)) / 2, 26, SDL_Color{10, 10, 12, 255}, 1, w - 16, FS_BOLD);
    }
}

// ---------------------------------------------------------------- toasts

struct Toast { std::string text; int ms; uint32_t until; bool started; };
static std::deque<Toast> s_toasts;

void toast(const std::string& s, int ms) {
    if (!s_toasts.empty() && s_toasts.back().text == s) {
        Toast& t = s_toasts.back();
        if (t.started) t.until = SDL_GetTicks() + ms;
        return;
    }
    if (s_toasts.size() > 3) s_toasts.pop_front();
    s_toasts.push_back({s, ms, 0, false});
}

void drawToasts() {
    if (s_toasts.empty()) return;
    Toast& t = s_toasts.front();
    if (!t.started) { t.started = true; t.until = g_ticks + t.ms; }
    if (g_ticks > t.until) { s_toasts.pop_front(); return; }
    int w = std::min(g_W - 80, textWidth(t.text, 24) + 56);
    int y = g_H - 120;
    float fade = SDL_clamp((t.until - g_ticks) / 250.0f, 0.0f, 1.0f);
    fillRound(SDL_Rect{g_W / 2 - w / 2, y, w, 56}, 28, withAlpha(SDL_Color{48, 48, 54, 255}, (Uint8)(240 * fade)));
    drawText(t.text, g_W / 2, y + (56 - fontHeight(24)) / 2, 24, withAlpha(g_theme.fg, (Uint8)(255 * fade)), 1, w - 40);
}

// ---------------------------------------------------------------- dialog

struct Dialog {
    bool active = false;
    std::string title, msg;
    std::vector<std::string> buttons;
    std::function<void(int)> cb;
};
static Dialog s_dlg;

void dialog(const std::string& title, const std::string& msg, std::vector<std::string> buttons, std::function<void(int)> cb) {
    s_dlg.active = true;
    s_dlg.title = title;
    s_dlg.msg = msg;
    s_dlg.buttons = std::move(buttons);
    s_dlg.cb = std::move(cb);
}

bool dialogActive() { return s_dlg.active; }

static SDL_Rect dlgRect(int& msgH) {
    int w = std::min(g_W - 60, 760);
    auto lines = wrapText(s_dlg.msg, 24, w - 72);
    msgH = (int)lines.size() * fontHeight(24) * 115 / 100;
    int h = 64 + 50 + msgH + 40 + 64 + 36;
    if (s_dlg.msg.empty()) h -= 30;
    return SDL_Rect{(g_W - w) / 2, (g_H - h) / 2, w, h};
}

// The dialog's draw also performs hit-testing (immediate mode); update only
// swallows everything else so screens underneath don't react.
void dialogUpdate() {}  // input is handled in dialogDraw, inside the dialog's layer

void dialogDraw() {
    if (!s_dlg.active) return;
    InputLayer layer;
    if (g_in.back) {
        auto cb = s_dlg.cb;
        s_dlg.active = false;
        g_in.consumed = true;
        if (cb) cb(-1);
        return;
    }
    fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 170});
    int msgH;
    SDL_Rect r = dlgRect(msgH);
    fillRound(r, 24, g_theme.panel);
    strokeRound(r, 24, 2, g_theme.line);
    drawText(s_dlg.title, r.x + 36, r.y + 32, 30, g_theme.fg, 0, r.w - 72, FS_BOLD);
    drawWrapped(s_dlg.msg, r.x + 36, r.y + 86, 24, g_theme.dim, r.w - 72);
    int n = (int)s_dlg.buttons.size();
    int bw = (r.w - 72 - (n - 1) * 16) / std::max(1, n);
    int result = -2;
    for (int i = 0; i < n; i++) {
        SDL_Rect b = {r.x + 36 + i * (bw + 16), r.y + r.h - 36 - 64, bw, 64};
        if (button(b, s_dlg.buttons[i], IC_NONE, i == n - 1 ? BTN_ACCENT : BTN_NORMAL)) result = i;
    }
    if (result == -2 && g_in.tap && !g_in.consumed && !inRect(g_in.sx, g_in.sy, r)) result = -1;
    g_in.consumed = true;
    if (result != -2) {
        auto cb = s_dlg.cb;
        s_dlg.active = false;
        if (cb) cb(result);
    }
}

// ---------------------------------------------------------------- keyboard

// Our own on-screen keyboard. The system keyboard (swkbd, a library applet)
// froze the whole console when opened from this app, so text is typed here
// instead: a modal loop that reads input and draws the keyboard full-screen
// until Done or Cancel. Touch, or the cursor when docked; B deletes (or
// cancels when empty), + is Done.
bool keyboard(const std::string& header, const std::string& initial, std::string& out, int maxLen, bool numeric, bool password) {
    static const char* LETTERS[4] = {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,."};
    static const char* SYMBOLS[4] = {"!@#$%^&*()", "~`=+[]{}\\|", "<>;:\"?_-+/", "'.,!?@#&*"};
    std::string text = initial;
    if ((int)text.size() > maxLen) text.resize(maxLen);
    bool shift = false, caps = false, sym = false, done = false, ok = false, armed = false;
    uint32_t lastShift = 0, bsNext = 0;
    SDL_Texture* prevTarget = SDL_GetRenderTarget(g_ren);
    int pw = g_W, ph = g_H;
    uint32_t last = SDL_GetTicks();
    appLog("keyboard: \"%s\"", header.c_str());

    while (!done && appletMainLoop()) {
        uint32_t now = SDL_GetTicks();
        g_dt = SDL_clamp((now - last) / 1000.0f, 0.001f, 0.1f);
        last = now;
        g_ticks = now;
        if (appletGetFocusState() != AppletFocusState_InFocus) {
            svcSleepThread(50'000'000ULL);
            continue;
        }
        g_W = 1280;
        g_H = 720;
        inputUpdate(0);
        inputLayersReset();  // this loop owns the input; no screen layers apply
        // the tap that opened us must end before keys react
        if (!armed) {
            if (!g_in.down) armed = true;
            g_in.consumed = true;
        }
        SDL_SetRenderTarget(g_ren, nullptr);
        SDL_SetRenderDrawColor(g_ren, g_theme.bg.r, g_theme.bg.g, g_theme.bg.b, 255);
        SDL_RenderClear(g_ren);

        // prompt and text field (showing the end when it's long)
        drawText(header, 60, 34, 24, g_theme.dim, 0, 1160);
        SDL_Rect field = {60, 76, 1160, 78};
        fillRound(field, 16, g_theme.panel2);
        strokeRound(field, 16, 2, g_theme.accent);
        std::string shown = text;
        if (password) {  // a dot per character (counted by UTF-8 lead bytes)
            shown.clear();
            for (unsigned char ch : text)
                if ((ch & 0xC0) != 0x80) shown += "\xE2\x80\xA2";
        }
        while (!shown.empty() && textWidth(shown, 30) > field.w - 56) shown.erase(0, 1);
        int tw = drawText(shown, field.x + 24, field.y + (field.h - fontHeight(30)) / 2, 30, g_theme.fg);
        if ((g_ticks / 500) % 2) fillRect(field.x + 26 + tw, field.y + 18, 3, field.h - 36, g_theme.accent);
        drawText(std::to_string(text.size()) + " / " + std::to_string(maxLen), field.x + field.w, field.y + field.h + 8, 16,
                 g_theme.faint, 2);

        auto type = [&](char c) {
            if ((int)text.size() >= maxLen) return;
            if (shift || caps) c = (char)toupper((unsigned char)c);
            text += c;
            if (shift && !caps) shift = false;
        };
        // the widest row is 9 keys plus Shift and Backspace: 9*96 + 8*8 + 2*(140+8) = 1224, inside 1280
        const int kw = 96, kh = 80, gap = 8, top = 196, wide = 140;
        if (numeric) {
            static const char* PAD[4] = {"123", "456", "789", "-0."};
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 3; c++) {
                    SDL_Rect k = {640 - (3 * 150 + 2 * gap) / 2 + c * (150 + gap), top + r * (kh + gap), 150, kh};
                    if (button(k, std::string(1, PAD[r][c]), IC_NONE, BTN_NORMAL, 30)) type(PAD[r][c]);
                }
        } else {
            const char* const* rows = sym ? SYMBOLS : LETTERS;
            for (int r = 0; r < 4; r++) {
                int n = (int)strlen(rows[r]);
                int rowW = n * kw + (n - 1) * gap + (r == 3 ? 2 * (wide + gap) : 0);
                int x = 640 - rowW / 2;
                if (r == 3) {  // shift / letters
                    SDL_Rect k = {x, top + r * (kh + gap), wide, kh};
                    if (sym) {
                        if (button(k, "ABC", IC_NONE, BTN_NORMAL, 24)) sym = false;
                    } else if (button(k, caps ? "CAPS" : "", caps ? IC_NONE : IC_UP, shift || caps ? BTN_ACCENT : BTN_NORMAL, 22)) {
                        // tap: shift for one letter; double tap: caps lock
                        if (g_ticks - lastShift < 350) { caps = !caps; shift = false; }
                        else if (caps) caps = false;
                        else shift = !shift;
                        lastShift = g_ticks;
                    }
                    x += wide + gap;
                }
                for (int i = 0; i < n; i++) {
                    char c = rows[r][i];
                    std::string label(1, (shift || caps) && !sym ? (char)toupper((unsigned char)c) : c);
                    if (button({x, top + r * (kh + gap), kw, kh}, label, IC_NONE, BTN_NORMAL, 30)) type(c);
                    x += kw + gap;
                }
                if (r == 3) {  // backspace, held to repeat
                    SDL_Rect k = {x, top + r * (kh + gap), wide, kh};
                    bool hit = button(k, "", IC_BACK);
                    bool held = g_in.down && armed && inRect(g_in.sx, g_in.sy, k) && inRect(g_in.x, g_in.y, k);
                    if (held && !bsNext) bsNext = g_ticks + 450;
                    if (!held) bsNext = 0;
                    if (!text.empty() && (hit || (held && g_ticks >= bsNext && bsNext))) {
                        text.pop_back();
                        if (held && !hit) bsNext = g_ticks + 60;
                    }
                }
            }
        }
        // bottom row
        int by = top + 4 * (kh + gap) + 6;
        if (!numeric && button({140, by, 160, kh}, sym ? "ABC" : "#+=", IC_NONE, BTN_NORMAL, 24)) sym = !sym;
        if (!numeric && button({140 + 168, by, 456, kh}, "space", IC_NONE, BTN_NORMAL, 22)) type(' ');
        if (numeric && button({640 - 75, by, 150, kh}, "", IC_BACK) && !text.empty()) text.pop_back();
        if (button({1280 - 140 - 368, by, 180, kh}, "Cancel", IC_CLOSE, BTN_NORMAL, 22)) done = true;
        if (button({1280 - 140 - 180, by, 180, kh}, "Done", IC_CHECK, BTN_ACCENT, 22)) { done = true; ok = true; }
        if (armed && g_in.back) {
            if (text.empty()) done = true;
            else text.pop_back();
        }
        if (armed && g_in.quit) { done = true; ok = true; }
        drawCursor();
        SDL_RenderPresent(g_ren);
    }

    // back to the frame we were called from; its touch is used up
    SDL_SetRenderTarget(g_ren, prevTarget);
    g_W = pw;
    g_H = ph;
    g_in.consumed = true;
    g_in.tap = g_in.singleTap = g_in.doubleTap = g_in.back = g_in.quit = false;
    if (ok) out = text;
    return ok;
}
