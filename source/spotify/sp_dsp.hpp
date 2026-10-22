// Audio effects for the Spotify receiver: a graphic/parametric equalizer built
// from biquad filters (RBJ audio EQ cookbook), and a tap of the output that the
// visualizer analyses with an FFT. No SDL here: the audio thread uses it.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dsp {

enum FilterType { F_PEAK, F_LOWSHELF, F_HIGHSHELF, F_LOWPASS, F_HIGHPASS, F_COUNT };
const char* filterName(int type);
bool filterHasGain(int type);

struct Band {
    int type = F_PEAK;
    float freq = 1000, gain = 0, q = 1.0f;
    bool on = true;
};

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void design(const Band& b, float rate);
    float responseDb(float freq, float rate) const;  // magnitude at freq
};

constexpr int kGraphicBands = 10;
extern const float kGraphicFreqs[kGraphicBands];
constexpr int kMaxParamBands = 8;
constexpr float kMaxGain = 12;

struct EqSettings {
    bool on = false;
    bool parametric = false;
    float graphic[kGraphicBands] = {};
    std::vector<Band> bands;  // parametric bands
    float preamp = 0;         // dB, added to the automatic headroom
    bool autoHeadroom = true; // lower the level by the curve's biggest boost

    std::vector<Band> activeBands() const;   // filters for the current mode
    float curveDb(float freq, float rate) const;  // combined response, without preamp
    float totalPreampDb(float rate) const;
    // Text forms used for settings and presets
    std::string graphicStr() const;
    void parseGraphic(const std::string& s);
    std::string bandsStr() const;
    void parseBands(const std::string& s);
};

struct Preset {
    std::string name;
    bool parametric;
    std::string data;  // graphicStr() or bandsStr()
    bool builtIn;
};
const std::vector<Preset>& builtInPresets();

// Lives in the audio output; set() may be called from any thread.
class Equalizer {
public:
    void set(const EqSettings& s, float rate);
    // Audio thread: call once per buffer, then process() each frame.
    void begin();
    bool active() const { return m_active; }
    void process(float& l, float& r) {
        float xl = l * m_pre, xr = r * m_pre;
        for (size_t i = 0; i < m_n; i++) {
            const Biquad& f = m_f[i];
            St& s = m_s[i];
            float yl = f.b0 * xl + s.l1;
            s.l1 = f.b1 * xl - f.a1 * yl + s.l2;
            s.l2 = f.b2 * xl - f.a2 * yl;
            float yr = f.b0 * xr + s.r1;
            s.r1 = f.b1 * xr - f.a1 * yr + s.r2;
            s.r2 = f.b2 * xr - f.a2 * yr;
            xl = yl;
            xr = yr;
        }
        l = xl;
        r = xr;
    }

private:
    struct St { float l1 = 0, l2 = 0, r1 = 0, r2 = 0; };
    std::mutex m_mx;
    bool m_dirty = false;
    std::vector<Biquad> m_pendF;
    float m_pendPre = 1;
    bool m_pendOn = false;
    // audio thread copies
    Biquad m_f[kMaxParamBands + kGraphicBands];
    St m_s[kMaxParamBands + kGraphicBands];
    size_t m_n = 0;
    float m_pre = 1;
    bool m_active = false;
};

// The last ~0.34 s of output (mono, 48 kHz), written by the audio thread.
class SpectrumTap {
public:
    static constexpr int kSize = 16384;
    void push(float v) {
        uint32_t p = m_pos.load(std::memory_order_relaxed);
        m_ring[p & (kSize - 1)] = v;
        m_pos.store(p + 1, std::memory_order_release);
    }
    // n samples ending `delay` samples before the newest one
    void read(float* out, int n, int delay) const;

private:
    float m_ring[kSize] = {};
    std::atomic<uint32_t> m_pos{0};
};

// FFT analysis for the visualizer (UI thread).
class Analyzer {
public:
    static constexpr int kFft = 2048;
    // bands: log-spaced levels 0..1, smoothed; also bass and overall level
    void update(const SpectrumTap& tap, int bands, float dt, int delaySamples);
    const std::vector<float>& levels() const { return m_levels; }
    const std::vector<float>& peaks() const { return m_peaks; }
    float bass() const { return m_bass; }
    float energy() const { return m_energy; }
    const float* wave() const { return m_wave; }  // last kWave samples
    static constexpr int kWave = 512;

private:
    std::vector<float> m_levels, m_peaks;
    float m_bass = 0, m_energy = 0;
    float m_buf[kFft], m_re[kFft], m_im[kFft], m_wave[kWave] = {};
};

}  // namespace dsp
