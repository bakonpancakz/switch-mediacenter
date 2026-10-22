// Book document model shared by the EPUB/TXT loader and the text reader.
// All text lives in one UTF-8 string; blocks and styled runs index into it,
// so a reading position is a single byte offset that survives relayout.
#pragma once
#include "app.h"
#include <unordered_map>

enum RunStyle : uint8_t {
    RS_BOLD = 1, RS_ITALIC = 2, RS_SUP = 4, RS_SUB = 8, RS_MONO = 16, RS_SMALL = 32, RS_BIG = 64,
};

struct Run {
    uint32_t start, end;  // byte range in Doc::text
    uint8_t style;
    int32_t link;         // index into Doc::links, -1 for none
};

enum BlockType : uint8_t { B_PARA, B_HEAD, B_LIST, B_PRE, B_HR, B_IMAGE };
enum BlockAlign : uint8_t { AL_AUTO, AL_LEFT, AL_CENTER, AL_RIGHT };

struct Block {
    uint8_t type = B_PARA;
    uint8_t level = 0;        // heading level 1..6
    uint8_t align = AL_AUTO;
    uint8_t quote = 0;        // blockquote nesting
    uint8_t list = 0;         // list nesting
    bool breakBefore = false; // start on a new page (chapter boundary)
    bool chapterTitle = false;
    bool hidden = false;      // footnote bodies: shown in popups, not in the flow
    bool noIndent = false;
    uint32_t start = 0, end = 0;
    uint32_t runBegin = 0, runEnd = 0;
    int32_t image = -1;
};

struct Link {
    std::string target;   // "path/in/epub.xhtml#id" or an external URL
    bool external = false;
    bool noteref = false; // looks like a footnote reference
};

struct ImageRes { std::string path; int w = 0, h = 0; };

struct Chapter {
    std::string title;
    uint32_t offset = 0;
    int depth = 0;
};

struct Doc {
    std::string path, title, author, err;
    bool epub = false;
    std::string text;
    std::vector<Run> runs;
    std::vector<Block> blocks;
    std::vector<Link> links;
    std::vector<ImageRes> images;
    std::vector<Chapter> toc;
    std::unordered_map<std::string, uint32_t> anchors;  // "file#id" or "file" -> offset

    // Resolves a link target to an offset (false if unknown).
    bool resolve(const std::string& target, uint32_t& off) const;
    int blockAt(uint32_t off) const;  // block containing (or after) offset
    // Text of a footnote starting at off (used for popups).
    std::string noteText(uint32_t off) const;
};

bool loadDoc(const std::string& path, Doc& doc);
// Reads one image from the EPUB (empty on failure).
std::string docImageData(const Doc& doc, int image);
bool imageDims(const uint8_t* d, size_t n, int& w, int& h);
