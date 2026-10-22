// Small helpers for the PC tool: text conversion, paths, logging, hashing and
// running ffmpeg.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

std::wstring wide(const std::string& utf8);
std::string utf8(const std::wstring& w);
// Paths are kept as UTF-8 with forward slashes; winPath turns one into what
// Windows calls take: backslashes, and the \\?\ prefix when an absolute path is
// long (or always, with longForm).
std::wstring winPath(const std::string& path, bool longForm = false);
std::string forwardSlashes(std::string path);
std::string lower(std::string s);
std::string extOf(const std::string& name);   // "Clip.MP4" -> "mp4"
std::string stemOf(const std::string& name);  // "a/b/Clip.mp4" -> "Clip"
std::string baseOf(const std::string& path);  // "a/b/Clip.mp4" -> "Clip.mp4"
std::string trim(const std::string& s);
// A '/'-separated path as it's named on the Switch's card: plain ASCII. Neither
// the console nor sphaira's MTP can find a file with other characters in its
// name, so each dot-separated part that has them keeps its ASCII (the rest
// becomes '_') and gains a short hash of the original: "Star☆Song.opus" is
// "Star_Song~3fa1c2d0.opus". Parts are hashed alone, so a subtitle still
// starts with its video's name. The library keeps the real names.
std::string cardSafe(const std::string& path);
std::string formatBytes(double bytes);
int64_t unixNow();
uint64_t monoMs();

// %LOCALAPPDATA%\MediaCenterSync (created on first use), '/'-separated UTF-8
std::string dataDir();

// A line in the log (dataDir()/log.txt, and the window when one listens).
void logLine(const std::string& s);
void setLogListener(std::function<void(const std::string&)> fn);

// Hex SHA-256 of data, the first 32 hex digits (128 bits): picture identities.
std::string hashHex(const void* data, size_t len);

bool readWhole(const std::string& path, std::vector<uint8_t>& out);
bool writeWhole(const std::string& path, const void* data, size_t len);

// Runs a program (no window, below-normal priority) and collects its standard
// output. Killed after timeoutMs. Returns the exit code, or -1 if it couldn't
// run or timed out.
int runProcess(const std::wstring& exe, const std::vector<std::wstring>& args, std::vector<uint8_t>& out, int timeoutMs,
               const std::atomic<bool>* cancel = nullptr);
