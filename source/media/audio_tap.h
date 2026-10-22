// What the visualizer hears for a song playing through mpv.
//
// mpv has no way to hand its audio to the app, so a thread decodes the same
// file again (cheap for music: a few percent of a core), a little ahead, and
// the main thread feeds it into a SpectrumTap in step with mpv's playback
// position: the Spotify tab's visualizer then works on it unchanged.
#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>

#include "core/thread.h"
#include "spotify/sp_dsp.hpp"

class AudioTap {
public:
    AudioTap();
    ~AudioTap();
    // Main thread, every frame a song shows: the file, mpv's position in it
    // (seconds) and whether it's playing. A jump in position (a seek) starts
    // decoding again from there.
    void follow(const std::string& path, double posSec, bool playing);
    void stop();  // nothing to follow (decoding stops)
    const dsp::SpectrumTap& tap() const { return m_tap; }

    static constexpr int kRate = 48000;

private:
    dsp::SpectrumTap m_tap;
    WorkerThread m_thread;
    std::mutex m_mx;
    std::condition_variable m_cv;
    bool m_quit = false;
    uint32_t m_gen = 0;       // bumped for every new file or seek: the decoder starts over
    std::string m_path;
    double m_seekTo = 0;
    std::deque<float> m_buf;  // decoded mono samples...
    int64_t m_bufStart = 0;   // ...the first of which is this sample of the song
    int64_t m_fed = 0;        // the tap has everything before this sample

    // main thread
    std::string m_following;
    double m_shown = 0, m_reported = -1;

    void restart(const std::string& path, double at);  // under m_mx
    void run();
    void decode(const std::string& path, double at, uint32_t gen);
};
