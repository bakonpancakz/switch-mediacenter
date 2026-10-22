// Books tab: a cover grid of the folders synced as Books (titles, authors and
// covers from the PC; PDF and CBR covers drawn here once), hosting the text and
// comic readers.
#include <algorithm>
#include <map>

#include "books/reader.h"
#include "tabs/common.h"

namespace {

class BooksTab : public Tab {
public:
    const char* name() override { return "Books"; }
    int icon() override { return IC_BOOK; }
    SDL_Color accent() override { return rgb(0xF2A541); }
    bool fullscreen() override { return m_reader != nullptr || !m_pending.empty(); }
    bool keepAwake() override { return m_reader != nullptr; }
    // The shelf is landscape like the other tabs (turning the whole tab on the
    // way in was jarring); a book turns to portrait when it opens.
    int rotation() override { return m_reader ? m_reader->rotation() : 0; }
    int brightness() override { return m_reader ? m_reader->brightness() : 100; }
    void enter() override { refresh(); }
    void shutdown() override { closeReader(); }

    void update(const SDL_Rect&) override {
        if (!m_pending.empty() && m_pendingShown) {
            std::string path = m_pending;
            m_pending.clear();
            std::string x = ext(path);
            BookReader* r = (isComicExt(x) || x == "pdf") ? openComicReader(path) : openTextReader(path);
            if (r->ok()) m_reader = r;
            else {
                toast(baseName(path) + ": " + r->error(), 4000);
                delete r;
            }
            g_in.consumed = true;
            return;
        }
        if (m_reader) {
            m_reader->update();
            if (m_reader->wantsClose()) {
                closeReader();
                g_in.consumed = true;
            }
        }
    }

    void draw(const SDL_Rect& area) override {
        refresh();
        if (!m_pending.empty()) {
            const ReadPalette& p = readPalette();
            fillRect(0, 0, g_W, g_H, p.bg);
            spinner(g_W / 2.0f, g_H / 2.0f - 40, 30, p.accent);
            drawText("Opening\xE2\x80\xA6", g_W / 2, g_H / 2 + 10, 28, p.fg, 1);
            drawText(fileStem(baseName(m_pending)), g_W / 2, g_H / 2 + 52, 22, p.dim, 1, g_W - 80);
            m_pendingShown = true;
            return;
        }
        if (m_reader) {
            // shared widgets (buttons, chips, sliders) follow the reading tint
            const ReadPalette& p = readPalette();
            Theme saved = g_theme;
            g_theme.accent = p.accent;
            g_theme.fg = p.fg;
            g_theme.dim = p.dim;
            g_theme.panel = p.panel;
            g_theme.panel2 = p.sel;
            g_theme.line = p.sel;
            g_theme.faint = p.sel;
            m_reader->draw();
            g_theme = saved;
            return;
        }
        drawLibrary(area);
    }

private:
    std::string m_pending;
    bool m_pendingShown = false;
    BookReader* m_reader = nullptr;
    uint32_t m_version = 0;
    std::vector<LibRoot> m_roots;
    std::map<int64_t, LibBook> m_books;  // by item id
    int64_t m_dir = 0;
    LibItem m_dirItem;
    std::vector<LibItem> m_entries;
    Scroller m_scroll;

    void closeReader() {
        if (!m_reader) return;
        m_reader->save();
        delete m_reader;
        m_reader = nullptr;
        posFlush();
    }

    void refresh() {
        uint32_t v = Library::get().version();
        if (v == m_version) return;
        m_version = v;
        m_roots = Library::get().roots(lib::C_BOOKS);
        m_books.clear();
        for (auto& b : Library::get().books()) m_books[b.item] = b;
        std::string last = cfgStr("books.last_dir", "");
        LibItem it;
        if (!last.empty() && Library::get().itemByPath(last, it) && it.kind == lib::K_DIR) open(it.id, true);
        else open(home(), true);
    }

    int64_t home() const { return m_roots.size() == 1 ? m_roots[0].item : 0; }

    void open(int64_t dir, bool keepScroll = false) {
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
        // books by title (the PC read it from the book), folders first
        std::stable_sort(m_entries.begin(), m_entries.end(), [this](const LibItem& a, const LibItem& b) {
            if ((a.kind == lib::K_DIR) != (b.kind == lib::K_DIR)) return a.kind == lib::K_DIR;
            if (a.kind == lib::K_DIR) return false;
            return lib::sortKey(titleOf(a)) < lib::sortKey(titleOf(b));
        });
        if (!keepScroll) m_scroll = Scroller();
        // (not while a sync has the library closed: the place to come back to is still the old one)
        if (Library::get().ready()) cfgSet("books.last_dir", m_dir ? m_dirItem.path : "");
    }

    std::string titleOf(const LibItem& e) const {
        auto b = m_books.find(e.id);
        return b != m_books.end() && !b->second.title.empty() ? b->second.title : fileStem(e.name);
    }

    void drawLibrary(const SDL_Rect& area) {
        if (!Library::get().ready() || m_roots.empty()) {
            drawText(name(), area.x + 32, 26, 36, g_theme.fg, 0, 0, FS_BOLD);
            drawNoLibrary(area, IC_BOOK, "Books");
            return;
        }
        int x0 = area.x + 32;
        bool sub = m_dir != home();
        if (sub && (iconButton(x0 + 22.0f, 46, 28, IC_BACK, g_theme.fg, g_theme.panel2) || g_in.back)) {
            open(m_dirItem.parent ? m_dirItem.parent : home());
            return;
        }
        int tx = sub ? x0 + 64 : x0;
        drawText(m_dir ? m_dirItem.name : name(), tx, 26, 36, g_theme.fg, 0, area.w - 100, FS_BOLD);

        SDL_Rect grid = {area.x, 96, area.w, area.h - 96};
        int tileW = 190, gap = 24;
        int cols = std::max(1, (grid.w - 48 + gap) / (tileW + gap));
        if (cols < 3) {  // the narrow (portrait) shelf: three smaller covers to a row
            cols = 3;
            tileW = (grid.w - 48 - 2 * gap) / 3;
        }
        int tileH = tileW * 3 / 2 + 95;
        int startX = grid.x + (grid.w - (cols * tileW + (cols - 1) * gap)) / 2;
        int rows = ((int)m_entries.size() + cols - 1) / cols;
        float contentH = rows * (tileH + gap) + 24.0f;
        m_scroll.update(grid, contentH);
        pushClip(grid);
        for (int i = 0; i < (int)m_entries.size(); i++) {
            int r = i / cols, c = i % cols;
            int y = grid.y + 8 + r * (tileH + gap) - (int)m_scroll.pos;
            if (y + tileH < grid.y || y > grid.y + grid.h) continue;
            const LibItem& e = m_entries[i];
            SDL_Rect tile = {startX + c * (tileW + gap), y, tileW, tileH};
            SDL_Rect cover = {tile.x, tile.y, tileW, tileW * 3 / 2};
            bool down = pressing(tile);
            if (e.kind == lib::K_DIR) {
                if (drawPicture(e.thumb, cover, 16, down ? 110 : 150)) drawIcon(IC_FOLDER, cover.x + cover.w / 2.0f, cover.y + cover.h / 2.0f, 72, g_theme.fg);
                else {
                    fillRound(cover, 16, down ? g_theme.line : g_theme.panel2);
                    drawIcon(IC_FOLDER, cover.x + cover.w / 2.0f, cover.y + cover.h / 2.0f, 84, g_theme.accent);
                }
                drawWrapped(e.name, tile.x + 4, cover.y + cover.h + 10, 20, g_theme.fg, tileW - 8, 2);
            } else {
                auto bk = m_books.find(e.id);
                bool comic = isComicExt(ext(e.path)) || ext(e.path) == "pdf";
                SDL_Texture* t = pictureGet(e.thumb);
                if (t) drawTexFit(t, cover, true, down ? 180 : 255, 16);
                else {
                    fillRound(cover, 16, g_theme.panel2);
                    drawIcon(comic ? IC_COMIC : IC_BOOK, cover.x + cover.w / 2.0f, cover.y + cover.h / 2.0f - 20, 64, g_theme.faint);
                    drawWrapped(titleOf(e), cover.x + 14, cover.y + cover.h / 2 + 30, 18, g_theme.dim, cover.w - 28, 3, 1);
                }
                int64_t pos = 0, total = 0;
                if (posGet(e.path, pos, total) && total > 0) {
                    float f = comic ? (total > 1 ? pos / (float)(total - 1) : 1.0f) : pos / (float)total;
                    f = SDL_clamp(f, 0.0f, 1.0f);
                    // a dark pill so the bar reads on any cover, with the percentage beside it
                    SDL_Rect pill = {cover.x + 8, cover.y + cover.h - 38, cover.w - 16, 30};
                    fillRound(pill, 15, SDL_Color{0, 0, 0, 205});
                    std::string pct = f >= 0.99f ? "Done" : std::to_string((int)(f * 100)) + "%";
                    int pw = drawText(pct, pill.x + pill.w - 12, pill.y + 5, 16, SDL_Color{255, 255, 255, 235}, 2, 0, FS_BOLD);
                    SDL_Rect bar = {pill.x + 14, pill.y + 11, pill.w - 28 - pw - 10, 8};
                    fillRound(bar, 4, SDL_Color{255, 255, 255, 60});
                    fillRound({bar.x, bar.y, std::max(8, (int)(bar.w * f)), 8}, 4, f >= 0.99f ? g_theme.ok : g_theme.accent);
                }
                int ty = cover.y + cover.h + 10;
                ty += drawWrapped(titleOf(e), tile.x + 4, ty, 20, g_theme.fg, tileW - 8, 2);
                if (bk != m_books.end() && !bk->second.author.empty()) drawText(bk->second.author, tile.x + 4, ty + 2, 17, g_theme.dim, 0, tileW - 8);
            }
            if (tapped(tile)) {
                if (e.kind == lib::K_DIR) { open(e.id); popClip(); return; }
                m_pending = e.path;
                m_pendingShown = false;
            }
        }
        popClip();
        drawScrollbar(grid, contentH, m_scroll.pos);
        if (m_entries.empty()) {
            drawIcon(IC_BOOK, area.x + area.w / 2.0f, area.h / 2.0f - 60, 72, g_theme.faint);
            drawText("No books here yet", area.x + area.w / 2, area.h / 2, 28, g_theme.fg, 1);
            drawText("This folder is empty on your PC.", area.x + area.w / 2, area.h / 2 + 44, 20, g_theme.dim, 1, area.w - 80);
        }
    }
};

}  // namespace

Tab* makeBooksTab() { return new BooksTab(); }
