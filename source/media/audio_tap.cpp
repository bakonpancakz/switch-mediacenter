#include "media/audio_tap.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "app.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace {
const int64_t kAhead = 2 * AudioTap::kRate;  // decoded at most this far past what's playing
}

AudioTap::AudioTap() { m_thread.start([this] { run(); }, 256 * 1024); }

AudioTap::~AudioTap() {
    {
        std::lock_guard<std::mutex> l(m_mx);
        m_quit = true;
    }
    m_cv.notify_all();
    m_thread.join();
}

void AudioTap::restart(const std::string& path, double at) {
    m_path = path;
    m_seekTo = at;
    m_gen++;
    m_buf.clear();
    m_bufStart = m_fed = (int64_t)(at * kRate);
    m_cv.notify_all();
}

void AudioTap::stop() {
    if (m_following.empty()) return;
    m_following.clear();
    std::lock_guard<std::mutex> l(m_mx);
    restart("", 0);
}

void AudioTap::follow(const std::string& path, double posSec, bool playing) {
    std::lock_guard<std::mutex> l(m_mx);
    if (path != m_following) {
        m_following = path;
        m_shown = m_reported = posSec;
        restart(path, posSec);
        return;
    }
    // mpv reports the position every so often: in between, it moves on with the clock
    if (playing) m_shown += g_dt;
    if (posSec != m_reported) {
        m_reported = posSec;
        if (fabs(posSec - m_shown) > 0.15) m_shown = posSec;  // a seek, or drifted
        else m_shown += (posSec - m_shown) * 0.2;
    }
    int64_t target = (int64_t)(m_shown * kRate);
    if (target < m_fed - kRate / 10 || target > m_fed + kRate) {  // jumped: decode from there
        restart(path, m_shown);
        return;
    }
    // what played since last frame goes into the tap (silence where the decoder is behind)
    for (int64_t i = m_fed; i < target; i++) {
        int64_t k = i - m_bufStart;
        m_tap.push(k >= 0 && k < (int64_t)m_buf.size() ? m_buf[k] : 0.0f);
    }
    if (target > m_fed) m_fed = target;
    while (!m_buf.empty() && m_bufStart < m_fed) {
        m_buf.pop_front();
        m_bufStart++;
    }
    m_cv.notify_all();  // room to decode further
}

void AudioTap::run() {
    uint32_t handled = 0;
    for (;;) {
        std::string path;
        double at;
        uint32_t gen;
        {
            std::unique_lock<std::mutex> l(m_mx);
            m_cv.wait(l, [&] { return m_quit || m_gen != handled; });
            if (m_quit) return;
            path = m_path;
            at = m_seekTo;
            gen = handled = m_gen;
        }
        if (!path.empty()) decode(path, at, gen);
    }
}

namespace {
struct Watch {
    std::mutex* mx;
    const bool* quit;
    const uint32_t* gen;
    uint32_t mine;
};
int interrupted(void* p) {
    auto* w = (Watch*)p;
    std::lock_guard<std::mutex> l(*w->mx);
    return *w->quit || *w->gen != w->mine;
}
}  // namespace

void AudioTap::decode(const std::string& path, double at, uint32_t gen) {
    Watch watch{&m_mx, &m_quit, &m_gen, gen};
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return;
    fmt->interrupt_callback.callback = interrupted;
    fmt->interrupt_callback.opaque = &watch;
    std::string url = "file:" + path;  // "sdmc:" would look like a protocol
    if (avformat_open_input(&fmt, url.c_str(), nullptr, nullptr) < 0) return;  // (it frees fmt)
    AVCodecContext* cc = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    std::vector<float> out;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0) break;
        const AVCodec* codec = nullptr;
        int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (si < 0 || !codec) break;
        cc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc, fmt->streams[si]->codecpar);
        cc->thread_count = 1;
        if (avcodec_open2(cc, codec, nullptr) < 0) break;
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        if (swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_FLT, kRate, &cc->ch_layout, cc->sample_fmt, cc->sample_rate, 0, nullptr) < 0 ||
            swr_init(swr) < 0)
            break;
        if (at > 0.05) av_seek_frame(fmt, -1, (int64_t)(at * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
        double tb = av_q2d(fmt->streams[si]->time_base);
        bool placed = false;  // the first frame's time set where the samples go
        int64_t next = 0;     // the song sample the next decoded one is
        while (!interrupted(&watch)) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) break;  // the end (the tap goes quiet on its own)
            if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
            avcodec_send_packet(cc, pkt);
            av_packet_unref(pkt);
            while (avcodec_receive_frame(cc, fr) == 0) {
                if (!placed) {
                    int64_t ts = fr->best_effort_timestamp;
                    next = ts != AV_NOPTS_VALUE ? (int64_t)llround(ts * tb * kRate) : (int64_t)(at * kRate);
                    placed = true;
                }
                int cap = swr_get_out_samples(swr, fr->nb_samples);
                out.resize(std::max(cap, 1));
                uint8_t* dst = (uint8_t*)out.data();
                int n = swr_convert(swr, &dst, cap, (const uint8_t**)fr->extended_data, fr->nb_samples);
                av_frame_unref(fr);
                if (n <= 0) continue;
                std::unique_lock<std::mutex> l(m_mx);
                if (m_quit || m_gen != gen) break;
                // into the buffer, lined up by song position (what's already played is skipped)
                for (int i = 0; i < n; i++, next++) {
                    if (next < m_bufStart + (int64_t)m_buf.size()) continue;
                    if (m_buf.empty()) m_bufStart = next;
                    m_buf.push_back(out[i]);
                }
                // no further ahead than needed
                m_cv.wait(l, [&] { return m_quit || m_gen != gen || m_bufStart + (int64_t)m_buf.size() < m_fed + kAhead; });
            }
        }
    } while (false);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
}
