// Fonts, cached text, and anti-aliased primitives (circles, rounded rects,
// lines, arcs and convex polygons) built on SDL_RenderGeometry.
#include "app.h"
#include <cmath>
#include <map>
#include <unordered_map>

SDL_Renderer* g_ren = nullptr;
int g_W = 1280, g_H = 720;
uint32_t g_ticks = 0;
float g_dt = 1 / 60.0f;

Theme g_theme = {
    {0, 0, 0, 255},        // bg
    {18, 18, 21, 255},     // panel
    {30, 30, 35, 255},     // panel2
    {44, 44, 50, 255},     // line
    {236, 236, 240, 255},  // fg
    {150, 150, 160, 255},  // dim
    {80, 80, 88, 255},     // faint
    {90, 140, 255, 255},   // accent
    {240, 80, 80, 255},    // danger
    {70, 200, 120, 255},   // ok
};

float g_fade = 1;
static inline Uint8 faded(Uint8 a) { return g_fade >= 1 ? a : (Uint8)(a * g_fade + 0.5f); }

SDL_Color rgb(uint32_t hex, Uint8 a) { return {(Uint8)(hex >> 16), (Uint8)(hex >> 8), (Uint8)hex, a}; }
SDL_Color withAlpha(SDL_Color c, Uint8 a) { c.a = a; return c; }
SDL_Color mix(SDL_Color a, SDL_Color b, float t) {
    auto l = [t](Uint8 x, Uint8 y) { return (Uint8)(x + (y - x) * t); };
    return {l(a.r, b.r), l(a.g, b.g), l(a.b, b.b), l(a.a, b.a)};
}

// ---------------------------------------------------------------- fonts

// A font file loaded on first use (the whole file stays in memory for SDL_ttf).
struct FontFile {
    const char* path;
    std::string data;
    bool tried = false;
    bool load() {
        if (!tried && path) {
            tried = true;
            if (!readFile(path, data) || data.empty()) { data.clear(); appLog("font %s missing", path); }
        }
        return !data.empty();
    }
};
// Faces with an italic file use it for italics instead of slanting the regular one.
// New faces go at the end: settings store faces by index.
struct Face { const char* name; FontFile regular, italic; const void* ptr = nullptr; size_t size = 0; };
static Face s_faces[] = {
    {"Switch system font", {nullptr}, {nullptr}},
    {"Inter", {"romfs:/fonts/inter.ttf"}, {nullptr}},
    {"Lexend", {"romfs:/fonts/lexend.ttf"}, {nullptr}},
    {"Noto Sans", {"romfs:/mpv/subfont.ttf"}, {nullptr}},
    {"Atkinson Hyperlegible", {"romfs:/fonts/atkinson.ttf"}, {nullptr}},
    {"Tinos (serif)", {"romfs:/fonts/tinos.ttf"}, {nullptr}},
    {"Literata (serif)", {"romfs:/fonts/literata.ttf"}, {"romfs:/fonts/literata-italic.ttf"}},
    {"Source Serif (serif)", {"romfs:/fonts/sourceserif.ttf"}, {"romfs:/fonts/sourceserif-italic.ttf"}},
    {"EB Garamond (serif)", {"romfs:/fonts/ebgaramond.ttf"}, {"romfs:/fonts/ebgaramond-italic.ttf"}},
    {"Crimson Pro (serif)", {"romfs:/fonts/crimsonpro.ttf"}, {"romfs:/fonts/crimsonpro-italic.ttf"}},
    {"Libre Baskerville (serif)", {"romfs:/fonts/librebaskerville.ttf"}, {"romfs:/fonts/librebaskerville-italic.ttf"}},
};
static const int FACE_COUNT = sizeof(s_faces) / sizeof(s_faces[0]);
static bool s_plInit = false;
static std::map<int, TTF_Font*> s_fonts;

int fontFaceCount() { return FACE_COUNT; }
const char* fontFaceName(int face) { return s_faces[SDL_clamp(face, 0, FACE_COUNT - 1)].name; }
int uiFace() { return SDL_clamp(cfgInt("app.font", 4), 0, FACE_COUNT - 1); }  // Atkinson Hyperlegible

// Loads a face's file on first use; falls back to the system font.
static Face& faceData(int face) {
    Face& f = s_faces[face];
    if (!f.ptr && f.regular.load()) { f.ptr = f.regular.data.data(); f.size = f.regular.data.size(); }
    return f.ptr ? f : s_faces[0];
}

// Glyph edges are linear coverage, which makes light text on the dark UI look
// thin and stair-stepped. Lifting the mid tones (a gamma curve on alpha) gives
// smoother, fuller edges, close to what desktop font renderers do.
void textGamma(SDL_Surface* s) {
    static uint8_t lut[256];
    static bool made = false;
    if (!made) {
        for (int i = 0; i < 256; i++) lut[i] = (uint8_t)lroundf(255.0f * powf(i / 255.0f, 0.78f));
        made = true;
    }
    if (!s || s->format->BytesPerPixel != 4 || s->format->Amask == 0) return;
    int shift = s->format->Ashift;
    uint32_t mask = s->format->Amask;
    for (int y = 0; y < s->h; y++) {
        uint32_t* p = (uint32_t*)((uint8_t*)s->pixels + y * s->pitch);
        for (int x = 0; x < s->w; x++) {
            uint32_t a = (p[x] & mask) >> shift;
            p[x] = (p[x] & ~mask) | ((uint32_t)lut[a] << shift);
        }
    }
}
static SDL_Texture* s_circle = nullptr;    // white disc, AA edge
static SDL_Texture* s_invCircle = nullptr; // white outside a disc (for corner masks)

static SDL_Texture* makeDisc(bool inverse) {
    const int N = 256;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, N, N, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    float c = N / 2.0f, r = N / 2.0f - 1;
    for (int y = 0; y < N; y++) {
        uint8_t* row = (uint8_t*)s->pixels + y * s->pitch;
        for (int x = 0; x < N; x++) {
            float d = sqrtf((x + 0.5f - c) * (x + 0.5f - c) + (y + 0.5f - c) * (y + 0.5f - c));
            float a = SDL_clamp(r - d + 0.5f, 0.0f, 1.0f);
            if (inverse) a = 1 - a;
            row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = 255;
            row[x * 4 + 3] = (uint8_t)(a * 255);
        }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(g_ren, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

bool gfxInit() {
    if (TTF_Init() != 0) return false;
    s_circle = makeDisc(false);
    s_invCircle = makeDisc(true);
    // face 0: the system font, or font.ttf on the SD card if present
    Face& sys = s_faces[0];
    if (readFile(APP_DIR "/font.ttf", sys.regular.data) && !sys.regular.data.empty()) {
        sys.ptr = sys.regular.data.data();
        sys.size = sys.regular.data.size();
        return true;
    }
    PlFontData fd;
    if (R_FAILED(plInitialize(PlServiceType_User))) return false;
    s_plInit = true;
    if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) return false;
    sys.ptr = fd.address;
    sys.size = fd.size;
    return true;
}

static void textCacheClear();

void gfxQuit() {
    textCacheClear();
    for (auto& kv : s_fonts) TTF_CloseFont(kv.second);
    s_fonts.clear();
    if (s_circle) SDL_DestroyTexture(s_circle);
    if (s_invCircle) SDL_DestroyTexture(s_invCircle);
    TTF_Quit();
    if (s_plInit) plExit();
}

TTF_Font* font(int size, int style, int face) {
    if (face < 0 || face >= FACE_COUNT) face = uiFace();
    int key = (face * 1024 + size) * 4 + (style & 3);
    auto it = s_fonts.find(key);
    if (it != s_fonts.end()) return it->second;
    Face& fd = faceData(face);
    bool realItalic = (style & FS_ITALIC) && &fd == &s_faces[face] && fd.italic.load();
    const void* ptr = realItalic ? fd.italic.data.data() : fd.ptr;
    size_t sz = realItalic ? fd.italic.data.size() : fd.size;
    TTF_Font* f = TTF_OpenFontRW(SDL_RWFromConstMem(ptr, (int)sz), 1, size);
    if (f) {
        int st = 0;
        if (style & FS_BOLD) st |= TTF_STYLE_BOLD;
        if ((style & FS_ITALIC) && !realItalic) st |= TTF_STYLE_ITALIC;
        TTF_SetFontStyle(f, st);
#ifdef TTF_HINTING_LIGHT_SUBPIXEL
        TTF_SetFontHinting(f, TTF_HINTING_LIGHT_SUBPIXEL);  // glyphs placed at fractional positions: even spacing
#else
        TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
#endif
        TTF_SetFontKerning(f, 1);
    }
    s_fonts[key] = f;
    return f;
}

int fontHeight(int size, int style, int face) {
    TTF_Font* f = font(size, style, face);
    return f ? TTF_FontHeight(f) : size;
}

TTF_Font* fontFor(const std::string& s, int size, int style, int face) {
    TTF_Font* f = font(size, style, face);
    if (!f) return nullptr;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }  // ASCII: every bundled face has it
        uint32_t cp = utf8Next(s, i);
        if (!TTF_GlyphIsProvided32(f, cp)) return font(size, style, 0);
    }
    return f;
}

// ---------------------------------------------------------------- text cache
// Strings are rendered white once and tinted with color mod at draw time.

struct CachedText { SDL_Texture* tex; int w, h; uint32_t used; };
static std::unordered_map<std::string, CachedText> s_cache;

static void textCacheClear() {
    for (auto& kv : s_cache) SDL_DestroyTexture(kv.second.tex);
    s_cache.clear();
}

void gfxTrimCache() {
    if (s_cache.size() < 600) return;
    uint32_t now = SDL_GetTicks();
    for (auto it = s_cache.begin(); it != s_cache.end();) {
        if (now - it->second.used > 3000) {
            SDL_DestroyTexture(it->second.tex);
            it = s_cache.erase(it);
        } else ++it;
    }
}

int textWidth(const std::string& s, int size, int style, int face) {
    int w = 0, h = 0;
    TTF_Font* f = fontFor(s, size, style, face);
    if (!s.empty() && f) TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return w;
}

static std::string ellipsize(const std::string& s, int size, int style, int maxW, int face) {
    if (maxW <= 0 || textWidth(s, size, style, face) <= maxW) return s;
    std::vector<size_t> cuts;
    for (size_t i = 0; i < s.size();) { cuts.push_back(i); utf8Next(s, i); }
    int lo = 0, hi = (int)cuts.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (textWidth(s.substr(0, cuts[mid]) + "\xE2\x80\xA6", size, style, face) <= maxW) lo = mid;
        else hi = mid - 1;
    }
    return s.substr(0, cuts[lo]) + "\xE2\x80\xA6";
}

int drawText(const std::string& str, int x, int y, int size, SDL_Color c, int align, int maxW, int style, int face) {
    if (str.empty() || c.a == 0) return 0;
    if (face < 0 || face >= FACE_COUNT) face = uiFace();
    std::string key = std::to_string((face * 1024 + size) * 4 + style) + "|" + std::to_string(maxW) + "|" + str;
    auto it = s_cache.find(key);
    if (it == s_cache.end()) {
        std::string s = ellipsize(str, size, style, maxW, face);
        TTF_Font* f = fontFor(s, size, style, face);
        if (!f) return 0;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), SDL_Color{255, 255, 255, 255});
        if (!surf) return 0;
        textGamma(surf);
        CachedText ct;
        ct.tex = SDL_CreateTextureFromSurface(g_ren, surf);
        ct.w = surf->w;
        ct.h = surf->h;
        SDL_FreeSurface(surf);
        if (!ct.tex) return 0;
        it = s_cache.emplace(key, ct).first;
    }
    CachedText& ct = it->second;
    ct.used = g_ticks;
    SDL_SetTextureColorMod(ct.tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(ct.tex, faded(c.a));
    SDL_Rect dst = {x, y, ct.w, ct.h};
    if (align == 1) dst.x -= ct.w / 2;
    else if (align == 2) dst.x -= ct.w;
    SDL_RenderCopy(g_ren, ct.tex, nullptr, &dst);
    return ct.w;
}

std::vector<std::string> wrapText(const std::string& s, int size, int maxW, int style) {
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= s.size()) {
        size_t nl = s.find('\n', p);
        if (nl == std::string::npos) nl = s.size();
        std::string para = s.substr(p, nl - p), line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            if (sp == std::string::npos) sp = para.size();
            std::string word = para.substr(i, sp - i);
            std::string cand = line.empty() ? word : line + " " + word;
            if (textWidth(cand, size, style) <= maxW || line.empty()) {
                // a single word wider than the line is split by characters
                if (line.empty() && textWidth(word, size, style) > maxW) {
                    std::string part;
                    for (size_t k = 0; k < word.size();) {
                        size_t k0 = k;
                        utf8Next(word, k);
                        std::string next = part + word.substr(k0, k - k0);
                        if (textWidth(next, size, style) > maxW && !part.empty()) { lines.push_back(part); part = word.substr(k0, k - k0); }
                        else part = next;
                    }
                    line = part;
                } else line = cand;
            } else {
                lines.push_back(line);
                line = word;
            }
            i = sp + 1;
        }
        lines.push_back(line);
        p = nl + 1;
    }
    return lines;
}

int drawWrapped(const std::string& s, int x, int y, int size, SDL_Color c, int maxW, int maxLines, int align, int style) {
    auto lines = wrapText(s, size, maxW, style);
    int lh = fontHeight(size, style) * 115 / 100;
    int n = (int)lines.size();
    if (maxLines > 0 && n > maxLines) {
        lines.resize(maxLines);
        lines.back() += "\xE2\x80\xA6";
        n = maxLines;
    }
    int ax = align == 1 ? x + maxW / 2 : align == 2 ? x + maxW : x;
    for (int i = 0; i < n; i++) drawText(lines[i], ax, y + i * lh, size, c, align, maxW, style);
    return n * lh;
}

// ---------------------------------------------------------------- primitives

void fillRect(int x, int y, int w, int h, SDL_Color c) {
    c.a = faded(c.a);
    if (c.a == 0 || w <= 0 || h <= 0) return;
    SDL_SetRenderDrawBlendMode(g_ren, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(g_ren, &r);
}

void fillRect(const SDL_Rect& r, SDL_Color c) { fillRect(r.x, r.y, r.w, r.h, c); }

static void discPart(SDL_Texture* t, int sx, int sy, int sw, int sh, float x, float y, float w, float h, SDL_Color c) {
    if (!t) return;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, faded(c.a));
    SDL_Rect src = {sx, sy, sw, sh};
    SDL_FRect dst = {x, y, w, h};
    SDL_RenderCopyF(g_ren, t, &src, &dst);
}

void fillCircle(float cx, float cy, float r, SDL_Color c) {
    if (c.a == 0 || r <= 0) return;
    discPart(s_circle, 0, 0, 256, 256, cx - r, cy - r, 2 * r, 2 * r, c);
}

void fillRound(const SDL_Rect& r, int rad, SDL_Color c) {
    if (c.a == 0 || r.w <= 0 || r.h <= 0) return;
    rad = std::min(rad, std::min(r.w, r.h) / 2);
    if (rad <= 0) { fillRect(r, c); return; }
    // corners come from the quadrants of the disc texture
    discPart(s_circle, 0, 0, 128, 128, r.x, r.y, rad, rad, c);
    discPart(s_circle, 128, 0, 128, 128, r.x + r.w - rad, r.y, rad, rad, c);
    discPart(s_circle, 0, 128, 128, 128, r.x, r.y + r.h - rad, rad, rad, c);
    discPart(s_circle, 128, 128, 128, 128, r.x + r.w - rad, r.y + r.h - rad, rad, rad, c);
    fillRect(r.x + rad, r.y, r.w - 2 * rad, r.h, c);
    fillRect(r.x, r.y + rad, rad, r.h - 2 * rad, c);
    fillRect(r.x + r.w - rad, r.y + rad, rad, r.h - 2 * rad, c);
}

// Masks the corners of a rect with the background color (for rounded images).
static void maskCorners(const SDL_Rect& r, int rad, SDL_Color bg) {
    rad = std::min(rad, std::min(r.w, r.h) / 2);
    if (rad <= 0) return;
    discPart(s_invCircle, 0, 0, 128, 128, r.x, r.y, rad, rad, bg);
    discPart(s_invCircle, 128, 0, 128, 128, r.x + r.w - rad, r.y, rad, rad, bg);
    discPart(s_invCircle, 0, 128, 128, 128, r.x, r.y + r.h - rad, rad, rad, bg);
    discPart(s_invCircle, 128, 128, 128, 128, r.x + r.w - rad, r.y + r.h - rad, rad, rad, bg);
}

static void geometry(const std::vector<SDL_Vertex>& v, const std::vector<int>& idx) {
    if (v.empty()) return;
    // Untextured geometry uses the renderer's draw blend mode, which fillRect
    // switches off for opaque colours: without this, gradients and soft edges
    // drawn after an opaque rect came out solid.
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(g_ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

static SDL_Vertex vert(float x, float y, SDL_Color c) {
    c.a = faded(c.a);
    return SDL_Vertex{{x, y}, c, {0, 0}};
}

// Ring between radii r0 < r1 from angle a0 to a1 (radians), with 1px soft edges.
static void ringGeom(float cx, float cy, float r0, float r1, float a0, float a1, SDL_Color c) {
    if (c.a == 0) return;
    float span = fabsf(a1 - a0);
    int seg = std::max(8, (int)(span * r1 / 4));
    seg = std::min(seg, 256);
    SDL_Color clear = withAlpha(c, 0);
    float radii[4] = {std::max(0.0f, r0 - 0.5f), r0 + 0.5f, r1 - 0.5f, r1 + 0.5f};
    if (radii[2] < radii[1]) radii[1] = radii[2] = (r0 + r1) / 2;
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    v.reserve((seg + 1) * 4);
    for (int i = 0; i <= seg; i++) {
        float a = a0 + (a1 - a0) * i / seg;
        float cs = cosf(a), sn = sinf(a);
        for (int k = 0; k < 4; k++) v.push_back(vert(cx + cs * radii[k], cy + sn * radii[k], (k == 0 || k == 3) ? clear : c));
    }
    for (int i = 0; i < seg; i++) {
        for (int k = 0; k < 3; k++) {
            int a = i * 4 + k, b = a + 1, d = a + 4, e = b + 4;
            idx.insert(idx.end(), {a, b, d, b, e, d});
        }
    }
    geometry(v, idx);
}

void strokeCircle(float cx, float cy, float r, float t, SDL_Color c) { ringGeom(cx, cy, r - t / 2, r + t / 2, 0, 2 * (float)M_PI, c); }

void drawArc(float cx, float cy, float r, float a0, float a1, float t, SDL_Color c) {
    ringGeom(cx, cy, r - t / 2, r + t / 2, a0, a1, c);
}

void strokeRound(const SDL_Rect& r, int rad, int t, SDL_Color c) {
    rad = std::min(rad, std::min(r.w, r.h) / 2);
    float h = t / 2.0f;
    float x0 = r.x + h, y0 = r.y + h, x1 = r.x + r.w - h, y1 = r.y + r.h - h;
    float rr = std::max(0.0f, rad - h);
    const float P = (float)M_PI;
    drawLine(x0 + rr, y0, x1 - rr, y0, (float)t, c, false);
    drawLine(x0 + rr, y1, x1 - rr, y1, (float)t, c, false);
    drawLine(x0, y0 + rr, x0, y1 - rr, (float)t, c, false);
    drawLine(x1, y0 + rr, x1, y1 - rr, (float)t, c, false);
    if (rr > 0) {
        drawArc(x0 + rr, y0 + rr, rr, P, 1.5f * P, (float)t, c);
        drawArc(x1 - rr, y0 + rr, rr, 1.5f * P, 2 * P, (float)t, c);
        drawArc(x1 - rr, y1 - rr, rr, 0, 0.5f * P, (float)t, c);
        drawArc(x0 + rr, y1 - rr, rr, 0.5f * P, P, (float)t, c);
    }
}

void drawLine(float x1, float y1, float x2, float y2, float t, SDL_Color c, bool caps) {
    if (c.a == 0) return;
    float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) {
        if (caps) fillCircle(x1, y1, t / 2, c);
        return;
    }
    float nx = -dy / len, ny = dx / len;
    float hi = std::max(0.0f, t / 2 - 0.5f), ho = t / 2 + 0.5f;
    SDL_Color clear = withAlpha(c, 0);
    std::vector<SDL_Vertex> v = {
        vert(x1 + nx * ho, y1 + ny * ho, clear), vert(x1 + nx * hi, y1 + ny * hi, c),
        vert(x1 - nx * hi, y1 - ny * hi, c),     vert(x1 - nx * ho, y1 - ny * ho, clear),
        vert(x2 + nx * ho, y2 + ny * ho, clear), vert(x2 + nx * hi, y2 + ny * hi, c),
        vert(x2 - nx * hi, y2 - ny * hi, c),     vert(x2 - nx * ho, y2 - ny * ho, clear),
    };
    std::vector<int> idx;
    for (int k = 0; k < 3; k++) idx.insert(idx.end(), {k, k + 1, k + 4, k + 1, k + 5, k + 4});
    geometry(v, idx);
    if (caps) {
        // half discs at the ends
        float a = atan2f(dy, dx);
        ringGeom(x1, y1, 0, t / 2, a + (float)M_PI / 2, a + 1.5f * (float)M_PI, c);
        ringGeom(x2, y2, 0, t / 2, a - (float)M_PI / 2, a + (float)M_PI / 2, c);
    }
}

void fillPoly(const SDL_FPoint* p, int n, SDL_Color c) {
    if (n < 3 || c.a == 0) return;
    float area = 0;
    for (int i = 0; i < n; i++) {
        const SDL_FPoint &a = p[i], &b = p[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    float sign = area > 0 ? 1.0f : -1.0f;  // outward normal direction
    std::vector<SDL_FPoint> en(n);
    for (int i = 0; i < n; i++) {
        float dx = p[(i + 1) % n].x - p[i].x, dy = p[(i + 1) % n].y - p[i].y;
        float l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-4f) l = 1;
        en[i] = {sign * dy / l, -sign * dx / l};
    }
    SDL_Color clear = withAlpha(c, 0);
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    for (int i = 0; i < n; i++) {
        SDL_FPoint a = en[(i + n - 1) % n], b = en[i];
        float mx = a.x + b.x, my = a.y + b.y, ml = sqrtf(mx * mx + my * my);
        if (ml < 1e-4f) { mx = b.x; my = b.y; ml = 1; }
        mx /= ml; my /= ml;
        float d = mx * b.x + my * b.y;
        float len = 0.5f / std::max(0.3f, d);
        v.push_back(vert(p[i].x - mx * len, p[i].y - my * len, c));      // inner
        v.push_back(vert(p[i].x + mx * len, p[i].y + my * len, clear));  // outer
    }
    for (int i = 1; i + 1 < n; i++) idx.insert(idx.end(), {0, i * 2, (i + 1) * 2});
    for (int i = 0; i < n; i++) {
        int a = i * 2, b = ((i + 1) % n) * 2;
        idx.insert(idx.end(), {a, a + 1, b, a + 1, b + 1, b});
    }
    geometry(v, idx);
}

void fillTri(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c) {
    SDL_FPoint p[3] = {{x1, y1}, {x2, y2}, {x3, y3}};
    fillPoly(p, 3, c);
}

void fillGradientV(const SDL_Rect& r, SDL_Color top, SDL_Color bottom) {
    std::vector<SDL_Vertex> v = {vert(r.x, r.y, top), vert(r.x + r.w, r.y, top), vert(r.x + r.w, r.y + r.h, bottom),
                                 vert(r.x, r.y + r.h, bottom)};
    geometry(v, {0, 1, 2, 0, 2, 3});
}

void drawTex(SDL_Texture* t, const SDL_Rect& dst, Uint8 alpha) {
    if (!t) return;
    SDL_SetTextureAlphaMod(t, faded(alpha));
    SDL_SetTextureColorMod(t, 255, 255, 255);
    SDL_RenderCopy(g_ren, t, nullptr, &dst);
}

void drawTexFit(SDL_Texture* t, const SDL_Rect& box, bool cover, Uint8 alpha, int radius) {
    if (!t) return;
    int tw, th;
    SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    float s = cover ? std::max(box.w / (float)tw, box.h / (float)th) : std::min(box.w / (float)tw, box.h / (float)th);
    SDL_SetTextureAlphaMod(t, faded(alpha));
    SDL_SetTextureColorMod(t, 255, 255, 255);
    if (cover) {
        // crop the source so the destination stays inside the box
        float sw = box.w / s, sh = box.h / s;
        SDL_Rect src = {(int)((tw - sw) / 2), (int)((th - sh) / 2), (int)sw, (int)sh};
        SDL_RenderCopy(g_ren, t, &src, &box);
        if (radius > 0) maskCorners(box, radius, g_theme.bg);
    } else {
        SDL_Rect dst = {box.x + (box.w - (int)(tw * s)) / 2, box.y + (box.h - (int)(th * s)) / 2, (int)(tw * s), (int)(th * s)};
        SDL_RenderCopy(g_ren, t, nullptr, &dst);
        if (radius > 0) maskCorners(dst, radius, g_theme.bg);
    }
}

static std::vector<SDL_Rect> s_clips;

void pushClip(const SDL_Rect& r) {
    SDL_Rect c = r;
    if (!s_clips.empty()) SDL_IntersectRect(&s_clips.back(), &r, &c);
    s_clips.push_back(c);
    SDL_RenderSetClipRect(g_ren, &c);
}

void popClip() {
    if (!s_clips.empty()) s_clips.pop_back();
    SDL_RenderSetClipRect(g_ren, s_clips.empty() ? nullptr : &s_clips.back());
}

void spinner(float cx, float cy, float r, SDL_Color c) {
    float t = g_ticks / 1000.0f * 5.0f;
    float span = 1.2f + 0.8f * sinf(g_ticks / 400.0f);
    drawArc(cx, cy, r, t, t + span * 2, std::max(3.0f, r / 6), c);
}

SDL_Texture* textureFromSurface(SDL_Surface* s) {
    if (!s) return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(g_ren, s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}
