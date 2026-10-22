// File operations with UTF-8, forward-slash paths (see winPath in util.h).
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace fsx {

FILE* open(const std::string& path, const char* mode);
int64_t size(const std::string& path);  // -1 when it isn't a file
bool isDir(const std::string& path);
bool mkdirs(const std::string& path);           // the folder and its parents
bool removeAll(const std::string& path);        // a file, or a folder and everything in it; true if gone
bool removeEmptyDir(const std::string& path);   // false when it has something in it
bool rename(const std::string& from, const std::string& to);  // replaces `to`
// Every entry under root (not root itself), parents before children.
// rel is '/'-separated and relative to root.
void walk(const std::string& root, const std::function<void(const std::string& rel, uint64_t size, bool dir)>& fn);

}  // namespace fsx
