// Videos and audio through ffmpeg.exe / ffprobe.exe: what a file is (length,
// size, date), a still for its thumbnail (skipping black frames) and a
// waveform for audio. Also decodes pictures Windows has no codec for.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "images.h"

struct FfTools {
    std::wstring ffmpeg, ffprobe;
    bool ok() const { return !ffmpeg.empty() && !ffprobe.empty(); }
};
// Looks in `dir` (if set), next to this exe, in the usual tools folder, then on PATH.
FfTools findFfmpeg(const std::string& dir);

struct MediaInfo {
    int64_t durationMs = 0;
    int width = 0, height = 0;  // as displayed (rotation applied)
    bool video = false, audio = false;
    int64_t taken = 0;          // creation_time, unix seconds (0 = unknown)
};

bool probeMedia(const FfTools& t, const std::string& path, MediaInfo& out, const std::atomic<bool>* cancel = nullptr);
// A representative frame: tries a few points and takes the first that isn't
// black or blank. maxSide limits the longer side.
bool videoFrame(const FfTools& t, const std::string& path, const MediaInfo& info, int maxSide, Image& out,
                const std::atomic<bool>* cancel = nullptr);
// `count` peaks (0-255, loudest = 255) across the whole file.
bool audioPeaks(const FfTools& t, const std::string& path, int count, std::vector<uint8_t>& out,
                const std::atomic<bool>* cancel = nullptr);
// The first frame of a picture ffmpeg can read (AVIF, JPEG XL, HEIC...).
bool ffmpegImage(const FfTools& t, const std::string& path, int maxSide, Image& out, const std::atomic<bool>* cancel = nullptr);
