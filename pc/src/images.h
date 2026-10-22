// Pictures on the PC: decoding with Windows Imaging Component (JPEG, PNG,
// GIF, BMP, TIFF, plus HEIC/WebP/AVIF when Windows has those codecs),
// EXIF orientation, scaling and JPEG encoding. Call imagesThreadInit() on
// every thread that uses these.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;  // w * h * 4
};

void imagesThreadInit();
void imagesThreadExit();

struct ImageInfo {
    int width = 0, height = 0;  // the full picture, as shown (after orientation)
    int64_t taken = 0;          // EXIF date taken, unix seconds (0 = none)
};

// Decodes a file (or memory), oriented upright, scaled so its longer side is at
// most maxSide (0 = full size).
bool loadImageFile(const std::string& path, int maxSide, Image& out, ImageInfo* info = nullptr);
bool loadImageMemory(const void* data, size_t len, int maxSide, Image& out, ImageInfo* info = nullptr);
// A 24- or 32-bit BMP (what ffmpeg writes with -c:v bmp).
bool decodeBmp(const std::vector<uint8_t>& bmp, Image& out);

Image scaleImage(const Image& in, int maxSide);  // high quality, never enlarges
bool encodeJpeg(const Image& img, float quality, std::vector<uint8_t>& out);

// Average brightness 0..255 and spread: tells a black or blank frame from a picture.
void imageStats(const Image& img, double& meanLuma, double& stdDev);
