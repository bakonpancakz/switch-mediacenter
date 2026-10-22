#include "sp_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>

namespace dsp {

static const float PI = 3.14159265358979f;

const float kGraphicFreqs[kGraphicBands] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

const char* filterName(int type) {
    static const char* names[] = {"Peak", "Low shelf", "High shelf", "Low pass", "High pass"};
    return names[std::clamp(type, 0, F_COUNT - 1)];
}
bool filterHasGain(int type) { return type == F_PEAK || type == F_LOWSHELF || type == F_HIGHSHELF; }

// ---------------------------------------------------------------- biquad

void Biquad::design(const Band& b, float rate) {
    float f = std::clamp(b.freq, 10.0f, rate * 0.45f);
    float q = std::clamp(b.q, 0.1f, 18.0f);
    float A = powf(10.0f, b.gain / 40.0f);
    float w0 = 2 * PI * f / rate, cs = cosf(w0), sn = sinf(w0);
    float alpha = sn / (2 * q), sq = 2 * sqrtf(A) * alpha;
    float a0 = 1;
    switch (b.type) {
        case F_LOWSHELF:
            b0 = A * ((A + 1) - (A - 1) * cs + sq);
            b1 = 2 * A * ((A - 1) - (A + 1) * cs);
            b2 = A * ((A + 1) - (A - 1) * cs - sq);
            a0 = (A + 1) + (A - 1) * cs + sq;
            a1 = -2 * ((A - 1) + (A + 1) * cs);
            a2 = (A + 1) + (A - 1) * cs - sq;
            break;
        case F_HIGHSHELF:
            b0 = A * ((A + 1) + (A - 1) * cs + sq);
            b1 = -2 * A * ((A - 1) + (A + 1) * cs);
            b2 = A * ((A + 1) + (A - 1) * cs - sq);
            a0 = (A + 1) - (A - 1) * cs + sq;
            a1 = 2 * ((A - 1) - (A + 1) * cs);
            a2 = (A + 1) - (A - 1) * cs - sq;
            break;
        case F_LOWPASS:
            b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2;
            a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
            break;
        case F_HIGHPASS:
            b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2;
            a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
            break;
        default:  // peak
            b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A;
            break;
    }
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
}

float Biquad::responseDb(float freq, float rate) const {
    float w = 2 * PI * freq / rate;
    std::complex<float> z1 = std::polar(1.0f, -w), z2 = z1 * z1;
    std::complex<float> h = (b0 + b1 * z1 + b2 * z2) / (1.0f + a1 * z1 + a2 * z2);
    return 20 * log10f(std::max(1e-6f, std::abs(h)));
}

// ---------------------------------------------------------------- settings

std::vector<Band> EqSettings::activeBands() const {
    std::vector<Band> out;
    if (parametric) {
        for (auto& b : bands) if (b.on && (!filterHasGain(b.type) || fabsf(b.gain) > 0.05f)) out.push_back(b);
    } else {
        for (int i = 0; i < kGraphicBands; i++) {
            if (fabsf(graphic[i]) < 0.05f) continue;
            Band b;
            b.type = F_PEAK;
            b.freq = kGraphicFreqs[i];
            b.gain = graphic[i];
            b.q = 1.41f;  // about one octave wide
            out.push_back(b);
        }
    }
    return out;
}

float EqSettings::curveDb(float freq, float rate) const {
    float db = 0;
    for (auto& b : activeBands()) {
        Biquad f;
        f.design(b, rate);
        db += f.responseDb(freq, rate);
    }
    return db;
}

float EqSettings::totalPreampDb(float rate) const {
    float pre = preamp;
    if (autoHeadroom) {
        float peak = 0;
        auto act = activeBands();
        std::vector<Biquad> fs(act.size());
        for (size_t i = 0; i < act.size(); i++) fs[i].design(act[i], rate);
        for (int i = 0; i <= 96; i++) {
            float f = 20 * powf(1000.0f, i / 96.0f);  // 20 Hz .. 20 kHz
            float db = 0;
            for (auto& b : fs) db += b.responseDb(f, rate);
            peak = std::max(peak, db);
        }
        pre -= peak;
    }
    return pre;
}

std::string EqSettings::graphicStr() const {
    std::string s;
    char buf[16];
    for (int i = 0; i < kGraphicBands; i++) {
        snprintf(buf, sizeof(buf), "%s%.1f", i ? "," : "", graphic[i]);
        s += buf;
    }
    return s;
}

void EqSettings::parseGraphic(const std::string& s) {
    const char* p = s.c_str();
    for (int i = 0; i < kGraphicBands; i++) {
        char* end = nullptr;
        float v = strtof(p, &end);
        graphic[i] = end == p ? 0 : std::clamp(v, -kMaxGain, kMaxGain);
        if (!end || !*end) { for (int k = i + 1; k < kGraphicBands; k++) graphic[k] = 0; break; }
        p = end + 1;
    }
}

std::string EqSettings::bandsStr() const {
    std::string s;
    char buf[64];
    for (size_t i = 0; i < bands.size(); i++) {
        const Band& b = bands[i];
        snprintf(buf, sizeof(buf), "%s%d:%.0f:%.1f:%.2f:%d", i ? ";" : "", b.type, b.freq, b.gain, b.q, b.on ? 1 : 0);
        s += buf;
    }
    return s;
}

void EqSettings::parseBands(const std::string& s) {
    bands.clear();
    size_t p = 0;
    while (p < s.size() && (int)bands.size() < kMaxParamBands) {
        size_t e = s.find(';', p);
        if (e == std::string::npos) e = s.size();
        Band b;
        int on = 1;
        if (sscanf(s.substr(p, e - p).c_str(), "%d:%f:%f:%f:%d", &b.type, &b.freq, &b.gain, &b.q, &on) >= 4) {
            b.type = std::clamp(b.type, 0, F_COUNT - 1);
            b.freq = std::clamp(b.freq, 20.0f, 20000.0f);
            b.gain = std::clamp(b.gain, -kMaxGain, kMaxGain);
            b.q = std::clamp(b.q, 0.1f, 18.0f);
            b.on = on != 0;
            bands.push_back(b);
        }
        p = e + 1;
    }
}

const std::vector<Preset>& builtInPresets() {
    static const std::vector<Preset> p = {
        {"Flat", false, "0,0,0,0,0,0,0,0,0,0", true},
        {"Bass boost", false, "6,5,4,2,0,0,0,0,0,0", true},
        {"Bass reducer", false, "-6,-5,-4,-2,0,0,0,0,0,0", true},
        {"Treble boost", false, "0,0,0,0,0,1,2,4,5,6", true},
        {"Treble reducer", false, "0,0,0,0,0,-1,-2,-4,-5,-6", true},
        {"Vocal", false, "-2,-3,-2,1,3,4,3,1,0,-1", true},
        {"Rock", false, "5,4,3,1,-1,-1,1,3,4,5", true},
        {"Pop", false, "-1,0,2,3,4,3,1,0,-1,-1", true},
        {"Jazz", false, "3,2,1,2,-1,-1,0,1,2,3", true},
        {"Classical", false, "4,3,2,1,-1,-1,0,2,3,4", true},
        {"Electronic", false, "5,4,1,0,-2,1,0,1,4,5", true},
        {"Hip-hop", false, "5,4,2,3,-1,-1,1,-1,2,3", true},
        {"Acoustic", false, "4,4,3,1,2,2,3,3,2,1", true},
        {"Loudness", false, "6,4,0,0,-2,0,-1,-4,5,1", true},
        {"Late night", false, "-4,-3,-1,1,2,2,1,0,-2,-4", true},
        // parametric: type:freq:gain:q:on
        {"Switch speakers", true, "4:110:0:0.71:1;0:220:2.5:1.00:1;0:2500:-1.5:1.50:1;2:8000:3:0.71:1", true},
        {"Warm", true, "1:120:3:0.71:1;0:3000:-2:1.20:1;2:10000:-2:0.71:1", true},
        {"Bright and clear", true, "1:90:-1:0.71:1;0:2500:2:1.00:1;2:9000:4:0.71:1", true},
        {"Sub bass", true, "0:45:6:0.90:1;0:300:-2:1.00:1", true},
        {"Vocal clarity", true, "4:80:0:0.71:1;0:300:-2:1.00:1;0:3000:3:1.20:1;0:7000:-1:2.00:1", true},
        {"Headphones", true, "1:105:5:0.71:1;0:200:-1:1.00:1;0:2800:2:1.40:1;2:10000:-2:0.71:1", true},
    };
    return p;
}

// ---------------------------------------------------------------- equalizer

void Equalizer::set(const EqSettings& s, float rate) {
    std::vector<Biquad> fs;
    for (auto& b : s.activeBands()) {
        Biquad f;
        f.design(b, rate);
        fs.push_back(f);
    }
    if (fs.size() > kMaxParamBands + kGraphicBands) fs.resize(kMaxParamBands + kGraphicBands);
    float pre = powf(10.0f, s.totalPreampDb(rate) / 20.0f);
    std::lock_guard<std::mutex> l(m_mx);
    m_pendF = std::move(fs);
    m_pendPre = pre;
    m_pendOn = s.on && (!m_pendF.empty() || fabsf(pre - 1) > 1e-3f);
    m_dirty = true;
}

void Equalizer::begin() {
    if (!m_mx.try_lock()) return;  // a change in progress: pick it up next buffer
    if (m_dirty) {
        // keep filter state when the band layout stays the same (smooth slider drags)
        if (m_pendF.size() != m_n) for (auto& s : m_s) s = St{};
        m_n = m_pendF.size();
        for (size_t i = 0; i < m_n; i++) m_f[i] = m_pendF[i];
        m_pre = m_pendPre;
        m_active = m_pendOn;
        if (!m_active) for (auto& s : m_s) s = St{};
        m_dirty = false;
    }
    m_mx.unlock();
}

// ---------------------------------------------------------------- analysis

void SpectrumTap::read(float* out, int n, int delay) const {
    uint32_t end = m_pos.load(std::memory_order_acquire) - (uint32_t)delay;
    uint32_t start = end - (uint32_t)n;
    for (int i = 0; i < n; i++) out[i] = m_ring[(start + i) & (kSize - 1)];
}

static void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2 * PI / len, wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

void Analyzer::update(const SpectrumTap& tap, int bands, float dt, int delaySamples) {
    const float rate = 48000;
    if ((int)m_levels.size() != bands) { m_levels.assign(bands, 0); m_peaks.assign(bands, 0); }
    tap.read(m_buf, kFft, delaySamples);
    float sum = 0;
    for (int i = 0; i < kFft; i++) {
        float w = 0.5f - 0.5f * cosf(2 * PI * i / (kFft - 1));
        m_re[i] = m_buf[i] * w;
        m_im[i] = 0;
        sum += m_buf[i] * m_buf[i];
    }
    for (int i = 0; i < kWave; i++) m_wave[i] = m_buf[kFft - kWave + i];
    fft(m_re, m_im, kFft);

    float up = 1 - expf(-dt / 0.025f), down = 1 - expf(-dt / 0.22f);
    const float fLo = 40, fHi = 16000, binHz = rate / kFft;
    float bassSum = 0;
    int bassN = 0;
    for (int b = 0; b < bands; b++) {
        float f0 = fLo * powf(fHi / fLo, b / (float)bands), f1 = fLo * powf(fHi / fLo, (b + 1) / (float)bands);
        int i0 = std::max(1, (int)(f0 / binHz)), i1 = std::max(i0, (int)(f1 / binHz));
        float pw = 0;
        for (int i = i0; i <= i1 && i < kFft / 2; i++) pw = std::max(pw, m_re[i] * m_re[i] + m_im[i] * m_im[i]);
        float mag = sqrtf(pw) * 4.0f / kFft;  // a full-scale sine reads 1.0
        float fc = sqrtf(f0 * f1);
        float db = 20 * log10f(mag + 1e-9f) + 3.0f * log2f(fc / 1000.0f);  // tilt so highs show up
        float v = std::clamp((db + 62) / 56.0f, 0.0f, 1.0f);
        float& L = m_levels[b];
        L += (v - L) * (v > L ? up : down);
        float& P = m_peaks[b];
        P = std::max(L, P - dt * 0.5f);
        if (fc < 160) { bassSum += L; bassN++; }
    }
    float bass = bassN ? bassSum / bassN : 0;
    m_bass += (bass - m_bass) * (bass > m_bass ? up : down);
    float rms = sqrtf(sum / kFft);
    float e = std::clamp(rms * 3.0f, 0.0f, 1.0f);
    m_energy += (e - m_energy) * (e > m_energy ? up : down);
}

}  // namespace dsp
