// libmpv wrapper: decoding (hardware where possible), subtitles via libass,
// and rendering into an SDL texture inside SDL's own GL context.
#pragma once
#include "app.h"

struct Track {
    int64_t id = 0;
    std::string type, title, lang, codec;
    bool selected = false, external = false;
    std::string label() const;
};

class Video {
public:
    static Video& get();
    bool init();          // lazily creates mpv (main thread)
    void shutdown();

    // How a file is played. Media: the Media tab's player (subtitles found next
    // to the video, preferred languages). Clip: a video or song shown in Files,
    // played as it is. Either way it starts from defaults (nothing a previous
    // file changed, like zoom, aspect, speed or a chosen track, carries over).
    enum Mode { MEDIA, CLIP };
    void load(const std::string& path, double startSec, Mode mode);
    void stop();
    void poll();          // handle mpv events (main thread, every frame)
    void render();        // draw a new frame into the texture if one is ready (preRender)
    SDL_Texture* texture() const { return m_tex; }

    bool active() const { return m_active; }
    bool loaded() const { return m_loaded; }
    bool frameReady() const { return m_frameReady; }  // the texture holds this file's picture (not the last one's)
    bool ended() const { return m_eof; }
    bool paused() const { return m_paused; }
    double position() const { return m_pos; }
    double duration() const { return m_dur; }
    std::string error() const { return m_error; }
    const std::string& path() const { return m_path; }

    void setPause(bool p);
    void seek(double sec, bool relative, bool exact = true);
    std::vector<Track> tracks();
    void selectTrack(const std::string& type, int64_t id);  // id 0 = off
    double getDouble(const char* prop, double def = 0);
    void setDouble(const char* prop, double v);
    void setString(const char* prop, const std::string& v);
    void command(std::initializer_list<const char*> args);

private:
    struct mpv_handle* m_mpv = nullptr;
    struct mpv_render_context* m_ctx = nullptr;
    SDL_Texture* m_tex = nullptr;
    unsigned m_fbo = 0;
    bool m_sw = false, m_active = false, m_loaded = false, m_eof = false, m_paused = false, m_frameReady = false;
    double m_pos = 0, m_dur = 0;
    std::string m_path, m_error;
    bool createGl();
    bool createSw();
};
