// Opening a file for FFmpeg (the GIF/APNG player, the visualizer's decoder):
// plain files by name, and a locked folder's through locks::LockedFile, which
// decrypts as FFmpeg reads.
#pragma once
#include <string>

struct AVFormatContext;
struct AVIOInterruptCB;

// nullptr when it can't be opened. probesize 0: FFmpeg's own.
AVFormatContext* openMediaInput(const std::string& path, const AVIOInterruptCB* interrupt, int64_t probesize = 0);
void closeMediaInput(AVFormatContext** fmt);
