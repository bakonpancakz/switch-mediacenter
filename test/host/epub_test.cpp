// Host test for the EPUB loader: builds a small book (as a directory) and
// checks chapters, TOC, styles, links, footnotes and images.
//   epub_test <workdir>
#include <direct.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdarg>
#include <cstring>
#include <map>
#include "../../source/books/book.h"
#include "archive.h"
#include "archive_entry.h"

// ---------------------------------------------------------------- app.h helpers
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string ext(const std::string& p) {
    size_t d = p.find_last_of('.'), s = p.find_last_of('/');
    if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
    return lower(p.substr(d + 1));
}
std::string baseName(const std::string& p) { size_t s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); }
std::string dirName(const std::string& p) { size_t s = p.find_last_of('/'); return s == std::string::npos ? "" : p.substr(0, s); }
std::string stem(const std::string& p) { std::string b = baseName(p); size_t d = b.find_last_of('.'); return d == std::string::npos ? b : b.substr(0, d); }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
uint32_t utf8Next(const std::string& s, size_t& i) {
    unsigned char c = s[i];
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
    else { i++; return 0xFFFD; }
    if (i + n > s.size()) { i = s.size(); return 0xFFFD; }
    for (int k = 1; k < n; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    i += n;
    return cp;
}
void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}
bool isImageExt(const std::string& e) { return e == "jpg" || e == "jpeg" || e == "png" || e == "gif" || e == "webp"; }
int jpegExifOrientation(const uint8_t*, size_t) { return 1; }
void appLog(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }

// ---------------------------------------------------------------- libarchive shim
struct archive { std::vector<std::string> names; std::string root, data; size_t idx = 0, pos = 0; };
struct archive_entry { std::string name; };
static archive_entry s_entry;
static void walk(const std::string& root, const std::string& rel, std::vector<std::string>& out) {
    DIR* d = opendir((rel.empty() ? root : root + "/" + rel).c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string r = rel.empty() ? n : rel + "/" + n;
        struct stat st;
        stat((root + "/" + r).c_str(), &st);
        if (S_ISDIR(st.st_mode)) walk(root, r, out);
        else out.push_back(r);
    }
    closedir(d);
}
archive* archive_read_new() { return new archive; }
int archive_read_support_format_zip(archive*) { return 0; }
int archive_read_support_format_all(archive*) { return 0; }
int archive_read_support_filter_all(archive*) { return 0; }
int archive_read_open_filename(archive* a, const char* p, size_t) { a->root = p; walk(p, "", a->names); return a->names.empty() ? -1 : 0; }
int archive_read_next_header(archive* a, archive_entry** e) {
    if (a->idx >= a->names.size()) return ARCHIVE_EOF;
    s_entry.name = a->names[a->idx++];
    readFile(a->root + "/" + s_entry.name, a->data);
    a->pos = 0;
    *e = &s_entry;
    return ARCHIVE_OK;
}
la_ssize_t archive_read_data(archive* a, void* buf, size_t n) {
    size_t k = std::min(n, a->data.size() - a->pos);
    memcpy(buf, a->data.data() + a->pos, k);
    a->pos += k;
    return (la_ssize_t)k;
}
int archive_read_data_skip(archive*) { return 0; }
int archive_read_free(archive* a) { delete a; return 0; }
const char* archive_entry_pathname(archive_entry* e) { return e->name.c_str(); }
int archive_entry_filetype(archive_entry*) { return AE_IFREG; }

// ---------------------------------------------------------------- test book
static void put(const std::string& path, const std::string& data) {
    std::string dir = dirName(path);
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') _mkdir(dir.substr(0, i).c_str());
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static std::string blockText(const Doc& d, const Block& b) { return d.text.substr(b.start, b.end - b.start); }
static int findBlock(const Doc& d, const std::string& text) {
    for (int i = 0; i < (int)d.blocks.size(); i++) if (blockText(d, d.blocks[i]) == text) return i;
    return -1;
}

int main(int argc, char** argv) {
    std::string root = std::string(argc > 1 ? argv[1] : "epubtest") + "/book";
    const char* xhtml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                        "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>t</title><link rel=\"stylesheet\" href=\"style.css\"/></head><body>";
    put(root + "/mimetype", "application/epub+zip");
    put(root + "/META-INF/container.xml",
        "<container><rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
    put(root + "/OEBPS/content.opf",
        "<package><metadata><dc:title>Test &amp; Book</dc:title><dc:creator>A. Writer</dc:creator></metadata><manifest>"
        "<item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c1\" href=\"text/ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" href=\"text/ch2.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"n\" href=\"text/notes.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"css\" href=\"text/style.css\" media-type=\"text/css\"/>"
        "<item id=\"img\" href=\"images/pic.png\" media-type=\"image/png\"/>"
        "</manifest><spine><itemref idref=\"c1\"/><itemref idref=\"c2\"/><itemref idref=\"n\"/></spine></package>");
    put(root + "/OEBPS/nav.xhtml",
        std::string(xhtml) + "<nav epub:type=\"toc\"><ol><li><a href=\"text/ch1.xhtml\">Chapter One</a></li>"
                             "<li><a href=\"text/ch2.xhtml\">The Second</a><ol><li><a href=\"text/ch2.xhtml#sec2\">Section 2.1</a></li></ol></li>"
                             "<li><a href=\"text/notes.xhtml\">Notes</a></li></ol></nav></body></html>");
    put(root + "/OEBPS/text/style.css", "/* c */ .it { font-style: italic } p.center { text-align: center } .gone{display:none}\n"
                                        "@media print { .it { font-style: normal } }");
    put(root + "/OEBPS/text/ch1.xhtml",
        std::string(xhtml) + "<h1>Chapter One</h1><p>Hello <span class=\"it\">world</span><a epub:type=\"noteref\" href=\"notes.xhtml#n1\">1</a>.</p>"
                             "<p>Second para with <b>bold</b> and a <a href=\"ch2.xhtml#sec2\">link to 2.1</a>.</p>"
                             "<p class=\"gone\">invisible</p><aside epub:type=\"footnote\" id=\"fa\"><p>Inline aside note</p></aside>"
                             "<div><img src=\"../images/pic.png\" alt=\"\"/></div><p>Line one<br/>Line two</p></body></html>");
    put(root + "/OEBPS/text/ch2.xhtml",
        std::string(xhtml) + "<p class=\"center\">The Second</p><p>Intro &mdash; text<sup><a href=\"notes.xhtml#n2\">2</a></sup>.</p>"
                             "<h2 id=\"sec2\">Section 2.1</h2><p>More.</p><ul><li>one</li><li>two</li></ul>"
                             "<p><a href=\"https://example.com\">web</a></p></body></html>");
    put(root + "/OEBPS/text/notes.xhtml",
        std::string(xhtml) + "<h1>Notes</h1><p id=\"n1\">1. The first note text.</p><p id=\"n2\">2. Second note.</p></body></html>");
    // 600x800 PNG header is all imageDims needs
    std::string png("\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\x02\x58\0\0\x03\x20\x08\x06\0\0\0", 33);
    put(root + "/OEBPS/images/pic.png", png);

    // loadDoc picks the loader by extension, so give the directory an .epub name
    Doc d;
    bool ok;
    rename(root.c_str(), (root + ".epub").c_str());
    ok = loadDoc(root + ".epub", d);
    rename((root + ".epub").c_str(), root.c_str());
    CHECK(ok, ("load: " + d.err).c_str());
    CHECK(d.title == "Test & Book" && d.author == "A. Writer", "title and author");
    for (size_t i = 0; i < d.blocks.size(); i++) {
        const Block& b = d.blocks[i];
        printf("  [%2zu] type=%d lvl=%d %s%s%s%s img=%d '%s'\n", i, b.type, b.level, b.breakBefore ? "BREAK " : "",
               b.chapterTitle ? "TITLE " : "", b.hidden ? "HIDDEN " : "", b.align == AL_CENTER ? "CENTER " : "", b.image,
               blockText(d, b).c_str());
    }
    for (auto& c : d.toc) printf("  toc depth=%d '%s' -> %u (block %d)\n", c.depth, c.title.c_str(), c.offset, d.blockAt(c.offset));
    CHECK(d.toc.size() == 4, "4 TOC entries");
    CHECK(d.toc.size() == 4 && d.toc[2].depth == 1 && d.toc[1].depth == 0, "TOC nesting");
    int ch1 = findBlock(d, "Chapter One"), ch2 = findBlock(d, "The Second"), s21 = findBlock(d, "Section 2.1"), notes = findBlock(d, "Notes");
    CHECK(ch1 >= 0 && d.blocks[ch1].chapterTitle && !d.blocks[ch1].breakBefore, "chapter 1 title, no break before the first page");
    CHECK(ch2 >= 0 && d.blocks[ch2].chapterTitle && d.blocks[ch2].breakBefore, "chapter 2 starts a new page with its title");
    CHECK(ch2 >= 0 && d.blocks[ch2].align == AL_CENTER, "CSS class alignment");
    CHECK(s21 >= 0 && d.blocks[s21].breakBefore && d.blocks[s21].type == B_HEAD, "TOC section heading starts a page");
    CHECK(notes >= 0 && d.blocks[notes].breakBefore, "notes chapter starts a page");
    CHECK(findBlock(d, "invisible") >= 0 && d.blocks[findBlock(d, "invisible")].hidden, "display:none kept out of the flow");
    int aside = findBlock(d, "Inline aside note");
    CHECK(aside >= 0 && d.blocks[aside].hidden, "footnote aside hidden from the flow");
    bool italic = false, bold = false;
    for (auto& r : d.runs) {
        std::string t = d.text.substr(r.start, r.end - r.start);
        if (t.find("world") != std::string::npos && (r.style & RS_ITALIC)) italic = true;
        if (trim(t) == "bold" && (r.style & RS_BOLD)) bold = true;
    }
    CHECK(italic, "italic from CSS class");
    CHECK(bold, "bold from <b>");
    int noterefs = 0, internal = 0, external = 0;
    for (auto& l : d.links) {
        printf("  link %s%s -> %s\n", l.noteref ? "[note] " : "", l.external ? "[ext] " : "", l.target.c_str());
        noterefs += l.noteref;
        external += l.external;
        internal += !l.external && !l.noteref;
    }
    CHECK(noterefs == 2 && external == 1 && internal == 1, "link kinds (2 notes, 1 internal, 1 external)");
    uint32_t off;
    CHECK(d.resolve("OEBPS/text/notes.xhtml#n1", off) && d.noteText(off) == "1. The first note text.", "footnote text for popup");
    CHECK(d.resolve("OEBPS/text/ch2.xhtml#sec2", off) && d.blockAt(off) == s21, "fragment link resolves to its heading");
    bool img = false;
    for (auto& b : d.blocks) if (b.type == B_IMAGE && b.image >= 0) img = d.images[b.image].w == 600 && d.images[b.image].h == 800;
    CHECK(img, "image block with size from header");
    CHECK(findBlock(d, "Line one\nLine two") >= 0, "<br/> becomes a line break");
    CHECK(findBlock(d, "\xE2\x80\xA2 one") >= 0, "list bullets");
    CHECK(findBlock(d, "Intro \xE2\x80\x94 text2.") >= 0, "entities and superscript text");
    printf(fails ? "EPUB TEST FAILED (%d)\n" : "EPUB TEST OK\n", fails);
    return fails ? 1 : 0;
}
