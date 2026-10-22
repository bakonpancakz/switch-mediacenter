#include "fsx.h"

#include <windows.h>

#include <vector>

#include "util.h"

namespace fsx {

FILE* open(const std::string& path, const char* mode) {
    std::wstring m(mode, mode + strlen(mode));
    return _wfopen(winPath(path).c_str(), m.c_str());
}

int64_t size(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(winPath(path).c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

bool isDir(const std::string& path) {
    DWORD a = GetFileAttributesW(winPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool mkdirs(const std::string& path) {
    if (path.empty() || isDir(path)) return true;
    size_t s = path.find_last_of('/');
    if (s != std::string::npos && s > 0 && !mkdirs(path.substr(0, s))) return false;
    return CreateDirectoryW(winPath(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool removeAll(const std::string& path) {
    std::wstring w = winPath(path);
    DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(w.c_str()) != 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((w + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            removeAll(path + "/" + utf8(fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(w.c_str()) != 0;
}

bool removeEmptyDir(const std::string& path) { return RemoveDirectoryW(winPath(path).c_str()) != 0; }

bool rename(const std::string& from, const std::string& to) {
    return MoveFileExW(winPath(from).c_str(), winPath(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void walk(const std::string& root, const std::function<void(const std::string&, uint64_t, bool)>& fn) {
    std::vector<std::string> todo = {""};
    while (!todo.empty()) {
        std::string rel = todo.back();
        todo.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((winPath(rel.empty() ? root : root + "/" + rel) + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::string r = rel.empty() ? utf8(fd.cFileName) : rel + "/" + utf8(fd.cFileName);
            bool dir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            fn(r, dir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, dir);
            if (dir) todo.push_back(r);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

}  // namespace fsx
