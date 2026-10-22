// Text reader for EPUB/TXT: rich layout (headings, bold/italic, justified
// paragraphs, images), one chapter per page run with a large chapter header,
// tappable links and footnote popups, and a touch toolbar. Opens on the
// contents screen.
#include "book.h"
#include "reader.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <unordered_map>

// ---------------------------------------------------------------- palette

static const ReadPalette OLED = {{0, 0, 0, 255}, {224, 222, 216, 255}, {128, 128, 134, 255},
                                 {242, 165, 65, 255}, {38, 38, 44, 255}, {20, 20, 24, 255}};
static const ReadPalette WARM = {{0, 0, 0, 255}, {255, 196, 120, 255}, {150, 110, 60, 255},
                                 {255, 150, 60, 255}, {44, 32, 20, 255}, {22, 16, 10, 255}};
static const ReadPalette NIGHT = {{0, 0, 0, 255}, {255, 20, 0, 255}, {150, 0, 0, 255},
                                  {255, 90, 40, 255}, {60, 0, 0, 255}, {24, 0, 0, 255}};

// 0 white, 1 warm (less blue light), 2 red (night)
int readColor() { return SDL_clamp(cfgInt("books.color", cfgBool("books.night", false) ? 2 : 0), 0, 2); }
const ReadPalette& readPalette() { int c = readColor(); return c == 2 ? NIGHT : c == 1 ? WARM : OLED; }
// Books are portrait only: the way the console is turned, or (held landscape,
// or with nothing to tell) the way chosen in the reader's settings.
int readRotation() {
    int d = deviceRotation();
    if (d == 90 || d == 270) return d;
    return cfgInt("books.rotation", 90) == 270 ? 270 : 90;
}

void readerStatusBar(int y, const std::string& left) {
    const ReadPalette& p = readPalette();
    char right[64];
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    u32 batt = 0;
    psmGetBatteryChargePercentage(&batt);
    snprintf(right, sizeof(right), "%02d:%02d   %u%%", lt ? lt->tm_hour : 0, lt ? lt->tm_min : 0, batt);
    int rw = drawText(right, g_W - 28, y, 18, p.dim, 2);
    drawText(left, 28, y, 18, p.dim, 0, g_W - 56 - rw - 24);
}

// ---------------------------------------------------------------- layout types

struct Frag {
    uint32_t start, end;
    int16_t x, y;      // x from the left text edge, y from the line top
    uint8_t size, fs;
    int32_t link;
};

struct Line {
    int32_t y = 0, h = 0;
    uint32_t fragBegin = 0, fragEnd = 0;
    uint32_t start = 0;  // text offset of the line
    int32_t block = -1;
    int32_t image = -1;
    int16_t ix = 0, iw = 0, ih = 0;
    bool hr = false;
};

struct LayoutCfg {
    int fontSize = 30, spacing = 145, margin = 48, face = 1;
    bool indent = true, justify = true;
    bool operator!=(const LayoutCfg& o) const {
        return fontSize != o.fontSize || spacing != o.spacing || margin != o.margin || indent != o.indent || justify != o.justify ||
               face != o.face;
    }
};

static LayoutCfg currentCfg() {
    LayoutCfg c;
    c.fontSize = SDL_clamp(cfgInt("books.font_size", 30), 16, 72);
    c.spacing = SDL_clamp(cfgInt("books.line_spacing", 145), 100, 220);
    c.margin = SDL_clamp(cfgInt("books.margin", 48), 0, 200);
    c.face = SDL_clamp(cfgInt("books.font", 1), 0, fontFaceCount() - 1);
    c.indent = cfgBool("books.indent", true);
    c.justify = cfgBool("books.justify", true);
    return c;
}

static const int TOP = 54, BOTTOM = 56;

static bool isCjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x3000 && cp <= 0x303F);
}

// ---------------------------------------------------------------- reader

class TextReader : public BookReader {
public:
    explicit TextReader(const std::string& path) {
        if (!loadDoc(path, m_doc)) { m_err = m_doc.err; return; }
        int64_t pos = 0, total = 0;
        m_started = posGet(path, pos, total);
        m_offset = (uint32_t)SDL_clamp(pos, (int64_t)0, (int64_t)m_doc.text.size());
        appLog("book %s: %d blocks, %d chapters, %d links, %d images", path.c_str(), (int)m_doc.blocks.size(),
               (int)m_doc.toc.size(), (int)m_doc.links.size(), (int)m_doc.images.size());
    }

    ~TextReader() override {
        for (auto& t : m_tex) if (t) SDL_DestroyTexture(t);
        for (auto& kv : m_imgTex) if (kv.second) SDL_DestroyTexture(kv.second);
    }

    bool ok() const override { return m_err.empty(); }
    std::string error() const override { return m_err; }
    bool wantsClose() const override { return m_close; }
    int rotation() const override { return readRotation(); }
    int brightness() const override { return SDL_clamp(cfgInt("books.brightness", 100), 10, 100); }

    void save() override {
        if (m_started) posSet(m_doc.path, m_offset, (int64_t)m_doc.text.size());
    }

    void update() override {
        LayoutCfg c = currentCfg();
        if (m_layW != g_W || m_layH != g_H || c != m_cfg || m_night != (readColor() == 2) || m_color != readColor()) {
            bool relayout = m_layW != g_W || m_layH != g_H || c != m_cfg;
            m_night = readColor() == 2;
            m_color = readColor();
            if (relayout) layout();
            // images are tinted when they're loaded
            for (auto& kv : m_imgTex) if (kv.second) SDL_DestroyTexture(kv.second);
            m_imgTex.clear();
            invalidate();
        }
    }

    void draw() override {
        if (m_mode == HOME) drawHome();
        else drawRead();
    }

private:
    enum Mode { HOME, READ };
    Doc m_doc;
    std::string m_err;
    Mode m_mode = HOME;
    bool m_close = false, m_started = false, m_night = false;
    int m_color = -1;

    // layout
    LayoutCfg m_cfg;
    int m_layW = 0, m_layH = 0, m_margin = 64, m_maxW = 1000;
    std::vector<Frag> m_frags;
    std::vector<Line> m_lines;
    std::vector<int> m_pages;        // first line of each page
    std::vector<int> m_tocPage;
    std::unordered_map<std::string, int> m_measure;
    int m_page = 0;
    uint32_t m_offset = 0;

    // rendering
    SDL_Texture* m_tex[2] = {nullptr, nullptr};
    int m_texPage[2] = {-1, -1};
    int m_cur = 0;
    uint32_t m_animStart = 0;
    int m_animDir = 0;
    std::map<int, SDL_Texture*> m_imgTex;

    // ui
    bool m_toolbar = false, m_settings = false, m_toc = false;
    Scroller m_tocScroll;
    bool m_tocCentered = false;
    std::vector<uint32_t> m_back;
    size_t m_backSeen = 0;  // m_back's size when m_backAnchor was set
    int m_backAnchor = 0;   // the page a jump landed on
    bool m_note = false;
    std::string m_noteText;
    uint32_t m_noteTarget = 0;
    float m_slider = 0;
    bool m_sliding = false;
    float m_pinch = 1.0f;

    // ------------------------------------------------------------ layout

    int measure(const std::string& s, int size, int fs) {
        std::string key;
        key.reserve(s.size() + 2);
        key += (char)size;
        key += (char)fs;
        key += s;
        auto it = m_measure.find(key);
        if (it != m_measure.end()) return it->second;
        int w = textWidth(s, size, fs, m_cfg.face);
        m_measure.emplace(std::move(key), w);
        return w;
    }

    int sizeFor(const Block& b, uint8_t rs, bool bigTitle) const {
        float s = (float)m_cfg.fontSize;
        if (b.type == B_HEAD) s *= b.level == 1 ? 1.5f : b.level == 2 ? 1.35f : b.level == 3 ? 1.2f : 1.1f;
        if (b.chapterTitle) s = m_cfg.fontSize * (bigTitle ? 1.9f : 1.3f);
        if (b.type == B_PRE) s *= 0.88f;
        if (rs & RS_BIG) s *= 1.2f;
        if (rs & RS_SMALL) s *= 0.85f;
        if (rs & (RS_SUP | RS_SUB)) s *= 0.66f;
        return SDL_clamp((int)lroundf(s), 10, 140);
    }

    static int fsFor(const Block& b, uint8_t rs) {
        int fs = 0;
        if ((rs & RS_BOLD) || b.type == B_HEAD || b.chapterTitle) fs |= FS_BOLD;
        if (rs & RS_ITALIC) fs |= FS_ITALIC;
        return fs;
    }

    struct Tok {
        uint32_t s, e;
        uint8_t rs;
        int32_t link;
        bool space, brk, cjk;
        int w;
        uint8_t size, fs;
    };

    struct Item { uint32_t s, e; int w, gap; bool justifiable; uint8_t size, fs, rs; int32_t link; };

    void layoutBlock(int bi, const Block& b, bool bigTitle, int firstIndent, std::vector<Line>& out) {
        const std::string& T = m_doc.text;
        int left = 36 * b.quote + (b.list > 1 ? 32 * (b.list - 1) : 0);
        int avail = m_maxW - left - 24 * b.quote;
        int align = b.align;
        if (align == AL_AUTO && b.chapterTitle) align = AL_CENTER;
        bool canJustify = m_cfg.justify && align == AL_AUTO && (b.type == B_PARA || b.type == B_LIST);
        int spacing = (b.type == B_HEAD || b.chapterTitle) ? std::min(m_cfg.spacing, 125) : m_cfg.spacing;

        // tokens
        std::vector<Tok> toks;
        bool pendingSpace = false, pendingBreak = false;
        for (uint32_t ri = b.runBegin; ri < b.runEnd; ri++) {
            const Run& r = m_doc.runs[ri];
            uint32_t i = std::max(r.start, b.start), end = std::min(r.end, b.end);
            int size = sizeFor(b, r.style, bigTitle), fs = fsFor(b, r.style);
            while (i < end) {
                char c = T[i];
                if (c == ' ') { pendingSpace = true; i++; continue; }
                if (c == '\n') {
                    toks.push_back({i, i, 0, -1, false, true, false, 0, (uint8_t)size, (uint8_t)fs});
                    pendingSpace = false;
                    i++;
                    continue;
                }
                size_t j = i;
                bool cjk = false;
                while (j < end && T[j] != ' ' && T[j] != '\n') {
                    size_t k = j;
                    uint32_t cp = utf8Next(T, k);
                    if (isCjk(cp)) {
                        if (j == i) { cjk = true; j = k; }
                        break;
                    }
                    j = k;
                }
                Tok t = {i, (uint32_t)j, r.style, r.link, pendingSpace, pendingBreak, cjk, 0, (uint8_t)size, (uint8_t)fs};
                t.w = measure(T.substr(i, j - i), size, fs);
                toks.push_back(t);
                pendingSpace = pendingBreak = false;
                i = (uint32_t)j;
            }
        }

        std::vector<Item> line;
        int lineW = 0, lineIdx = 0;
        uint32_t emptyStart = b.start;  // offset used for blank lines (keeps line starts ordered)
        auto spaceW = [&](int size, int fs) { return measure(" ", size, fs); };
        auto emit = [&](bool forced, bool last) {
            Line L;
            L.block = bi;
            L.start = line.empty() ? emptyStart : line[0].s;
            int indent = lineIdx == 0 ? firstIndent : 0;
            int room = avail - indent;
            int natural = lineW;
            bool just = canJustify && !forced && !last && natural < room;
            int gaps = 0;
            for (size_t k = 1; k < line.size(); k++) if (line[k].justifiable) gaps++;
            int extra = just && gaps ? room - natural : 0;
            int x = left + indent;
            if (align == AL_CENTER) x += std::max(0, (room - natural) / 2);
            else if (align == AL_RIGHT) x += std::max(0, room - natural);
            int maxH = 0, maxAsc = 0;
            for (auto& it : line) {
                TTF_Font* f = font(it.size, it.fs);
                maxH = std::max(maxH, f ? TTF_FontHeight(f) : it.size);
                maxAsc = std::max(maxAsc, f ? TTF_FontAscent(f) : it.size);
            }
            if (line.empty()) {
                TTF_Font* f = font(m_cfg.fontSize, 0, m_cfg.face);
                maxH = f ? TTF_FontHeight(f) : m_cfg.fontSize;
            }
            L.h = maxH * spacing / 100;
            int baseline = (L.h - maxH) / 2 + maxAsc;
            L.fragBegin = (uint32_t)m_frags.size();
            int gapIdx = 0;
            for (size_t k = 0; k < line.size(); k++) {
                const Item& it = line[k];
                int g = it.gap;
                if (k > 0 && it.justifiable && extra > 0) {
                    gapIdx++;
                    g += extra * gapIdx / gaps - extra * (gapIdx - 1) / gaps;
                }
                if (k > 0) x += g;
                TTF_Font* f = font(it.size, it.fs);
                int asc = f ? TTF_FontAscent(f) : it.size;
                int y = baseline - asc;
                if (it.rs & RS_SUP) y -= m_cfg.fontSize * 2 / 5;
                if (it.rs & RS_SUB) y += m_cfg.fontSize / 6;
                // merge with the previous fragment when the spacing is natural
                Frag* prev = m_frags.size() > L.fragBegin ? &m_frags.back() : nullptr;
                if (prev && !just && prev->size == it.size && prev->fs == it.fs && prev->link == it.link && prev->y == y &&
                    (it.rs & (RS_SUP | RS_SUB)) == 0 && T.compare(prev->end, it.s - prev->end, std::string(it.s - prev->end, ' ')) == 0 &&
                    (it.s - prev->end) <= 1) {
                    prev->end = it.e;
                } else {
                    m_frags.push_back({it.s, it.e, (int16_t)x, (int16_t)y, it.size, it.fs, it.link});
                }
                x += it.w;
            }
            L.fragEnd = (uint32_t)m_frags.size();
            out.push_back(L);
            line.clear();
            lineW = 0;
            lineIdx++;
        };

        size_t i = 0;
        while (i < toks.size()) {
            if (toks[i].brk && toks[i].s == toks[i].e) {
                emptyStart = toks[i].s;
                emit(true, false);
                i++;
                continue;
            }
            // a group of glued tokens (no space between, not CJK)
            size_t j = i + 1;
            while (j < toks.size() && !toks[j].space && !toks[j].cjk && !toks[j - 1].cjk && !(toks[j].brk && toks[j].s == toks[j].e)) j++;
            int gw = 0;
            for (size_t k = i; k < j; k++) gw += toks[k].w;
            int indent = lineIdx == 0 ? firstIndent : 0;
            int room = avail - indent;
            int sw = (!line.empty() && toks[i].space) ? spaceW(toks[i].size, toks[i].fs) : 0;
            if (!line.empty() && lineW + sw + gw > room) {
                emit(false, false);
                sw = 0;
                indent = 0;
                room = avail;
            }
            if (line.empty() && gw > room) {
                // longer than a line: split by characters
                for (size_t k = i; k < j; k++) {
                    const Tok& t = toks[k];
                    uint32_t p = t.s;
                    while (p < t.e) {
                        uint32_t q = p, best = p;
                        int bw = 0;
                        while (q < t.e) {
                            size_t n = q;
                            utf8Next(T, n);
                            int w = measure(T.substr(p, n - p), t.size, t.fs);
                            if (lineW + w > (lineIdx == 0 ? avail - firstIndent : avail) && best > p) break;
                            best = (uint32_t)n;
                            bw = w;
                            q = (uint32_t)n;
                        }
                        line.push_back({p, best, bw, 0, false, t.size, t.fs, t.rs, t.link});
                        lineW += bw;
                        p = best;
                        if (p < t.e) emit(false, false);
                    }
                }
                i = j;
                continue;
            }
            for (size_t k = i; k < j; k++) {
                const Tok& t = toks[k];
                int g = k == i ? sw : 0;
                line.push_back({t.s, t.e, t.w, g, k == i && sw > 0, t.size, t.fs, t.rs, t.link});
                lineW += g + t.w;
            }
            i = j;
        }
        if (!line.empty() || out.empty()) emit(true, true);
        else if (!out.empty()) {
            // the block ended right after a forced break; the last line shouldn't justify
        }
    }

    void layout() {
        m_cfg = currentCfg();
        m_layW = g_W;
        m_layH = g_H;
        m_measure.clear();
        m_frags.clear();
        m_lines.clear();
        m_pages.clear();
        m_margin = std::min(m_cfg.margin, g_W / 5);
        m_maxW = g_W - 2 * m_margin;
        int pageH = g_H - TOP - BOTTOM;
        TTF_Font* bf = font(m_cfg.fontSize, 0, m_cfg.face);
        int baseLine = (bf ? TTF_FontHeight(bf) : m_cfg.fontSize) * m_cfg.spacing / 100;

        int y = 0;
        bool pageEmpty = true;
        m_pages.push_back(0);
        auto newPage = [&]() {
            if (pageEmpty) return;
            m_pages.push_back((int)m_lines.size());
            y = 0;
            pageEmpty = true;
        };
        int prevType = -1;
        bool prevTitle = false;
        int gapAfter = 0;
        std::vector<Line> bl;
        for (int bi = 0; bi < (int)m_doc.blocks.size(); bi++) {
            const Block& b = m_doc.blocks[bi];
            if (b.hidden) continue;
            if (b.breakBefore) newPage();
            bl.clear();
            int before;
            if (b.chapterTitle) before = prevTitle ? baseLine / 3 : (pageEmpty ? pageH / 9 : baseLine * 3 / 2);
            else if (b.type == B_HEAD) before = baseLine * 9 / 10;
            else if (b.type == B_IMAGE || b.type == B_HR) before = baseLine / 2;
            else before = m_cfg.indent ? baseLine / 8 : baseLine * 3 / 5;
            if (prevTitle && !b.chapterTitle) before = std::max(before, baseLine * 6 / 5);
            before = std::max(before, gapAfter);
            gapAfter = 0;

            if (b.type == B_IMAGE) {
                Line L;
                L.block = bi;
                L.image = b.image;
                L.start = b.start;
                const ImageRes& im = m_doc.images[b.image];
                int w = im.w > 0 ? im.w : 600, h = im.h > 0 ? im.h : 800;
                float s = std::min({m_maxW / (float)w, (pageH * 0.96f) / (float)h, 2.0f});
                L.iw = (int16_t)std::max(1, (int)(w * s));
                L.ih = (int16_t)std::max(1, (int)(h * s));
                L.ix = (int16_t)((m_maxW - L.iw) / 2);
                L.h = L.ih;
                bl.push_back(L);
            } else if (b.type == B_HR) {
                Line L;
                L.block = bi;
                L.hr = true;
                L.start = b.start;
                L.h = baseLine;
                bl.push_back(L);
            } else {
                bool firstPara = prevType != B_PARA || prevTitle;
                int indent = (m_cfg.indent && b.type == B_PARA && !b.noIndent && b.align == AL_AUTO && !firstPara)
                                 ? m_cfg.fontSize * 3 / 2 : 0;
                bool bigTitle = b.chapterTitle && !prevTitle;
                layoutBlock(bi, b, bigTitle, indent, bl);
            }
            int blockH = 0;
            for (auto& l : bl) blockH += l.h;
            if (pageEmpty && !b.chapterTitle) before = 0;
            // keep headings with what follows, and images whole
            bool keep = b.type == B_HEAD || b.chapterTitle || b.type == B_IMAGE;
            int need = blockH + (b.type == B_IMAGE ? 0 : 2 * baseLine);
            if (keep && !pageEmpty && y + before + need > pageH) {
                newPage();
                before = b.chapterTitle ? pageH / 9 : 0;
            }
            for (auto& l : bl) {
                if (!pageEmpty && y + before + l.h > pageH) {
                    newPage();
                    before = 0;
                }
                l.y = y + before;
                y = l.y + l.h;
                before = 0;
                pageEmpty = false;
                m_lines.push_back(l);
            }
            prevType = b.type;
            prevTitle = b.chapterTitle;
            if (b.chapterTitle) gapAfter = baseLine * 6 / 5;
        }
        if (m_lines.empty()) { Line L; m_lines.push_back(L); }
        if (m_pages.size() > 1 && m_pages.back() >= (int)m_lines.size()) m_pages.pop_back();
        m_tocPage.clear();
        for (auto& c : m_doc.toc) m_tocPage.push_back(pageOf(c.offset));
        m_page = pageOf(m_offset);
        appLog("layout: %d lines, %d pages", (int)m_lines.size(), (int)m_pages.size());
    }

    int pageOf(uint32_t off) const {
        int lo = 0, hi = (int)m_pages.size() - 1, best = 0;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (m_lines[m_pages[mid]].start <= off) { best = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return best;
    }

    int pageLines(int p, int& first) const {
        first = m_pages[p];
        int last = p + 1 < (int)m_pages.size() ? m_pages[p + 1] : (int)m_lines.size();
        return last - first;
    }

    // ------------------------------------------------------------ rendering

    void invalidate() { m_texPage[0] = m_texPage[1] = -1; }

    SDL_Texture* imageTex(int idx, int w, int h) {
        auto it = m_imgTex.find(idx);
        if (it != m_imgTex.end()) return it->second;
        if (m_imgTex.size() > 8) {
            for (auto& kv : m_imgTex) if (kv.second) SDL_DestroyTexture(kv.second);
            m_imgTex.clear();
        }
        std::string data = docImageData(m_doc, idx);
        SDL_Texture* t = nullptr;
        if (!data.empty()) {
            SDL_Surface* s = loadImageMem(data.data(), data.size(), w, h);
            if (s) {
                if (m_color == 2) {
                    for (int y = 0; y < s->h; y++) {
                        uint8_t* p = (uint8_t*)s->pixels + y * s->pitch;
                        for (int x = 0; x < s->w; x++, p += 4) { p[0] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8); p[1] = p[2] = 0; }
                    }
                } else if (m_color == 1) {
                    for (int y = 0; y < s->h; y++) {
                        uint8_t* p = (uint8_t*)s->pixels + y * s->pitch;
                        for (int x = 0; x < s->w; x++, p += 4) { p[1] = (uint8_t)(p[1] * 200 >> 8); p[2] = (uint8_t)(p[2] * 140 >> 8); }
                    }
                }
                t = textureFromSurface(s);
                SDL_FreeSurface(s);
            }
        }
        m_imgTex[idx] = t;
        return t;
    }

    void renderPage(int page, int slot) {
        if (!m_tex[slot]) {
            m_tex[slot] = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 1280, 1280);
            if (!m_tex[slot]) return;
        }
        m_texPage[slot] = page;
        const ReadPalette& p = readPalette();
        SDL_Texture* prev = SDL_GetRenderTarget(g_ren);
        SDL_SetRenderTarget(g_ren, m_tex[slot]);
        SDL_SetRenderDrawColor(g_ren, p.bg.r, p.bg.g, p.bg.b, 255);
        SDL_RenderClear(g_ren);
        int first, n = pageLines(page, first);
        for (int li = first; li < first + n; li++) {
            const Line& L = m_lines[li];
            int top = TOP + L.y;
            if (L.image >= 0) {
                SDL_Texture* t = imageTex(L.image, L.iw, L.ih);
                SDL_Rect dst = {m_margin + L.ix, top, L.iw, L.ih};
                if (t) SDL_RenderCopy(g_ren, t, nullptr, &dst);
                else fillRect(dst, p.sel);
                continue;
            }
            if (L.hr) {
                int w = m_maxW / 3;
                fillRect(m_margin + (m_maxW - w) / 2, top + L.h / 2, w, 2, p.dim);
                continue;
            }
            for (uint32_t fi = L.fragBegin; fi < L.fragEnd; fi++) {
                const Frag& f = m_frags[fi];
                std::string s = m_doc.text.substr(f.start, f.end - f.start);
                if (s.empty()) continue;
                TTF_Font* fnt = fontFor(s, f.size, f.fs, m_cfg.face);
                if (!fnt) continue;
                SDL_Color c = f.link >= 0 ? p.accent : p.fg;
                SDL_Surface* surf = TTF_RenderUTF8_Blended(fnt, s.c_str(), c);
                if (!surf) continue;
                textGamma(surf);
                SDL_Texture* t = SDL_CreateTextureFromSurface(g_ren, surf);
                SDL_Rect dst = {m_margin + f.x, top + f.y, surf->w, surf->h};
                SDL_RenderCopy(g_ren, t, nullptr, &dst);
                SDL_DestroyTexture(t);
                SDL_FreeSurface(surf);
            }
        }
        SDL_SetRenderTarget(g_ren, prev);
    }

    SDL_Texture* pageTex(int page) {
        for (int s = 0; s < 2; s++) if (m_texPage[s] == page) return m_tex[s];
        // overwrite the slot that isn't showing the other page we still need
        int keep = (page == m_page && m_animDir) ? m_animFrom : m_page;
        int slot = m_texPage[0] == keep ? 1 : 0;
        renderPage(page, slot);
        return m_tex[slot];
    }

    // ------------------------------------------------------------ navigation

    void goPage(int p, bool animate = true) {
        p = SDL_clamp(p, 0, (int)m_pages.size() - 1);
        if (p == m_page) return;
        m_animDir = animate && abs(p - m_page) == 1 ? (p > m_page ? 1 : -1) : 0;
        m_animFrom = m_page;
        m_animStart = g_ticks;
        m_page = p;
        m_offset = m_lines[m_pages[p]].start;
        m_started = true;
        save();
    }
    int m_animFrom = 0;

    void goOffset(uint32_t off) {
        int p = pageOf(off);
        m_animDir = 0;
        m_page = p;
        m_offset = off;
        m_started = true;
        save();
    }

    int chapterIndex() const {
        int c = -1;
        uint32_t best = 0;
        uint32_t here = m_lines[m_pages[m_page]].start;
        for (int i = 0; i < (int)m_doc.toc.size(); i++)
            if (m_doc.toc[i].offset <= here && (c < 0 || m_doc.toc[i].offset >= best)) { c = i; best = m_doc.toc[i].offset; }
        return c;
    }

    std::string chapterTitle() const {
        int c = chapterIndex();
        return c >= 0 ? m_doc.toc[c].title : m_doc.title;
    }

    // pages left in the current chapter
    int chapterPagesLeft() const {
        int c = chapterIndex();
        int end = (int)m_pages.size();
        if (c >= 0) {
            for (int i = 0; i < (int)m_doc.toc.size(); i++)
                if (m_tocPage[i] > m_page && m_tocPage[i] < end) end = m_tocPage[i];
        }
        return end - m_page - 1;
    }

    void followLink(int li) {
        const Link& l = m_doc.links[li];
        if (l.external) { toast(l.target, 4000); return; }
        uint32_t off;
        if (!m_doc.resolve(l.target, off)) { toast("Link target not found"); return; }
        if (l.noteref) {
            m_noteText = m_doc.noteText(off);
            m_noteTarget = off;
            m_note = !m_noteText.empty();
            if (m_note) return;
        }
        m_back.push_back(m_lines[m_pages[m_page]].start);
        goOffset(off);
    }

    // Finds a link fragment near (x, y) on the current page.
    int linkAt(int x, int y) const {
        int first, n = pageLines(m_page, first);
        int best = -1;
        float bestD = 1e9f;
        for (int li = first; li < first + n; li++) {
            const Line& L = m_lines[li];
            for (uint32_t fi = L.fragBegin; fi < L.fragEnd; fi++) {
                const Frag& f = m_frags[fi];
                if (f.link < 0) continue;
                int fx = m_margin + f.x, fy = TOP + L.y + f.y;
                int fw = std::max(1, textWidth(m_doc.text.substr(f.start, f.end - f.start), f.size, f.fs, m_cfg.face));
                int fh = fontHeight(f.size, f.fs, m_cfg.face);
                // generous hit box: footnote markers are tiny
                float dx = x < fx ? fx - x : x > fx + fw ? x - fx - fw : 0;
                float dy = y < fy ? fy - y : y > fy + fh ? y - fy - fh : 0;
                float d = sqrtf(dx * dx + dy * dy);
                float slop = m_doc.links[f.link].noteref ? 34.0f : 14.0f;
                if (d <= slop && d < bestD) { bestD = d; best = f.link; }
            }
        }
        return best;
    }

    // ------------------------------------------------------------ reading view

    SDL_Rect topBar() const { return {0, 0, g_W, 84}; }
    SDL_Rect bottomBar() const { return {0, g_H - 150, g_W, 150}; }

    void drawRead() {
        const ReadPalette& p = readPalette();
        if (m_layW == 0) layout();
        bool overlay = m_settings || m_toc || m_note;
        if (g_in.toggleUi && !g_in.consumed && !overlay) m_toolbar = !m_toolbar;  // Y
        bool live = !g_in.consumed && !overlay;

        // pinch to change the font size
        if (live && g_in.pinch) {
            m_pinch *= g_in.pinchScale;
            g_in.consumed = true;
        } else if (m_pinch != 1.0f && !g_in.down) {
            int fsz = SDL_clamp((int)lroundf(m_cfg.fontSize * m_pinch / 2) * 2, 16, 72);
            m_pinch = 1.0f;
            if (fsz != m_cfg.fontSize) {
                cfgSetInt("books.font_size", fsz);
                toast("Font size " + std::to_string(fsz), 1200);
            }
        }

        // page
        float t = m_animDir ? SDL_clamp((g_ticks - m_animStart) / 220.0f, 0.0f, 1.0f) : 1.0f;
        if (t >= 1.0f) m_animDir = 0;
        SDL_Rect src = {0, 0, g_W, g_H};
        if (m_animDir) {
            float e = 1 - (1 - t) * (1 - t) * (1 - t);
            int off = (int)(g_W * e);
            SDL_Texture* from = pageTex(m_animFrom);
            SDL_Rect a = {-m_animDir * off, 0, g_W, g_H};
            SDL_RenderCopy(g_ren, from, &src, &a);
            SDL_Texture* to = pageTex(m_page);
            SDL_Rect b = {m_animDir * (g_W - off), 0, g_W, g_H};
            SDL_RenderCopy(g_ren, to, &src, &b);
        } else {
            SDL_Texture* tex = pageTex(m_page);
            SDL_Rect dst = {0, 0, g_W, g_H};
            SDL_RenderCopy(g_ren, tex, &src, &dst);
            // prepare the next page in the spare slot
        }
        readerStatusBar(16, chapterTitle());
        char buf[96];
        int left = chapterPagesLeft();
        snprintf(buf, sizeof(buf), "%d / %d%s", m_page + 1, (int)m_pages.size(),
                 left > 0 ? ("   \xC2\xB7   " + std::to_string(left) + (left == 1 ? " page left in chapter" : " pages left in chapter")).c_str() : "");
        drawText(buf, g_W / 2, g_H - 38, 18, p.dim, 1);

        // "Back to page N" after a jump: goes away once you read on a few pages from where you landed
        if (m_back.size() != m_backSeen) { m_backSeen = m_back.size(); m_backAnchor = m_page; }
        if (!m_back.empty() && abs(m_page - m_backAnchor) > 2) { m_back.clear(); m_backSeen = 0; }
        bool showChip = !m_back.empty() && !overlay && !m_toolbar;
        SDL_Rect chipR = {24, g_H - 120, 280, 56};

        // gestures on the page
        if (live) {
            bool inBars = m_toolbar && (inRect(g_in.sx, g_in.sy, topBar()) || inRect(g_in.sx, g_in.sy, bottomBar()));
            if (showChip && inRect(g_in.sx, g_in.sy, chipR)) inBars = true;  // the chip handles its own taps
            if (g_in.back) { if (m_toolbar) m_toolbar = false; else { m_mode = HOME; m_tocCentered = false; } }
            dpadForMedia();  // the d-pad's left/right turn pages too (see ui/input.cpp)
            if (g_in.seekX) {
                goPage(m_page + g_in.seekX);
                m_toolbar = false;
            } else if (g_in.swipeX && !inBars) {
                goPage(m_page + (g_in.swipeX < 0 ? 1 : -1));
                m_toolbar = false;
            } else if (g_in.tap && !inBars) {
                int link = linkAt(g_in.x, g_in.y);
                if (link >= 0) followLink(link);
                else {
                    float fx = g_in.x / (float)g_W;
                    if (fx < 0.28f) { goPage(m_page - 1); m_toolbar = false; }
                    else if (fx > 0.72f) { goPage(m_page + 1); m_toolbar = false; }
                    else m_toolbar = !m_toolbar;
                }
                g_in.consumed = true;
            }
        }

        // back chip after following a link
        if (showChip) {
            fillRound(chipR, 28, withAlpha(p.sel, 235));
            drawIcon(IC_BACK, chipR.x + 32.0f, chipR.y + 28.0f, 26, p.accent);
            drawText("Back to page " + std::to_string(pageOf(m_back.back()) + 1), chipR.x + 54, chipR.y + 15, 22, p.fg, 0, 210);
            if (tapped(chipR)) {
                uint32_t off = m_back.back();
                m_back.pop_back();
                m_backSeen = m_back.size();
                goOffset(off);
                m_backAnchor = m_page;
            }
        }

        if (m_toolbar && !overlay) drawToolbar();
        if (m_note) drawNote();
        if (m_settings) drawSettings();
        if (m_toc) drawTocOverlay();
    }

    void drawToolbar() {
        const ReadPalette& p = readPalette();
        SDL_Rect tb = topBar(), bb = bottomBar();
        fillRect(tb, withAlpha(p.panel, 245));
        fillRect(tb.x, tb.y + tb.h - 1, tb.w, 1, p.sel);
        if (iconButton(48, 42, 30, IC_BACK, p.fg, SDL_Color{0, 0, 0, 0})) { m_mode = HOME; m_toolbar = false; m_tocCentered = false; }
        drawText(m_doc.title, 92, 26, 26, p.fg, 0, g_W - 92 - 260, FS_BOLD);
        if (iconButton(g_W - 196.0f, 42, 30, IC_LIST, p.fg, SDL_Color{0, 0, 0, 0})) { m_toc = true; m_tocCentered = false; }
        if (iconButton(g_W - 124.0f, 42, 30, IC_TEXT, p.fg, SDL_Color{0, 0, 0, 0})) m_settings = true;
        if (iconButton(g_W - 52.0f, 42, 30, IC_MOON, m_night ? p.accent : p.fg, SDL_Color{0, 0, 0, 0}))
            cfgSetInt("books.color", m_night ? 0 : 2);

        fillRect(bb, withAlpha(p.panel, 245));
        fillRect(bb.x, bb.y, bb.w, 1, p.sel);
        int n = (int)m_pages.size();
        if (!m_sliding) m_slider = n > 1 ? m_page / (float)(n - 1) : 0;
        SDL_Rect sr = {100, bb.y + 92, g_W - 200, 8};
        bool released = false;
        bool moving = slider(sr, m_slider, p.accent, p.sel, &released);
        // dragging lasts until the finger lifts, even while it holds still (the value
        // stops changing, e.g. pinned at page 1): only then is the slider reset to the page
        if (moving) m_sliding = !released;
        if (!g_in.down) m_sliding = false;
        int target = (int)lroundf(m_slider * (n - 1));
        std::string label = m_sliding ? "Page " + std::to_string(target + 1) + " of " + std::to_string(n)
                                      : chapterTitle();
        drawText(label, g_W / 2, bb.y + 24, 24, p.fg, 1, g_W - 260);
        if (!m_sliding) {
            int pct = n > 1 ? m_page * 100 / (n - 1) : 100;
            drawText(std::to_string(pct) + "%", g_W / 2, bb.y + 56, 18, p.dim, 1);
        }
        if (released && target != m_page) {
            m_back.push_back(m_lines[m_pages[m_page]].start);
            goPage(target, false);
        }
        if (iconButton(48, bb.y + 96.0f, 30, IC_PREV, p.fg, SDL_Color{0, 0, 0, 0})) jumpChapter(-1);
        if (iconButton(g_W - 48.0f, bb.y + 96.0f, 30, IC_NEXT, p.fg, SDL_Color{0, 0, 0, 0})) jumpChapter(1);
        // swallow taps on the bars
        if (g_in.tap && (inRect(g_in.x, g_in.y, tb) || inRect(g_in.x, g_in.y, bb))) g_in.consumed = true;
    }

    void jumpChapter(int dir) {
        if (m_doc.toc.empty()) { goPage(m_page + dir * 10, false); return; }
        int c = chapterIndex();
        int startPage = c >= 0 ? m_tocPage[c] : 0;
        if (dir < 0 && c >= 0 && m_page > startPage) { goPage(startPage, false); return; }
        // next/previous TOC entry on a different page
        int best = -1;
        for (int i = 0; i < (int)m_doc.toc.size(); i++) {
            int pg = m_tocPage[i];
            if (dir > 0 && pg > m_page && (best < 0 || pg < m_tocPage[best])) best = i;
            if (dir < 0 && pg < m_page && (best < 0 || pg > m_tocPage[best])) best = i;
        }
        if (best >= 0) goPage(m_tocPage[best], false);
    }

    void drawNote() {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        const ReadPalette& p = readPalette();
        fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 150});
        int w = std::min(g_W - 48, 900);
        auto lines = wrapText(m_noteText, 26, w - 64);
        int lh = fontHeight(26) * 130 / 100;
        int shown = std::min((int)lines.size(), (g_H / 2) / lh);
        int h = 70 + shown * lh + 100;
        SDL_Rect r = {(g_W - w) / 2, g_H - h - 24, w, h};
        fillRound(r, 22, p.panel);
        strokeRound(r, 22, 2, p.sel);
        drawText("Note", r.x + 32, r.y + 22, 20, p.accent, 0, 0, FS_BOLD);
        for (int i = 0; i < shown; i++) drawText(lines[i] + (i == shown - 1 && shown < (int)lines.size() ? "\xE2\x80\xA6" : ""),
                                                 r.x + 32, r.y + 58 + i * lh, 26, p.fg, 0, w - 64);
        SDL_Rect go = {r.x + 32, r.y + h - 84, 240, 60}, close = {r.x + w - 32 - 200, r.y + h - 84, 200, 60};
        if (button(go, "Go to note", IC_RIGHT)) {
            m_back.push_back(m_lines[m_pages[m_page]].start);
            goOffset(m_noteTarget);
            m_note = false;
        }
        if (button(close, "Close", IC_NONE, BTN_ACCENT)) m_note = false;
        if (!g_in.consumed && (g_in.tap && !inRect(g_in.x, g_in.y, r))) { m_note = false; g_in.consumed = true; }
        if (g_in.back) m_note = false;
        g_in.consumed = true;
    }

    void drawSettings() {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        const ReadPalette& p = readPalette();
        fillRect(0, 0, g_W, g_H, SDL_Color{0, 0, 0, 120});
        int w = std::min(g_W - 32, 820), rowH = 76;
        int rows = 9;
        int h = 80 + rows * rowH + 24;
        SDL_Rect r = {(g_W - w) / 2, g_H - h - 16, w, h};
        fillRound(r, 24, p.panel);
        strokeRound(r, 24, 2, p.sel);
        drawText("Reading settings", r.x + 32, r.y + 24, 28, p.fg, 0, 0, FS_BOLD);
        if (iconButton(r.x + w - 50.0f, r.y + 44.0f, 28, IC_CLOSE, p.fg, SDL_Color{0, 0, 0, 0})) m_settings = false;
        int x = r.x + 32, rw = w - 64, y = r.y + 84;
        int d;
        if ((d = stepper({x, y, rw, rowH}, "Font", fontFaceName(m_cfg.face))))
            cfgSetInt("books.font", (m_cfg.face + d + fontFaceCount()) % fontFaceCount());
        y += rowH;
        if ((d = stepper({x, y, rw, rowH}, "Text size", std::to_string(m_cfg.fontSize))))
            cfgSetInt("books.font_size", SDL_clamp(m_cfg.fontSize + d * 2, 16, 72));
        y += rowH;
        if ((d = stepper({x, y, rw, rowH}, "Line spacing", std::to_string(m_cfg.spacing) + "%")))
            cfgSetInt("books.line_spacing", SDL_clamp(m_cfg.spacing + d * 10, 100, 220));
        y += rowH;
        if ((d = stepper({x, y, rw, rowH}, "Margins", std::to_string(m_cfg.margin))))
            cfgSetInt("books.margin", SDL_clamp(m_cfg.margin + d * 8, 0, 200));
        y += rowH;
        drawText("Paragraphs", x, y + 24, 24, p.fg);
        if (chip({x + rw - 330, y + 12, 160, 52}, "Indented", m_cfg.indent)) cfgSetBool("books.indent", true);
        if (chip({x + rw - 160, y + 12, 160, 52}, "Spaced", !m_cfg.indent)) cfgSetBool("books.indent", false);
        y += rowH;
        bool j = m_cfg.justify;
        drawText("Justify text", x, y + 24, 24, p.fg);
        if (toggle({x, y, rw, rowH}, j)) cfgSetBool("books.justify", j);
        y += rowH;
        drawText("Text color", x, y + 24, 24, p.fg);
        static const char* colors[] = {"White", "Warm", "Red"};
        for (int i = 0; i < 3; i++)
            if (chip({x + rw - 3 * 130 + i * 130 + 10, y + 12, 120, 52}, colors[i], readColor() == i)) cfgSetInt("books.color", i);
        y += rowH;
        drawText("Brightness", x, y + 24, 24, p.fg);
        float b = (brightness() - 10) / 90.0f;
        if (slider({x + 260, y + 34, rw - 280, 8}, b, p.accent, p.sel)) cfgSetInt("books.brightness", 10 + (int)lroundf(b * 90));
        y += rowH;
        drawText("Rotation", x, y + 24, 24, p.fg);
        int rot = rotation();
        if (chip({x + rw - 330, y + 12, 160, 52}, "Turned left", rot == 90)) cfgSetInt("books.rotation", 90);
        if (chip({x + rw - 160, y + 12, 160, 52}, "Turned right", rot == 270)) cfgSetInt("books.rotation", 270);
        if (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r)) m_settings = false;
        if (g_in.back) m_settings = false;
        g_in.consumed = true;
    }

    // ------------------------------------------------------------ contents

    void drawTocList(const SDL_Rect& area, bool closeOnPick) {
        const ReadPalette& p = readPalette();
        int rowH = 64, n = (int)m_doc.toc.size();
        float contentH = (float)n * rowH;
        int cur = chapterIndex();
        if (!m_tocCentered) {
            m_tocScroll.scrollTo(cur * (float)rowH - area.h / 2 + rowH / 2, area, contentH);
            m_tocCentered = true;
        }
        m_tocScroll.update(area, contentH);
        pushClip(area);
        int first = std::max(0, (int)(m_tocScroll.pos / rowH));
        for (int i = first; i < n && i * rowH - m_tocScroll.pos < area.h; i++) {
            SDL_Rect row = {area.x, area.y + i * rowH - (int)m_tocScroll.pos, area.w, rowH};
            bool isCur = i == cur;
            if (pressing(row)) fillRound(row, 12, p.sel);
            else if (isCur) fillRound(row, 12, withAlpha(p.accent, 40));
            if (isCur) fillRound({row.x + 4, row.y + 14, 5, rowH - 28}, 2, p.accent);
            int indent = 24 + m_doc.toc[i].depth * 32;
            std::string pg = std::to_string(m_tocPage[i] + 1);
            int pw = drawText(pg, row.x + row.w - 20, row.y + 20, 20, p.dim, 2);
            drawText(m_doc.toc[i].title, row.x + indent, row.y + 17, m_doc.toc[i].depth ? 22 : 24,
                     isCur ? p.accent : p.fg, 0, row.w - indent - pw - 44, isCur ? FS_BOLD : 0);
            if (tapped(row)) {
                m_back.clear();
                goOffset(m_doc.toc[i].offset);
                m_mode = READ;
                if (closeOnPick) m_toc = false;
            }
        }
        popClip();
        drawScrollbar(area, contentH, m_tocScroll.pos);
        if (n == 0) drawText("This book has no table of contents.", area.x + area.w / 2, area.y + 40, 22, p.dim, 1);
    }

    void drawTocOverlay() {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        const ReadPalette& p = readPalette();
        fillRect(0, 0, g_W, g_H, p.bg);
        if (iconButton(48, 44, 30, IC_CLOSE, p.fg, SDL_Color{0, 0, 0, 0}) || g_in.back) m_toc = false;
        drawText("Contents", 92, 24, 30, p.fg, 0, 0, FS_BOLD);
        drawTocList({24, 96, g_W - 48, g_H - 110}, true);
        g_in.consumed = true;
    }

    void drawHome() {
        const ReadPalette& p = readPalette();
        if (m_layW == 0) layout();
        // the settings sheet on top owns the input
        bool sheet = m_settings, c0 = g_in.consumed;
        if (sheet) g_in.consumed = true;
        if (iconButton(48, 44, 30, IC_BACK, p.fg, SDL_Color{0, 0, 0, 0}) || (!sheet && g_in.back)) m_close = true;
        drawText("Library", 86, 30, 22, p.dim);
        bool portrait = g_W < g_H;
        int coverW = portrait ? 200 : 250, coverH = coverW * 3 / 2;
        SDL_Rect cover = {portrait ? 32 : 60, 96, coverW, coverH};
        SDL_Texture* ct = bookCover(m_doc.path);
        if (ct) {
            // the thumbnail is shared with the library, so tint with a colour mod and reset it
            int c = readColor();
            if (c == 2) SDL_SetTextureColorMod(ct, 255, 30, 10);
            else if (c == 1) SDL_SetTextureColorMod(ct, 255, 200, 140);
            drawTexFit(ct, cover, true, 255, 14);
            SDL_SetTextureColorMod(ct, 255, 255, 255);
        } else {
            fillRound(cover, 14, p.sel);
            drawIcon(IC_BOOK, cover.x + coverW / 2.0f, cover.y + coverH / 2.0f, 72, p.dim);
        }
        int tx = portrait ? cover.x + coverW + 28 : cover.x, ty = portrait ? cover.y : cover.y + coverH + 20;
        int tw = portrait ? g_W - tx - 32 : 340;
        int h = drawWrapped(m_doc.title, tx, ty, 28, p.fg, tw, 2, 0, FS_BOLD);
        ty += h + 2;
        if (!m_doc.author.empty()) { drawText(m_doc.author, tx, ty, 22, p.dim, 0, tw); ty += 32; }
        int n = (int)m_pages.size();
        std::string prog = m_started ? std::to_string(n > 1 ? m_page * 100 / (n - 1) : 100) + "% read  \xC2\xB7  page " +
                                           std::to_string(m_page + 1) + " of " + std::to_string(n)
                                     : std::to_string(n) + " pages";
        drawText(prog, tx, ty + 4, 20, p.dim, 0, tw);
        SDL_Rect go = {tx, portrait ? ty + 44 : g_H - 96, std::min(tw, 340), 64};
        if (button(go, m_started ? "Continue reading" : "Start reading", IC_PLAY, BTN_ACCENT)) {
            if (!m_started && !m_doc.toc.empty()) goOffset(0);
            m_mode = READ;
            m_started = true;
            save();
        }
        int listTop = portrait ? cover.y + coverH + 96 : 150;
        SDL_Rect list = portrait ? SDL_Rect{24, listTop, g_W - 48, g_H - listTop - 16}
                                 : SDL_Rect{460, listTop, g_W - 460 - 24, g_H - listTop - 16};
        drawText("Contents", list.x + 12, list.y - 52, 28, p.fg, 0, 0, FS_BOLD);
        drawTocList(list, false);
        if (iconButton(g_W - 52.0f, 44, 30, IC_TEXT, p.fg, p.sel)) m_settings = true;
        if (sheet) {
            g_in.consumed = c0;
            drawSettings();
        }
    }
};

BookReader* openTextReader(const std::string& path) { return new TextReader(path); }
