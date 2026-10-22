// Now-playing visuals: the "ultrablur" backdrop made from the cover art and the
// audio visualizer.
#pragma once
#include "app.h"
#include "sp_dsp.hpp"

// A heavily blurred, saturated colour field from the cover, drawn as a smooth
// vertex-coloured mesh that drifts slowly. Crossfades when the cover changes.
class Ultrablur {
public:
    void setCover(SDL_Surface* s);  // RGBA32, any size
    void clear() { m_has = false; }
    bool ready() const { return m_has; }
    // glow: 0..1, brightens the field (the visualizer's level)
    void draw(const SDL_Rect& area, float glow);
    SDL_Color accent() const { return m_accent; }  // a vivid colour from the cover

private:
    static const int F = 40;
    float m_cur[F * F * 3] = {}, m_prev[F * F * 3] = {};
    bool m_has = false, m_hadPrev = false;
    uint32_t m_changedAt = 0;
    SDL_Color m_accent = {255, 255, 255, 255};
    void sample(const float* field, float u, float v, float out[3]) const;
};

// (saved by number: new ones go at the end)
enum VisStyle { VIS_OFF, VIS_BARS, VIS_WAVE, VIS_RIBBONS, VIS_PULSE, VIS_MIRROR, VIS_MOUNTAINS, VIS_STARS, VIS_WATERFALL, VIS_HALO, VIS_COUNT };
const char* visStyleName(int s);

class Visualizer {
public:
    void update(const dsp::SpectrumTap& tap, int delaySamples);
    // Draws along the bottom of `area` (bars, wave, scope), or across it (stars).
    void drawBackground(const SDL_Rect& area, int style, SDL_Color c);
    // Pulse and halo: the scale to draw the cover at, and what goes around it.
    float coverScale(int style) const;
    void drawCoverGlow(const SDL_Rect& art, int style, SDL_Color c);
    float level() const { return m_an.energy(); }
    float bass() const { return m_an.bass(); }

private:
    dsp::Analyzer m_an;
    static const int kBands = 56;
    float m_phase = 0;  // ribbons' flow, sped up by the music
    void drawRibbons(const SDL_Rect& area, SDL_Color c);
    void drawMirror(const SDL_Rect& area, SDL_Color c);
    void drawMountains(const SDL_Rect& area, SDL_Color c);
    void drawStars(const SDL_Rect& area, SDL_Color c);
    void drawWaterfall(const SDL_Rect& area, SDL_Color c);
    void drawHalo(const SDL_Rect& art, SDL_Color c);

    struct Star { float x, y, z; };
    std::vector<Star> m_stars;
    std::vector<std::vector<float>> m_history;  // the waterfall's rows, newest first
    float m_rowClock = 0;
};
