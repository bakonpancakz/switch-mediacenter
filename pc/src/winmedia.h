// What Windows itself can do with media files, so ffmpeg is only a fallback:
// - thumbnails from the Shell (whatever thumbnail handlers are installed, like
//   Icaros for videos, HEIC, comics; the same pictures Explorer shows)
// - lengths, sizes, rotation and decoded audio from Media Foundation
// - the first page of a PDF from Windows' own PDF renderer
// Call winMediaThreadInit() on each thread that uses these.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "images.h"
#include "media.h"

void winMediaThreadInit();
void winMediaThreadExit();

// The Shell's thumbnail, fitted inside size x size. Only real thumbnails (no file-type icons).
bool shellThumbnail(const std::string& path, int size, Image& out);
// Length, picture size (as shown: rotation applied) and which streams there are.
bool mfProbe(const std::string& path, MediaInfo& out);
// `count` loudness peaks (0-255) across the whole file, and its length.
bool mfAudioPeaks(const std::string& path, int count, std::vector<uint8_t>& out, int64_t* durationMs,
                  const std::atomic<bool>* cancel = nullptr);
// A PDF's first page, rendered `height` pixels tall.
bool pdfFirstPage(const std::string& path, int height, Image& out);
