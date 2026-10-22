// Image decoding (turbojpeg for fast scaled JPEG decodes, SDL_image for the
// rest), EXIF orientation and box downscaling.
#include "app.h"
#include <SDL2/SDL_image.h>
#include <turbojpeg.h>
#include <avif/avif.h>
#include <cstring>
#include <algorithm>
#include "ui/anim.h"


// ---------------------------------------------------------------- EXIF

int jpegExifOrientation(const uint8_t* d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;
    size_t p = 2;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) return 1;
        uint8_t marker = d[p + 1];
        if (marker == 0xDA || marker == 0xD9) return 1;  // start of scan: no EXIF before it
        size_t len = (d[p + 2] << 8) | d[p + 3];
        if (marker == 0xE1 && len >= 16 && p + 2 + len <= n && memcmp(d + p + 4, "Exif\0\0", 6) == 0) {
            const uint8_t* t = d + p + 10;
            size_t tn = len - 8;
            bool le = t[0] == 'I';
            auto u16 = [&](size_t o) -> uint32_t { return o + 2 > tn ? 0 : le ? (t[o] | t[o + 1] << 8) : (t[o] << 8 | t[o + 1]); };
            auto u32 = [&](size_t o) -> uint32_t {
                return o + 4 > tn ? 0 : le ? (t[o] | t[o + 1] << 8 | t[o + 2] << 16 | (uint32_t)t[o + 3] << 24)
                                           : ((uint32_t)t[o] << 24 | t[o + 1] << 16 | t[o + 2] << 8 | t[o + 3]);
            };
            size_t ifd = u32(4);
            uint32_t count = u16(ifd);
            for (uint32_t i = 0; i < count && i < 256; i++) {
                size_t e = ifd + 2 + i * 12;
                if (u16(e) == 0x0112) {
                    uint32_t v = u16(e + 8);
                    return (v >= 1 && v <= 8) ? (int)v : 1;
                }
            }
            return 1;
        }
        p += 2 + len;
    }
    return 1;
}

static SDL_Surface* applyOrientation(SDL_Surface* s, int o) {
    if (o <= 1 || o > 8) return s;
    int w = s->w, h = s->h;
    bool swap = o >= 5;
    SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, swap ? h : w, swap ? w : h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!d) return s;
    for (int y = 0; y < d->h; y++) {
        uint32_t* out = (uint32_t*)((uint8_t*)d->pixels + y * d->pitch);
        for (int x = 0; x < d->w; x++) {
            int sx, sy;
            switch (o) {
                case 2: sx = w - 1 - x; sy = y; break;
                case 3: sx = w - 1 - x; sy = h - 1 - y; break;
                case 4: sx = x; sy = h - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = h - 1 - x; break;
                case 7: sx = w - 1 - y; sy = h - 1 - x; break;
                default: sx = w - 1 - y; sy = x; break;  // 8
            }
            out[x] = *((uint32_t*)((uint8_t*)s->pixels + sy * s->pitch) + sx);
        }
    }
    SDL_FreeSurface(s);
    return d;
}

// ---------------------------------------------------------------- scaling

static void boxScale(SDL_Surface* src, SDL_Surface* dst) {
    const int sw = src->w, sh = src->h, dw = dst->w, dh = dst->h;
    std::vector<int> x0(dw + 1);
    for (int x = 0; x <= dw; x++) x0[x] = (int)((long long)x * sw / dw);
    std::vector<uint32_t> acc((size_t)dw * 4);
    for (int y = 0; y < dh; y++) {
        int ya = (int)((long long)y * sh / dh), yb = std::max(ya + 1, (int)((long long)(y + 1) * sh / dh));
        std::fill(acc.begin(), acc.end(), 0);
        for (int sy = ya; sy < yb; sy++) {
            const uint8_t* row = (const uint8_t*)src->pixels + (size_t)sy * src->pitch;
            for (int x = 0; x < dw; x++) {
                int xa = x0[x], xb = std::max(xa + 1, x0[x + 1]);
                uint32_t r = 0, g = 0, b = 0, a = 0;
                for (int sx = xa; sx < xb; sx++) {
                    const uint8_t* p = row + sx * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                }
                uint32_t* A = &acc[x * 4];
                A[0] += r; A[1] += g; A[2] += b; A[3] += a;
            }
        }
        uint8_t* out = (uint8_t*)dst->pixels + (size_t)y * dst->pitch;
        for (int x = 0; x < dw; x++) {
            uint32_t cnt = (uint32_t)(std::max(x0[x] + 1, x0[x + 1]) - x0[x]) * (yb - ya);
            for (int c = 0; c < 4; c++) out[x * 4 + c] = (uint8_t)(acc[x * 4 + c] / cnt);
        }
    }
}

SDL_Surface* scaleSurface(SDL_Surface* src, int w, int h) {
    SDL_Surface* rgba = src->format->format == SDL_PIXELFORMAT_RGBA32 ? src : SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
    if (!rgba) return nullptr;
    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, w), std::max(1, h), 32, SDL_PIXELFORMAT_RGBA32);
    if (out) {
        if (w <= rgba->w && h <= rgba->h) boxScale(rgba, out);
        else {
            SDL_SetSurfaceBlendMode(rgba, SDL_BLENDMODE_NONE);
            SDL_BlitScaled(rgba, nullptr, out, nullptr);
        }
    }
    if (rgba != src) SDL_FreeSurface(rgba);
    return out;
}

// Fits (w,h) into (maxW,maxH) keeping aspect; 0 means unlimited.
static void fitSize(int w, int h, int maxW, int maxH, int& ow, int& oh) {
    float s = 1.0f;
    if (maxW > 0) s = std::min(s, maxW / (float)w);
    if (maxH > 0) s = std::min(s, maxH / (float)h);
    ow = std::max(1, (int)(w * s + 0.5f));
    oh = std::max(1, (int)(h * s + 0.5f));
}

static SDL_Surface* decodeJpeg(const uint8_t* data, size_t len, int maxW, int maxH, int swapped, int& fullW, int& fullH) {
    tjhandle tj = tjInitDecompress();
    if (!tj) return nullptr;
    int w, h, sub, cs;
    if (tjDecompressHeader3(tj, data, (unsigned long)len, &w, &h, &sub, &cs) != 0) { tjDestroy(tj); return nullptr; }
    fullW = w;
    fullH = h;
    // targets are given after orientation; compare against the stored axes
    int tw = swapped ? maxH : maxW, th = swapped ? maxW : maxH;
    int n = 0;
    tjscalingfactor* sf = tjGetScalingFactors(&n);
    int bestW = w, bestH = h;
    // no target size means full resolution (otherwise every scale "fits" and 1/8 wins)
    for (int i = 0; i < n && (tw > 0 || th > 0); i++) {
        int sw = TJSCALED(w, sf[i]), sh = TJSCALED(h, sf[i]);
        bool fits = (tw <= 0 || sw >= tw) && (th <= 0 || sh >= th);
        // smallest decode that is still at least the target size
        if (fits && sw * sh < bestW * bestH) { bestW = sw; bestH = sh; }
    }
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, bestW, bestH, 32, SDL_PIXELFORMAT_RGBA32);
    if (s && tjDecompress2(tj, data, (unsigned long)len, (unsigned char*)s->pixels, bestW, s->pitch, bestH, TJPF_RGBA,
                           TJFLAG_FASTDCT) != 0) {
        // progressive/odd JPEGs may still decode with warnings; treat hard errors as failure
        if (tjGetErrorCode(tj) == TJERR_FATAL) { SDL_FreeSurface(s); s = nullptr; }
    }
    tjDestroy(tj);
    return s;
}

// AVIF: an ISO-BMFF file whose "ftyp" box lists the avif/avis brand.
static bool isAvif(const uint8_t* d, size_t len) {
    if (len < 16 || memcmp(d + 4, "ftyp", 4) != 0) return false;
    size_t box = ((size_t)d[0] << 24) | ((size_t)d[1] << 16) | ((size_t)d[2] << 8) | d[3];
    box = std::min(box, len);
    for (size_t i = 8; i + 4 <= box; i += 4)
        if (!memcmp(d + i, "avif", 4) || !memcmp(d + i, "avis", 4)) return true;
    return false;
}

// SDL_image 2.0.4 has no AVIF support; libavif (with dav1d) decodes it. First
// frame of an animated one. Rotation/mirroring flags in the file are ignored.
static SDL_Surface* decodeAvif(const uint8_t* data, size_t len) {
    avifDecoder* dec = avifDecoderCreate();
    if (!dec) return nullptr;
    dec->maxThreads = 2;
    SDL_Surface* s = nullptr;
    avifResult r = avifDecoderSetIOMemory(dec, data, len);
    if (r == AVIF_RESULT_OK) r = avifDecoderParse(dec);
    if (r == AVIF_RESULT_OK) r = avifDecoderNextImage(dec);
    if (r == AVIF_RESULT_OK) {
        s = SDL_CreateRGBSurfaceWithFormat(0, (int)dec->image->width, (int)dec->image->height, 32, SDL_PIXELFORMAT_RGBA32);
        if (s) {
            avifRGBImage rgb;
            avifRGBImageSetDefaults(&rgb, dec->image);
            rgb.format = AVIF_RGB_FORMAT_RGBA;
            rgb.depth = 8;
            rgb.pixels = (uint8_t*)s->pixels;
            rgb.rowBytes = (uint32_t)s->pitch;
            r = avifImageYUVToRGB(dec->image, &rgb);
            if (r != AVIF_RESULT_OK) { SDL_FreeSurface(s); s = nullptr; }
        }
    }
    if (r != AVIF_RESULT_OK) appLog("avif decode failed: %s", avifResultToString(r));
    avifDecoderDestroy(dec);
    return s;
}


SDL_Surface* loadImageMem(const void* data, size_t len, int maxW, int maxH, int* origW, int* origH) {
    const uint8_t* d = (const uint8_t*)data;
    int orient = 1;
    SDL_Surface* s = nullptr;
    int fw = 0, fh = 0;
    if (len > 3 && d[0] == 0xFF && d[1] == 0xD8) {
        orient = jpegExifOrientation(d, len);
        s = decodeJpeg(d, len, maxW, maxH, orient >= 5, fw, fh);
    } else if (isAvif(d, len)) {
        s = decodeAvif(d, len);
        if (!s) return nullptr;
        fw = s->w;
        fh = s->h;
    }
    if (!s) {
        SDL_Surface* raw = IMG_Load_RW(SDL_RWFromConstMem(data, (int)len), 1);
        if (!raw) raw = webpAnimFirstFrame(d, len);  // SDL_image can't read animated WebP
        if (!raw) return nullptr;
        fw = raw->w;
        fh = raw->h;
        s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(raw);
        if (!s) return nullptr;
    }
    bool swap = orient >= 5;
    int ow = swap ? fh : fw, oh = swap ? fw : fh;
    if (origW) *origW = ow;
    if (origH) *origH = oh;
    // downscale (in stored orientation) then rotate the smaller image
    int tw, th;
    fitSize(ow, oh, maxW, maxH, tw, th);
    if (swap) std::swap(tw, th);
    if (tw < s->w || th < s->h) {
        SDL_Surface* sc = scaleSurface(s, std::min(tw, s->w), std::min(th, s->h));
        if (sc) { SDL_FreeSurface(s); s = sc; }
    }
    return applyOrientation(s, orient);
}

SDL_Surface* loadImage(const std::string& path, int maxW, int maxH, int* origW, int* origH) {
    std::string data;
    if (!readFile(path, data) || data.empty()) return nullptr;
    return loadImageMem(data.data(), data.size(), maxW, maxH, origW, origH);
}
