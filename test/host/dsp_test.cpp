// Host test for the Spotify equalizer and analyzer (source/spotify/sp_dsp.cpp).
#include <cmath>
#include <cstdio>
#include <vector>

#include "../../source/spotify/sp_dsp.hpp"

static int fails = 0;
static void check(bool ok, const char* what, double got) {
    printf("%s %s (%.2f)\n", ok ? "ok" : "FAIL", what, got);
    if (!ok) fails++;
}

// RMS gain in dB of the equalizer for a sine at freq (after it settles)
static double measure(dsp::Equalizer& eq, float freq) {
    const float rate = 48000;
    double in = 0, out = 0;
    for (int blk = 0; blk < 40; blk++) {
        eq.begin();
        for (int i = 0; i < 1200; i++) {
            int n = blk * 1200 + i;
            float x = 0.25f * sinf(2 * 3.14159265f * freq * n / rate), l = x, r = x;
            if (eq.active()) eq.process(l, r);
            if (blk >= 20) { in += x * x; out += l * l; }
        }
    }
    return 10 * log10(out / in);
}

int main() {
    const float rate = 48000;
    // single filters against their design targets
    dsp::Biquad b;
    b.design({dsp::F_PEAK, 1000, 6, 1.41f, true}, rate);
    check(fabs(b.responseDb(1000, rate) - 6) < 0.05, "peak +6 dB at its centre", b.responseDb(1000, rate));
    check(fabs(b.responseDb(60, rate)) < 0.3, "peak flat far below", b.responseDb(60, rate));
    b.design({dsp::F_LOWSHELF, 100, 6, 0.71f, true}, rate);
    check(fabs(b.responseDb(20, rate) - 6) < 0.3, "low shelf +6 dB at 20 Hz", b.responseDb(20, rate));
    check(fabs(b.responseDb(5000, rate)) < 0.1, "low shelf flat at 5 kHz", b.responseDb(5000, rate));
    b.design({dsp::F_HIGHPASS, 200, 0, 0.71f, true}, rate);
    check(fabs(b.responseDb(200, rate) + 3) < 0.1, "high pass -3 dB at cutoff", b.responseDb(200, rate));
    check(b.responseDb(50, rate) < -20, "high pass blocks 50 Hz", b.responseDb(50, rate));

    // the processed signal matches the predicted curve (graphic mode, no headroom)
    dsp::EqSettings s;
    s.on = true;
    s.autoHeadroom = false;
    s.graphic[5] = 6;  // 1 kHz
    dsp::Equalizer eq;
    eq.set(s, rate);
    double g = measure(eq, 1000);
    check(fabs(g - s.curveDb(1000, rate)) < 0.2, "processed gain matches the curve at 1 kHz", g);
    check(fabs(measure(eq, 100)) < 0.5, "100 Hz untouched", measure(eq, 100));

    // automatic headroom pulls the peak back to 0 dB
    s.autoHeadroom = true;
    eq.set(s, rate);
    g = measure(eq, 1000);
    check(fabs(g) < 0.3, "headroom keeps the boosted band at 0 dB", g);
    check(fabs(s.totalPreampDb(rate) + s.curveDb(1000, rate)) < 0.3, "preamp equals minus the peak", s.totalPreampDb(rate));

    // off means bypass
    s.on = false;
    eq.set(s, rate);
    g = measure(eq, 1000);
    check(fabs(g) < 0.01, "bypass when off", g);

    // text forms round-trip
    dsp::EqSettings t;
    t.parseGraphic(s.graphicStr());
    check(t.graphic[5] == 6 && t.graphic[0] == 0, "graphic round trip", t.graphic[5]);
    s.bands = {{dsp::F_LOWSHELF, 105, 5, 0.71f, true}, {dsp::F_PEAK, 2800, -2.5f, 1.4f, false}};
    t.parseBands(s.bandsStr());
    check(t.bands.size() == 2 && t.bands[1].type == dsp::F_PEAK && !t.bands[1].on && fabs(t.bands[1].gain + 2.5f) < 0.01,
          "parametric round trip", (double)t.bands.size());
    for (auto& p : dsp::builtInPresets()) {
        dsp::EqSettings q;
        if (p.parametric) q.parseBands(p.data);
        else q.parseGraphic(p.data);
        bool ok = p.parametric ? !q.bands.empty() : true;
        if (!ok) check(false, p.name.c_str(), 0);
    }
    printf("ok %d presets parse\n", (int)dsp::builtInPresets().size());

    // analyzer: a 1 kHz tone lights up the band around 1 kHz, silence decays
    dsp::SpectrumTap tap;
    for (int n = 0; n < 16384; n++) tap.push(0.5f * sinf(2 * 3.14159265f * 1000 * n / rate));
    dsp::Analyzer an;
    for (int i = 0; i < 30; i++) an.update(tap, 56, 1 / 60.0f, 0);
    const auto& lv = an.levels();
    int best = 0;
    for (int i = 0; i < 56; i++) if (lv[i] > lv[best]) best = i;
    float f0 = 40 * powf(400.0f, best / 56.0f), f1 = 40 * powf(400.0f, (best + 1) / 56.0f);
    check(f0 <= 1000 && f1 >= 1000 * 0.97f && lv[best] > 0.7f, "1 kHz tone peaks in the 1 kHz band", lv[best]);
    check(lv[3] < 0.2f, "low bands stay quiet", lv[3]);
    for (int n = 0; n < 16384; n++) tap.push(0);
    for (int i = 0; i < 120; i++) an.update(tap, 56, 1 / 60.0f, 0);
    check(an.levels()[best] < 0.05f, "silence decays", an.levels()[best]);

    printf("%s\n", fails ? "DSP FAILED" : "DSP OK");
    return fails ? 1 : 0;
}
