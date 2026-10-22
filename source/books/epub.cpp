// EPUB and plain-text loading into the Doc model: XHTML is turned into styled
// blocks (headings, lists, quotes, images, bold/italic runs, links), simple
// CSS class rules are honoured, footnote bodies are hidden for popups, and
// every chapter starts on its own page with its title marked as a header.
#include "book.h"
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <cstring>
#include <map>

// ---------------------------------------------------------------- Doc helpers

int Doc::blockAt(uint32_t off) const {
    int lo = 0, hi = (int)blocks.size() - 1, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (blocks[mid].start <= off) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

bool Doc::resolve(const std::string& target, uint32_t& off) const {
    auto it = anchors.find(target);
    if (it == anchors.end()) {
        size_t hash = target.find('#');
        if (hash == std::string::npos) return false;
        it = anchors.find(target.substr(0, hash));
        if (it == anchors.end()) return false;
    }
    off = it->second;
    return true;
}

std::string Doc::noteText(uint32_t off) const {
    if (blocks.empty()) return "";
    int b = blockAt(off);
    std::string out;
    for (int i = b; i < (int)blocks.size() && out.size() < 900; i++) {
        const Block& bl = blocks[i];
        if (i > b && (bl.breakBefore || bl.type == B_HEAD)) break;
        uint32_t s = i == b ? std::max(off, bl.start) : bl.start;
        if (bl.end > s) {
            if (!out.empty()) out += "\n";
            out += text.substr(s, bl.end - s);
        }
        // a note is usually one block; keep going only if we only got a marker
        if (trim(out).size() > 6) break;
    }
    for (auto& c : out) if (c == '\n') c = ' ';
    return trim(out);
}

// ---------------------------------------------------------------- encoding

static bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (int k = 1; k < n; k++) if ((s[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

static std::string toUtf8(const std::string& raw) {
    const unsigned char* u = (const unsigned char*)raw.data();
    if (raw.size() >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) return raw.substr(3);
    if (raw.size() >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF))) {
        bool le = u[0] == 0xFF;
        std::string out;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            uint32_t c = le ? (u[i] | (u[i + 1] << 8)) : ((u[i] << 8) | u[i + 1]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < raw.size()) {
                uint32_t c2 = le ? (u[i + 2] | (u[i + 3] << 8)) : ((u[i + 2] << 8) | u[i + 3]);
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i += 2;
            }
            appendUtf8(out, c);
        }
        return out;
    }
    if (validUtf8(raw)) return raw;
    static const uint16_t cp1252[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030,
        0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
    std::string out;
    for (unsigned char c : raw) appendUtf8(out, (c >= 0x80 && c < 0xA0) ? cp1252[c - 0x80] : c);
    return out;
}

// ---------------------------------------------------------------- XML helpers

static std::string attr(const std::string& tag, const char* name) {
    size_t n = strlen(name);
    for (size_t p = 0; (p = tag.find(name, p)) != std::string::npos; p += n) {
        if (p > 0 && !isspace((unsigned char)tag[p - 1]) && tag[p - 1] != ':') continue;
        size_t q = p + n;
        while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
        if (q >= tag.size() || tag[q] != '=') continue;
        q++;
        while (q < tag.size() && isspace((unsigned char)tag[q])) q++;
        if (q >= tag.size()) return "";
        char quote = tag[q];
        if (quote == '"' || quote == '\'') {
            size_t e = tag.find(quote, q + 1);
            return tag.substr(q + 1, e == std::string::npos ? std::string::npos : e - q - 1);
        }
        size_t e = q;
        while (e < tag.size() && !isspace((unsigned char)tag[e]) && tag[e] != '>' && tag[e] != '/') e++;
        return tag.substr(q, e - q);
    }
    return "";
}

static void decodeEntity(const std::string& ent, std::string& out) {
    if (!ent.empty() && ent[0] == '#') {
        uint32_t cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                          : strtoul(ent.c_str() + 1, nullptr, 10);
        if (cp == 0xAD || cp == 0x200B || cp == 0xFEFF) return;  // soft hyphen, zero width
        if (cp && cp < 0x110000) appendUtf8(out, cp);
        return;
    }
    static const std::map<std::string, uint32_t> named = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
        {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"copy", 0xA9}, {"reg", 0xAE},
        {"trade", 0x2122}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"bull", 0x2022}, {"middot", 0xB7}, {"shy", 0},
        {"thinsp", ' '}, {"ensp", ' '}, {"emsp", ' '}, {"eacute", 0xE9}, {"egrave", 0xE8}, {"ecirc", 0xEA},
        {"aacute", 0xE1}, {"agrave", 0xE0}, {"acirc", 0xE2}, {"ouml", 0xF6}, {"uuml", 0xFC}, {"auml", 0xE4},
        {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"Auml", 0xC4}, {"szlig", 0xDF}, {"ccedil", 0xE7}, {"deg", 0xB0},
        {"times", 0xD7}, {"prime", 0x2032}, {"Prime", 0x2033}, {"dagger", 0x2020}, {"Dagger", 0x2021},
        {"sect", 0xA7}, {"para", 0xB6}, {"iexcl", 0xA1}, {"iquest", 0xBF}, {"ntilde", 0xF1}, {"oacute", 0xF3},
        {"iacute", 0xED}, {"uacute", 0xFA}, {"euro", 0x20AC}, {"pound", 0xA3}, {"zwj", 0}, {"zwnj", 0}};
    auto it = named.find(ent);
    if (it != named.end()) { if (it->second) appendUtf8(out, it->second); }
    else { out += '&'; out += ent; out += ';'; }
}

static std::string decodeText(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            size_t e = s.find(';', i);
            if (e != std::string::npos && e - i < 12) { decodeEntity(s.substr(i + 1, e - i - 1), out); i = e; continue; }
        }
        out += s[i];
    }
    return out;
}

static std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}

// Resolve an href relative to `base` (a file inside the archive). Keeps "#frag".
static std::string resolveHref(const std::string& base, const std::string& hrefIn) {
    std::string href = hrefIn, frag;
    size_t hash = href.find('#');
    if (hash != std::string::npos) { frag = href.substr(hash); href = href.substr(0, hash); }
    href = urlDecode(href);
    if (href.empty()) return base + frag;
    size_t slash = base.find_last_of('/');
    std::string full = (slash == std::string::npos ? "" : base.substr(0, slash + 1)) + href;
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= full.size()) {
        size_t e = full.find('/', p);
        if (e == std::string::npos) e = full.size();
        std::string seg = full.substr(p, e - p);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        p = e + 1;
    }
    std::string r;
    for (auto& s : parts) { if (!r.empty()) r += '/'; r += s; }
    return r + frag;
}

template <class F> static void forEachTag(const std::string& x, F f) {
    size_t p = 0;
    while ((p = x.find('<', p)) != std::string::npos) {
        size_t e = x.find('>', p);
        if (e == std::string::npos) break;
        std::string tag = x.substr(p + 1, e - p - 1);
        bool closing = !tag.empty() && tag[0] == '/';
        size_t ns = closing ? 1 : 0, ne = ns;
        while (ne < tag.size() && !isspace((unsigned char)tag[ne]) && tag[ne] != '/') ne++;
        std::string name = lower(tag.substr(ns, ne - ns));
        size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(colon + 1);
        f(name, tag, closing, e + 1);
        p = e + 1;
    }
}

static std::string stripTags(const std::string& x) {
    std::string out;
    bool in = false;
    for (char c : x) {
        if (c == '<') in = true;
        else if (c == '>') { in = false; out += ' '; }
        else if (!in) out += c;
    }
    std::string d = decodeText(out), r;
    bool sp = false;
    for (char c : d) {
        if (isspace((unsigned char)c)) sp = true;
        else { if (sp && !r.empty()) r += ' '; sp = false; r += c; }
    }
    return r;
}

static std::string normTitle(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (isalnum(c) || c >= 0x80) o += (char)tolower(c);
    return o;
}

// ---------------------------------------------------------------- CSS

struct CssFlags {
    int8_t bold = -1, italic = -1, align = -1;
    bool hidden = false, pageBreak = false, big = false, noIndent = false, small = false;
    void merge(const CssFlags& o) {
        if (o.bold >= 0) bold = o.bold;
        if (o.italic >= 0) italic = o.italic;
        if (o.align >= 0) align = o.align;
        hidden |= o.hidden; pageBreak |= o.pageBreak; big |= o.big; noIndent |= o.noIndent; small |= o.small;
    }
};

static CssFlags parseDecls(const std::string& declsIn) {
    CssFlags f;
    std::string decls = lower(declsIn);
    size_t p = 0;
    while (p < decls.size()) {
        size_t semi = decls.find(';', p);
        if (semi == std::string::npos) semi = decls.size();
        std::string d = decls.substr(p, semi - p);
        p = semi + 1;
        size_t colon = d.find(':');
        if (colon == std::string::npos) continue;
        std::string k = trim(d.substr(0, colon)), v = trim(d.substr(colon + 1));
        if (k == "font-weight") {
            if (v.find("bold") != std::string::npos || v == "600" || v == "700" || v == "800" || v == "900") f.bold = 1;
            else if (v == "normal" || v == "400" || v == "300") f.bold = 0;
        } else if (k == "font-style") f.italic = (v.find("italic") != std::string::npos || v.find("oblique") != std::string::npos) ? 1 : 0;
        else if (k == "text-align") f.align = v.find("center") != std::string::npos ? AL_CENTER : v.find("right") != std::string::npos ? AL_RIGHT : AL_AUTO;
        else if (k == "display" && v.find("none") != std::string::npos) f.hidden = true;
        else if ((k == "page-break-before" && v.find("always") != std::string::npos) || (k == "break-before" && v.find("page") != std::string::npos)) f.pageBreak = true;
        else if (k == "text-indent" && (v == "0" || v.rfind("0", 0) == 0 || v[0] == '-')) f.noIndent = true;
        else if (k == "font-size") {
            float n = (float)atof(v.c_str());
            if (v.find("large") != std::string::npos && v.find("larger") == std::string::npos) f.big = true;
            else if (v.find("larger") != std::string::npos) f.big = true;
            else if (v.find("small") != std::string::npos) f.small = true;
            else if (v.find("em") != std::string::npos && n >= 1.25f) f.big = true;
            else if (v.find('%') != std::string::npos && n >= 125) f.big = true;
            else if (v.find("em") != std::string::npos && n > 0 && n < 0.85f) f.small = true;
        }
    }
    return f;
}

static void parseCss(const std::string& cssIn, std::unordered_map<std::string, CssFlags>& rules) {
    std::string css;
    for (size_t i = 0; i < cssIn.size(); i++) {  // strip comments
        if (cssIn.compare(i, 2, "/*") == 0) {
            size_t e = cssIn.find("*/", i + 2);
            if (e == std::string::npos) break;
            i = e + 1;
        } else css += cssIn[i];
    }
    size_t p = 0;
    while (p < css.size()) {
        size_t open = css.find('{', p);
        if (open == std::string::npos) break;
        size_t close = css.find('}', open);
        if (close == std::string::npos) break;
        std::string sels = css.substr(p, open - p);
        if (sels.find('@') != std::string::npos) {  // @media etc: skip the nested block wholesale
            int depth = 0;
            size_t q = open;
            for (; q < css.size(); q++) {
                if (css[q] == '{') depth++;
                else if (css[q] == '}' && --depth == 0) break;
            }
            p = q + 1;
            continue;
        }
        CssFlags f = parseDecls(css.substr(open + 1, close - open - 1));
        size_t s = 0;
        while (s <= sels.size()) {
            size_t comma = sels.find(',', s);
            if (comma == std::string::npos) comma = sels.size();
            std::string sel = lower(trim(sels.substr(s, comma - s)));
            s = comma + 1;
            if (sel.empty() || sel.find_first_of(" >+~:[*") != std::string::npos) continue;
            rules[sel].merge(f);
        }
        p = close + 1;
    }
}

// ---------------------------------------------------------------- XHTML -> blocks

struct Part {
    std::string text;
    std::vector<Run> runs;
    std::vector<Block> blocks;
    std::vector<std::pair<std::string, uint32_t>> anchors;  // id -> offset
};

struct Elem {
    std::string name;
    uint8_t style = 0;
    int link = -1;
    bool hidden = false, block = false, pre = false;
    int8_t align = -1;
    int linkTextStart = -1;
};

class HtmlParser {
public:
    HtmlParser(Doc& doc, const std::string& file, const std::unordered_map<std::string, CssFlags>& css,
               const std::map<std::string, std::string>& images)
        : m_doc(doc), m_file(file), m_css(css), m_images(images) {}

    void parse(const std::string& h, Part& out) {
        m_out = &out;
        size_t i = 0, n = h.size();
        while (i < n) {
            char c = h[i];
            if (c == '<') {
                if (h.compare(i, 4, "<!--") == 0) {
                    size_t e = h.find("-->", i + 4);
                    i = e == std::string::npos ? n : e + 3;
                    continue;
                }
                if (h.compare(i, 9, "<![CDATA[") == 0) {
                    size_t e = h.find("]]>", i);
                    std::string d = h.substr(i + 9, e == std::string::npos ? std::string::npos : e - i - 9);
                    text(d);
                    i = e == std::string::npos ? n : e + 3;
                    continue;
                }
                if (h.compare(i, 2, "<?") == 0 || h.compare(i, 2, "<!") == 0) {
                    size_t e = h.find('>', i);
                    i = e == std::string::npos ? n : e + 1;
                    continue;
                }
                size_t e = i + 1;
                char q = 0;
                while (e < n && (q || h[e] != '>')) {
                    if (q) { if (h[e] == q) q = 0; }
                    else if (h[e] == '"' || h[e] == '\'') q = h[e];
                    e++;
                }
                std::string tag = h.substr(i + 1, e - i - 1);
                i = e < n ? e + 1 : n;
                bool closing = !tag.empty() && tag[0] == '/';
                bool selfClose = !tag.empty() && tag.back() == '/';
                size_t ns = closing ? 1 : 0, ne = ns;
                while (ne < tag.size() && !isspace((unsigned char)tag[ne]) && tag[ne] != '/' && tag[ne] != '>') ne++;
                std::string name = lower(tag.substr(ns, ne - ns));
                size_t colon = name.find(':');
                if (colon != std::string::npos) name = name.substr(colon + 1);
                if (!closing && (name == "head" || name == "script" || name == "style" || name == "title") && !selfClose) {
                    size_t end = lower(h.substr(i, std::min(n - i, (size_t)200000))).find("</" + name);
                    if (end == std::string::npos) { i = n; continue; }
                    size_t gt = h.find('>', i + end);
                    i = gt == std::string::npos ? n : gt + 1;
                    continue;
                }
                if (closing) closeTag(name);
                else openTag(name, tag, selfClose);
                continue;
            }
            size_t e = h.find('<', i);
            if (e == std::string::npos) e = n;
            text(h.substr(i, e - i));
            i = e;
        }
        closeBlock();
    }

private:
    Doc& m_doc;
    std::string m_file;
    const std::unordered_map<std::string, CssFlags>& m_css;
    const std::map<std::string, std::string>& m_images;  // archive path -> path (exists)
    Part* m_out = nullptr;
    std::vector<Elem> m_stack;
    bool m_inBlock = false, m_space = false, m_pendingBreak = false;
    Block m_cur;
    int m_quote = 0, m_list = 0;
    std::vector<int> m_counters;
    std::string m_prefix;

    bool hidden() const { for (auto& e : m_stack) if (e.hidden) return true; return false; }
    bool pre() const { for (auto& e : m_stack) if (e.pre) return true; return false; }
    uint8_t style() const { uint8_t s = 0; for (auto& e : m_stack) s |= e.style; return s; }
    int link() const { for (int i = (int)m_stack.size() - 1; i >= 0; i--) if (m_stack[i].link >= 0) return m_stack[i].link; return -1; }
    int8_t align() const { for (int i = (int)m_stack.size() - 1; i >= 0; i--) if (m_stack[i].align >= 0) return m_stack[i].align; return AL_AUTO; }

    CssFlags flagsFor(const std::string& name, const std::string& tag) {
        CssFlags f;
        auto it = m_css.find(name);
        if (it != m_css.end()) f.merge(it->second);
        std::string cls = attr(tag, "class");
        size_t p = 0;
        while (p < cls.size()) {
            size_t sp = cls.find(' ', p);
            if (sp == std::string::npos) sp = cls.size();
            std::string c = lower(cls.substr(p, sp - p));
            p = sp + 1;
            if (c.empty()) continue;
            auto a = m_css.find("." + c);
            if (a != m_css.end()) f.merge(a->second);
            auto b = m_css.find(name + "." + c);
            if (b != m_css.end()) f.merge(b->second);
        }
        std::string st = attr(tag, "style");
        if (!st.empty()) f.merge(parseDecls(st));
        return f;
    }

    void openBlock() {
        Part& o = *m_out;
        m_cur = Block();
        // nearest block-level ancestor decides the type
        for (int i = (int)m_stack.size() - 1; i >= 0; i--) {
            const std::string& n = m_stack[i].name;
            if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6') { m_cur.type = B_HEAD; m_cur.level = n[1] - '0'; break; }
            if (n == "li") { m_cur.type = B_LIST; break; }
            if (n == "pre") { m_cur.type = B_PRE; break; }
            if (m_stack[i].block) break;
        }
        m_cur.align = align();
        m_cur.quote = (uint8_t)std::min(m_quote, 6);
        m_cur.list = (uint8_t)std::min(m_list, 6);
        m_cur.hidden = hidden();
        m_cur.breakBefore = m_pendingBreak;
        m_pendingBreak = false;
        m_cur.noIndent = m_pendingNoIndent;
        m_pendingNoIndent = false;
        m_cur.start = (uint32_t)o.text.size();
        m_cur.runBegin = (uint32_t)o.runs.size();
        m_inBlock = true;
        m_space = false;
        if (!m_prefix.empty()) {
            std::string p = m_prefix;
            m_prefix.clear();
            append(p, 0, -1);
        }
    }

    void closeBlock() {
        if (!m_inBlock) return;
        Part& o = *m_out;
        while (o.text.size() > m_cur.start && (o.text.back() == ' ' || o.text.back() == '\n')) {
            o.text.pop_back();
            if (!o.runs.empty() && o.runs.back().end > o.text.size()) {
                o.runs.back().end = (uint32_t)o.text.size();
                if (o.runs.back().end <= o.runs.back().start) o.runs.pop_back();
            }
        }
        m_cur.end = (uint32_t)o.text.size();
        m_cur.runEnd = (uint32_t)o.runs.size();
        m_inBlock = false;
        if (m_cur.end > m_cur.start) o.blocks.push_back(m_cur);
        else {
            o.runs.resize(m_cur.runBegin);
            m_pendingBreak |= m_cur.breakBefore;
        }
    }

    void append(const std::string& s, uint8_t st, int lk) {
        Part& o = *m_out;
        uint32_t a = (uint32_t)o.text.size();
        o.text += s;
        uint32_t b = (uint32_t)o.text.size();
        if (!o.runs.empty() && o.runs.size() > m_cur.runBegin && o.runs.back().end == a && o.runs.back().style == st &&
            o.runs.back().link == lk)
            o.runs.back().end = b;
        else o.runs.push_back({a, b, st, lk});
    }

    void text(const std::string& raw) {
        if (raw.empty()) return;
        bool isPre = pre();
        std::string piece;
        piece.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); i++) {
            char c = raw[i];
            if (c == '&') {
                size_t e = raw.find(';', i);
                if (e != std::string::npos && e - i < 12) {
                    decodeEntity(raw.substr(i + 1, e - i - 1), piece);
                    i = e;
                    continue;
                }
            }
            if (!isPre && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) {
                if (!piece.empty()) { emit(piece); piece.clear(); }
                m_space = true;
                continue;
            }
            if (isPre && c == '\r') continue;
            if (isPre && c == '\t') { piece += "    "; continue; }
            piece += c;
        }
        if (!piece.empty()) emit(piece);
    }

    void emit(const std::string& piece) {
        if (!m_inBlock) {
            if (piece.find_first_not_of(" \n") == std::string::npos) return;
            openBlock();
        }
        Part& o = *m_out;
        if (m_space && o.text.size() > m_cur.start && o.text.back() != ' ' && o.text.back() != '\n') {
            // the space belongs to the text before it (no bold/underlined gap before a bold word)
            bool hasRun = o.runs.size() > m_cur.runBegin;
            append(" ", hasRun ? o.runs.back().style : style(), hasRun ? o.runs.back().link : link());
        }
        m_space = false;
        append(piece, style(), link());
    }

    void anchor(const std::string& tag) {
        std::string id = attr(tag, "id");
        if (id.empty()) id = attr(tag, "name");
        if (!id.empty()) m_out->anchors.push_back({id, (uint32_t)m_out->text.size()});
    }

    void openTag(const std::string& name, const std::string& tag, bool selfClose) {
        anchor(tag);
        CssFlags f = flagsFor(name, tag);
        if (name == "br") {
            if (m_inBlock) { append("\n", 0, -1); m_space = false; }
            return;
        }
        if (name == "hr") {
            closeBlock();
            if (!hidden()) {
                Block b;
                b.type = B_HR;
                b.start = b.end = (uint32_t)m_out->text.size();
                b.runBegin = b.runEnd = (uint32_t)m_out->runs.size();
                b.breakBefore = m_pendingBreak;
                m_pendingBreak = false;
                m_out->blocks.push_back(b);
            }
            return;
        }
        if (name == "img" || name == "image") {
            std::string src = attr(tag, "src");
            if (src.empty()) src = attr(tag, "href");  // svg <image xlink:href>
            std::string path = resolveHref(m_file, src);
            size_t hash = path.find('#');
            if (hash != std::string::npos) path = path.substr(0, hash);
            if (!f.hidden && !hidden() && m_images.count(path)) {
                closeBlock();
                int idx = -1;
                for (int i = 0; i < (int)m_doc.images.size(); i++) if (m_doc.images[i].path == path) idx = i;
                if (idx < 0) { idx = (int)m_doc.images.size(); m_doc.images.push_back({path, 0, 0}); }
                Block b;
                b.type = B_IMAGE;
                b.image = idx;
                b.align = AL_CENTER;
                b.start = b.end = (uint32_t)m_out->text.size();
                b.runBegin = b.runEnd = (uint32_t)m_out->runs.size();
                b.breakBefore = m_pendingBreak;
                m_pendingBreak = false;
                m_out->blocks.push_back(b);
            }
            if (selfClose || name == "img") return;
        }
        static const char* blocks[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote", "section",
                                       "article", "header", "footer", "nav", "main", "figure", "figcaption", "dt", "dd",
                                       "pre", "table", "tr", "ul", "ol", "dl", "aside", "center", "address", "caption",
                                       "body", "hgroup", "html", "svg", "tbody", "thead"};
        bool isBlock = false;
        for (const char* b : blocks) if (name == b) { isBlock = true; break; }
        if (selfClose) return;
        Elem e;
        e.name = name;
        e.block = isBlock;
        e.hidden = f.hidden || name == "svg";
        if (name == "aside") {
            std::string t = lower(attr(tag, "type") + " " + attr(tag, "role"));
            if (t.find("footnote") != std::string::npos || t.find("endnote") != std::string::npos || t.find("rearnote") != std::string::npos)
                e.hidden = true;
        }
        if (name == "b" || name == "strong" || name == "th" || f.bold == 1) e.style |= RS_BOLD;
        if (name == "i" || name == "em" || name == "cite" || name == "dfn" || name == "var" || f.italic == 1) e.style |= RS_ITALIC;
        if (name == "sup") e.style |= RS_SUP;
        if (name == "sub") e.style |= RS_SUB;
        if (name == "code" || name == "kbd" || name == "samp" || name == "tt") e.style |= RS_MONO;
        if (name == "small" || f.small) e.style |= RS_SMALL;
        if (name == "big" || f.big) e.style |= RS_BIG;
        if (name == "center") e.align = AL_CENTER;
        if (f.align >= 0) e.align = f.align;
        if (name == "pre") e.pre = true;
        if (name == "a") {
            std::string href = attr(tag, "href");
            if (!href.empty()) {
                Link l;
                bool ext = href.find("://") != std::string::npos || startsWith(lower(href), "mailto:");
                l.external = ext;
                l.target = ext ? href : resolveHref(m_file, href);
                std::string t = lower(attr(tag, "type") + " " + attr(tag, "role"));
                l.noteref = t.find("noteref") != std::string::npos;
                e.link = (int)m_doc.links.size();
                m_doc.links.push_back(l);
                e.linkTextStart = (int)m_out->text.size();
            }
        }
        if (isBlock) {
            closeBlock();
            if (f.pageBreak) m_pendingBreak = true;
            if (name == "blockquote") m_quote++;
            if (name == "ul" || name == "ol") {
                m_list++;
                std::string st = attr(tag, "start");
                m_counters.push_back(name == "ol" ? (st.empty() ? 1 : atoi(st.c_str())) : -1);
            }
            if (name == "li") {
                int orphan = -1;
                int& c = m_counters.empty() ? orphan : m_counters.back();
                if (c >= 0) m_prefix = std::to_string(c++) + ". ";
                else m_prefix = m_list > 1 ? "\xE2\x97\xA6 " : "\xE2\x80\xA2 ";
            }
        }
        if (name == "td" || name == "th") {
            if (m_inBlock && m_out->text.size() > m_cur.start) { append("   ", 0, -1); m_space = false; }
        }
        m_stack.push_back(e);
        if (e.block && f.noIndent && m_inBlock) m_cur.noIndent = true;
        if (isBlock && f.noIndent) m_pendingNoIndent = true;
    }

    bool m_pendingNoIndent = false;

    void closeTag(const std::string& name) {
        int idx = -1;
        for (int i = (int)m_stack.size() - 1; i >= 0; i--) if (m_stack[i].name == name) { idx = i; break; }
        if (idx < 0) return;
        while ((int)m_stack.size() > idx) {
            Elem e = m_stack.back();
            if (e.link >= 0 && e.linkTextStart >= 0) {
                // short numeric/symbol links ("12", "[3]", "*") are footnote references
                std::string t = trim(m_out->text.substr(std::min((size_t)e.linkTextStart, m_out->text.size())));
                bool digit = t.find_first_of("0123456789*\xE2") != std::string::npos;
                if (!t.empty() && t.size() <= 6 && digit) m_doc.links[e.link].noteref = true;
                if (style() & RS_SUP) m_doc.links[e.link].noteref = true;
            }
            if (e.block) {
                closeBlock();
                if (e.name == "blockquote") m_quote = std::max(0, m_quote - 1);
                if (e.name == "ul" || e.name == "ol") {
                    m_list = std::max(0, m_list - 1);
                    if (!m_counters.empty()) m_counters.pop_back();
                }
            }
            m_stack.pop_back();
        }
    }
};

// ---------------------------------------------------------------- image headers

bool imageDims(const uint8_t* d, size_t n, int& w, int& h) {
    if (n >= 24 && memcmp(d, "\x89PNG", 4) == 0) {
        w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
        h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
        return w > 0 && h > 0;
    }
    if (n >= 10 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
        w = d[6] | (d[7] << 8);
        h = d[8] | (d[9] << 8);
        return w > 0 && h > 0;
    }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t p = 2;
        while (p + 9 < n) {
            if (d[p] != 0xFF) { p++; continue; }
            uint8_t m = d[p + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
            size_t len = (d[p + 2] << 8) | d[p + 3];
            if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = (d[p + 5] << 8) | d[p + 6];
                w = (d[p + 7] << 8) | d[p + 8];
                int o = jpegExifOrientation(d, n);
                if (o >= 5) std::swap(w, h);
                return w > 0 && h > 0;
            }
            p += 2 + len;
        }
        return false;
    }
    if (n >= 30 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0) {
        if (memcmp(d + 12, "VP8X", 4) == 0) {
            w = 1 + (d[24] | (d[25] << 8) | (d[26] << 16));
            h = 1 + (d[27] | (d[28] << 8) | (d[29] << 16));
            return true;
        }
        if (memcmp(d + 12, "VP8L", 4) == 0) {
            uint32_t b = d[21] | (d[22] << 8) | (d[23] << 16) | ((uint32_t)d[24] << 24);
            w = 1 + (b & 0x3FFF);
            h = 1 + ((b >> 14) & 0x3FFF);
            return true;
        }
        if (memcmp(d + 12, "VP8 ", 4) == 0) {
            w = (d[26] | (d[27] << 8)) & 0x3FFF;
            h = (d[28] | (d[29] << 8)) & 0x3FFF;
            return w > 0 && h > 0;
        }
    }
    return false;
}

// ---------------------------------------------------------------- EPUB

struct TocEntry { std::string target, title; int depth; };

static void parseNav(const std::string& nav, const std::string& navPath, std::vector<TocEntry>& out) {
    // only the <nav epub:type="toc"> section, if marked
    std::string x = nav;
    size_t tocPos = std::string::npos;
    for (size_t p = 0; (p = x.find("<nav", p)) != std::string::npos; p += 4) {
        size_t e = x.find('>', p);
        std::string tag = x.substr(p, e - p);
        if (tag.find("toc") != std::string::npos) { tocPos = p; break; }
    }
    if (tocPos != std::string::npos) {
        size_t end = x.find("</nav", tocPos);
        x = x.substr(tocPos, end == std::string::npos ? std::string::npos : end - tocPos);
    }
    int depth = 0;
    forEachTag(x, [&](const std::string& name, const std::string& tag, bool closing, size_t end) {
        if (name == "ol" || name == "ul") depth += closing ? -1 : 1;
        else if (name == "a" && !closing) {
            size_t close = x.find("</a", end);
            std::string t = stripTags(x.substr(end, close == std::string::npos ? 0 : close - end));
            std::string href = attr(tag, "href");
            if (!href.empty() && !t.empty()) out.push_back({resolveHref(navPath, href), t, std::max(0, depth - 1)});
        }
    });
}

static void parseNcx(const std::string& ncx, const std::string& ncxPath, std::vector<TocEntry>& out) {
    int depth = 0;
    std::string lastText;
    forEachTag(ncx, [&](const std::string& name, const std::string& tag, bool closing, size_t end) {
        if (name == "navpoint") {
            if (closing) depth--;
            else depth++;
        } else if (!closing && name == "text") {
            size_t close = ncx.find("</", end);
            lastText = stripTags(ncx.substr(end, close == std::string::npos ? 0 : close - end));
        } else if (!closing && name == "content") {
            std::string src = attr(tag, "src");
            if (!src.empty() && !lastText.empty()) out.push_back({resolveHref(ncxPath, src), lastText, std::max(0, depth - 1)});
        }
    });
}

static std::string fileOf(const std::string& target) {
    size_t h = target.find('#');
    return h == std::string::npos ? target : target.substr(0, h);
}

static void mergePart(Doc& doc, Part& part, const std::string& file, bool breakFirst, const std::string& tocTitle) {
    uint32_t base = (uint32_t)doc.text.size();
    // Synthesize a title when the file's opening blocks don't show one.
    bool hasTitle = false;
    int seen = 0;
    for (auto& b : part.blocks) {
        if (b.hidden) continue;
        if (++seen > 4) break;
        std::string t = part.text.substr(b.start, b.end - b.start);
        if (b.type == B_HEAD || (!tocTitle.empty() && normTitle(t) == normTitle(tocTitle))) { hasTitle = true; break; }
    }
    if (!hasTitle && !tocTitle.empty() && seen > 0) {
        Block h;
        h.type = B_HEAD;
        h.level = 1;
        h.start = base;
        doc.text += tocTitle;
        h.end = (uint32_t)doc.text.size();
        h.runBegin = (uint32_t)doc.runs.size();
        doc.runs.push_back({h.start, h.end, RS_BOLD, -1});
        h.runEnd = (uint32_t)doc.runs.size();
        h.chapterTitle = true;
        h.breakBefore = breakFirst;
        doc.blocks.push_back(h);
        doc.text += '\n';
        breakFirst = false;
        base = (uint32_t)doc.text.size();
    }
    doc.anchors.emplace(file, base);
    for (auto& a : part.anchors) doc.anchors.emplace(file + "#" + a.first, base + a.second);
    uint32_t runBase = (uint32_t)doc.runs.size();
    for (auto r : part.runs) { r.start += base; r.end += base; doc.runs.push_back(r); }
    bool first = true;
    size_t firstBlock = doc.blocks.size();
    for (auto b : part.blocks) {
        b.start += base;
        b.end += base;
        b.runBegin += runBase;
        b.runEnd += runBase;
        if (first && !b.hidden) { b.breakBefore |= breakFirst; first = false; }
        doc.blocks.push_back(b);
    }
    // mark the opening heading(s) as the chapter header
    int checked = 0;
    for (size_t i = firstBlock; i < doc.blocks.size() && checked < 4; i++) {
        Block& b = doc.blocks[i];
        if (b.hidden) continue;
        checked++;
        std::string t = part.text.substr(b.start - base, b.end - b.start);  // part text isn't appended yet
        bool titleLike = b.type == B_HEAD || (!tocTitle.empty() && normTitle(t) == normTitle(tocTitle));
        if (titleLike && t.size() < 200) b.chapterTitle = true;
        else if (b.type == B_IMAGE) continue;
        else if (checked > 1 && !titleLike) break;
    }
    doc.text += part.text;
    doc.text += '\n';
}

static bool loadEpub(Doc& doc) {
    std::map<std::string, std::string> files;
    std::map<std::string, std::string> images;
    archive* a = archive_read_new();
    archive_read_support_format_zip(a);
    if (archive_read_open_filename(a, doc.path.c_str(), 1 << 16) != ARCHIVE_OK) {
        archive_read_free(a);
        doc.err = "Could not open EPUB";
        return false;
    }
    archive_entry* e;
    std::vector<char> buf(1 << 16);
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        std::string name = archive_entry_pathname(e) ? archive_entry_pathname(e) : "";
        std::string x = ext(name);
        if (isImageExt(x) || x == "svg") {
            images[name] = name;
            archive_read_data_skip(a);
            continue;
        }
        if (x != "xml" && x != "opf" && x != "ncx" && x != "xhtml" && x != "html" && x != "htm" && x != "css") {
            archive_read_data_skip(a);
            continue;
        }
        std::string& d = files[name];
        la_ssize_t got;
        while ((got = archive_read_data(a, buf.data(), buf.size())) > 0) d.append(buf.data(), (size_t)got);
    }
    archive_read_free(a);

    std::string opfPath = attr(files["META-INF/container.xml"], "full-path");
    if (opfPath.empty() || !files.count(opfPath))
        for (auto& kv : files) if (ext(kv.first) == "opf") { opfPath = kv.first; break; }
    if (!files.count(opfPath)) { doc.err = "EPUB has no package file"; return false; }
    const std::string opf = files[opfPath];

    std::map<std::string, std::string> manifest;
    std::vector<std::string> spine;
    std::string ncxPath, navPath;
    forEachTag(opf, [&](const std::string& name, const std::string& tag, bool closing, size_t end) {
        if (closing) return;
        if (name == "item") {
            std::string id = attr(tag, "id"), href = resolveHref(opfPath, attr(tag, "href"));
            manifest[id] = href;
            if (attr(tag, "media-type") == "application/x-dtbncx+xml") ncxPath = href;
            if (attr(tag, "properties").find("nav") != std::string::npos) navPath = href;
        } else if (name == "itemref") {
            if (attr(tag, "linear") != "no") spine.push_back(attr(tag, "idref"));
        } else if (name == "spine" && ncxPath.empty()) {
            std::string toc = attr(tag, "toc");
            if (!toc.empty() && manifest.count(toc)) ncxPath = manifest[toc];
        } else if (name == "title" && doc.title.empty()) {
            size_t close = opf.find("</", end);
            doc.title = stripTags(opf.substr(end, close == std::string::npos ? 0 : close - end));
        } else if (name == "creator" && doc.author.empty()) {
            size_t close = opf.find("</", end);
            doc.author = stripTags(opf.substr(end, close == std::string::npos ? 0 : close - end));
        }
    });

    std::unordered_map<std::string, CssFlags> css;
    for (auto& kv : files) if (ext(kv.first) == "css") parseCss(kv.second, css);

    std::vector<TocEntry> toc;
    if (files.count(navPath)) parseNav(files[navPath], navPath, toc);
    if (toc.empty() && files.count(ncxPath)) parseNcx(files[ncxPath], ncxPath, toc);
    // first TOC title per file (for entries pointing at the file itself)
    std::map<std::string, std::string> fileTitle;
    for (auto& t : toc) {
        std::string f = fileOf(t.target);
        if (!fileTitle.count(f)) fileTitle[f] = t.title;
    }

    bool firstFile = true;
    for (auto& id : spine) {
        auto m = manifest.find(id);
        if (m == manifest.end() || !files.count(m->second)) continue;
        const std::string& file = m->second;
        // inline <style> blocks contribute rules too
        const std::string& html = files[file];
        for (size_t p = 0; (p = html.find("<style", p)) != std::string::npos;) {
            size_t s = html.find('>', p), e2 = html.find("</style", p);
            if (s == std::string::npos || e2 == std::string::npos) break;
            parseCss(html.substr(s + 1, e2 - s - 1), css);
            p = e2 + 1;
        }
        Part part;
        HtmlParser parser(doc, file, css, images);
        parser.parse(html, part);
        if (part.blocks.empty()) {
            doc.anchors.emplace(file, (uint32_t)doc.text.size());
            continue;
        }
        mergePart(doc, part, file, !firstFile, fileTitle.count(file) ? fileTitle[file] : "");
        firstFile = false;
    }
    if (doc.blocks.empty()) { doc.err = "EPUB contains no readable text"; return false; }

    // Table of contents, resolved to offsets; headings targeted by fragment links start pages.
    for (auto& t : toc) {
        uint32_t off;
        if (!doc.resolve(t.target, off)) continue;
        doc.toc.push_back({t.title, off, std::min(t.depth, 4)});
        int b = doc.blockAt(off);
        Block& bl = doc.blocks[b];
        if (t.target.find('#') != std::string::npos && (bl.type == B_HEAD || bl.chapterTitle) && bl.start >= off) {
            bl.breakBefore = b > 0;
            bl.chapterTitle = true;
        }
    }
    if (doc.toc.empty()) {
        for (auto& b : doc.blocks)
            if (b.chapterTitle && !b.hidden) doc.toc.push_back({doc.text.substr(b.start, std::min(120u, b.end - b.start)), b.start, 0});
    }
    // links into hidden notes are footnotes
    for (auto& l : doc.links) {
        if (l.external || l.noteref) continue;
        uint32_t off;
        if (doc.resolve(l.target, off) && doc.blocks[doc.blockAt(off)].hidden) l.noteref = true;
    }
    // image sizes from their headers (images are read on demand later)
    if (!doc.images.empty()) {
        std::map<std::string, int> want;
        for (int i = 0; i < (int)doc.images.size(); i++) want[doc.images[i].path] = i;
        a = archive_read_new();
        archive_read_support_format_zip(a);
        if (archive_read_open_filename(a, doc.path.c_str(), 1 << 16) == ARCHIVE_OK) {
            while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
                const char* n = archive_entry_pathname(e);
                auto it = n ? want.find(n) : want.end();
                if (it == want.end()) { archive_read_data_skip(a); continue; }
                std::string head;
                la_ssize_t got;
                while (head.size() < (256 << 10) && (got = archive_read_data(a, buf.data(), buf.size())) > 0)
                    head.append(buf.data(), (size_t)got);
                imageDims((const uint8_t*)head.data(), head.size(), doc.images[it->second].w, doc.images[it->second].h);
                archive_read_data_skip(a);
            }
        }
        archive_read_free(a);
    }
    return true;
}

std::string docImageData(const Doc& doc, int image) {
    if (image < 0 || image >= (int)doc.images.size()) return "";
    archive* a = archive_read_new();
    archive_read_support_format_zip(a);
    std::string out;
    if (archive_read_open_filename(a, doc.path.c_str(), 1 << 16) == ARCHIVE_OK) {
        archive_entry* e;
        std::vector<char> buf(1 << 16);
        while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
            const char* n = archive_entry_pathname(e);
            if (n && doc.images[image].path == n) {
                la_ssize_t got;
                while ((got = archive_read_data(a, buf.data(), buf.size())) > 0) out.append(buf.data(), (size_t)got);
                break;
            }
            archive_read_data_skip(a);
        }
    }
    archive_read_free(a);
    return out;
}

// ---------------------------------------------------------------- TXT / Markdown

static bool looksLikeChapter(const std::string& l) {
    std::string t = trim(l);
    if (t.empty() || t.size() > 70) return false;
    std::string lo = lower(t);
    static const char* starts[] = {"chapter ", "part ", "book ", "prologue", "epilogue", "interlude", "afterword",
                                   "foreword", "introduction", "preface", "act "};
    for (const char* s : starts) if (lo.rfind(s, 0) == 0) return true;
    // a lone roman numeral or number ("XII", "12.")
    std::string core = t;
    while (!core.empty() && (core.back() == '.' || core.back() == ':')) core.pop_back();
    if (!core.empty() && core.size() <= 6 && core.find_first_not_of("IVXLC") == std::string::npos) return true;
    return false;
}

static void loadTxt(Doc& doc, const std::string& raw) {
    std::string s = toUtf8(raw);
    bool md = ext(doc.path) == "md";
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= s.size()) {
        size_t nl = s.find('\n', p);
        if (nl == std::string::npos) nl = s.size();
        std::string l = s.substr(p, nl - p);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        for (auto& c : l) if (c == '\t') c = ' ';
        lines.push_back(l);
        p = nl + 1;
    }
    size_t blank = 0;
    for (auto& l : lines) if (l.find_first_not_of(' ') == std::string::npos) blank++;
    bool rewrap = lines.size() > 20 && blank * 8 > lines.size();

    auto addBlock = [&](const std::string& t, bool heading, int level) {
        Block b;
        b.type = heading ? B_HEAD : B_PARA;
        b.level = (uint8_t)level;
        b.start = (uint32_t)doc.text.size();
        b.runBegin = (uint32_t)doc.runs.size();
        doc.text += t;
        b.end = (uint32_t)doc.text.size();
        doc.runs.push_back({b.start, b.end, (uint8_t)(heading ? RS_BOLD : 0), -1});
        b.runEnd = (uint32_t)doc.runs.size();
        if (heading) {
            b.chapterTitle = level <= 2;
            b.breakBefore = !doc.blocks.empty() && level <= 2;
            if (level <= 2) doc.toc.push_back({t, b.start, level - 1});
        }
        doc.blocks.push_back(b);
        doc.text += '\n';
    };

    std::string para;
    auto flush = [&]() {
        std::string t = trim(para);
        para.clear();
        if (t.empty()) return;
        addBlock(t, false, 0);
    };
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& l = lines[i];
        bool isBlank = l.find_first_not_of(' ') == std::string::npos;
        if (md && !l.empty() && l[0] == '#') {
            flush();
            size_t lvl = l.find_first_not_of('#');
            addBlock(trim(l.substr(lvl == std::string::npos ? l.size() : lvl)), true, (int)std::min<size_t>(lvl, 6));
            continue;
        }
        bool prevBlank = i == 0 || lines[i - 1].find_first_not_of(' ') == std::string::npos;
        bool nextBlank = i + 1 >= lines.size() || lines[i + 1].find_first_not_of(' ') == std::string::npos;
        if (!md && prevBlank && nextBlank && looksLikeChapter(l)) {
            flush();
            addBlock(trim(l), true, 1);
            continue;
        }
        if (!rewrap) {
            flush();
            if (!isBlank) addBlock(trim(l), false, 0);
            continue;
        }
        if (isBlank) { flush(); continue; }
        if (!para.empty()) para += ' ';
        para += trim(l);
    }
    flush();
    doc.anchors[""] = 0;
}

bool loadDoc(const std::string& path, Doc& doc) {
    doc = Doc();
    doc.path = path;
    doc.epub = ext(path) == "epub";
    if (doc.epub) {
        if (!loadEpub(doc)) return false;
    } else {
        std::string raw;
        if (!readFile(path, raw)) { doc.err = "Could not read file"; return false; }
        loadTxt(doc, raw);
        if (doc.blocks.empty()) { doc.err = "File is empty"; return false; }
    }
    if (doc.title.empty()) doc.title = stem(path);
    return true;
}
