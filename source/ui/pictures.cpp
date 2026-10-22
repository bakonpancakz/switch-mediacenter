// Pictures for the screens: thumbnails, posters and covers the PC made, read
// out of system.db by id (one open file, no file per picture).
//
// Two worker threads read and decode; the main thread uploads a few textures a
// frame (picturesTick). Only what's asked for gets loaded: everything on screen
// is asked for every frame, and a request not repeated in the last couple of
// frames (scrolled past, another folder or tab) is dropped instead of read. (It
// used to be kept for 300 ms, which through a folder of thousands meant
// hundreds of pictures already gone by, loading ahead of the ones on screen.)
#include "app.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "core/library.h"
#include "core/thread.h"

namespace {

struct Entry {
    SDL_Texture* tex = nullptr;
    bool done = false, failed = false;
    uint32_t used = 0;
};

struct Loaded {
    int64_t id;
    SDL_Surface* surf;
    uint32_t gen;
};

std::mutex s_mx;
std::condition_variable s_cv;
std::vector<int64_t> s_queue;  // most recent request at the back
std::vector<Loaded> s_results;
std::unordered_map<int64_t, Entry> s_pics;
std::unordered_map<int64_t, uint32_t> s_wantedAt;  // the frame each picture was last asked for
std::set<int64_t> s_busy;
uint32_t s_frame = 1;  // counted by picturesTick
uint32_t s_gen = 1;  // bumped by picturesReset: older work is thrown away
bool s_quit = false;
WorkerThread s_threads[2];
uint32_t s_holdUntil = 0;  // main thread: nothing new is queued before this (a fast scroll)

bool stale(uint32_t wantedAt) { return s_frame - wantedAt > 2; }

void worker() {
    for (;;) {
        int64_t id;
        uint32_t gen;
        {
            std::unique_lock<std::mutex> l(s_mx);
            for (;;) {
                s_cv.wait(l, [] { return s_quit || !s_queue.empty(); });
                if (s_quit) return;
                id = s_queue.back();
                s_queue.pop_back();
                gen = s_gen;
                auto w = s_wantedAt.find(id);
                if (w != s_wantedAt.end() && !stale(w->second)) break;
                // not on screen any more: skipped (it's asked for again if it comes back)
                if (w != s_wantedAt.end()) s_wantedAt.erase(w);
                s_busy.erase(id);
            }
        }
        SDL_Surface* s = nullptr;
        std::string data;
        if (Library::get().pictureData(id, data)) s = loadImageMem(data.data(), data.size(), 0, 0);
        if (!s) appLog("picture %lld: couldn't read it", (long long)id);
        std::lock_guard<std::mutex> l(s_mx);
        s_busy.erase(id);
        s_wantedAt.erase(id);
        s_results.push_back({id, s, gen});
    }
}

}  // namespace

void picturesInit() {
    for (auto& t : s_threads) t.start(worker, 512 * 1024);
}

void picturesShutdown() {
    {
        std::lock_guard<std::mutex> l(s_mx);
        s_quit = true;
        s_queue.clear();
    }
    s_cv.notify_all();
    for (auto& t : s_threads) t.join();
    for (auto& r : s_results)
        if (r.surf) SDL_FreeSurface(r.surf);
    s_results.clear();
    for (auto& kv : s_pics)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    s_pics.clear();
}

void picturesReset() {
    std::lock_guard<std::mutex> l(s_mx);
    s_gen++;
    s_queue.clear();
    s_busy.clear();
    s_wantedAt.clear();
    for (auto& kv : s_pics)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    s_pics.clear();
}

void picturesTick() {
    // a few new textures a frame: after a hitch a screenful can be ready at
    // once, and uploading them all in one frame made another hitch
    std::vector<Loaded> got;
    uint32_t gen;
    {
        std::lock_guard<std::mutex> l(s_mx);
        s_frame++;
        size_t n = std::min<size_t>(4, s_results.size());
        got.assign(s_results.begin(), s_results.begin() + n);
        s_results.erase(s_results.begin(), s_results.begin() + n);
        gen = s_gen;
    }
    for (auto& r : got) {
        if (r.gen != gen) {  // from before the library was reopened
            if (r.surf) SDL_FreeSurface(r.surf);
            continue;
        }
        Entry& e = s_pics[r.id];
        e.done = true;
        e.used = g_ticks;
        e.failed = !r.surf;
        if (r.surf) {
            if (e.tex) SDL_DestroyTexture(e.tex);
            e.tex = textureFromSurface(r.surf);
            SDL_FreeSurface(r.surf);
        }
    }
    // the least recently used textures go once there are many
    if (s_pics.size() > 500) {
        std::vector<std::pair<uint32_t, int64_t>> order;
        for (auto& kv : s_pics)
            if (kv.second.done) order.push_back({kv.second.used, kv.first});
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < order.size() && s_pics.size() > 380; i++) {
            auto it = s_pics.find(order[i].second);
            if (it->second.tex) SDL_DestroyTexture(it->second.tex);
            s_pics.erase(it);
        }
    }
}

void picturesHoldOff() { s_holdUntil = g_ticks + 150; }

SDL_Texture* pictureGet(int64_t id) {
    if (id <= 0) return nullptr;
    // Only arrived pictures have an entry. (Every request used to make one: past
    // 500 of them, from scrolling through a big folder, the cache trimming below
    // threw out every loaded picture each frame, the ones on screen included.)
    auto found = s_pics.find(id);
    if (found != s_pics.end()) {
        found->second.used = g_ticks;
        return found->second.tex;
    }
    // scrolling fast: it'll be off screen before it could load, and the
    // workers are kept for where the scroll stops
    if ((int32_t)(s_holdUntil - g_ticks) > 0) return nullptr;
    std::lock_guard<std::mutex> l(s_mx);
    s_wantedAt[id] = s_frame;
    if (s_busy.count(id)) {
        // queued already: bump it to the front of the line
        auto it = std::find(s_queue.begin(), s_queue.end(), id);
        if (it != s_queue.end()) {
            s_queue.erase(it);
            s_queue.push_back(id);
        }
        return nullptr;
    }
    s_queue.push_back(id);
    s_busy.insert(id);
    if (s_queue.size() > 300) {
        s_busy.erase(s_queue.front());
        s_queue.erase(s_queue.begin());
    }
    s_cv.notify_one();
    return nullptr;
}

bool pictureFailed(int64_t id) {
    auto it = s_pics.find(id);
    return id <= 0 || (it != s_pics.end() && it->second.failed);
}

SDL_Texture* bookCover(const std::string& path) {
    // asked every frame by the reader's front page: look the book up once per library
    static std::string s_path;
    static uint32_t s_version = 0;
    static int64_t s_thumb = 0;
    if (path != s_path || s_version != Library::get().version()) {
        s_path = path;
        s_version = Library::get().version();
        LibItem it;
        s_thumb = Library::get().itemByPath(path, it) ? it.thumb : 0;
    }
    return pictureGet(s_thumb);
}
