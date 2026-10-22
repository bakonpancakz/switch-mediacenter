// Book readers hosted by the Books tab.
#pragma once
#include "app.h"

struct BookReader {
    virtual ~BookReader() {}
    virtual bool ok() const = 0;
    virtual std::string error() const { return ""; }
    virtual void update() = 0;
    virtual void draw() = 0;
    virtual bool wantsClose() const = 0;  // user asked to go back to the library
    virtual int rotation() const = 0;
    virtual int brightness() const = 0;
    virtual void save() = 0;
};

BookReader* openTextReader(const std::string& path);
BookReader* openComicReader(const std::string& path);

// Reader palette: true black, or red-on-black night mode.
struct ReadPalette { SDL_Color bg, fg, dim, accent, sel, panel; };
const ReadPalette& readPalette();
int readColor();     // 0 white, 1 warm, 2 red
int readRotation();  // 90 or 270: books are read in portrait
void readerStatusBar(int y, const std::string& left);
