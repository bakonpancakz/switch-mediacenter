#include "video.h"
#include <GLES2/gl2.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>
extern "C" {
#include <libavutil/log.h>
}
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <cstring>

#include "core/locks.h"

// A locked folder's files go to mpv as mclock://<path>: it reads them through
// locks::LockedFile, which decrypts at whatever offset it asks for (so seeking works).
static const char* LOCKED_PROTOCOL = "mclock";

static int64_t lockedRead(void* cookie, char* buf, uint64_t n) { return ((locks::LockedFile*)cookie)->read(buf, (size_t)n); }
static int64_t lockedSeek(void* cookie, int64_t to) {
    return to >= 0 && ((locks::LockedFile*)cookie)->seek((uint64_t)to) ? to : MPV_ERROR_GENERIC;
}
static int64_t lockedSize(void* cookie) { return (int64_t)((locks::LockedFile*)cookie)->size(); }
static void lockedClose(void* cookie) { delete (locks::LockedFile*)cookie; }

static int openLocked(void*, char* uri, mpv_stream_cb_info* info) {
    std::string prefix = std::string(LOCKED_PROTOCOL) + "://";
    std::string path = std::string(uri).substr(std::min(prefix.size(), strlen(uri)));
    auto* f = new locks::LockedFile;
    if (!f->open(path)) {
        delete f;
        return MPV_ERROR_LOADING_FAILED;
    }
    info->cookie = f;
    info->read_fn = lockedRead;
    info->seek_fn = lockedSeek;
    info->size_fn = lockedSize;
    info->close_fn = lockedClose;
    return 0;
}

static std::atomic<bool> s_redraw{false};

std::string Track::label() const {
    std::string s = title;
    if (!lang.empty()) s = s.empty() ? lang : s + " (" + lang + ")";
    if (s.empty()) s = (type == "sub" ? "Subtitle " : "Track ") + std::to_string(id);
    if (!codec.empty()) s += "  \xC2\xB7  " + codec;
    if (external) s += "  \xC2\xB7  external";
    return s;
}

Video& Video::get() {
    static Video v;
    return v;
}

// ---------------------------------------------------------------- GL plumbing
// mpv draws with raw GL in SDL's context. SDL caches GL state, so everything
// mpv may touch is saved before and restored after each frame.

typedef void (*PFN_GetIntegerv)(GLenum, GLint*);
typedef void (*PFN_GetFloatv)(GLenum, GLfloat*);
typedef GLboolean (*PFN_IsEnabled)(GLenum);
typedef void (*PFN_Enable)(GLenum);
typedef void (*PFN_UseProgram)(GLuint);
typedef void (*PFN_BindFramebuffer)(GLenum, GLuint);
typedef void (*PFN_BindBuffer)(GLenum, GLuint);
typedef void (*PFN_ActiveTexture)(GLenum);
typedef void (*PFN_BindTexture)(GLenum, GLuint);
typedef void (*PFN_Viewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_Scissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (*PFN_BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
typedef void (*PFN_BlendEquationSeparate)(GLenum, GLenum);
typedef void (*PFN_ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFN_PixelStorei)(GLenum, GLint);
typedef void (*PFN_GetVertexAttribiv)(GLuint, GLenum, GLint*);
typedef void (*PFN_VertexAttribArray)(GLuint);
typedef void (*PFN_ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (*PFN_GenFramebuffers)(GLsizei, GLuint*);
typedef void (*PFN_DeleteFramebuffers)(GLsizei, const GLuint*);
typedef void (*PFN_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFN_CheckFramebufferStatus)(GLenum);
typedef void (*PFN_BindVertexArray)(GLuint);

static struct {
    PFN_GetIntegerv GetIntegerv;
    PFN_GetFloatv GetFloatv;
    PFN_IsEnabled IsEnabled;
    PFN_Enable Enable, Disable;
    PFN_UseProgram UseProgram;
    PFN_BindFramebuffer BindFramebuffer;
    PFN_BindBuffer BindBuffer;
    PFN_ActiveTexture ActiveTexture;
    PFN_BindTexture BindTexture;
    PFN_Viewport Viewport;
    PFN_Scissor Scissor;
    PFN_BlendFuncSeparate BlendFuncSeparate;
    PFN_BlendEquationSeparate BlendEquationSeparate;
    PFN_ClearColor ClearColor;
    PFN_PixelStorei PixelStorei;
    PFN_GetVertexAttribiv GetVertexAttribiv;
    PFN_VertexAttribArray EnableVertexAttribArray, DisableVertexAttribArray;
    PFN_ColorMask ColorMask;
    PFN_GenFramebuffers GenFramebuffers;
    PFN_DeleteFramebuffers DeleteFramebuffers;
    PFN_FramebufferTexture2D FramebufferTexture2D;
    PFN_CheckFramebufferStatus CheckFramebufferStatus;
    PFN_BindVertexArray BindVertexArray;
} gl;

static bool loadGl() {
#define L(name) gl.name = (decltype(gl.name))SDL_GL_GetProcAddress("gl" #name)
    L(GetIntegerv); L(GetFloatv); L(IsEnabled); L(Enable); L(Disable); L(UseProgram); L(BindFramebuffer);
    L(BindBuffer); L(ActiveTexture); L(BindTexture); L(Viewport); L(Scissor); L(BlendFuncSeparate);
    L(BlendEquationSeparate); L(ClearColor); L(PixelStorei); L(GetVertexAttribiv); L(EnableVertexAttribArray);
    L(DisableVertexAttribArray); L(ColorMask); L(GenFramebuffers); L(DeleteFramebuffers); L(FramebufferTexture2D);
    L(CheckFramebufferStatus);
    gl.BindVertexArray = (PFN_BindVertexArray)SDL_GL_GetProcAddress("glBindVertexArray");
    if (!gl.BindVertexArray) gl.BindVertexArray = (PFN_BindVertexArray)SDL_GL_GetProcAddress("glBindVertexArrayOES");
#undef L
    return gl.GetIntegerv && gl.BindFramebuffer && gl.GenFramebuffers && gl.FramebufferTexture2D && gl.UseProgram &&
           gl.GetVertexAttribiv && gl.BlendFuncSeparate;
}

struct GlState {
    GLint program, fbo, arrayBuf, elemBuf, active, tex[4], vp[4], sc[4];
    GLint bSrcRgb, bDstRgb, bSrcA, bDstA, bEqRgb, bEqA, unpack, pack;
    GLfloat clear[4];
    GLboolean blend, scissor, depth, cull, stencil, mask[4];
    GLint attribs[8];
    void save() {
        gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
        gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        gl.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuf);
        gl.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
        for (int i = 0; i < 4; i++) {
            gl.ActiveTexture(GL_TEXTURE0 + i);
            gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]);
        }
        gl.GetIntegerv(GL_VIEWPORT, vp);
        gl.GetIntegerv(GL_SCISSOR_BOX, sc);
        gl.GetIntegerv(GL_BLEND_SRC_RGB, &bSrcRgb);
        gl.GetIntegerv(GL_BLEND_DST_RGB, &bDstRgb);
        gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &bSrcA);
        gl.GetIntegerv(GL_BLEND_DST_ALPHA, &bDstA);
        gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &bEqRgb);
        gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &bEqA);
        gl.GetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
        gl.GetIntegerv(GL_PACK_ALIGNMENT, &pack);
        gl.GetFloatv(GL_COLOR_CLEAR_VALUE, clear);
        GLint m[4];
        gl.GetIntegerv(GL_COLOR_WRITEMASK, m);
        for (int i = 0; i < 4; i++) mask[i] = (GLboolean)m[i];
        blend = gl.IsEnabled(GL_BLEND);
        scissor = gl.IsEnabled(GL_SCISSOR_TEST);
        depth = gl.IsEnabled(GL_DEPTH_TEST);
        cull = gl.IsEnabled(GL_CULL_FACE);
        stencil = gl.IsEnabled(GL_STENCIL_TEST);
        for (int i = 0; i < 8; i++) gl.GetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribs[i]);
    }
    void restore() {
        if (gl.BindVertexArray) gl.BindVertexArray(0);
        gl.UseProgram(program);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, arrayBuf);
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, elemBuf);
        for (int i = 0; i < 4; i++) {
            gl.ActiveTexture(GL_TEXTURE0 + i);
            gl.BindTexture(GL_TEXTURE_2D, tex[i]);
        }
        gl.ActiveTexture(active);
        gl.Viewport(vp[0], vp[1], vp[2], vp[3]);
        gl.Scissor(sc[0], sc[1], sc[2], sc[3]);
        gl.BlendFuncSeparate(bSrcRgb, bDstRgb, bSrcA, bDstA);
        gl.BlendEquationSeparate(bEqRgb, bEqA);
        gl.PixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        gl.PixelStorei(GL_PACK_ALIGNMENT, pack);
        gl.ClearColor(clear[0], clear[1], clear[2], clear[3]);
        gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
        auto set = [](GLenum cap, GLboolean on) { if (on) gl.Enable(cap); else gl.Disable(cap); };
        set(GL_BLEND, blend);
        set(GL_SCISSOR_TEST, scissor);
        set(GL_DEPTH_TEST, depth);
        set(GL_CULL_FACE, cull);
        set(GL_STENCIL_TEST, stencil);
        for (int i = 0; i < 8; i++) {
            if (attribs[i]) gl.EnableVertexAttribArray(i);
            else gl.DisableVertexAttribArray(i);
        }
    }
};

static void* getProc(void*, const char* name) { return SDL_GL_GetProcAddress(name); }
static void onUpdate(void*) { s_redraw = true; }

static const int VW = 1280, VH = 720;

bool Video::createGl() {
    if (!loadGl()) { appLog("video: GL entry points missing"); return false; }
    m_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, VW, VH);
    if (!m_tex) return false;
    // find the GL name behind the SDL texture and wrap it in our own FBO
    SDL_RenderFlush(g_ren);
    GlState st;
    st.save();
    if (SDL_GL_BindTexture(m_tex, nullptr, nullptr) != 0) {
        appLog("video: SDL_GL_BindTexture failed: %s", SDL_GetError());
        st.restore();
        SDL_DestroyTexture(m_tex);
        m_tex = nullptr;
        return false;
    }
    GLint name = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &name);
    SDL_GL_UnbindTexture(m_tex);
    GLuint fbo = 0;
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)name, 0);
    GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    st.restore();
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        appLog("video: framebuffer incomplete (0x%x)", status);
        gl.DeleteFramebuffers(1, &fbo);
        SDL_DestroyTexture(m_tex);
        m_tex = nullptr;
        return false;
    }
    m_fbo = fbo;

    mpv_opengl_init_params glInit = {getProc, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    st.save();
    int rc = mpv_render_context_create(&m_ctx, m_mpv, params);
    st.restore();
    if (rc < 0) {
        appLog("video: GL render context failed: %s", mpv_error_string(rc));
        gl.DeleteFramebuffers(1, &fbo);
        m_fbo = 0;
        SDL_DestroyTexture(m_tex);
        m_tex = nullptr;
        m_ctx = nullptr;
        return false;
    }
    return true;
}

bool Video::createSw() {
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&m_ctx, m_mpv, params) < 0) return false;
    m_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XBGR8888, SDL_TEXTUREACCESS_STREAMING, VW, VH);
    m_sw = true;
    return m_tex != nullptr;
}

// mpv points FFmpeg's (process-wide) log hook at its own logger. The app also
// decodes video stills with FFmpeg on its own threads, and their messages then
// went into mpv from outside it, at the same moment mpv was opening a file:
// suspected of the hang when playing a video from the photo viewer. So the hook
// comes back here: errors only, a couple a second.
static void appAvLog(void*, int level, const char* fmt, va_list ap) {
    if (level > AV_LOG_ERROR) return;
    static std::mutex mx;
    static uint32_t second = 0;
    static int count = 0;
    std::lock_guard<std::mutex> l(mx);
    uint32_t now = SDL_GetTicks() / 1000;
    if (now != second) { second = now; count = 0; }
    if (++count > 2) return;
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    appLog("ffmpeg: %s", buf);
}

bool Video::init() {
    if (m_mpv) return m_ctx != nullptr;
    m_mpv = mpv_create();
    if (!m_mpv) { m_error = "Could not start mpv"; return false; }
    auto opt = [&](const char* k, const char* v) {
        int rc = mpv_set_option_string(m_mpv, k, v);
        if (rc < 0) appLog("mpv option %s=%s: %s", k, v, mpv_error_string(rc));
    };
    opt("config", "no");
    opt("config-dir", "romfs:/mpv");
    opt("sub-fonts-dir", "romfs:/mpv");
    opt("osd-fonts-dir", "romfs:/mpv");
    opt("sub-font", "Noto Sans");
    opt("vo", "libmpv");
    opt("ao", "hos,sdl");
    // The console mixes at 48 kHz in stereo. Handing the renderer anything else
    // (most music is 44.1 kHz) left the rate conversion to it, and music came
    // out distorted; mpv's own resampler does it cleanly first, as Spotify's does.
    opt("audio-samplerate", "48000");
    opt("audio-channels", "stereo");
    opt("audio-format", "s16");
    opt("hwdec", cfgStr("media.hwdec", "auto-copy").c_str());
    // mpv's own default list. "all" also sent Motion JPEG (camera .MOV clips) to
    // the hardware decoder, which crashed the console on the first frame.
    opt("hwdec-codecs", cfgStr("media.hwdec_codecs", "h264,vc1,hevc,vp8,vp9,av1,prores,ffv1").c_str());
    opt("profile", "fast");
    opt("vd-lavc-threads", "4");
    opt("input-default-bindings", "no");
    opt("input-vo-keyboard", "no");
    opt("osc", "no");
    opt("terminal", "no");
    opt("idle", "yes");
    opt("keep-open", "yes");
    opt("sub-auto", "fuzzy");
    // off: "fuzzy" grabs any audio file containing the video name (a ringtone
    // next to the music video), and mpv then prefers it over the real track
    opt("audio-file-auto", cfgStr("media.audio_file_auto", "no").c_str());
    opt("embeddedfonts", "yes");
    opt("sub-scale", std::to_string(cfgInt("media.sub_scale", 100) / 100.0).c_str());
    opt("demuxer-max-bytes", "48MiB");
    opt("demuxer-max-back-bytes", "16MiB");
    opt("volume-max", "100");  // past 100 is amplification, which distorts
    opt("slang", cfgStr("media.slang", "eng,en,enUS").c_str());
    opt("alang", cfgStr("media.alang", "jpn,ja,eng,en").c_str());
    mpv_request_log_messages(m_mpv, "warn");
    if (mpv_initialize(m_mpv) < 0) {
        m_error = "mpv failed to initialise";
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }
    av_log_set_callback(appAvLog);  // see appAvLog
    mpv_stream_cb_add_ro(m_mpv, LOCKED_PROTOCOL, nullptr, openLocked);  // a locked folder's videos and songs
    mpv_observe_property(m_mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 3, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 4, "eof-reached", MPV_FORMAT_FLAG);
    bool ok = !cfgBool("media.cpu_render", false) && createGl();
    if (!ok) {
        appLog("video: using the CPU renderer");
        ok = createSw();
    }
    if (!ok) { m_error = "No video renderer available"; return false; }
    mpv_render_context_set_update_callback(m_ctx, onUpdate, nullptr);
    appLog("video: mpv ready (%s renderer)", m_sw ? "software" : "OpenGL");
    return true;
}

void Video::shutdown() {
    if (m_ctx) {
        SDL_RenderFlush(g_ren);
        GlState st;
        if (!m_sw) st.save();
        mpv_render_context_free(m_ctx);
        if (!m_sw) st.restore();
        m_ctx = nullptr;
    }
    if (m_mpv) { mpv_terminate_destroy(m_mpv); m_mpv = nullptr; }
    if (m_fbo && gl.DeleteFramebuffers) { gl.DeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_tex) { SDL_DestroyTexture(m_tex); m_tex = nullptr; }
}

void Video::command(std::initializer_list<const char*> args) {
    if (!m_mpv) return;
    std::vector<const char*> v(args);
    v.push_back(nullptr);
    mpv_command_async(m_mpv, 0, v.data());
}

static const uint64_t LOADFILE_REPLY = 1;  // tags loadfile's reply among mpv's events

void Video::load(const std::string& path, double startSec, Mode mode) {
    if (!init()) return;
    m_path = path;
    m_pos = startSec;
    m_dur = 0;
    m_loaded = m_eof = m_frameReady = false;
    m_error.clear();
    m_active = true;
    appLog("video: loading %s", path.c_str());
    // Everything this file starts with goes with the loadfile command as its own
    // per-file options: one call that doesn't wait for mpv (setting them one by
    // one did, about twenty times, and stalled the swipe to the next video while
    // mpv was still busy with the last one). Per-file options also drop back to
    // the defaults afterwards, so nothing the last file's controls changed (zoom,
    // aspect, speed, a chosen track) carries over.
    std::string opts;
    auto opt = [&](const char* key, const std::string& value) {
        if (!opts.empty()) opts += ',';
        opts += std::string(key) + "=%" + std::to_string(value.size()) + "%" + value;  // %n%: commas and = inside are fine
    };
    char start[32];
    snprintf(start, sizeof(start), "%.3f", startSec);
    opt("start", startSec > 0 ? start : "none");
    opt("pause", "no");
    // The Switch's audio output only starts once its buffer has some sound in
    // it, so a very short sound could end before anything played: music files
    // get half a second of silence after them, which pushes the real sound out.
    opt("af", isAudioExt(ext(path)) ? "apad=pad_dur=0.5" : "");
    for (const char* p : {"video-zoom", "video-pan-x", "video-pan-y", "sub-delay", "panscan", "video-rotate"}) opt(p, "0");
    opt("speed", "1");
    opt("video-aspect-override", "no");
    opt("volume", std::to_string(cfgInt("media.volume", 100)));
    opt("aid", "auto");
    opt("sid", "auto");
    // a clip in Files plays as it is: no subtitles found by name, no language picking
    bool media = mode == MEDIA;
    opt("sub-auto", media ? "fuzzy" : "no");
    opt("slang", media ? cfgStr("media.slang", "eng,en,enUS") : "");
    opt("alang", media ? cfgStr("media.alang", "jpn,ja,eng,en") : "");
    opt("sub-scale", media ? std::to_string(cfgInt("media.sub_scale", 100) / 100.0) : "1");
    // mpv 0.39: loadfile <url> <flags> <index> <options> (the index is unused with replace)
    uint64_t nonce;
    std::string url = locks::fileNonce(path, nonce) ? std::string(LOCKED_PROTOCOL) + "://" + path : path;  // decrypted as mpv reads
    const char* cmd[] = {"loadfile", url.c_str(), "replace", "-1", opts.c_str(), nullptr};
    mpv_command_async(m_mpv, LOADFILE_REPLY, cmd);
}

void Video::stop() {
    if (!m_mpv) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command(m_mpv, cmd);
    m_active = m_loaded = false;
}

void Video::poll() {
    if (!m_mpv) return;
    // A damaged file can make mpv log hundreds of lines a second. Each one used
    // to be written to log.txt from here, which stalled the app (and the HOME
    // button) for seconds: keep at most a few a second, and a bounded number of
    // events per frame.
    static uint32_t s_logSecond = 0;
    static int s_logCount = 0, s_logDropped = 0;
    for (int n = 0; n < 256; n++) {
        mpv_event* ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                uint32_t sec = SDL_GetTicks() / 1000;
                if (sec != s_logSecond) {
                    if (s_logDropped) appLog("mpv: %d more messages left out", s_logDropped);
                    s_logSecond = sec;
                    s_logCount = s_logDropped = 0;
                }
                if (++s_logCount > 8) { s_logDropped++; break; }
                auto* m = (mpv_event_log_message*)ev->data;
                std::string t = m->text;
                while (!t.empty() && t.back() == '\n') t.pop_back();
                appLog("mpv[%s] %s", m->prefix, t.c_str());
                break;
            }
            case MPV_EVENT_FILE_LOADED: m_loaded = true; m_eof = false; break;
            case MPV_EVENT_COMMAND_REPLY:
                // a loadfile mpv turned down (say, an option it didn't take) never plays: say so
                if (ev->reply_userdata == LOADFILE_REPLY && ev->error < 0) {
                    appLog("video: loadfile failed: %s", mpv_error_string(ev->error));
                    m_error = "Can't play this file";
                    m_active = false;
                }
                break;
            case MPV_EVENT_END_FILE: {
                auto* e = (mpv_event_end_file*)ev->data;
                if (e->reason == MPV_END_FILE_REASON_ERROR) {
                    m_error = std::string("Can't play this file: ") + mpv_error_string(e->error);
                    appLog("video: %s", m_error.c_str());
                    m_active = false;
                }
                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto* p = (mpv_event_property*)ev->data;
                if (!p->data) break;
                if (ev->reply_userdata == 1 && p->format == MPV_FORMAT_DOUBLE) m_pos = *(double*)p->data;
                else if (ev->reply_userdata == 2 && p->format == MPV_FORMAT_DOUBLE) m_dur = *(double*)p->data;
                else if (ev->reply_userdata == 3 && p->format == MPV_FORMAT_FLAG) m_paused = *(int*)p->data;
                else if (ev->reply_userdata == 4 && p->format == MPV_FORMAT_FLAG) m_eof = *(int*)p->data;
                break;
            }
            default: break;
        }
    }
}

void Video::render() {
    if (!m_ctx || !m_tex) return;
    if (!s_redraw.exchange(false)) return;
    if (!(mpv_render_context_update(m_ctx) & MPV_RENDER_UPDATE_FRAME)) return;
    if (m_sw) {
        void* pixels;
        int pitch;
        if (SDL_LockTexture(m_tex, nullptr, &pixels, &pitch) != 0) return;
        int size[2] = {VW, VH};
        size_t stride = (size_t)pitch;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_SW_SIZE, size},
            {MPV_RENDER_PARAM_SW_FORMAT, (void*)"rgb0"},
            {MPV_RENDER_PARAM_SW_STRIDE, &stride},
            {MPV_RENDER_PARAM_SW_POINTER, pixels},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(m_ctx, params);
        SDL_UnlockTexture(m_tex);
        if (m_loaded) m_frameReady = true;
        return;
    }
    SDL_RenderFlush(g_ren);
    GlState st;
    st.save();
    mpv_opengl_fbo fbo = {(int)m_fbo, VW, VH, 0};
    int flip = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(m_ctx, params);
    st.restore();
    if (m_loaded && !m_frameReady) {
        m_frameReady = true;
        appLog("video: first frame");
    }
}

void Video::setPause(bool p) {
    if (!m_mpv) return;
    int f = p ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &f);
    m_paused = p;
}

void Video::seek(double sec, bool relative, bool exact) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", sec);
    command({"seek", buf, relative ? (exact ? "relative+exact" : "relative") : (exact ? "absolute+exact" : "absolute+keyframes")});
    if (!relative) m_pos = sec;
    else m_pos += sec;
    m_eof = false;
}

double Video::getDouble(const char* prop, double def) {
    double v = def;
    if (m_mpv) mpv_get_property(m_mpv, prop, MPV_FORMAT_DOUBLE, &v);
    return v;
}

void Video::setDouble(const char* prop, double v) {
    if (m_mpv) mpv_set_property_async(m_mpv, 0, prop, MPV_FORMAT_DOUBLE, &v);
}

void Video::setString(const char* prop, const std::string& v) {
    if (m_mpv) mpv_set_property_string(m_mpv, prop, v.c_str());
}

std::vector<Track> Video::tracks() {
    std::vector<Track> out;
    if (!m_mpv) return out;
    mpv_node node;
    if (mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &node) < 0) return out;
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num; i++) {
            mpv_node& t = node.u.list->values[i];
            if (t.format != MPV_FORMAT_NODE_MAP) continue;
            Track tr;
            for (int k = 0; k < t.u.list->num; k++) {
                const char* key = t.u.list->keys[k];
                mpv_node& v = t.u.list->values[k];
                if (!strcmp(key, "id") && v.format == MPV_FORMAT_INT64) tr.id = v.u.int64;
                else if (!strcmp(key, "type") && v.format == MPV_FORMAT_STRING) tr.type = v.u.string;
                else if (!strcmp(key, "title") && v.format == MPV_FORMAT_STRING) tr.title = v.u.string;
                else if (!strcmp(key, "lang") && v.format == MPV_FORMAT_STRING) tr.lang = v.u.string;
                else if (!strcmp(key, "codec") && v.format == MPV_FORMAT_STRING) tr.codec = v.u.string;
                else if (!strcmp(key, "selected") && v.format == MPV_FORMAT_FLAG) tr.selected = v.u.flag;
                else if (!strcmp(key, "external") && v.format == MPV_FORMAT_FLAG) tr.external = v.u.flag;
            }
            out.push_back(tr);
        }
    }
    mpv_free_node_contents(&node);
    return out;
}

void Video::selectTrack(const std::string& type, int64_t id) {
    if (!m_mpv) return;
    const char* prop = type == "sub" ? "sid" : type == "audio" ? "aid" : "vid";
    if (id <= 0) mpv_set_property_string(m_mpv, prop, "no");
    else mpv_set_property_string(m_mpv, prop, std::to_string(id).c_str());
}
