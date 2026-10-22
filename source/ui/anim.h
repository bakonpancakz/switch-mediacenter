// Animated images (GIF, APNG, animated WebP) for the photo viewer. A worker
// thread decodes frames a few ahead of what's on screen and loops forever;
// texture() shows each one for its delay. Supports pause and seeking. Only the
// photo being viewed has one.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include "app.h"
#include "core/thread.h"

struct AVFormatContext;

// Looks at the file header: a GIF with more than one frame, a PNG with an
// animation chunk (APNG) or a WebP with the animation flag.
bool isAnimatedImage(const std::string& path);

// First frame of an animated WebP (SDL_image can't read those at all).
SDL_Surface* webpAnimFirstFrame(const uint8_t* data, size_t len);

class Animation {
public:
    // Frames are scaled down to fit maxW x maxH.
    Animation(const std::string& path, int maxW, int maxH);
    ~Animation();
    const std::string& path() const { return m_path; }
    // Main thread, every frame: the frame to show now, or nullptr until the
    // first one is ready (show the still image meanwhile).
    SDL_Texture* texture();
    bool failed();
    // It turned out to have one frame (found on its own thread, so making one
    // never waits on the card): the still image is all there is.
    bool still();

    void setPaused(bool p);
    bool paused() const { return m_paused; }
    // Known shortly after opening (a quick pass over the file); 0 until then.
    int durationMs();
    int frameCount();
    // The frame on screen
    int positionMs() const { return m_posMs; }
    int frameIndex() const { return m_index; }
    // Shows the frame playing at `ms` (even while paused).
    void seek(int ms);

private:
    struct Frame { SDL_Surface* surf; int delayMs, startMs, index; };
    enum Pass { P_END, P_QUIT, P_SEEK, P_FAIL };
    std::string m_path;
    int m_maxW, m_maxH;
    WorkerThread m_thread, m_measurer;
    std::mutex m_mx;
    std::condition_variable m_cv;
    std::deque<Frame> m_frames;  // decoded, waiting to be shown
    bool m_quit = false, m_failed = false;
    int m_kind = 0;                    // 0 not looked at yet, 1 animated, 2 a still
    bool m_firstQueued = false;        // the first frame is decoded: the measurer may start reading
    std::atomic<bool> m_stop{false};   // m_quit for FFmpeg's reads: closing interrupts them
    static int interrupted(void* self) { return ((Animation*)self)->m_stop.load(); }
    AVFormatContext* openInput();      // the file, with reads that stop when closing
    int m_seekTo = -1;           // worker: restart and skip to this time
    int m_totalMs = 0, m_count = 0;
    std::string m_data;          // WebP: the whole file (the decoder reads from it)

    // main thread
    SDL_Texture* m_tex = nullptr;
    int m_texW = 0, m_texH = 0;
    uint32_t m_nextAt = 0;
    bool m_paused = false, m_showNext = false;
    int m_posMs = 0, m_index = 0;

    void run();
    void measure();  // own thread: fills m_totalMs/m_count without decoding pixels
    Pass passFfmpeg(int startAt, int& frames);
    Pass passWebp(void* dec, int startAt, int& frames);
    // Worker: queue a frame (scaled), waiting while the queue is full.
    Pass push(SDL_Surface* s, int delayMs, int startMs, int index);
};
