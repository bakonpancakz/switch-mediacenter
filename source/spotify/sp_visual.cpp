#include "sp_visual.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

// ---------------------------------------------------------------- ultrablur

static void blurAxis(float* img, int n, bool horizontal, float sigma) {
    int rad = (int)ceilf(sigma * 2.5f);
    std::vector<float> k(rad * 2 + 1);
    float sum = 0;
    for (int i = -rad; i <= rad; i++) sum += k[i + rad] = expf(-(i * i) / (2 * sigma * sigma));
    for (auto& v : k) v /= sum;
    std::vector<float> line(n * 3);
    for (int a = 0; a < n; a++) {
        for (int b = 0; b < n; b++) {
            float acc[3] = {0, 0, 0};
            for (int i = -rad; i <= rad; i++) {
                int bb = std::clamp(b + i, 0, n - 1);
                const float* p = horizontal ? &img[(a * n + bb) * 3] : &img[(bb * n + a) * 3];
                for (int c = 0; c < 3; c++) acc[c] += p[c] * k[i + rad];
            }
            for (int c = 0; c < 3; c++) line[b * 3 + c] = acc[c];
        }
        for (int b = 0; b < n; b++) {
            float* p = horizontal ? &img[(a * n + b) * 3] : &img[(b * n + a) * 3];
            for (int c = 0; c < 3; c++) p[c] = line[b * 3 + c];
        }
    }
}

void Ultrablur::setCover(SDL_Surface* s) {
    SDL_Surface* small = s ? scaleSurface(s, F, F) : nullptr;
    if (!small) { m_has = false; return; }
    float next[F * F * 3];
    for (int y = 0; y < F; y++) {
        const uint8_t* row = (const uint8_t*)small->pixels + y * small->pitch;
        for (int x = 0; x < F; x++)
            for (int c = 0; c < 3; c++) next[(y * F + x) * 3 + c] = row[x * 4 + c] / 255.0f;
    }
    SDL_FreeSurface(small);

    // the most colourful, reasonably bright spot becomes the accent
    float best = -1;
    float acc[3] = {1, 1, 1};
    for (int i = 0; i < F * F; i++) {
        const float* p = &next[i * 3];
        float mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
        float score = (mx - mn) * (0.35f + mx);
        if (score > best) { best = score; acc[0] = p[0]; acc[1] = p[1]; acc[2] = p[2]; }
    }
    float mx = std::max({acc[0], acc[1], acc[2], 0.05f});
    for (int c = 0; c < 3; c++) acc[c] = acc[c] / mx * 0.85f + 0.15f;  // full brightness, a touch of white
    m_accent = {(Uint8)(acc[0] * 255), (Uint8)(acc[1] * 255), (Uint8)(acc[2] * 255), 255};

    // very soft: two gaussian passes, wider than the image's features
    for (int pass = 0; pass < 2; pass++) {
        blurAxis(next, F, true, 4.5f);
        blurAxis(next, F, false, 4.5f);
    }
    // vivid but dim enough for white text on top
    float meanY = 0;
    for (int i = 0; i < F * F; i++) {
        float* p = &next[i * 3];
        float Y = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        for (int c = 0; c < 3; c++) p[c] = std::clamp(Y + (p[c] - Y) * 1.8f, 0.0f, 1.0f);
        meanY += 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    }
    meanY /= F * F;
    float gain = std::clamp(0.30f / std::max(meanY, 0.01f), 0.5f, 2.5f);
    for (auto& v : next) v = std::clamp(v * gain, 0.0f, 0.85f);

    if (m_has) {
        // start the crossfade from whatever is on screen now
        float k = std::min(1.0f, (g_ticks - m_changedAt) / 1200.0f);
        for (int i = 0; i < F * F * 3; i++) m_prev[i] = m_hadPrev ? m_prev[i] + (m_cur[i] - m_prev[i]) * k : m_cur[i];
        m_hadPrev = true;
    } else m_hadPrev = false;
    std::copy(next, next + F * F * 3, m_cur);
    m_has = true;
    m_changedAt = g_ticks;
}

void Ultrablur::sample(const float* f, float u, float v, float out[3]) const {
    float x = std::clamp(u, 0.0f, 1.0f) * (F - 1), y = std::clamp(v, 0.0f, 1.0f) * (F - 1);
    int x0 = std::min((int)x, F - 2), y0 = std::min((int)y, F - 2);
    float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 3; c++) {
        float a = f[(y0 * F + x0) * 3 + c], b = f[(y0 * F + x0 + 1) * 3 + c];
        float d = f[((y0 + 1) * F + x0) * 3 + c], e = f[((y0 + 1) * F + x0 + 1) * 3 + c];
        out[c] = (a + (b - a) * fx) + ((d + (e - d) * fx) - (a + (b - a) * fx)) * fy;
    }
}

void Ultrablur::draw(const SDL_Rect& area, float glow) {
    if (!m_has) return;
    const int GX = 20, GY = 12;
    float t = g_ticks / 1000.0f;
    float k = std::min(1.0f, (g_ticks - m_changedAt) / 1200.0f);
    k = k * k * (3 - 2 * k);
    float bright = 0.9f + 0.45f * std::clamp(glow, 0.0f, 1.0f);
    std::vector<SDL_Vertex> v;
    v.reserve((GX + 1) * (GY + 1));
    for (int j = 0; j <= GY; j++) {
        for (int i = 0; i <= GX; i++) {
            float u = i / (float)GX, w = j / (float)GY;
            // slow, layered drift: the colours wander like a lava lamp
            float su = 0.5f + (u - 0.5f) * 0.72f + 0.11f * sinf(t * 0.19f + w * 2.4f + 1.3f) + 0.04f * sinf(t * 0.43f + u * 5.1f);
            float sv = 0.5f + (w - 0.5f) * 0.72f + 0.11f * cosf(t * 0.15f + u * 2.2f) + 0.04f * cosf(t * 0.37f + w * 4.4f);
            float a[3], b[3];
            sample(m_cur, su, sv, a);
            if (m_hadPrev && k < 1) {
                sample(m_prev, su, sv, b);
                for (int c = 0; c < 3; c++) a[c] = b[c] + (a[c] - b[c]) * k;
            }
            SDL_Color col = {(Uint8)std::min(255.0f, a[0] * bright * 255), (Uint8)std::min(255.0f, a[1] * bright * 255),
                             (Uint8)std::min(255.0f, a[2] * bright * 255), 255};
            v.push_back(SDL_Vertex{{area.x + area.w * u, area.y + area.h * w}, col, {0, 0}});
        }
    }
    std::vector<int> idx;
    idx.reserve(GX * GY * 6);
    for (int j = 0; j < GY; j++)
        for (int i = 0; i < GX; i++) {
            int a = j * (GX + 1) + i, b = a + 1, c = a + GX + 1, d = c + 1;
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(g_ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

// ---------------------------------------------------------------- visualizer

const char* visStyleName(int s) {
    static const char* n[] = {"Off", "Bars", "Wave", "Ribbons", "Pulse", "Mirror", "Mountains", "Stars", "Waterfall", "Halo"};
    return n[std::clamp(s, 0, VIS_COUNT - 1)];
}

void Visualizer::update(const dsp::SpectrumTap& tap, int delaySamples) {
    m_an.update(tap, kBands, g_dt, delaySamples);
    m_phase += g_dt * (0.6f + 2.2f * m_an.energy());
}

// Level at a position 0..1 across the spectrum, smoothed over neighbouring bands
static float levelAt(const std::vector<float>& lv, float u) {
    int n = (int)lv.size();
    float x = std::clamp(u, 0.0f, 1.0f) * (n - 1), acc = 0, wsum = 0;
    for (int k = -3; k <= 3; k++) {
        int i = std::clamp((int)x + k, 0, n - 1);
        float w = expf(-(i - x) * (i - x) / 4.0f);
        acc += lv[i] * w;
        wsum += w;
    }
    return acc / wsum;
}

// A soft line: a strip whose alpha fades from the centre line to both edges.
static void softLine(const std::vector<SDL_FPoint>& pts, const std::vector<float>& halfW, SDL_Color c) {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    for (size_t i = 0; i < pts.size(); i++) {
        SDL_FPoint p = pts[i];
        // normal from neighbours
        SDL_FPoint a = pts[i ? i - 1 : 0], b = pts[i + 1 < pts.size() ? i + 1 : i];
        float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
        float nx = l > 0 ? -dy / l : 0, ny = l > 0 ? dx / l : 1;
        float h = halfW[i];
        v.push_back(SDL_Vertex{{p.x + nx * h, p.y + ny * h}, withAlpha(c, 0), {0, 0}});
        v.push_back(SDL_Vertex{p, c, {0, 0}});
        v.push_back(SDL_Vertex{{p.x - nx * h, p.y - ny * h}, withAlpha(c, 0), {0, 0}});
        if (i) {
            int s = (int)(i - 1) * 3, t = s + 3;
            idx.insert(idx.end(), {s, t, s + 1, s + 1, t, t + 1, s + 1, t + 1, s + 2, s + 2, t + 1, t + 2});
        }
    }
    if (!v.empty()) SDL_RenderGeometry(g_ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

void Visualizer::drawRibbons(const SDL_Rect& area, SDL_Color c) {
    const auto& lv = m_an.levels();
    float maxH = area.h * 0.26f, cy = area.y + area.h - maxH * 0.62f;
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_ADD);  // overlapping strands glow
    const int N = 90;
    struct R { float freq, speed, off, amp; };
    static const R rib[4] = {{1.3f, 1.0f, 0.0f, 1.0f}, {2.1f, -0.7f, 1.7f, 0.8f}, {0.8f, 0.45f, 3.1f, 0.9f}, {3.0f, 1.4f, 4.6f, 0.55f}};
    for (int r = 0; r < 4; r++) {
        std::vector<SDL_FPoint> pts(N + 1);
        std::vector<float> core(N + 1), glow(N + 1);
        for (int i = 0; i <= N; i++) {
            float u = i / (float)N;
            float L = levelAt(lv, u);
            float env = powf(sinf(3.14159f * u), 0.8f);  // taper at the ends
            float y = sinf(u * rib[r].freq * 6.2832f + m_phase * rib[r].speed + rib[r].off) * (0.12f + 0.88f * L) * env;
            pts[i] = {area.x + area.w * u, cy - y * maxH * 0.5f * rib[r].amp};
            core[i] = 1.5f + 5 * L * env;
            glow[i] = 8 + 26 * L * env;
        }
        SDL_Color cc = c;
        if (r % 2) cc = mix(c, SDL_Color{255, 255, 255, 255}, 0.35f);
        softLine(pts, glow, withAlpha(cc, 38));
        softLine(pts, core, withAlpha(cc, 200));
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
}

void Visualizer::drawMirror(const SDL_Rect& area, SDL_Color c) {
    const auto& lv = m_an.levels();
    float maxH = area.h * 0.13f, cy = area.y + area.h - maxH * 1.25f;
    const int N = 80;
    // bass in the middle, highs to both sides; mirrored above and below the centre line
    std::vector<SDL_FPoint> top(N + 1), bottom(N + 1);
    for (int i = 0; i <= N; i++) {
        float u = i / (float)N, d = fabsf(u - 0.5f) * 2;
        float L = levelAt(lv, d * 0.92f);
        float h = (0.04f + L) * maxH * (1 - d * d * 0.3f);
        top[i] = {area.x + area.w * u, cy - h};
        bottom[i] = {area.x + area.w * u, cy + h};
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_ADD);
    for (int layer = 0; layer < 2; layer++) {
        std::vector<SDL_Vertex> v;
        std::vector<int> idx;
        SDL_Color edge = withAlpha(c, layer ? 150 : 40), mid = withAlpha(c, layer ? 70 : 20);
        float grow = layer ? 1.0f : 1.35f;  // a wider, fainter glow behind
        for (int i = 0; i <= N; i++) {
            float ht = (cy - top[i].y) * grow;
            v.push_back(SDL_Vertex{{top[i].x, cy - ht}, edge, {0, 0}});
            v.push_back(SDL_Vertex{{top[i].x, cy}, mid, {0, 0}});
            v.push_back(SDL_Vertex{{top[i].x, cy + ht}, edge, {0, 0}});
            if (i) {
                int s = (i - 1) * 3, t = s + 3;
                idx.insert(idx.end(), {s, t, s + 1, s + 1, t, t + 1, s + 1, t + 1, s + 2, s + 2, t + 1, t + 2});
            }
        }
        SDL_RenderGeometry(g_ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
    }
    std::vector<float> w(top.size(), 2.5f);
    softLine(top, w, withAlpha(c, 230));
    softLine(bottom, w, withAlpha(c, 230));
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
}

static void stripFill(const std::vector<SDL_FPoint>& top, float bottom, SDL_Color cTop, SDL_Color cBottom) {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    for (size_t i = 0; i < top.size(); i++) {
        v.push_back(SDL_Vertex{top[i], cTop, {0, 0}});
        v.push_back(SDL_Vertex{{top[i].x, bottom}, cBottom, {0, 0}});
        if (i) {
            int a = (int)(i - 1) * 2, b = a + 1, c = a + 2, d = a + 3;
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    if (!v.empty()) SDL_RenderGeometry(g_ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

static std::vector<SDL_FPoint> smoothCurve(const std::vector<float>& lv, float x0, float w, float bottom, float h) {
    int n = (int)lv.size();
    std::vector<SDL_FPoint> pts;
    auto at = [&](int i) { return lv[std::clamp(i, 0, n - 1)]; };
    const int sub = 4;
    for (int i = 0; i < n; i++) {
        for (int s = 0; s < sub; s++) {
            float t = s / (float)sub;
            // Catmull-Rom through the band levels
            float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
            float y = 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
            float x = x0 + w * (i + t + 0.5f) / n;
            if (x > x0 + w) break;
            pts.push_back({x, bottom - std::clamp(y, 0.0f, 1.0f) * h});
        }
    }
    pts.insert(pts.begin(), SDL_FPoint{x0, pts.empty() ? bottom : pts.front().y});
    pts.push_back({x0 + w, pts.back().y});
    return pts;
}

void Visualizer::drawBackground(const SDL_Rect& area, int style, SDL_Color c) {
    const auto& lv = m_an.levels();
    const auto& pk = m_an.peaks();
    if (lv.empty()) return;
    float bottom = (float)(area.y + area.h), maxH = area.h * 0.26f;
    int n = (int)lv.size();
    if (style == VIS_BARS) {
        float bw = area.w / (float)n, gap = std::max(2.0f, bw * 0.28f);
        for (int i = 0; i < n; i++) {
            float h = std::max(3.0f, lv[i] * maxH);
            int x = (int)(area.x + i * bw + gap / 2), w = std::max(2, (int)(bw - gap));
            fillGradientV({x, (int)(bottom - h), w, (int)h}, withAlpha(c, 150), withAlpha(c, 25));
            int py = (int)(bottom - std::max(3.0f, pk[i] * maxH)) - 6;
            fillRound({x, py, w, 4}, 2, withAlpha(c, 210));
        }
    } else if (style == VIS_WAVE) {
        // peaks as a faint back layer, levels in front
        auto back = smoothCurve(pk, (float)area.x, (float)area.w, bottom, maxH * 1.05f);
        stripFill(back, bottom, withAlpha(c, 60), withAlpha(c, 0));
        auto front = smoothCurve(lv, (float)area.x, (float)area.w, bottom, maxH);
        stripFill(front, bottom, withAlpha(c, 140), withAlpha(c, 10));
        for (size_t i = 1; i < front.size(); i++)
            drawLine(front[i - 1].x, front[i - 1].y, front[i].x, front[i].y, 3, withAlpha(c, 230), false);
    } else if (style == VIS_RIBBONS) {
        drawRibbons(area, c);
    } else if (style == VIS_MIRROR) {
        drawMirror(area, c);
    } else if (style == VIS_MOUNTAINS) {
        drawMountains(area, c);
    } else if (style == VIS_STARS) {
        drawStars(area, c);
    } else if (style == VIS_WATERFALL) {
        drawWaterfall(area, c);
    }
}

// Three ranges of hills, back to front: the peaks, the spectrum turned round,
// and the spectrum itself, each lower, brighter and less smoothed than the last.
void Visualizer::drawMountains(const SDL_Rect& area, SDL_Color c) {
    const auto& lv = m_an.levels();
    const auto& pk = m_an.peaks();
    int n = (int)lv.size();
    float bottom = (float)(area.y + area.h), maxH = area.h * 0.26f;
    std::vector<float> back(n), mid(n);
    for (int i = 0; i < n; i++) {
        back[i] = levelAt(pk, i / (float)(n - 1));
        mid[i] = levelAt(lv, 1 - i / (float)(n - 1));
    }
    SDL_Color far = mix(c, SDL_Color{0, 0, 0, 255}, 0.45f), near = mix(c, SDL_Color{255, 255, 255, 255}, 0.18f);
    stripFill(smoothCurve(back, (float)area.x, (float)area.w, bottom, maxH * 1.35f), bottom, withAlpha(far, 70), withAlpha(far, 20));
    stripFill(smoothCurve(mid, (float)area.x, (float)area.w, bottom, maxH * 1.05f), bottom, withAlpha(c, 95), withAlpha(c, 25));
    auto front = smoothCurve(lv, (float)area.x, (float)area.w, bottom, maxH * 0.8f);
    stripFill(front, bottom, withAlpha(near, 175), withAlpha(near, 40));
    std::vector<float> w(front.size(), 2.0f);
    softLine(front, w, withAlpha(near, 230));
}

// Flying through stars: faster with the music, bigger on the beat.
void Visualizer::drawStars(const SDL_Rect& area, SDL_Color c) {
    if (m_stars.empty()) {
        m_stars.resize(170);
        for (auto& s : m_stars) s = {(rand() % 2001 - 1000) / 1000.0f, (rand() % 2001 - 1000) / 1000.0f, (rand() % 1000 + 1) / 1000.0f};
    }
    float e = m_an.energy(), b = m_an.bass();
    float cx = area.x + area.w / 2.0f, cy = area.y + area.h / 2.0f, scale = std::max(area.w, area.h) * 0.35f;
    for (auto& s : m_stars) {
        s.z -= g_dt * (0.08f + 0.9f * e * e);
        if (s.z <= 0.03f) s = {(rand() % 2001 - 1000) / 1000.0f, (rand() % 2001 - 1000) / 1000.0f, 1.0f};
        float sx = cx + s.x / s.z * scale, sy = cy + s.y / s.z * scale;
        if (sx < area.x || sx > area.x + area.w || sy < area.y || sy > area.y + area.h) continue;
        float near = 1 - s.z;
        float r = 0.8f + near * near * (3.5f + 3 * b);
        fillCircle(sx, sy, r, withAlpha(mix(c, SDL_Color{255, 255, 255, 255}, near * 0.6f), (Uint8)std::min(255.0f, 40 + 215 * near)));
    }

}

// A spectrogram rising from the bottom: each row the spectrum a moment ago.
void Visualizer::drawWaterfall(const SDL_Rect& area, SDL_Color c) {
    const auto& lv = m_an.levels();
    const int rows = 44;
    m_rowClock += g_dt;
    if (m_history.empty() || m_rowClock > 1 / 30.0f) {
        m_rowClock = 0;
        m_history.insert(m_history.begin(), lv);
        if ((int)m_history.size() > rows) m_history.pop_back();
    }
    int n = (int)lv.size();
    float h = area.h * 0.34f, rowH = h / rows, bw = area.w / (float)n, bottom = (float)(area.y + area.h);
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    v.reserve(m_history.size() * n * 4);
    for (size_t r = 0; r < m_history.size(); r++) {
        float fade = 1 - r / (float)rows, y1 = bottom - r * rowH, y0 = y1 - rowH + 1;
        for (int i = 0; i < n && i < (int)m_history[r].size(); i++) {
            float L = m_history[r][i];
            if (L < 0.04f) continue;
            SDL_Color col = withAlpha(mix(c, SDL_Color{255, 255, 255, 255}, L * L * 0.5f), (Uint8)(230 * L * fade));
            float x0 = area.x + i * bw + 1, x1 = area.x + (i + 1) * bw - 1;
            int k = (int)v.size();
            v.push_back({{x0, y0}, col, {0, 0}});
            v.push_back({{x1, y0}, col, {0, 0}});
            v.push_back({{x1, y1}, col, {0, 0}});
            v.push_back({{x0, y1}, col, {0, 0}});
            idx.insert(idx.end(), {k, k + 1, k + 2, k, k + 2, k + 3});
        }
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    if (!v.empty()) SDL_RenderGeometry(g_ren, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
}

// Bars all the way round the cover, the low notes at the bottom, mirrored left and right.
void Visualizer::drawHalo(const SDL_Rect& art, SDL_Color c) {
    const auto& lv = m_an.levels();
    float cx = art.x + art.w / 2.0f, cy = art.y + art.h / 2.0f;
    float R = sqrtf((float)(art.w * art.w + art.h * art.h)) / 2 * 0.86f + 10;
    const int N = 96;
    float thick = std::max(2.0f, 2 * 3.14159f * R / N * 0.45f);
    for (int i = 0; i < N; i++) {
        float a = i / (float)N * 2 * 3.14159f;  // 0 at the bottom, going round
        float u = fabsf(i / (float)N * 2 - 1);   // 1 at the bottom, 0 at the top
        float L = levelAt(lv, (1 - u) * 0.9f);
        float len = 4 + L * R * 0.5f;
        float dx = sinf(a), dy = cosf(a);
        drawLine(cx + dx * R, cy + dy * R, cx + dx * (R + len), cy + dy * (R + len), thick, withAlpha(c, (Uint8)(90 + 150 * L)), true);
    }

}

float Visualizer::coverScale(int style) const {
    if (style != VIS_PULSE) return 1.0f;
    float b = m_an.bass();
    return 1.0f + 0.05f * b * b;
}

void Visualizer::drawCoverGlow(const SDL_Rect& art, int style, SDL_Color c) {
    if (style == VIS_HALO) { drawHalo(art, c); return; }
    if (style != VIS_PULSE) return;
    float b = m_an.bass();
    for (int k = 7; k >= 1; k--) {
        int grow = (int)(k * (6 + 10 * b));
        Uint8 a = (Uint8)std::clamp(70.0f * b * (1 - k / 8.0f), 0.0f, 255.0f);
        fillRound({art.x - grow, art.y - grow, art.w + grow * 2, art.h + grow * 2}, 18 + grow, withAlpha(c, a));
    }
}
