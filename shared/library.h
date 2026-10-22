// system.db: what the PC writes to the SD card and the Switch reads (and never
// writes): every item, its metadata and its pictures. The schema and the
// constants both sides agree on. Bump SCHEMA when it changes: the Switch
// refuses a library it doesn't know and asks for a newer app.
#pragma once
#include <cstdio>
#include <string>

namespace lib {

constexpr int SCHEMA = 2;

enum Category { C_FILES = 0, C_MEDIA = 1, C_BOOKS = 2, C_COUNT };
// The app's folder on the SD card (sdmc:/switch/mediacenter):
//   MediaCenter.nro                  the app
//   system.db                        the library, pictures and all (written by the PC)
//   .incoming/                       files on their way in during a sync
//   .media/files|media|books/<Name>  the synced folders
//   settings.ini, positions.txt, log.txt...  the Switch's own small text files
constexpr const char* DB_FILE = "system.db";
constexpr const char* INCOMING_DIR = ".incoming";
inline const char* categoryDir(int c) { return c == C_MEDIA ? ".media/media" : c == C_BOOKS ? ".media/books" : ".media/files"; }
inline const char* categoryLabel(int c) { return c == C_MEDIA ? "Media" : c == C_BOOKS ? "Books" : "Files"; }

// K_LOCKED: a locked folder (see lockbox.h). Its contents aren't in items:
// they're in its row of locks, encrypted.
enum Kind { K_DIR = 0, K_IMAGE = 1, K_VIDEO = 2, K_AUDIO = 3, K_BOOK = 4, K_LOCKED = 5 };
// Locked folders' files go in <mapping folder>/.locked/<lock id>/, under names that say nothing.
constexpr const char* LOCKED_DIR = ".locked";
enum BookFormat { B_EPUB = 1, B_PDF = 2, B_COMIC = 3, B_TEXT = 4 };

// Lowercase extension without the dot.
inline bool isImageExt(const std::string& e) {
    return e == "jpg" || e == "jpeg" || e == "jfif" || e == "png" || e == "gif" || e == "bmp" || e == "webp" || e == "avif" ||
           e == "heic" || e == "heif" || e == "tif" || e == "tiff" || e == "jxl" || e == "tga";
}
// What the Switch decodes itself. Other pictures are synced as a JPEG the PC
// makes (the original would be no use on the Switch).
inline bool switchDecodesImage(const std::string& e) {
    return e == "jpg" || e == "jpeg" || e == "jfif" || e == "png" || e == "gif" || e == "bmp" || e == "webp";
}
inline bool isVideoExt(const std::string& e) {
    return e == "mp4" || e == "mkv" || e == "webm" || e == "avi" || e == "mov" || e == "m4v" || e == "ts" || e == "m2ts" ||
           e == "wmv" || e == "flv" || e == "mpg" || e == "mpeg" || e == "3gp" || e == "ogv";
}
inline bool isAudioExt(const std::string& e) {
    return e == "mp3" || e == "m4a" || e == "aac" || e == "flac" || e == "ogg" || e == "oga" || e == "opus" || e == "wav" ||
           e == "wma" || e == "aiff" || e == "aif" || e == "alac" || e == "mka";
}
inline int bookFormat(const std::string& e) {
    if (e == "epub") return B_EPUB;
    if (e == "pdf") return B_PDF;
    if (e == "cbz" || e == "cbr" || e == "cb7" || e == "cbt") return B_COMIC;
    if (e == "txt" || e == "md") return B_TEXT;
    return 0;
}
// Copied next to videos, not listed: subtitles mpv picks up by name.
inline bool isSidecarExt(const std::string& e) {
    return e == "srt" || e == "ass" || e == "ssa" || e == "vtt" || e == "sub" || e == "idx" || e == "sup";
}

// Picture sizes, in pixels (the longer side).
constexpr int THUMB = 360;       // Files grid tiles
constexpr int VIEW = 2560;       // pictures converted for the Switch
constexpr int COVER_H = 480;     // book covers (height)
constexpr int WAVE_PEAKS = 120;  // audio waveform samples

inline const char* SCHEMA_SQL =
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;\n"
    // A mapping: category, name ("Remote") and its folder (".media/files/Remote"); item = its top folder
    "CREATE TABLE roots(id INTEGER PRIMARY KEY, category INTEGER NOT NULL, name TEXT NOT NULL, dir TEXT NOT NULL,"
    " item INTEGER NOT NULL);\n"
    // Every folder and file. path is relative to the app folder; sort is a
    // key that sorts naturally ("2" before "10") with plain byte comparison.
    "CREATE TABLE items(id INTEGER PRIMARY KEY, parent INTEGER NOT NULL, root INTEGER NOT NULL, kind INTEGER NOT NULL,"
    " name TEXT NOT NULL, sort TEXT NOT NULL, path TEXT NOT NULL, size INTEGER NOT NULL DEFAULT 0,"
    " mtime INTEGER NOT NULL DEFAULT 0, taken INTEGER NOT NULL DEFAULT 0, width INTEGER NOT NULL DEFAULT 0,"
    " height INTEGER NOT NULL DEFAULT 0, duration INTEGER NOT NULL DEFAULT 0, thumb INTEGER NOT NULL DEFAULT 0,"
    " wave BLOB, children INTEGER NOT NULL DEFAULT 0);\n"
    "CREATE INDEX items_by_parent ON items(parent, kind <> 0, sort);\n"
    "CREATE INDEX items_by_path ON items(path);\n"
    // Thumbnails, posters, covers (JPEG), each once: hash identifies the picture
    "CREATE TABLE pictures(id INTEGER PRIMARY KEY, hash TEXT NOT NULL UNIQUE, data BLOB NOT NULL,"
    " width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0);\n"
    // Every file the PC put on the card (media and subtitles) with its size and
    // date on the PC: how the next sync knows what's there without looking
    "CREATE TABLE synced(path TEXT PRIMARY KEY, size INTEGER NOT NULL, mtime INTEGER NOT NULL) WITHOUT ROWID;\n"
    "CREATE TABLE shows(id INTEGER PRIMARY KEY, root INTEGER NOT NULL, title TEXT NOT NULL, sort TEXT NOT NULL,"
    " year INTEGER NOT NULL DEFAULT 0, overview TEXT NOT NULL DEFAULT '', poster INTEGER NOT NULL DEFAULT 0,"
    " backdrop INTEGER NOT NULL DEFAULT 0, tmdb INTEGER NOT NULL DEFAULT 0, added INTEGER NOT NULL DEFAULT 0);\n"
    // number: 0 = specials, -1 = episodes whose season isn't known
    "CREATE TABLE seasons(id INTEGER PRIMARY KEY, show INTEGER NOT NULL, number INTEGER NOT NULL, title TEXT NOT NULL,"
    " ord INTEGER NOT NULL);\n"
    "CREATE TABLE episodes(id INTEGER PRIMARY KEY, show INTEGER NOT NULL, season INTEGER NOT NULL, number INTEGER NOT NULL,"
    " title TEXT NOT NULL DEFAULT '', overview TEXT NOT NULL DEFAULT '', still INTEGER NOT NULL DEFAULT 0,"
    " item INTEGER NOT NULL, ord INTEGER NOT NULL);\n"
    "CREATE INDEX episodes_by_show ON episodes(show, ord);\n"
    "CREATE TABLE movies(id INTEGER PRIMARY KEY, root INTEGER NOT NULL, title TEXT NOT NULL, sort TEXT NOT NULL,"
    " year INTEGER NOT NULL DEFAULT 0, overview TEXT NOT NULL DEFAULT '', poster INTEGER NOT NULL DEFAULT 0,"
    " backdrop INTEGER NOT NULL DEFAULT 0, runtime INTEGER NOT NULL DEFAULT 0, tmdb INTEGER NOT NULL DEFAULT 0,"
    " item INTEGER NOT NULL, added INTEGER NOT NULL DEFAULT 0);\n"
    "CREATE TABLE books(id INTEGER PRIMARY KEY, root INTEGER NOT NULL, item INTEGER NOT NULL, title TEXT NOT NULL,"
    " author TEXT NOT NULL DEFAULT '', sort TEXT NOT NULL, cover INTEGER NOT NULL DEFAULT 0, format INTEGER NOT NULL,"
    " pages INTEGER NOT NULL DEFAULT 0, added INTEGER NOT NULL DEFAULT 0);\n";

// Locked folders (lockbox.h), added to libraries made before them too:
//   locks: per locked folder (item), its salt, iteration count and PIN check
//   tag, whether the PIN is all digits (a number pad on the Switch), and its
//   items, encrypted (lockbox::encodeItems) with nonce
//   locked_pictures: its pictures, each encrypted with its own nonce; tag names
//   the picture without saying what it is
inline const char* LOCKS_SQL =
    "CREATE TABLE IF NOT EXISTS locks(item INTEGER PRIMARY KEY, lock TEXT NOT NULL, salt BLOB NOT NULL,"
    " iterations INTEGER NOT NULL, check_tag TEXT NOT NULL, numeric INTEGER NOT NULL DEFAULT 1, nonce INTEGER NOT NULL,"
    " data BLOB NOT NULL);\n"
    "CREATE TABLE IF NOT EXISTS locked_pictures(id INTEGER PRIMARY KEY, lock TEXT NOT NULL, tag TEXT NOT NULL,"
    " nonce INTEGER NOT NULL, data BLOB NOT NULL, width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0);\n"
    "CREATE INDEX IF NOT EXISTS locked_pictures_by_tag ON locked_pictures(lock, tag);\n";

// "Episode 10" -> "episode 0000000010": a key that sorts naturally byte by byte.
inline std::string sortKey(const std::string& s) {
    std::string k;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c >= '0' && c <= '9') {
            size_t j = i;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
            size_t z = i;
            while (z < j - 1 && s[z] == '0') z++;
            std::string digits = s.substr(z, j - z);
            if (digits.size() < 10) k += std::string(10 - digits.size(), '0');
            k += digits;
            i = j;
        } else {
            k += (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
            i++;
        }
    }
    return k;
}

}  // namespace lib
