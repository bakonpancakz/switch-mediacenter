// Comic reader for CBZ/CBR/CB7/CBT archives and PDFs. A worker thread pulls
// pages out of the archive (or renders them with MuPDF) and decodes/downscales
// them; everything is touch driven: tap the sides to turn, pinch or double-tap
// to zoom, drag to pan. Zoomed-in PDF pages get a sharp re-render of the
// visible area once the view settles.
#include "reader.h"
#include "pdf.h"
#include "core/thread.h"
#include <SDL2/SDL_image.h>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cmath>
#include <map>

enum { FIT_PAGE, FIT_WIDTH, FIT_HEIGHT, FIT_COUNT };
static const char* FIT_NAMES[FIT_COUNT] = {"Fit page", "Fit width", "Fit height"};

static bool isPageName(const std::string& name) {
    if (!isImageExt(ext(name))) return false;
    std::string b = baseName(name);
    if (!b.empty() && b[0] == '.') return false;
    return name.find("__MACOSX/") == std::string::npos;
}

static archive* openArchive(const std::string& path) {
    archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), 1 << 16) != ARCHIVE_OK) {
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

struct Page { std::string name; int archiveIndex; };
struct Decoded { SDL_Surface* surf; int ow, oh; int gen; float uw = 0; };  // uw: exact page width (PDF points)
struct PageTex { SDL_Texture* tex; int ow, oh; float rs, uw; };  // rs: texture pixels per page unit
// A sharp render of part of a zoomed PDF page, in pixels of the page drawn at `scale`.
struct TileReq { int page = -1; float scale = 0; int x = 0, y = 0, w = 0, h = 0; int seq = 0; };

class ComicReader : public BookReader {
public:
    explicit ComicReader(const std::string& path) : m_path(path) {
        if (ext(path) == "pdf") openPdf();
        else openComic();
        if (!m_err.empty()) return;
        int64_t pos = 0, total = 0;
        if (posGet(m_path, pos, total)) m_page = SDL_clamp((int)pos, 0, (int)m_pages.size() - 1);
        SDL_RendererInfo info;
        SDL_GetRendererInfo(g_ren, &info);
        m_maxTex = info.max_texture_width > 0 ? std::min(info.max_texture_width, info.max_texture_height) : 4096;
        m_mtx = SDL_CreateMutex();
        m_cond = SDL_CreateCond();
        m_thread = SDL_CreateThreadWithStackSize(workerMain, "comic", 1024 * 1024, this);
        m_fit = cfgInt("books.fit", FIT_PAGE) % FIT_COUNT;
        m_red = (readColor() == 2) && cfgBool("books.tint", true);
        request();
    }

    void openPdf() {
        std::string err;
        m_pdf = pdfOpen(m_path, err);
        if (!m_pdf) { m_err = "Could not open PDF: " + err; return; }
        int n = pdfPageCount(m_pdf);
        for (int i = 0; i < n; i++) m_pages.push_back({"", i});
    }

    void openComic() {
        archive* a = openArchive(m_path);
        if (!a) { m_err = "Could not open archive. If it was just synced, it may still be downloading."; return; }
        archive_entry* e;
        int idx = 0;
        while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
            if (archive_entry_filetype(e) == AE_IFREG) {
                const char* n = archive_entry_pathname(e);
                if (n && isPageName(n)) m_pages.push_back({n, idx});
            }
            idx++;
            archive_read_data_skip(a);
        }
        archive_read_free(a);
        if (m_pages.empty()) { m_err = "No images found in archive"; return; }
        std::sort(m_pages.begin(), m_pages.end(), [](const Page& x, const Page& y) { return naturalLess(x.name, y.name); });
    }

    ~ComicReader() override {
        if (m_thread) {
            SDL_LockMutex(m_mtx);
            m_quit = true;
            pdfAbort(m_pdf);  // don't wait for a slow page to finish
            SDL_CondSignal(m_cond);
            SDL_UnlockMutex(m_mtx);
            SDL_WaitThread(m_thread, nullptr);
        }
        flush();
        if (m_cond) SDL_DestroyCond(m_cond);
        if (m_mtx) SDL_DestroyMutex(m_mtx);
        pdfClose(m_pdf);  // after the worker is gone: it renders with this
    }

    bool ok() const override { return m_err.empty(); }
    std::string error() const override { return m_err; }
    bool wantsClose() const override { return m_close; }
    int rotation() const override { return readRotation(); }
    int brightness() const override { return SDL_clamp(cfgInt("books.brightness", 100), 10, 100); }
    void save() override { posSet(m_path, m_page, (int)m_pages.size()); }

    void update() override {
        collect();
        int fit = cfgInt("books.fit", FIT_PAGE) % FIT_COUNT;
        bool red = (readColor() == 2) && cfgBool("books.tint", true);
        if (fit != m_fit || red != m_red || m_canvasW != g_W || m_canvasH != g_H) {
            m_fit = fit;
            m_red = red;
            flush();
            m_zoom = 1;
            m_resetPan = 1;
            request();
        }
    }

    void draw() override {
        const ReadPalette& p = readPalette();
        bool rtl = cfgBool("books.rtl", false);
        const PageTex* pt = current();
        float dispW = 0, dispH = 0;
        if (pt) displaySize(*pt, dispW, dispH);
        float maxX = std::max(0.0f, dispW - g_W), maxY = std::max(0.0f, dispH - g_H);
        if (pt && m_resetPan) {
            m_panY = m_resetPan > 0 ? 0 : maxY;
            m_panX = rtl ? maxX : 0;
            m_resetPan = 0;
        }
        if (g_in.toggleUi && !g_in.consumed) m_toolbar = !m_toolbar;  // Y
        bool live = !g_in.consumed && !m_toolbar;

        if (live) {
            if (g_in.back) m_close = true;
            if (g_in.pinchScale != 1.0f && pt) zoomAt(m_zoom * g_in.pinchScale, g_in.px, g_in.py, dispW, dispH);
            if (g_in.pinch && pt) { m_panX -= g_in.pdx; m_panY -= g_in.pdy; }
            bool pannable = maxX > 0 || maxY > 0;
            if (g_in.down && !g_in.multi && g_in.moved && pannable) { m_panX -= g_in.dx; m_panY -= g_in.dy; }
            // Edges turn pages on every tap, at once, so quick taps skim through
            // pages. Double-tap zoom and the toolbar belong to the middle third.
            auto middle = [](int x) { float fx = x / (float)g_W; return fx > 0.3f && fx < 0.7f; };
            if (g_in.tap && !middle(g_in.x)) {
                advance(((g_in.x >= 0.7f * g_W) != rtl) ? 1 : -1, maxY);
            } else if (g_in.doubleTap && pt && middle(g_in.tapX)) {
                if (m_zoom > 1.01f) { m_zoom = 1; m_resetPan = 1; }
                else zoomAt(2.5f, (float)g_in.tapX, (float)g_in.tapY, dispW, dispH);
            } else if (g_in.singleTap && middle(g_in.tapX)) {
                m_toolbar = true;
            }
            if (g_in.swipeX && (!pannable || g_in.buttonsDown)) turn(g_in.swipeX < 0 ? (rtl ? -1 : 1) : (rtl ? 1 : -1), 1);
            // the d-pad's left/right turn the page the way it reads, ZL/ZR zoom (see ui/input.cpp)
            dpadForMedia();
            if (g_in.seekX) turn(g_in.seekX * (rtl ? -1 : 1), 1);
            if (g_in.scroll) m_panY += g_in.scroll;
        }

        pt = current();
        if (pt) displaySize(*pt, dispW, dispH);
        maxX = std::max(0.0f, dispW - g_W);
        maxY = std::max(0.0f, dispH - g_H);
        m_panX = SDL_clamp(m_panX, 0.0f, maxX);
        m_panY = SDL_clamp(m_panY, 0.0f, maxY);

        if (pt) {
            SDL_Rect dst;
            dst.w = (int)dispW;
            dst.h = (int)dispH;
            dst.x = dispW <= g_W ? (g_W - dst.w) / 2 : -(int)m_panX;
            dst.y = dispH <= g_H ? (g_H - dst.h) / 2 : -(int)m_panY;
            SDL_RenderCopy(g_ren, pt->tex, nullptr, &dst);
            if (m_pdf) sharpen(*pt, dst, dispW, dispH);
        } else {
            spinner(g_W / 2.0f, g_H / 2.0f, 28, p.dim);
        }
        if (g_ticks < m_hudUntil && !m_toolbar) {
            std::string s = std::to_string(m_page + 1) + " / " + std::to_string(m_pages.size());
            int w = textWidth(s, 22) + 32;
            fillRound({g_W / 2 - w / 2, g_H - 56, w, 40}, 20, withAlpha(p.panel, 220));
            drawText(s, g_W / 2, g_H - 48, 22, p.fg, 1);
        }
        if (m_toolbar) drawToolbar(rtl);
    }

private:
    std::string m_path, m_err;
    std::vector<Page> m_pages;
    int m_page = 0;
    float m_zoom = 1.0f, m_panX = 0, m_panY = 0;
    int m_resetPan = 1;
    int m_maxTex = 4096;
    uint32_t m_hudUntil = 0;
    std::map<int, PageTex> m_tex;
    bool m_close = false, m_toolbar = false, m_sliding = false;
    float m_slider = 0;

    SDL_mutex* m_mtx = nullptr;
    SDL_cond* m_cond = nullptr;
    SDL_Thread* m_thread = nullptr;
    bool m_quit = false;
    std::vector<int> m_want;
    std::map<int, Decoded> m_ready;
    int m_gen = 0;
    int m_canvasW = 0, m_canvasH = 0, m_fit = FIT_PAGE;
    bool m_red = false;

    // PDF: the document (used only by the worker once it runs) and sharp zoom tiles
    PdfDoc* m_pdf = nullptr;
    TileReq m_tileWant;                 // guarded by m_mtx; page < 0 = none
    SDL_Surface* m_tileSurf = nullptr;  // finished by the worker, guarded by m_mtx
    TileReq m_tileSurfReq;
    SDL_Texture* m_tileTex = nullptr;   // main thread
    TileReq m_tile, m_tilePending;
    int m_tileSeq = 0;
    int m_renderingPage = -1;      // PDF page the worker is rendering (m_mtx)
    bool m_renderingTile = false;  // ... or a tile (m_mtx)
    uint32_t m_viewChangedAt = 0;
    float m_lastZoom = 0, m_lastPanX = -1, m_lastPanY = -1;

    void drawToolbar(bool rtl) {
        InputLayer layer;  // on top: gets the input, the screen under it doesn't
        const ReadPalette& p = readPalette();
        SDL_Rect tb = {0, 0, g_W, 84}, bb = {0, g_H - 140, g_W, 140};
        fillRect(tb, withAlpha(p.panel, 240));
        if (iconButton(48, 42, 30, IC_BACK, p.fg, SDL_Color{0, 0, 0, 0})) m_close = true;
        drawText(stem(m_path), 92, 26, 24, p.fg, 0, g_W - 92 - 540, FS_BOLD);
        if (chip({g_W - 520, 18, 170, 48}, FIT_NAMES[m_fit], false)) cfgSetInt("books.fit", (m_fit + 1) % FIT_COUNT);
        if (chip({g_W - 340, 18, 200, 48}, rtl ? "Right to left" : "Left to right", rtl)) cfgSetBool("books.rtl", !rtl);
        bool night = (readColor() == 2);
        if (iconButton(g_W - 96.0f, 42, 28, IC_MOON, night ? p.accent : p.fg, SDL_Color{0, 0, 0, 0})) cfgSetInt("books.color", night ? 0 : 2);
        if (iconButton(g_W - 36.0f, 42, 24, IC_CLOSE, p.fg, SDL_Color{0, 0, 0, 0})) m_toolbar = false;

        fillRect(bb, withAlpha(p.panel, 240));
        int n = (int)m_pages.size();
        if (!m_sliding) m_slider = n > 1 ? m_page / (float)(n - 1) : 0;
        if (rtl && !m_sliding) m_slider = 1 - m_slider;
        bool released = false;
        bool moving = slider({100, bb.y + 90, g_W - 200, 8}, m_slider, p.accent, p.sel, &released);
        // dragging lasts until the finger lifts, even while it holds still (the value
        // stops changing, e.g. pinned at page 1): only then is the slider reset to the page
        if (moving) m_sliding = !released;
        if (!g_in.down) m_sliding = false;
        float v = rtl ? 1 - m_slider : m_slider;
        int target = (int)lroundf(v * (n - 1));
        drawText("Page " + std::to_string((m_sliding ? target : m_page) + 1) + " of " + std::to_string(n), g_W / 2, bb.y + 28, 24, p.fg, 1);
        if (released) turn(target - m_page, 1);
        if (iconButton(48, bb.y + 94.0f, 28, rtl ? IC_NEXT : IC_PREV, p.fg, SDL_Color{0, 0, 0, 0})) turn(-1, 1);
        if (iconButton(g_W - 48.0f, bb.y + 94.0f, 28, rtl ? IC_PREV : IC_NEXT, p.fg, SDL_Color{0, 0, 0, 0})) turn(1, 1);
        if (g_in.back) m_toolbar = false;
        if (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, tb) && !inRect(g_in.x, g_in.y, bb)) {
            // the tap that closes the toolbar is used up, or its delayed single tap reopens it
            m_toolbar = false;
            consumeInput();
        }
        g_in.consumed = true;
    }

    const PageTex* current() const {
        auto it = m_tex.find(m_page);
        return it == m_tex.end() ? nullptr : &it->second;
    }

    static float fitScale(int ow, int oh, int W, int H, int fit) {
        switch (fit) {
            case FIT_WIDTH: return W / (float)ow;
            case FIT_HEIGHT: return H / (float)oh;
            default: return std::min(W / (float)ow, H / (float)oh);
        }
    }

    void displaySize(const PageTex& pt, float& w, float& h) const {
        float s = fitScale(pt.ow, pt.oh, g_W, g_H, m_fit) * m_zoom;
        w = pt.ow * s;
        h = pt.oh * s;
    }

    // Zoom so the image point under (px, py) stays put.
    void zoomAt(float nz, float px, float py, float dispW, float dispH) {
        nz = SDL_clamp(nz, 1.0f, 6.0f);
        if (dispW <= 0 || nz == m_zoom) return;
        float ox = dispW <= g_W ? (g_W - dispW) / 2 : -m_panX;
        float oy = dispH <= g_H ? (g_H - dispH) / 2 : -m_panY;
        float fx = (px - ox) / dispW, fy = (py - oy) / dispH;
        float k = nz / m_zoom;
        m_zoom = nz;
        float nw = dispW * k, nh = dispH * k;
        m_panX = fx * nw - px;
        m_panY = fy * nh - py;
    }

    void turn(int delta, int resetPan) {
        int np = SDL_clamp(m_page + delta, 0, (int)m_pages.size() - 1);
        m_hudUntil = g_ticks + 1500;
        if (np == m_page) return;
        m_page = np;
        m_resetPan = resetPan;
        save();
        request();
    }

    void advance(int dir, float maxY) {
        if (dir > 0 && m_panY < maxY - 1) { m_panY += g_H * 0.85f; return; }
        if (dir < 0 && m_panY > 1 && maxY > 0) { m_panY -= g_H * 0.85f; return; }
        turn(dir, dir > 0 ? 1 : -1);
    }

    void flush() {
        if (m_mtx) SDL_LockMutex(m_mtx);
        for (auto& kv : m_ready) SDL_FreeSurface(kv.second.surf);
        m_ready.clear();
        m_want.clear();
        if (m_tileSurf) SDL_FreeSurface(m_tileSurf);
        m_tileSurf = nullptr;
        m_tileWant = TileReq();
        m_gen++;
        if (m_mtx) SDL_UnlockMutex(m_mtx);
        for (auto& kv : m_tex) SDL_DestroyTexture(kv.second.tex);
        m_tex.clear();
        if (m_tileTex) SDL_DestroyTexture(m_tileTex);
        m_tileTex = nullptr;
        m_tile = m_tilePending = TileReq();
    }

    // Zoomed past the page texture's resolution: once the view has been still a
    // moment, render the visible area (plus a margin for small pans) at screen
    // resolution and draw it over the page.
    void sharpen(const PageTex& pt, const SDL_Rect& dst, float dispW, float dispH) {
        float scale = dispW / pt.uw;  // screen pixels per page unit right now
        if (m_zoom != m_lastZoom || m_panX != m_lastPanX || m_panY != m_lastPanY) {
            m_lastZoom = m_zoom;
            m_lastPanX = m_panX;
            m_lastPanY = m_panY;
            m_viewChangedAt = g_ticks;
        }
        auto same = [&](const TileReq& t) { return t.page == m_page && fabsf(t.scale - scale) <= scale * 0.002f; };
        if (m_tileTex && same(m_tile)) {
            SDL_Rect td = {dst.x + m_tile.x, dst.y + m_tile.y, m_tile.w, m_tile.h};
            SDL_RenderCopy(g_ren, m_tileTex, nullptr, &td);
        }
        if (scale <= pt.rs * 1.1f || g_in.down || g_ticks - m_viewChangedAt < 180) return;
        // visible part of the page, in pixels of the page at this scale
        int vx0 = std::max(0, -dst.x), vy0 = std::max(0, -dst.y);
        int vx1 = std::min((int)dispW, g_W - dst.x), vy1 = std::min((int)dispH, g_H - dst.y);
        if (vx1 <= vx0 || vy1 <= vy0) return;
        auto covers = [&](const TileReq& t) { return same(t) && t.x <= vx0 && t.y <= vy0 && t.x + t.w >= vx1 && t.y + t.h >= vy1; };
        if ((m_tileTex && covers(m_tile)) || covers(m_tilePending)) return;
        int m = g_W / 3;
        TileReq t;
        t.page = m_page;
        t.scale = scale;
        t.x = std::max(0, vx0 - m);
        t.y = std::max(0, vy0 - m);
        t.w = std::min((int)dispW, vx1 + m) - t.x;
        t.h = std::min((int)dispH, vy1 + m) - t.y;
        t.seq = ++m_tileSeq;
        m_tilePending = t;
        SDL_LockMutex(m_mtx);
        m_tileWant = t;
        if (m_renderingTile) pdfAbort(m_pdf);
        SDL_CondSignal(m_cond);
        SDL_UnlockMutex(m_mtx);
    }

    void request() {
        int n = (int)m_pages.size();
        for (auto it = m_tex.begin(); it != m_tex.end();) {
            if (it->first < m_page - 2 || it->first > m_page + 3) {
                SDL_DestroyTexture(it->second.tex);
                it = m_tex.erase(it);
            } else ++it;
        }
        SDL_LockMutex(m_mtx);
        m_want.clear();
        // PDF pages can take seconds each: only the neighbours, and stop a page
        // render that's no longer next to the one on screen
        static const std::vector<int> comicAhead = {0, 1, -1, 2, 3}, pdfAhead = {0, 1, -1};
        for (int off : m_pdf ? pdfAhead : comicAhead) {
            int i = m_page + off;
            if (i >= 0 && i < n && !m_tex.count(i) && !m_ready.count(i)) m_want.push_back(i);
        }
        if (m_pdf && m_renderingPage >= 0 && abs(m_renderingPage - m_page) > 1) pdfAbort(m_pdf);
        m_canvasW = g_W;
        m_canvasH = g_H;
        SDL_CondSignal(m_cond);
        SDL_UnlockMutex(m_mtx);
    }

    void collect() {
        std::map<int, Decoded> got;
        SDL_LockMutex(m_mtx);
        got.swap(m_ready);
        int gen = m_gen;
        SDL_Surface* tile = m_tileSurf;
        TileReq tileReq = m_tileSurfReq;
        m_tileSurf = nullptr;
        SDL_UnlockMutex(m_mtx);
        if (tile) {
            // only the latest request counts: an older tile is for a view we left
            if (tileReq.seq == m_tilePending.seq) {
                if (m_tileTex) SDL_DestroyTexture(m_tileTex);
                m_tileTex = SDL_CreateTextureFromSurface(g_ren, tile);
                m_tile = tileReq;
                m_tilePending = TileReq();
            }
            SDL_FreeSurface(tile);
        }
        for (auto& kv : got) {
            Decoded& dc = kv.second;
            if (dc.gen == gen && !m_tex.count(kv.first) && kv.first >= m_page - 2 && kv.first <= m_page + 3) {
                PageTex pt;
                pt.tex = SDL_CreateTextureFromSurface(g_ren, dc.surf);
                pt.ow = dc.ow;
                pt.oh = dc.oh;
                pt.uw = dc.uw > 0 ? dc.uw : (float)dc.ow;
                pt.rs = pt.uw > 0 ? dc.surf->w / pt.uw : 1;
                if (pt.tex) m_tex[kv.first] = pt;
            }
            SDL_FreeSurface(dc.surf);
        }
    }

    static void tintRed(SDL_Surface* s) {
        for (int y = 0; y < s->h; y++) {
            uint8_t* px = (uint8_t*)s->pixels + (size_t)y * s->pitch;
            for (int x = 0; x < s->w; x++, px += 4) {
                px[0] = (uint8_t)((px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8);
                px[1] = px[2] = 0;
            }
        }
    }

    static int workerMain(void* self) { return ((ComicReader*)self)->worker(); }

    int worker() {
        WorkerThread::spreadCores();
        archive* a = nullptr;
        int nextIndex = 0;
        std::vector<char> buf(1 << 16);
        for (;;) {
            SDL_LockMutex(m_mtx);
            while (!m_quit && m_want.empty() && m_tileWant.page < 0) SDL_CondWait(m_cond, m_mtx);
            if (m_quit) { SDL_UnlockMutex(m_mtx); break; }
            if (m_tileWant.page >= 0) {
                // a sharp tile for the page on screen goes first
                TileReq t = m_tileWant;
                m_tileWant = TileReq();
                int gen = m_gen;
                bool red = m_red;
                m_renderingTile = true;
                pdfClearAbort(m_pdf);  // under m_mtx: an abort is always meant for the current job
                SDL_UnlockMutex(m_mtx);
                SDL_Surface* s = pdfRenderRegion(m_pdf, t.page, t.scale, t.x, t.y, t.w, t.h);
                if (s && red) tintRed(s);
                SDL_LockMutex(m_mtx);
                m_renderingTile = false;
                if (s && gen == m_gen && !m_quit) {
                    if (m_tileSurf) SDL_FreeSurface(m_tileSurf);
                    m_tileSurf = s;
                    m_tileSurfReq = t;
                } else if (s) SDL_FreeSurface(s);
                SDL_UnlockMutex(m_mtx);
                continue;
            }
            int page = m_want.front();
            m_want.erase(m_want.begin());
            int gen = m_gen, W = m_canvasW, H = m_canvasH, fit = m_fit;
            bool red = m_red;
            if (m_pdf) {
                m_renderingPage = page;
                pdfClearAbort(m_pdf);
            }
            SDL_UnlockMutex(m_mtx);

            if (m_pdf) {
                // vector pages: 1.5x the fit size, so a little zoom stays crisp;
                // past that, tiles of the visible area take over
                SDL_Surface* s = nullptr;
                float pw = 0, ph = 0;
                if (pdfPageSize(m_pdf, page, pw, ph)) {
                    float sc = 1.5f * fitScale((int)lroundf(pw), (int)lroundf(ph), W, H, fit);
                    sc = std::min(sc, std::min(m_maxTex / pw, m_maxTex / ph));
                    sc = std::min(sc, sqrtf(4e6f / (pw * ph)));  // at most ~4 Mpx (16 MB) per page
                    s = pdfRender(m_pdf, page, sc);
                }
                SDL_LockMutex(m_mtx);
                m_renderingPage = -1;
                bool aborted = !s && pdfAborted(m_pdf);
                SDL_UnlockMutex(m_mtx);
                if (aborted) continue;  // asked again if it's wanted later
                int ow = (int)lroundf(pw), oh = (int)lroundf(ph);
                if (!s) {
                    s = SDL_CreateRGBSurfaceWithFormat(0, 64, 96, 32, SDL_PIXELFORMAT_RGBA32);
                    if (!s) continue;
                    SDL_FillRect(s, nullptr, SDL_MapRGB(s->format, 80, 80, 80));
                    ow = 612;
                    oh = 792;
                }
                if (red) tintRed(s);
                SDL_LockMutex(m_mtx);
                if (gen == m_gen && !m_quit) m_ready[page] = {s, ow, oh, gen, pw > 0 ? pw : (float)ow};
                else SDL_FreeSurface(s);
                SDL_UnlockMutex(m_mtx);
                continue;
            }
            int target = m_pages[page].archiveIndex;
            if (!a || target < nextIndex) {
                if (a) archive_read_free(a);
                a = openArchive(m_path);
                nextIndex = 0;
                if (!a) continue;
            }
            std::string data;
            archive_entry* e;
            bool found = false;
            while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
                int idx = nextIndex++;
                if (idx != target) { archive_read_data_skip(a); continue; }
                la_ssize_t got;
                while ((got = archive_read_data(a, buf.data(), buf.size())) > 0) data.append(buf.data(), (size_t)got);
                found = got == 0;
                break;
            }
            if (!found) { archive_read_free(a); a = nullptr; continue; }

            // decode at twice the fit size: sharp at 2x zoom, clean when minified
            int ow = 0, oh = 0;
            SDL_Surface* s = nullptr;
            {
                int fw = 0, fh = 0;
                s = loadImageMem(data.data(), data.size(), 0, 0, &fw, &fh);
                if (s) {
                    ow = fw;
                    oh = fh;
                    float sc = std::min(1.0f, 2.0f * fitScale(ow, oh, W, H, fit));
                    sc = std::min(sc, std::min(m_maxTex / (float)s->w, m_maxTex / (float)s->h));
                    int tw = std::max(1, (int)lroundf(s->w * sc)), th = std::max(1, (int)lroundf(s->h * sc));
                    if (tw < s->w || th < s->h) {
                        SDL_Surface* sc2 = scaleSurface(s, tw, th);
                        if (sc2) { SDL_FreeSurface(s); s = sc2; }
                    }
                    if (red) tintRed(s);
                }
            }
            if (!s) {
                s = SDL_CreateRGBSurfaceWithFormat(0, 64, 96, 32, SDL_PIXELFORMAT_RGBA32);
                if (!s) continue;
                SDL_FillRect(s, nullptr, SDL_MapRGB(s->format, 80, 80, 80));
                ow = 640;
                oh = 960;
            }
            SDL_LockMutex(m_mtx);
            if (gen == m_gen && !m_quit) m_ready[page] = {s, ow, oh, gen};
            else SDL_FreeSurface(s);
            SDL_UnlockMutex(m_mtx);
        }
        if (a) archive_read_free(a);
        return 0;
    }
};

BookReader* openComicReader(const std::string& path) { return new ComicReader(path); }
