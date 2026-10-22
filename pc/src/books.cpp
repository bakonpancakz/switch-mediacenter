#include "books.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "library.h"
#include "util.h"
#include "winmedia.h"
#include "zip.h"

std::string xmlUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        uint32_t cp = 0;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (ent.size() > 1 && ent[0] == '#') cp = (uint32_t)strtoul(ent.c_str() + (ent[1] == 'x' ? 2 : 1), nullptr, ent[1] == 'x' ? 16 : 10);
        if (!cp) { o += s[i]; continue; }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 63)); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
        i = e;
    }
    return o;
}

std::string titleFromFileName(const std::string& stem) {
    std::string t = stem;
    std::replace(t.begin(), t.end(), '_', ' ');
    return trim(t);
}

namespace {

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

// The value of attr="..." inside one tag's text ("<item id='x' href=...>").
std::string attr(const std::string& tag, const std::string& name) {
    size_t p = 0;
    while ((p = tag.find(name, p)) != std::string::npos) {
        bool start = p > 0 && (isspace((unsigned char)tag[p - 1]) || tag[p - 1] == ':');
        size_t q = p + name.size();
        while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
        if (start && q < tag.size() && tag[q] == '=') {
            q++;
            while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
            if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
                size_t e = tag.find(tag[q], q + 1);
                if (e != std::string::npos) return xmlUnescape(tag.substr(q + 1, e - q - 1));
            }
        }
        p = q;
    }
    return "";
}

// Every tag named `name` (any namespace prefix): its text and, for elements, the text inside.
struct Tag { std::string open, inner; };
std::vector<Tag> tags(const std::string& xml, const std::string& name) {
    std::vector<Tag> out;
    std::string lx = lower(xml);
    size_t p = 0;
    while ((p = lx.find('<', p)) != std::string::npos) {
        size_t n = p + 1;
        size_t colon = lx.find_first_of(": \t\r\n/>", n);
        if (colon != std::string::npos && lx[colon] == ':') n = colon + 1;
        if (lx.compare(n, name.size(), name) == 0 && n + name.size() < lx.size() &&
            strchr(" \t\r\n/>", lx[n + name.size()])) {
            size_t e = lx.find('>', p);
            if (e == std::string::npos) break;
            Tag t;
            t.open = xml.substr(p, e - p + 1);
            if (xml[e - 1] != '/') {
                // the closing tag, with whatever prefix
                size_t c = lx.find("</", e);
                while (c != std::string::npos) {
                    size_t cn = c + 2, cc = lx.find_first_of(":>", cn);
                    if (cc != std::string::npos && lx[cc] == ':') cn = cc + 1;
                    if (lx.compare(cn, name.size(), name) == 0) break;
                    c = lx.find("</", c + 2);
                }
                if (c != std::string::npos) t.inner = xml.substr(e + 1, c - e - 1);
            }
            out.push_back(t);
            p = e;
        } else p++;
    }
    return out;
}

std::string stripTags(const std::string& s) {
    std::string o;
    bool in = false;
    for (char c : s) {
        if (c == '<') in = true;
        else if (c == '>') in = false;
        else if (!in) o += c;
    }
    return trim(xmlUnescape(o));
}

std::string joinPath(const std::string& dir, const std::string& href) {
    std::string h = urlDecode(href);
    size_t hash = h.find('#');
    if (hash != std::string::npos) h = h.substr(0, hash);
    std::vector<std::string> parts;
    std::string all = dir.empty() ? h : dir + "/" + h;
    size_t p = 0;
    while (p <= all.size()) {
        size_t e = all.find('/', p);
        if (e == std::string::npos) e = all.size();
        std::string part = all.substr(p, e - p);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        p = e + 1;
    }
    std::string o;
    for (auto& s : parts) o += (o.empty() ? "" : "/") + s;
    return o;
}

const ZipReader::Entry* findLoose(const ZipReader& z, const std::string& name) {
    if (auto* e = z.find(name)) return e;
    std::string ln = lower(name);  // some books get the case wrong
    for (auto& e : z.entries())
        if (lower(e.name) == ln) return &e;
    return nullptr;
}

bool readEpub(const std::string& path, int coverH, BookInfo& out) {
    ZipReader z;
    if (!z.open(path)) return false;
    std::vector<uint8_t> buf;
    auto* c = findLoose(z, "META-INF/container.xml");
    if (!c || !z.read(*c, buf, 1 << 20)) return false;
    std::string container(buf.begin(), buf.end());
    std::string opfPath;
    for (auto& t : tags(container, "rootfile")) {
        opfPath = attr(t.open, "full-path");
        if (!opfPath.empty()) break;
    }
    auto* oe = findLoose(z, opfPath);
    if (!oe || !z.read(*oe, buf, 8 << 20)) return false;
    std::string opf(buf.begin(), buf.end());
    std::string dir = opfPath.find('/') == std::string::npos ? "" : opfPath.substr(0, opfPath.rfind('/'));

    auto titles = tags(opf, "title");
    if (!titles.empty()) out.title = stripTags(titles[0].inner);
    std::vector<std::string> authors;
    for (auto& t : tags(opf, "creator")) {
        std::string role = attr(t.open, "role");
        std::string name = stripTags(t.inner);
        if (!name.empty() && (role.empty() || role == "aut")) authors.push_back(name);
    }
    for (size_t i = 0; i < authors.size() && i < 3; i++) out.author += (i ? ", " : "") + authors[i];

    // the cover: EPUB 3 "cover-image", EPUB 2 <meta name="cover">, or an image named like one
    std::string coverId;
    for (auto& t : tags(opf, "meta"))
        if (lower(attr(t.open, "name")) == "cover") coverId = attr(t.open, "content");
    std::string href, guess;
    for (auto& t : tags(opf, "item")) {
        std::string id = attr(t.open, "id"), h = attr(t.open, "href"), type = lower(attr(t.open, "media-type"));
        bool image = type.compare(0, 6, "image/") == 0;
        if (attr(t.open, "properties").find("cover-image") != std::string::npos) { href = h; break; }
        if (!coverId.empty() && id == coverId && image) href = h;
        if (image && guess.empty() && (lower(id).find("cover") != std::string::npos || lower(h).find("cover") != std::string::npos))
            guess = h;
    }
    if (href.empty()) href = guess;
    if (!href.empty()) {
        auto* ce = findLoose(z, joinPath(dir, href));
        if (ce && z.read(*ce, buf, 32 << 20)) loadImageMemory(buf.data(), buf.size(), 0, out.cover);
        if (!out.cover.bgra.empty()) {
            // fit the cover's height
            Image& c2 = out.cover;
            int side = c2.h >= c2.w ? coverH : (int)((int64_t)coverH * c2.w / c2.h);
            c2 = scaleImage(c2, side);
        }
    }
    return true;
}

bool isPageName(const std::string& n) {
    std::string e = extOf(n);
    if (!(e == "jpg" || e == "jpeg" || e == "png" || e == "webp" || e == "gif" || e == "bmp" || e == "avif")) return false;
    std::string b = baseOf(n);
    return !b.empty() && b[0] != '.' && n.find("__MACOSX") == std::string::npos;
}

bool readCbz(const std::string& path, int coverH, BookInfo& out) {
    ZipReader z;
    if (!z.open(path)) return false;
    std::vector<const ZipReader::Entry*> pages;
    for (auto& e : z.entries())
        if (isPageName(e.name)) pages.push_back(&e);
    out.pages = (int)pages.size();
    if (pages.empty()) return true;
    auto first = *std::min_element(pages.begin(), pages.end(), [](const ZipReader::Entry* a, const ZipReader::Entry* b) {
        return lib::sortKey(a->name) < lib::sortKey(b->name);
    });
    std::vector<uint8_t> buf;
    if (z.read(*first, buf, 64 << 20) && loadImageMemory(buf.data(), buf.size(), 0, out.cover)) {
        Image& c = out.cover;
        int side = c.h >= c.w ? coverH : (int)((int64_t)coverH * c.w / c.h);
        c = scaleImage(c, side);
    }
    return true;
}

}  // namespace

namespace {

// 7-Zip reads the archives Windows can't (RAR, 7z, tar comics), if it's installed.
std::wstring find7z() {
    for (const wchar_t* p : {L"C:\\Program Files\\7-Zip\\7z.exe", L"C:\\Program Files (x86)\\7-Zip\\7z.exe"})
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
    wchar_t found[MAX_PATH];
    return SearchPathW(nullptr, L"7z.exe", nullptr, MAX_PATH, found, nullptr) ? found : L"";
}

bool readComicWith7z(const std::string& path, int coverH, BookInfo& out) {
    static const std::wstring exe = find7z();
    if (exe.empty()) return false;
    std::vector<uint8_t> listing;
    if (runProcess(exe, {L"l", L"-slt", L"-ba", L"-sccUTF-8", L"--", winPath(path)}, listing, 60000) != 0) return false;
    // "Path = x" opens each entry; "Folder = +" marks folders
    std::string txt(listing.begin(), listing.end()), cur;
    std::vector<std::string> pages;
    bool folder = false;
    auto flush = [&] {
        if (!cur.empty() && !folder && isPageName(cur)) pages.push_back(cur);
        cur.clear();
        folder = false;
    };
    size_t p = 0;
    while (p < txt.size()) {
        size_t e = txt.find('\n', p);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 7, "Path = ") == 0) { flush(); cur = line.substr(7); }
        else if (line == "Folder = +") folder = true;
    }
    flush();
    out.pages = (int)pages.size();
    if (pages.empty()) return true;
    std::string first = *std::min_element(pages.begin(), pages.end(),
                                          [](const std::string& a, const std::string& b) { return lib::sortKey(a) < lib::sortKey(b); });
    std::vector<uint8_t> data;
    // names come and go as UTF-8 (-sccUTF-8): 7-Zip would otherwise use the console code page
    if (runProcess(exe, {L"e", L"-so", L"-sccUTF-8", L"--", winPath(path), wide(first)}, data, 60000) != 0 || data.empty()) return true;
    if (loadImageMemory(data.data(), data.size(), 0, out.cover)) {
        Image& c = out.cover;
        c = scaleImage(c, c.h >= c.w ? coverH : (int)((int64_t)coverH * c.w / c.h));
    }
    return true;
}

}  // namespace

bool readBook(const std::string& path, int format, int coverH, BookInfo& out) {
    out = BookInfo();
    std::string ext = extOf(path);
    bool ok = true;
    if (format == lib::B_EPUB) ok = readEpub(path, coverH, out);
    else if (format == lib::B_COMIC && ext == "cbz") ok = readCbz(path, coverH, out);
    else if (format == lib::B_COMIC) ok = readComicWith7z(path, coverH, out);
    else if (format == lib::B_PDF) pdfFirstPage(path, coverH, out.cover);
    if (out.title.empty()) out.title = titleFromFileName(stemOf(path));
    return ok;
}
