// Draws the Media Center icon (256x256) as a PPM: a violet-to-coral gradient,
// a white photo card with a sun and mountains, and a dark play badge.
#include <algorithm>
#include <cmath>
#include <cstdio>

struct C { double r, g, b; };
static C mix(C a, C b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
static C over(C dst, C src, double a) { return mix(dst, src, a); }

static double roundRect(double x, double y, double x0, double y0, double x1, double y1, double r) {
    double cx = std::clamp(x, x0 + r, x1 - r), cy = std::clamp(y, y0 + r, y1 - r);
    return std::hypot(x - cx, y - cy) - r;  // <0 inside
}
static bool inTri(double px, double py, double ax, double ay, double bx, double by, double cx, double cy) {
    auto s = [](double x1, double y1, double x2, double y2, double x3, double y3) { return (x1 - x3) * (y2 - y3) - (x2 - x3) * (y1 - y3); };
    double d1 = s(px, py, ax, ay, bx, by), d2 = s(px, py, bx, by, cx, cy), d3 = s(px, py, cx, cy, ax, ay);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static C shade(double x, double y) {
    const C violet = {0x7C / 255.0, 0x8C / 255.0, 0xFF / 255.0}, pink = {0xFF / 255.0, 0x7A / 255.0, 0xA8 / 255.0},
            coral = {0xFF / 255.0, 0xA8 / 255.0, 0x6B / 255.0}, white = {1, 1, 1}, ink = {0x1C / 255.0, 0x1B / 255.0, 0x2E / 255.0};
    double t = std::clamp((x + y) / 512.0, 0.0, 1.0);
    C c = t < 0.5 ? mix(violet, pink, t * 2) : mix(pink, coral, (t - 0.5) * 2);
    // soft shadow under the card
    double sh = roundRect(x, y, 46, 70, 210, 202, 20);
    if (sh < 14) c = over(c, {0, 0, 0}, 0.18 * std::clamp(1 - (sh + 0) / 14, 0.0, 1.0));
    // the card
    if (roundRect(x, y, 42, 60, 206, 192, 20) < 0) {
        c = white;
        // the picture inside it
        if (roundRect(x, y, 56, 74, 192, 178, 10) < 0) {
            double k = (y - 74) / 104.0;
            c = mix({0.80, 0.85, 1.0}, {1.0, 0.86, 0.90}, k);  // sky
            if (std::hypot(x - 158, y - 104) < 15) c = coral;  // sun
            // two mountains
            if (inTri(x, y, 56, 178, 104, 112, 152, 178)) c = mix(violet, pink, 0.25);
            if (inTri(x, y, 112, 178, 150, 130, 192, 178)) c = mix(violet, pink, 0.6);
        }
    }
    // play badge
    double d = std::hypot(x - 194, y - 186);
    if (d < 44) c = white;  // ring
    if (d < 38) {
        c = ink;
        if (inTri(x, y, 182, 168, 182, 204, 212, 186)) c = white;
    }
    return c;
}

int main(int argc, char** argv) {
    const int N = 256, SS = 4;
    FILE* f = fopen(argc > 1 ? argv[1] : "icon.ppm", "wb");
    fprintf(f, "P6\n%d %d\n255\n", N, N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            C acc = {0, 0, 0};
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    C c = shade(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS);
                    acc.r += c.r; acc.g += c.g; acc.b += c.b;
                }
            unsigned char px[3] = {(unsigned char)std::lround(acc.r / (SS * SS) * 255), (unsigned char)std::lround(acc.g / (SS * SS) * 255),
                                   (unsigned char)std::lround(acc.b / (SS * SS) * 255)};
            fwrite(px, 1, 3, f);
        }
    fclose(f);
    return 0;
}
