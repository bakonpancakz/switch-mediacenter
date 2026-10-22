#include "sp_eq_ui.hpp"

#include <algorithm>
#include <cmath>

#include "sp_audio.hpp"

static const char* PRESET_FILE = APP_DIR "/spotify-eq.txt";
static const float RATE = 48000;
static const float FMIN = 20, FMAX = 20000, DBRANGE = 15;

static float freqToX(float f, const SDL_Rect& r) { return r.x + r.w * logf(f / FMIN) / logf(FMAX / FMIN); }
static float xToFreq(float x, const SDL_Rect& r) { return FMIN * powf(FMAX / FMIN, std::clamp((x - r.x) / r.w, 0.0f, 1.0f)); }
static float dbToY(float db, const SDL_Rect& r) { return r.y + r.h / 2.0f - db / DBRANGE * (r.h / 2.0f); }
static float yToDb(float y, const SDL_Rect& r) { return (r.y + r.h / 2.0f - y) / (r.h / 2.0f) * DBRANGE; }

static std::string freqLabel(float f) {
    char b[24];
    if (f >= 1000) snprintf(b, sizeof(b), f >= 10000 ? "%.0fk" : "%.1fk", f / 1000);
    else snprintf(b, sizeof(b), "%.0f", f);
    std::string s = b;
    if (s.size() > 3 && s.compare(s.size() - 3, 3, ".0k") == 0) s.replace(s.size() - 3, 3, "k");
    return s;
}

static std::string dbLabel(float db) {
    char b[24];
    snprintf(b, sizeof(b), "%+.1f dB", fabsf(db) < 0.05f ? 0.0f : db);
    return b;
}

// label on top, "-  value  +" below; returns -1/0/+1
static int miniStepper(const SDL_Rect& r, const std::string& label, const std::string& value) {
    drawText(label, r.x + r.w / 2, r.y, 18, g_theme.dim, 1, r.w);
    int by = r.y + 28;
    SDL_Rect minus = {r.x, by, 56, 52}, plus = {r.x + r.w - 56, by, 56, 52};
    drawText(value, r.x + r.w / 2, by + (52 - fontHeight(22)) / 2, 22, g_theme.fg, 1, r.w - 120, FS_BOLD);
    int d = 0;
    if (button(minus, "", IC_MINUS)) d = -1;
    if (button(plus, "", IC_PLUS)) d = 1;
    return d;
}

static std::vector<dsp::Band> defaultBands() {
    std::vector<dsp::Band> b(5);
    b[0] = {dsp::F_LOWSHELF, 80, 0, 0.71f, true};
    b[1] = {dsp::F_PEAK, 250, 0, 1.0f, true};
    b[2] = {dsp::F_PEAK, 1000, 0, 1.0f, true};
    b[3] = {dsp::F_PEAK, 4000, 0, 1.0f, true};
    b[4] = {dsp::F_HIGHSHELF, 10000, 0, 0.71f, true};
    return b;
}

EqSheet::EqSheet() {
    load();
    loadCustom();
}

void EqSheet::load() {
    m_s.on = cfgBool("spotify.eq.on", false);
    m_s.parametric = cfgBool("spotify.eq.parametric", false);
    m_s.parseGraphic(cfgStr("spotify.eq.graphic", "0,0,0,0,0,0,0,0,0,0"));
    m_s.parseBands(cfgStr("spotify.eq.bands", ""));
    if (m_s.bands.empty()) m_s.bands = defaultBands();
    m_s.preamp = std::clamp((float)atof(cfgStr("spotify.eq.preamp", "0").c_str()), -12.0f, 12.0f);
    m_s.autoHeadroom = cfgBool("spotify.eq.headroom", true);
    m_preset = cfgStr("spotify.eq.preset", "Flat");
}

void EqSheet::save() {
    cfgSetBool("spotify.eq.on", m_s.on);
    cfgSetBool("spotify.eq.parametric", m_s.parametric);
    cfgSet("spotify.eq.graphic", m_s.graphicStr());
    cfgSet("spotify.eq.bands", m_s.bandsStr());
    char b[16];
    snprintf(b, sizeof(b), "%.1f", m_s.preamp);
    cfgSet("spotify.eq.preamp", b);
    cfgSetBool("spotify.eq.headroom", m_s.autoHeadroom);
    cfgSet("spotify.eq.preset", m_preset);
}

// One preset per line: name <tab> G|P <tab> data
void EqSheet::loadCustom() {
    m_custom.clear();
    std::string txt;
    if (!readFile(PRESET_FILE, txt)) return;
    size_t p = 0;
    while (p < txt.size()) {
        size_t e = txt.find('\n', p);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(p, e - p);
        p = e + 1;
        size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        m_custom.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1) == "P", trim(line.substr(t2 + 1)), false});
    }
}

void EqSheet::saveCustom() {
    std::string txt;
    for (auto& p : m_custom) txt += p.name + "\t" + (p.parametric ? "P" : "G") + "\t" + p.data + "\n";
    writeFile(PRESET_FILE, txt);
}

void EqSheet::apply(AudioOutput* audio) {
    if (audio) audio->eq.set(m_s, RATE);
}

void EqSheet::edited(AudioOutput* audio) {
    m_preset = "Custom";
    save();
    apply(audio);
}

void EqSheet::choosePreset(const dsp::Preset& p, AudioOutput* audio) {
    m_s.parametric = p.parametric;
    if (p.parametric) {
        m_s.parseBands(p.data);
        m_sel = 0;
    } else m_s.parseGraphic(p.data);
    m_s.on = true;
    m_preset = p.name;
    save();
    apply(audio);
}

// ---------------------------------------------------------------- drawing

void EqSheet::draw(const SDL_Rect& area, AudioOutput* audio) {
    InputLayer layer;  // on top: gets the input, the screen under it doesn't
    // the preset list on top owns the input
    bool c0 = g_in.consumed;
    if (m_presets) g_in.consumed = true;
    fillRect(area, withAlpha(g_theme.panel, 248));
    int x = area.x + 32, w = area.w - 64;
    drawText("Equalizer", x, 22, 32, g_theme.fg, 0, 0, FS_BOLD);
    if (iconButton(area.x + area.w - 50.0f, 46, 28, IC_CLOSE, g_theme.fg, SDL_Color{0, 0, 0, 0}) || (g_in.back && !m_presets)) {
        open = false;
        g_in.consumed = true;
        return;
    }
    if (!audio) {
        drawWrapped("The equalizer works on the built-in receiver. Spotify is playing through the Switchify sysmodule, "
                    "whose audio this app can't process.",
                    x, 110, 24, g_theme.dim, w, 4);
        g_in.consumed = true;
        return;
    }
    // on/off
    SDL_Rect onR = {area.x + area.w - 260, 18, 170, 56};
    drawText(m_s.on ? "On" : "Off", onR.x, onR.y + 14, 24, m_s.on ? g_theme.accent : g_theme.dim, 0, 0, FS_BOLD);
    if (toggle(onR, m_s.on)) { save(); apply(audio); }

    // mode and presets
    int y = 92;
    if (chip({x, y, 150, 50}, "Graphic", !m_s.parametric, 20) && m_s.parametric) { m_s.parametric = false; edited(audio); }
    if (chip({x + 160, y, 170, 50}, "Parametric", m_s.parametric, 20) && !m_s.parametric) { m_s.parametric = true; m_sel = 0; edited(audio); }
    if (button({x + w - 520, y - 3, 340, 56}, "Preset: " + m_preset, IC_LIST, BTN_NORMAL, 20)) { m_presets = true; m_presetScroll = Scroller(); }
    if (button({x + w - 170, y - 3, 170, 56}, "Save", IC_BOOKMARK, BTN_NORMAL, 20)) {
        std::string name;
        if (keyboard("Preset name", m_preset == "Custom" ? "" : m_preset, name, 32) && !trim(name).empty()) {
            name = trim(name);
            for (char& c : name) if (c == '\t' || c == '\n') c = ' ';
            m_custom.erase(std::remove_if(m_custom.begin(), m_custom.end(), [&](const dsp::Preset& p) { return p.name == name; }),
                           m_custom.end());
            m_custom.push_back({name, m_s.parametric, m_s.parametric ? m_s.bandsStr() : m_s.graphicStr(), false});
            saveCustom();
            m_preset = name;
            save();
            toast("Saved preset \"" + name + "\"");
        }
    }

    // response curve
    SDL_Rect plot = {x, 160, w, 190};
    drawCurve(plot, audio);

    // band controls
    SDL_Rect ctl = {x, plot.y + plot.h + 18, w, area.h - (plot.y + plot.h + 18) - 92};
    if (m_s.parametric) drawParametric(ctl, audio);
    else drawGraphic(ctl, audio);

    // level
    int by = area.h - 80;
    float total = m_s.totalPreampDb(RATE);
    if (int d = miniStepper({x, by - 10, 300, 80}, "Preamp", dbLabel(m_s.preamp))) {
        m_s.preamp = std::clamp(m_s.preamp + d * 0.5f, -12.0f, 12.0f);
        save();
        apply(audio);
    }
    SDL_Rect hr = {x + 340, by + 14, w - 340, 56};
    drawText("Prevent clipping", hr.x, hr.y + 4, 22, g_theme.fg);
    char buf[64];
    snprintf(buf, sizeof(buf), "Output level %+.1f dB", total);
    drawText(buf, hr.x, hr.y + 32, 16, g_theme.dim);
    if (toggle(hr, m_s.autoHeadroom)) { save(); apply(audio); }

    if (m_presets) {
        g_in.consumed = c0;
        drawPresetList(area, audio);
    }
    g_in.consumed = true;
}

void EqSheet::drawCurve(const SDL_Rect& r, AudioOutput* audio) {
    fillRound(r, 14, SDL_Color{0, 0, 0, 120});
    pushClip(r);
    for (float db : {-12.0f, -6.0f, 6.0f, 12.0f}) fillRect(r.x, (int)dbToY(db, r), r.w, 1, withAlpha(g_theme.fg, 20));
    fillRect(r.x, (int)dbToY(0, r), r.w, 1, withAlpha(g_theme.fg, 50));
    for (float f : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
        int fx = (int)freqToX(f, r);
        fillRect(fx, r.y, 1, r.h, withAlpha(g_theme.fg, f == 100 || f == 1000 || f == 10000 ? 36 : 16));
        drawText(freqLabel(f), fx + 4, r.y + r.h - 22, 14, withAlpha(g_theme.fg, 90));
    }
    drawText("+12", r.x + 6, (int)dbToY(12, r) - 9, 14, withAlpha(g_theme.fg, 90));
    drawText("-12", r.x + 6, (int)dbToY(-12, r) - 9, 14, withAlpha(g_theme.fg, 90));

    // the combined response
    auto act = m_s.activeBands();
    std::vector<dsp::Biquad> fs(act.size());
    for (size_t i = 0; i < act.size(); i++) fs[i].design(act[i], RATE);
    SDL_Color c = m_s.on ? g_theme.accent : g_theme.dim;
    std::vector<SDL_FPoint> pts;
    const int N = 120;
    for (int i = 0; i <= N; i++) {
        float px = r.x + r.w * i / (float)N, f = xToFreq(px, r), db = 0;
        for (auto& b : fs) db += b.responseDb(f, RATE);
        pts.push_back({px, dbToY(std::clamp(db, -DBRANGE * 1.2f, DBRANGE * 1.2f), r)});
    }
    float zero = dbToY(0, r);
    for (size_t i = 1; i < pts.size(); i++) {
        // shade between the curve and 0 dB
        SDL_FPoint a = pts[i - 1], b = pts[i];
        SDL_Color fc = withAlpha(c, 45);
        SDL_Vertex v[4] = {{a, fc, {0, 0}}, {b, fc, {0, 0}}, {{a.x, zero}, withAlpha(c, 8), {0, 0}}, {{b.x, zero}, withAlpha(c, 8), {0, 0}}};
        int idx[6] = {0, 1, 2, 1, 3, 2};
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        SDL_RenderGeometry(g_ren, nullptr, v, 4, idx, 6);
    }
    for (size_t i = 1; i < pts.size(); i++) drawLine(pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, 3, c, false);

    if (m_s.parametric) {
        // draggable nodes; tap empty space to add a band there
        int hit = -1;
        float bestD = 56;
        for (int i = 0; i < (int)m_s.bands.size(); i++) {
            const dsp::Band& b = m_s.bands[i];
            float nx = freqToX(b.freq, r), ny = dbToY(dsp::filterHasGain(b.type) ? b.gain : 0, r);
            float d = hypotf(g_in.x - nx, g_in.y - ny);
            if (d < bestD) { bestD = d; hit = i; }
        }
        if (g_in.pressed && !g_in.consumed && inRect(g_in.x, g_in.y, r) && hit >= 0) { m_drag = 100 + hit; m_sel = hit; }
        if (m_drag >= 100 && m_drag - 100 < (int)m_s.bands.size()) {
            if (g_in.down) {
                dsp::Band& b = m_s.bands[m_drag - 100];
                if (g_in.moved) {  // a touch without a drag only selects the band
                    float f = std::clamp(xToFreq((float)g_in.x, r), 20.0f, 20000.0f), gn = b.gain;
                    if (dsp::filterHasGain(b.type)) gn = std::clamp(roundf(yToDb((float)g_in.y, r) * 2) / 2, -dsp::kMaxGain, dsp::kMaxGain);
                    if (f != b.freq || gn != b.gain) { b.freq = f; b.gain = gn; edited(audio); }
                }
                g_in.consumed = true;
            } else m_drag = -1;
        }
        for (int i = 0; i < (int)m_s.bands.size(); i++) {
            const dsp::Band& b = m_s.bands[i];
            float nx = freqToX(b.freq, r), ny = dbToY(dsp::filterHasGain(b.type) ? b.gain : 0, r);
            bool sel = i == m_sel;
            fillCircle(nx, ny, sel ? 17 : 14, b.on ? (sel ? g_theme.fg : c) : g_theme.faint);
            drawText(std::to_string(i + 1), (int)nx, (int)ny - fontHeight(16, FS_BOLD) / 2, 16, SDL_Color{10, 10, 12, 255}, 1, 0, FS_BOLD);
        }
        if (hit < 0 && (int)m_s.bands.size() < dsp::kMaxParamBands && tapped(r)) {
            dsp::Band b;
            b.freq = xToFreq((float)g_in.x, r);
            b.gain = std::clamp(roundf(yToDb((float)g_in.y, r) * 2) / 2, -dsp::kMaxGain, dsp::kMaxGain);
            m_s.bands.push_back(b);
            m_sel = (int)m_s.bands.size() - 1;
            edited(audio);
        }
    }
    popClip();
}

void EqSheet::drawGraphic(const SDL_Rect& r, AudioOutput* audio) {
    int n = dsp::kGraphicBands;
    float colW = r.w / (float)n;
    int top = r.y + 30, bottom = r.y + r.h - 34;
    for (int i = 0; i < n; i++) {
        SDL_Rect col = {(int)(r.x + i * colW), r.y, (int)colW, r.h};
        float cx = col.x + colW / 2;
        float& g = m_s.graphic[i];
        if (g_in.pressed && !g_in.consumed && inRect(g_in.x, g_in.y, col)) m_drag = i;
        if (m_drag == i) {
            if (g_in.down) {
                float v = (bottom - g_in.y) / (float)(bottom - top) * 2 - 1;  // -1..1
                float ng = std::clamp(roundf(v * dsp::kMaxGain * 2) / 2, -dsp::kMaxGain, dsp::kMaxGain);
                if (ng != g) { g = ng; edited(audio); }
                g_in.consumed = true;
            } else m_drag = -1;
        }
        if (g_in.doubleTap && inRect(g_in.tapX, g_in.tapY, col)) { g = 0; edited(audio); }
        // track, centre mark, fill from 0 dB, knob
        fillRound({(int)cx - 3, top, 6, bottom - top}, 3, withAlpha(g_theme.fg, 40));
        float mid = (top + bottom) / 2.0f, ky = mid - g / dsp::kMaxGain * (bottom - top) / 2;
        fillRect((int)cx - 10, (int)mid, 20, 1, withAlpha(g_theme.fg, 70));
        int y0 = (int)std::min(mid, ky), y1 = (int)std::max(mid, ky);
        if (y1 > y0) fillRound({(int)cx - 3, y0, 6, y1 - y0}, 3, m_s.on ? g_theme.accent : g_theme.dim);
        fillCircle(cx, ky, m_drag == i ? 17 : 14, g_theme.fg);
        char b[16];
        snprintf(b, sizeof(b), "%+.1f", fabsf(g) < 0.05f ? 0.0f : g);
        drawText(b, (int)cx, r.y, 16, m_drag == i ? g_theme.fg : g_theme.dim, 1);
        drawText(freqLabel(dsp::kGraphicFreqs[i]), (int)cx, bottom + 10, 18, g_theme.fg, 1);
    }
}

void EqSheet::drawParametric(const SDL_Rect& r, AudioOutput* audio) {
    auto& bands = m_s.bands;
    int x = r.x, y = r.y;
    // band picker
    for (int i = 0; i < (int)bands.size(); i++)
        if (chip({x + i * 66, y, 58, 48}, std::to_string(i + 1), i == m_sel, 20)) m_sel = i;
    int ax = x + (int)bands.size() * 66;
    if ((int)bands.size() < dsp::kMaxParamBands && button({ax, y, 58, 48}, "", IC_PLUS)) {
        dsp::Band b;
        b.freq = 1000;
        bands.push_back(b);
        m_sel = (int)bands.size() - 1;
        edited(audio);
    }
    if (bands.empty()) {
        drawText("Tap the curve or + to add a band.", r.x + r.w / 2, r.y + 120, 22, g_theme.dim, 1);
        return;
    }
    m_sel = std::clamp(m_sel, 0, (int)bands.size() - 1);
    dsp::Band& b = bands[m_sel];
    if (button({r.x + r.w - 180, y, 180, 48}, "Remove", IC_TRASH, BTN_NORMAL, 18)) {
        bands.erase(bands.begin() + m_sel);
        m_sel = std::max(0, m_sel - 1);
        edited(audio);
        return;
    }
    SDL_Rect onR = {r.x + r.w - 370, y - 4, 170, 56};
    drawText("Band on", onR.x, onR.y + 16, 20, g_theme.fg);
    if (toggle(onR, b.on)) edited(audio);

    // filter type
    y += 62;
    int tw = (r.w - 4 * 10) / dsp::F_COUNT;
    for (int t = 0; t < dsp::F_COUNT; t++)
        if (chip({r.x + t * (tw + 10), y, tw, 48}, dsp::filterName(t), b.type == t, 20) && b.type != t) { b.type = t; edited(audio); }

    // frequency, gain, Q
    y += 62;
    int sw = (r.w - 2 * 24) / 3;
    if (int d = miniStepper({r.x, y, sw, 82}, "Frequency", freqLabel(b.freq) + " Hz")) {
        b.freq = std::clamp(b.freq * powf(2.0f, d / 6.0f), 20.0f, 20000.0f);  // sixth-octave steps
        edited(audio);
    }
    if (dsp::filterHasGain(b.type)) {
        if (int d = miniStepper({r.x + sw + 24, y, sw, 82}, "Gain", dbLabel(b.gain))) {
            b.gain = std::clamp(b.gain + d * 0.5f, -dsp::kMaxGain, dsp::kMaxGain);
            edited(audio);
        }
    } else drawText("No gain for this filter", r.x + sw + 24 + sw / 2, y + 40, 18, g_theme.dim, 1);
    char q[16];
    snprintf(q, sizeof(q), "%.2f", b.q);
    if (int d = miniStepper({r.x + 2 * (sw + 24), y, sw, 82}, b.type == dsp::F_PEAK ? "Width (Q)" : "Slope (Q)", q)) {
        b.q = std::clamp(b.q * powf(1.25f, (float)d), 0.1f, 18.0f);
        edited(audio);
    }
}

void EqSheet::drawPresetList(const SDL_Rect& area, AudioOutput* audio) {
    InputLayer layer;  // on top: gets the input, the screen under it doesn't
    fillRect(area, SDL_Color{0, 0, 0, 170});
    int w = std::min(620, area.w - 64), h = area.h - 80;
    SDL_Rect r = {area.x + (area.w - w) / 2, 40, w, h};
    fillRound(r, 22, g_theme.panel2);
    drawText("Presets", r.x + 28, r.y + 22, 28, g_theme.fg, 0, 0, FS_BOLD);
    if (iconButton(r.x + w - 44.0f, r.y + 42.0f, 26, IC_CLOSE, g_theme.fg, SDL_Color{0, 0, 0, 0})) { m_presets = false; return; }
    struct Row { const dsp::Preset* p; bool head; const char* title; };
    std::vector<Row> rows;
    if (!m_custom.empty()) {
        rows.push_back({nullptr, true, "Your presets"});
        for (auto& p : m_custom) rows.push_back({&p, false, nullptr});
    }
    rows.push_back({nullptr, true, "Graphic"});
    for (auto& p : dsp::builtInPresets()) if (!p.parametric) rows.push_back({&p, false, nullptr});
    rows.push_back({nullptr, true, "Parametric"});
    for (auto& p : dsp::builtInPresets()) if (p.parametric) rows.push_back({&p, false, nullptr});

    SDL_Rect list = {r.x + 12, r.y + 80, w - 24, h - 92};
    const int rowH = 58;
    m_presetScroll.update(list, rows.size() * (float)rowH);
    pushClip(list);
    int del = -1;
    for (size_t i = 0; i < rows.size(); i++) {
        int y = list.y + (int)i * rowH - (int)m_presetScroll.pos;
        if (y + rowH < list.y || y > list.y + list.h) continue;
        const Row& row = rows[i];
        if (row.head) { drawText(row.title, list.x + 16, y + 26, 18, g_theme.dim, 0, 0, FS_BOLD); continue; }
        SDL_Rect rr = {list.x, y + 3, list.w, rowH - 6};
        bool cur = row.p->name == m_preset;
        if (cur) fillRound(rr, 12, withAlpha(g_theme.accent, 50));
        else if (pressing(rr)) fillRound(rr, 12, g_theme.line);
        drawText(row.p->name, rr.x + 18, rr.y + (rr.h - fontHeight(22)) / 2, 22, cur ? g_theme.accent : g_theme.fg, 0, rr.w - 110);
        if (!row.p->builtIn) {
            drawText(row.p->parametric ? "parametric" : "graphic", rr.x + rr.w - 70, rr.y + 16, 15, g_theme.dim, 2);
            if (iconButton(rr.x + rr.w - 30.0f, rr.y + rr.h / 2.0f, 22, IC_TRASH, g_theme.dim, SDL_Color{0, 0, 0, 0}))
                del = (int)(row.p - m_custom.data());
        }
        if (tapped(rr)) {
            dsp::Preset p = *row.p;
            popClip();
            choosePreset(p, audio);
            m_presets = false;
            return;
        }
    }
    popClip();
    if (del >= 0) {
        std::string name = m_custom[del].name;
        dialog("Delete preset?", "\"" + name + "\" will be removed.", {"Cancel", "Delete"}, [this, name](int b) {
            if (b != 1) return;
            m_custom.erase(std::remove_if(m_custom.begin(), m_custom.end(), [&](const dsp::Preset& p) { return p.name == name; }),
                           m_custom.end());
            saveCustom();
        });
    }
    if (g_in.back || (g_in.tap && !g_in.consumed && !inRect(g_in.x, g_in.y, r))) m_presets = false;
    g_in.consumed = true;
}
