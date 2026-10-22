#include "media/media_input.h"

#include <cstdio>

#include "core/locks.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace {

int readCb(void* opaque, uint8_t* buf, int n) {
    int64_t got = ((locks::LockedFile*)opaque)->read(buf, (size_t)n);
    return got > 0 ? (int)got : got == 0 ? AVERROR_EOF : AVERROR(EIO);
}

int64_t seekCb(void* opaque, int64_t offset, int whence) {
    auto* f = (locks::LockedFile*)opaque;
    if (whence & AVSEEK_SIZE) return (int64_t)f->size();
    int64_t to;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: to = offset; break;
    case SEEK_CUR: to = (int64_t)f->pos() + offset; break;
    case SEEK_END: to = (int64_t)f->size() + offset; break;
    default: return -1;
    }
    return to >= 0 && f->seek((uint64_t)to) ? to : -1;
}

}  // namespace

AVFormatContext* openMediaInput(const std::string& path, const AVIOInterruptCB* interrupt, int64_t probesize) {
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return nullptr;
    if (interrupt) fmt->interrupt_callback = *interrupt;
    if (probesize) fmt->probesize = probesize;
    uint64_t nonce;
    if (!locks::fileNonce(path, nonce)) {
        std::string url = "file:" + path;  // "sdmc:" would look like a protocol
        if (avformat_open_input(&fmt, url.c_str(), nullptr, nullptr) < 0) return nullptr;  // (it frees fmt)
        return fmt;
    }
    // a locked folder's: through our own reader
    auto* file = new locks::LockedFile;
    const int bufSize = 64 * 1024;
    uint8_t* buf = (uint8_t*)av_malloc(bufSize);
    AVIOContext* io = buf && file->open(path) ? avio_alloc_context(buf, bufSize, 0, file, readCb, nullptr, seekCb) : nullptr;
    if (!io) {
        av_free(buf);
        delete file;
        avformat_free_context(fmt);
        return nullptr;
    }
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {  // (frees fmt, but not our io)
        av_freep(&io->buffer);
        avio_context_free(&io);
        delete file;
        return nullptr;
    }
    return fmt;
}

void closeMediaInput(AVFormatContext** fmt) {
    if (!fmt || !*fmt) return;
    AVIOContext* io = ((*fmt)->flags & AVFMT_FLAG_CUSTOM_IO) ? (*fmt)->pb : nullptr;
    avformat_close_input(fmt);
    if (io) {
        delete (locks::LockedFile*)io->opaque;
        av_freep(&io->buffer);
        avio_context_free(&io);
    }
}
