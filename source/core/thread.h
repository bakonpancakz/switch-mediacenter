// std::thread look-alike with an explicit stack size. Worker threads here
// decode video frames, JPEGs and do TLS handshakes, which need more stack than
// a default thread is guaranteed to have on the Switch.
//
// Threads are always joined, never detached: libnx doesn't support detaching
// (its thread_detach is a stub), so a detached thread's stack stays mapped
// after it ends. On exit the launcher (sphaira's hbl) then can't reuse that
// memory and crashes with 0xD401.
#pragma once
#include <pthread.h>
#include <atomic>
#include <functional>
#include <memory>
#include <cstdio>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <chrono>
#include <thread>
#endif

class WorkerThread {
public:
    WorkerThread() = default;
    // Waits for the thread (see waitAndJoin).
    ~WorkerThread() { waitAndJoin(3000); }
    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    bool start(std::function<void()> fn, size_t stack = 256 * 1024) {
        waitAndJoin(3000);  // a restarted thread object: finish the previous one
        auto* s = new Start{std::move(fn), std::make_shared<std::atomic<bool>>(false)};
        m_finished = s->finished;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, (stack + 0xFFF) & ~(size_t)0xFFF);
        int rc = pthread_create(&m_t, &a, entry, s);
        pthread_attr_destroy(&a);
        if (rc) { delete s; m_finished.reset(); return false; }
        m_started = true;
        m_done = false;
        return true;
    }
    bool joinable() const { return m_started && !m_done; }
    // Has the thread's function returned? (join() then won't block)
    bool finished() const { return m_finished && m_finished->load(); }
    void join() { if (joinable()) { pthread_join(m_t, nullptr); m_done = true; } }

    // Joins once the thread has finished, waiting up to timeoutMs. A thread
    // that's still running after that is detached as a last resort (logged:
    // its stack leaks), rather than hanging whoever is waiting.
    void waitAndJoin(int timeoutMs) {
        if (!joinable()) return;
        for (int waited = 0; !finished() && waited < timeoutMs; waited += 10) sleepMs(10);
        if (finished()) join();
        else {
            fprintf(stderr, "WorkerThread: still running after %d ms, detaching\n", timeoutMs);
            pthread_detach(m_t);
            m_done = true;
        }
    }

private:
    struct Start {
        std::function<void()> fn;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    static void* entry(void* p) {
        spreadCores();
        auto* s = (Start*)p;
        s->fn();
        *s->finished = true;
        delete s;
        return nullptr;
    }
    static void sleepMs(int ms) {
#ifdef __SWITCH__
        svcSleepThread((u64)ms * 1000000ULL);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
    }
    pthread_t m_t{};
    std::shared_ptr<std::atomic<bool>> m_finished;

public:
    // New threads start on the app's default core (core 0), next to the UI loop.
    // Let them run on cores 0-2, preferring 1 and 2 so the UI keeps core 0.
    static void spreadCores() {
#ifdef __SWITCH__
        static std::atomic<int> next{0};
        int core = 1 + (next++ & 1);
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 0x7);
#endif
    }

private:
    bool m_started = false, m_done = true;
};
