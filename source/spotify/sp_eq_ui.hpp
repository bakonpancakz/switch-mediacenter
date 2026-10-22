// Equalizer sheet for the Spotify tab: graphic (10 bands) and parametric (up to
// 8 bands) modes, a live response curve, built-in and saved presets.
#pragma once
#include "app.h"
#include "sp_dsp.hpp"

class AudioOutput;

class EqSheet {
public:
    EqSheet();
    bool open = false;
    void apply(AudioOutput* audio);  // push the current settings to the audio thread
    bool enabled() const { return m_s.on; }
    // audio is null when the receiver can't be processed (Switchify sysmodule)
    void draw(const SDL_Rect& area, AudioOutput* audio);

private:
    dsp::EqSettings m_s;
    std::string m_preset;
    std::vector<dsp::Preset> m_custom;
    int m_sel = 0;        // selected parametric band
    int m_drag = -1;      // graphic band or parametric node being dragged
    bool m_presets = false;
    Scroller m_presetScroll;

    void load();
    void save();
    void loadCustom();
    void saveCustom();
    void edited(AudioOutput* audio);  // a manual change: no longer a named preset
    void choosePreset(const dsp::Preset& p, AudioOutput* audio);
    void drawCurve(const SDL_Rect& r, AudioOutput* audio);
    void drawGraphic(const SDL_Rect& r, AudioOutput* audio);
    void drawParametric(const SDL_Rect& r, AudioOutput* audio);
    void drawPresetList(const SDL_Rect& area, AudioOutput* audio);
};
