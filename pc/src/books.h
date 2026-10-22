// Book details for the shelf: title, author and cover from EPUB metadata, the
// first page of a CBZ. Other formats get their file name as the title (and
// the Switch draws PDF/CBR covers itself).
#pragma once
#include <string>

#include "images.h"

struct BookInfo {
    std::string title, author;
    Image cover;   // empty when there's none
    int pages = 0; // comics: pictures in the archive
};

bool readBook(const std::string& path, int format, int coverHeight, BookInfo& out);

// Exposed for the tests.
std::string xmlUnescape(const std::string& s);
std::string titleFromFileName(const std::string& stem);
