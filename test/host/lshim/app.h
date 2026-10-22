// Host shim of app.h for the library test: what core/library.cpp and
// core/sqlite_vfs.cpp use, with the app's folder in the current directory.
#pragma once
#include <sys/stat.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#define APP_DIR "libtest_app"

inline bool fileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
inline void appLog(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("  log: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
inline void svcSleepThread(unsigned long long ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
