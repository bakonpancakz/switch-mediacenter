// Host-test stand-in for source/app.h: just the helpers the EPUB loader uses.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

std::string lower(std::string s);
std::string ext(const std::string& path);
std::string baseName(const std::string& path);
std::string dirName(const std::string& path);
std::string stem(const std::string& path);
std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& p);
bool readFile(const std::string& path, std::string& out);
uint32_t utf8Next(const std::string& s, size_t& i);
void appendUtf8(std::string& out, uint32_t cp);
bool isImageExt(const std::string& e);
int jpegExifOrientation(const uint8_t* d, size_t n);
void appLog(const char* fmt, ...);
