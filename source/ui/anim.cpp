#include "anim.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

static const size_t kQueue = 3;  // frames decoded ahead

static std::string readHead(const std::string& path, size_t n) {
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    s.resize(n);
    s.resize(fread(&s[0], 1, n, f));
    fclose(f);
    return s;
}

static bool isWebpAnim(const uint8_t* d, size_t len) {
    // RIFF <size> WEBP VP8X <size> <flags>: flag 0x02 = animation
    return len >= 21 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4) && !memcmp(d + 12, "VP8X", 4) && (d[20] & 0x02);
}

bool isAnimatedImage(const std::string& path) {
    std::string h = readHead(path, 256 * 1024);
    const uint8_t* d = (const uint8_t*)h.data();
    size_t n = h.size();
    if (n >= 6 && !memcmp(d, "GIF8", 4)) {
        // more than one graphic control block means more than one frame
        int gce = 0;
        for (size_t i = 0; i + 3 <= n && gce < 2; i++)
            if (d[i] == 0x21 && d[i + 1] == 0xF9 && d[i + 2] == 0x04) gce++;
        return gce >= 2;
    }
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        // APNG: an acTL chunk before the first IDAT
        size_t p = 8;
        while (p + 8 <= n) {
            size_t len = ((size_t)d[p] << 24) | ((size_t)d[p + 1] << 16) | ((size_t)d[p + 2] << 8) | d[p + 3];
            if (!memcmp(d + p + 4, "acTL", 4)) return true;
            if (!memcmp(d + p + 4, "IDAT", 4)) return false;
            p += 12 + len;
        }
        return false;
    }
    return isWebpAnim(d, n);
}

SDL_Surface* webpAnimFirstFrame(const uint8_t* data, size_t len) {
    if (!isWebpAnim(data, len)) return nullptr;
    WebPAnimDecoderOptions o;
    if (!WebPAnimDecoderOptionsInit(&o)) return nullptr;
    o.color_mode = MODE_RGBA;
    o.use_threads = 0;
    WebPData wd = {data, len};
    WebPAnimDecoder* dec = WebPAnimDecoderNew(&wd, &o);
    if (!dec) return nullptr;
    WebPAnimInfo info;
    SDL_Surface* s = nullptr;
    uint8_t* buf = nullptr;
    int ts = 0;
    if (WebPAnimDecoderGetInfo(dec, &info) && WebPAnimDecoderGetNext(dec, &buf, &ts)) {
        s = SDL_CreateRGBSurfaceWithFormat(0, (int)info.canvas_width, (int)info.canvas_height, 32, SDL_PIXELFORMAT_RGBA32);
        if (s)
            for (int y = 0; y < s->h; y++)
                memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, buf + (size_t)y * info.canvas_width * 4, info.canvas_width * 4);
    }
    WebPAnimDecoderDelete(dec);
    return s;
}

// ---------------------------------------------------------------- player

// Browsers show frames with a 0-10 ms delay at 100 ms; so do we.
static int sanitizeDelay(int ms) { return ms <= 10 ? 100 : ms; }

Animation::Animation(const std::string& path, int maxW, int maxH) : m_path(path), m_maxW(maxW), m_maxH(maxH) {
    m_thread.start([this] { run(); }, 512 * 1024);
    // measuring reads the whole file: on its own thread, so playback starts at once
    m_measurer.start([this] { measure(); }, 256 * 1024);
}

Animation::~Animation() {
    {
        std::lock_guard<std::mutex> l(m_mx);
        m_quit = true;
    }
    m_stop = true;  // a file being opened or read gives up now, not when it's done
    m_cv.notify_all();
    m_thread.join();
    m_measurer.join();
    for (auto& f : m_frames) SDL_FreeSurface(f.surf);
    if (m_tex) SDL_DestroyTexture(m_tex);
}

bool Animation::still() {
    std::lock_guard<std::mutex> l(m_mx);
    return m_kind == 2;
}

AVFormatContext* Animation::openInput() {
    std::string url = "file:" + m_path;  // "sdmc:" would look like a protocol
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return nullptr;
    fmt->interrupt_callback.callback = interrupted;
    fmt->interrupt_callback.opaque = this;
    // GIF and APNG say their size up front: probing megabytes of a big one from
    // the card only delayed the first frame
    fmt->probesize = 256 * 1024;
    if (avformat_open_input(&fmt, url.c_str(), nullptr, nullptr) < 0) return nullptr;  // (it frees fmt)
    return fmt;
}

bool Animation::failed() {
    std::lock_guard<std::mutex> l(m_mx);
    return m_failed;
}
int Animation::durationMs() {
    std::lock_guard<std::mutex> l(m_mx);
    return m_totalMs;
}
int Animation::frameCount() {
    std::lock_guard<std::mutex> l(m_mx);
    return m_count;
}

void Animation::setPaused(bool p) {
    if (p == m_paused) return;
    m_paused = p;
    if (!p) m_nextAt = g_ticks;  // resume with the next frame
}

void Animation::seek(int ms) {
    {
        std::lock_guard<std::mutex> l(m_mx);
        if (m_totalMs > 0) ms = SDL_clamp(ms, 0, m_totalMs - 1);
        for (auto& f : m_frames) SDL_FreeSurface(f.surf);
        m_frames.clear();
        m_seekTo = std::max(0, ms);
    }
    m_showNext = true;  // show where we landed even when paused
    m_posMs = ms;
    m_cv.notify_all();
}

SDL_Texture* Animation::texture() {
    Frame f = {nullptr, 0, 0, 0};
    {
        std::lock_guard<std::mutex> l(m_mx);
        if (m_frames.empty()) return m_tex;
        bool due = m_showNext || (!m_paused && (!m_tex || (int32_t)(g_ticks - m_nextAt) >= 0));
        if (!due) return m_tex;
        f = m_frames.front();
        m_frames.pop_front();
    }
    m_cv.notify_all();
    if (!m_tex || m_texW != f.surf->w || m_texH != f.surf->h) {
        if (m_tex) SDL_DestroyTexture(m_tex);
        m_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, f.surf->w, f.surf->h);
        if (m_tex) SDL_SetTextureBlendMode(m_tex, SDL_BLENDMODE_BLEND);
        m_texW = f.surf->w;
        m_texH = f.surf->h;
    }
    if (m_tex) SDL_UpdateTexture(m_tex, nullptr, f.surf->pixels, f.surf->pitch);
    // keep the rhythm, but don't race to catch up after a stall or a seek
    uint32_t base = (!m_showNext && m_nextAt && g_ticks - m_nextAt < 250) ? m_nextAt : g_ticks;
    m_nextAt = base + (uint32_t)f.delayMs;
    m_showNext = false;
    m_posMs = f.startMs;
    m_index = f.index;
    SDL_FreeSurface(f.surf);
    return m_tex;
}

Animation::Pass Animation::push(SDL_Surface* s, int delayMs, int startMs, int index) {
    float k = std::min({1.0f, m_maxW / (float)s->w, m_maxH / (float)s->h});
    if (k < 1) {
        SDL_Surface* sc = scaleSurface(s, std::max(1, (int)(s->w * k)), std::max(1, (int)(s->h * k)));
        if (sc) { SDL_FreeSurface(s); s = sc; }
    }
    std::unique_lock<std::mutex> l(m_mx);
    m_cv.wait(l, [this] { return m_quit || m_seekTo >= 0 || m_frames.size() < kQueue; });
    if (m_quit || m_seekTo >= 0) {
        SDL_FreeSurface(s);
        return m_quit ? P_QUIT : P_SEEK;
    }
    m_frames.push_back({s, delayMs, startMs, index});
    if (!m_firstQueued) {
        m_firstQueued = true;
        m_cv.notify_all();  // (the measurer waits for it)
    }
    return P_END;  // queued; keep going
}

void Animation::run() {
    bool animated = isAnimatedImage(m_path);
    {
        std::lock_guard<std::mutex> l(m_mx);
        m_kind = animated ? 1 : 2;
    }
    m_cv.notify_all();  // (the measurer waits for this)
    if (!animated) return;
    std::string head = readHead(m_path, 32);
    bool webp = isWebpAnim((const uint8_t*)head.data(), head.size());
    if (webp && (!readFile(m_path, m_data) || m_data.empty() || m_data.size() > (64u << 20))) {
        std::lock_guard<std::mutex> l(m_mx);
        m_failed = true;
        return;
    }
    WebPAnimDecoder* dec = nullptr;
    if (webp) {
        WebPAnimDecoderOptions o;
        if (WebPAnimDecoderOptionsInit(&o)) {
            o.color_mode = MODE_RGBA;
            o.use_threads = 0;
            WebPData wd = {(const uint8_t*)m_data.data(), m_data.size()};
            dec = WebPAnimDecoderNew(&wd, &o);
        }
    }
    int startAt = 0;
    bool failed = webp && !dec;
    while (!failed) {
        int frames = 0;
        Pass r = webp ? passWebp(dec, startAt, frames) : passFfmpeg(startAt, frames);
        if (r == P_QUIT) break;
        if (r == P_FAIL) { failed = true; break; }
        std::unique_lock<std::mutex> l(m_mx);
        if (r == P_END && frames <= 1) {
            // a still (or the end of a one-frame file): it stays until a seek or close
            m_cv.wait(l, [this] { return m_quit || m_seekTo >= 0; });
        }
        if (m_quit) break;
        if (m_seekTo >= 0) {
            startAt = m_seekTo;
            m_seekTo = -1;
        } else startAt = 0;  // loop forever, like a browser
    }
    if (dec) WebPAnimDecoderDelete(dec);
    if (failed) {
        appLog("animation failed: %s", m_path.c_str());
        std::lock_guard<std::mutex> l(m_mx);
        m_failed = true;
    }
}

// Total length and frame count, from the container alone (no pixel decoding).
void Animation::measure() {
    {
        std::unique_lock<std::mutex> l(m_mx);
        // after the first frame: until then the card's time is all for showing it
        m_cv.wait(l, [this] { return m_quit || m_kind == 2 || m_failed || (m_kind == 1 && m_firstQueued); });
        if (m_quit || m_kind != 1 || m_failed) return;
    }
    int total = 0, count = 0;
    std::string head = readHead(m_path, 32), data;
    bool webp = isWebpAnim((const uint8_t*)head.data(), head.size());
    if (webp) {
        if (!readFile(m_path, data)) return;
        WebPData wd = {(const uint8_t*)data.data(), data.size()};
        WebPDemuxer* dmx = WebPDemux(&wd);
        WebPIterator it;
        if (dmx && WebPDemuxGetFrame(dmx, 1, &it)) {
            do {
                total += sanitizeDelay(it.duration);
                count++;
            } while (WebPDemuxNextFrame(&it));
            WebPDemuxReleaseIterator(&it);
        }
        if (dmx) WebPDemuxDelete(dmx);
    } else {
        if (AVFormatContext* fmt = openInput()) {
            int si = avformat_find_stream_info(fmt, nullptr) >= 0 ? av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
            if (si >= 0) {
                double tb = av_q2d(fmt->streams[si]->time_base);
                AVPacket* pkt = av_packet_alloc();
                while (av_read_frame(fmt, pkt) >= 0) {
                    if (pkt->stream_index == si) {
                        total += sanitizeDelay(pkt->duration > 0 ? (int)(pkt->duration * tb * 1000 + 0.5) : 0);
                        count++;
                    }
                    av_packet_unref(pkt);
                    std::lock_guard<std::mutex> l(m_mx);
                    if (m_quit) break;
                }
                av_packet_free(&pkt);
            }
            avformat_close_input(&fmt);
        }
    }
    std::lock_guard<std::mutex> l(m_mx);
    m_totalMs = total;
    m_count = count;
}

Animation::Pass Animation::passWebp(void* decp, int startAt, int& frames) {
    WebPAnimDecoder* dec = (WebPAnimDecoder*)decp;
    WebPAnimDecoderReset(dec);
    WebPAnimInfo info;
    if (!WebPAnimDecoderGetInfo(dec, &info)) return P_FAIL;
    int prev = 0, start = 0;
    while (WebPAnimDecoderHasMoreFrames(dec)) {
        uint8_t* buf = nullptr;
        int ts = 0;
        if (!WebPAnimDecoderGetNext(dec, &buf, &ts)) return frames > 0 ? P_END : P_FAIL;
        int delay = sanitizeDelay(ts - prev);  // timestamps are each frame's end time
        prev = ts;
        int index = frames++;
        int frameStart = start;
        start += delay;
        if (start <= startAt) {  // before the seek target: decoded (frames build on each other), not shown
            std::lock_guard<std::mutex> l(m_mx);
            if (m_quit) return P_QUIT;
            if (m_seekTo >= 0) return P_SEEK;
            continue;
        }
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, (int)info.canvas_width, (int)info.canvas_height, 32, SDL_PIXELFORMAT_RGBA32);
        if (!s) return P_FAIL;
        for (int y = 0; y < s->h; y++)
            memcpy((uint8_t*)s->pixels + (size_t)y * s->pitch, buf + (size_t)y * info.canvas_width * 4, info.canvas_width * 4);
        Pass r = push(s, delay, frameStart, index);
        if (r != P_END) return r;
    }
    return P_END;
}

// GIF and APNG: FFmpeg's decoders handle frame disposal and blending, so every
// frame comes out as a full picture. Each pass opens the file from the start.
Animation::Pass Animation::passFfmpeg(int startAt, int& frames) {
    AVFormatContext* fmt = openInput();
    if (!fmt) return m_stop ? P_QUIT : P_FAIL;
    AVCodecContext* cc = nullptr;
    AVFrame* fr = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    SwsContext* sws = nullptr;
    Pass result = P_FAIL;
    int start = 0;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        const AVCodec* codec = nullptr;
        int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (si < 0 || !codec) break;
        cc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc, fmt->streams[si]->codecpar);
        cc->thread_count = 1;
        if (avcodec_open2(cc, codec, nullptr) < 0) break;
        double tb = av_q2d(fmt->streams[si]->time_base);
        result = P_END;
        bool eof = false;
        while (result == P_END && !eof) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) {
                eof = true;
                avcodec_send_packet(cc, nullptr);  // drain
            } else if (pkt->stream_index != si) {
                av_packet_unref(pkt);
                continue;
            } else {
                avcodec_send_packet(cc, pkt);
                av_packet_unref(pkt);
            }
            while (result == P_END && avcodec_receive_frame(cc, fr) == 0) {
                int delay = sanitizeDelay(fr->duration > 0 ? (int)(fr->duration * tb * 1000 + 0.5) : 0);
                int index = frames++;
                int frameStart = start;
                start += delay;
                if (start <= startAt) {  // before the seek target: decoded, not shown
                    std::lock_guard<std::mutex> l(m_mx);
                    if (m_quit) result = P_QUIT;
                    else if (m_seekTo >= 0) result = P_SEEK;
                    continue;
                }
                int w = fr->width, h = fr->height;
                float k = std::min({1.0f, m_maxW / (float)w, m_maxH / (float)h});
                int tw = std::max(1, (int)(w * k)), th = std::max(1, (int)(h * k));
                sws = sws_getCachedContext(sws, w, h, (AVPixelFormat)fr->format, tw, th, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                           nullptr, nullptr, nullptr);
                SDL_Surface* s = sws ? SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_RGBA32) : nullptr;
                if (!s) { result = frames > 1 ? P_END : P_FAIL; eof = true; break; }
                uint8_t* dst[4] = {(uint8_t*)s->pixels, nullptr, nullptr, nullptr};
                int ls[4] = {s->pitch, 0, 0, 0};
                sws_scale(sws, fr->data, fr->linesize, 0, h, dst, ls);
                result = push(s, delay, frameStart, index);
            }
        }
    } while (0);
    sws_freeContext(sws);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    if (cc) avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    if (result == P_END && frames == 0) return P_FAIL;
    return result;
}
