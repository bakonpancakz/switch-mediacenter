// MuPDF calls are wrapped in fz_try/fz_catch, which use setjmp/longjmp: keep
// C++ objects with destructors out of the try blocks.
//
// Rendering can take seconds per page in heavy magazines (MuPDF 1.17 decodes
// JPEG 2000 images at full size), so every render takes the document's cookie:
// pdfAbort() from another thread makes MuPDF stop at the next object.
#include "pdf.h"

#include <mupdf/fitz.h>

#include <algorithm>
#include <mutex>

#include "app.h"

struct PdfDoc {
    fz_context* ctx = nullptr;
    fz_document* doc = nullptr;
    int pages = 0;
    fz_cookie cookie = {};
};

// Process memory, for the log: heavy pages are where it runs out.
static void memMB(int& usedMB, int& totalMB) {
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    usedMB = (int)(used >> 20);
    totalMB = (int)(total >> 20);
}

PdfDoc* pdfOpen(const std::string& path, std::string& err) {
    // the store caches fonts and decoded images between renders (so zoom tiles
    // don't decode a page's images again); the default (256 MB) is too much here
    fz_context* ctx = fz_new_context(nullptr, nullptr, 64 << 20);
    if (!ctx) { err = "Out of memory"; return nullptr; }
    fz_document* doc = nullptr;
    int pages = 0;
    const char* fail = nullptr;
    char msg[256] = "";
    fz_var(doc);
    fz_var(pages);
    fz_var(fail);
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = fz_open_document(ctx, path.c_str());
        if (fz_needs_password(ctx, doc) && !fz_authenticate_password(ctx, doc, "")) fail = "This PDF is password protected";
        else pages = fz_count_pages(ctx, doc);
    }
    fz_catch(ctx) {
        snprintf(msg, sizeof(msg), "%s", fz_caught_message(ctx));
        fail = msg;
    }
    if (!fail && pages <= 0) fail = "The PDF has no pages";
    if (fail) {
        err = fail;
        appLog("pdf %s: %s", path.c_str(), fail);
        if (doc) fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
        return nullptr;
    }
    PdfDoc* d = new PdfDoc;
    d->ctx = ctx;
    d->doc = doc;
    d->pages = pages;
    int used, total;
    memMB(used, total);
    appLog("pdf %s: %d pages, memory %d/%d MB", path.c_str(), pages, used, total);
    return d;
}

void pdfClose(PdfDoc* d) {
    if (!d) return;
    fz_drop_document(d->ctx, d->doc);
    fz_drop_context(d->ctx);
    delete d;
}

int pdfPageCount(PdfDoc* d) { return d ? d->pages : 0; }

void pdfAbort(PdfDoc* d) {
    if (d) d->cookie.abort = 1;
}
void pdfClearAbort(PdfDoc* d) {
    if (d) d->cookie.abort = 0;
}
bool pdfAborted(PdfDoc* d) { return d && d->cookie.abort; }

bool pdfPageSize(PdfDoc* d, int page, float& w, float& h) {
    fz_page* pg = nullptr;
    fz_rect r = {0, 0, 0, 0};
    bool ok = true;
    fz_var(pg);
    fz_var(r);
    fz_try(d->ctx) {
        pg = fz_load_page(d->ctx, d->doc, page);
        r = fz_bound_page(d->ctx, pg);
    }
    fz_always(d->ctx) { fz_drop_page(d->ctx, pg); }
    fz_catch(d->ctx) {
        appLog("pdf page %d: %s", page, fz_caught_message(d->ctx));
        ok = false;
    }
    w = r.x1 - r.x0;
    h = r.y1 - r.y0;
    return ok && w > 0 && h > 0;
}

// RGB pixmap -> opaque RGBA32 surface
static SDL_Surface* toSurface(fz_context* ctx, fz_pixmap* pix) {
    int w = fz_pixmap_width(ctx, pix), h = fz_pixmap_height(ctx, pix), n = fz_pixmap_components(ctx, pix);
    int stride = fz_pixmap_stride(ctx, pix);
    const unsigned char* src = fz_pixmap_samples(ctx, pix);
    if (n < 3 || w <= 0 || h <= 0) return nullptr;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return nullptr;
    for (int y = 0; y < h; y++) {
        const unsigned char* sp = src + (size_t)y * stride;
        uint8_t* dp = (uint8_t*)s->pixels + (size_t)y * s->pitch;
        for (int x = 0; x < w; x++, sp += n, dp += 4) {
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = 255;
        }
    }
    return s;
}

// Renders part of the page drawn at `scale`, or all of it (whole). Same setup
// as fz_new_pixmap_from_page (MuPDF 1.17 util.c), so pixel (x, y) of a region
// is pixel (x, y) of a whole-page render: the whole render's origin is the
// rounded, scaled page bounds, which needn't be 0. nullptr on error or abort.
static SDL_Surface* renderBox(PdfDoc* d, int page, float scale, bool whole, int x, int y, int w, int h) {
    fz_context* ctx = d->ctx;
    fz_page* pg = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    fz_var(pg);
    fz_var(pix);
    fz_var(dev);
    uint64_t t0 = nowMs();
    fz_try(ctx) {
        pg = fz_load_page(ctx, d->doc, page);
        fz_matrix ctm = fz_scale(scale, scale);
        fz_irect full = fz_round_rect(fz_transform_rect(fz_bound_page(ctx, pg), ctm));
        fz_irect box = whole ? full : fz_irect{full.x0 + x, full.y0 + y, full.x0 + x + w, full.y0 + y + h};
        // the pixmap's bbox picks which part of the scaled page gets drawn; no
        // alpha, painted white like a sheet of paper
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), box, nullptr, 0);
        fz_clear_pixmap_with_value(ctx, pix, 0xFF);
        dev = fz_new_draw_device(ctx, ctm, pix);
        fz_run_page(ctx, pg, dev, fz_identity, &d->cookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_page(ctx, pg);
    }
    fz_catch(ctx) {
        appLog("pdf render page %d: %s", page, fz_caught_message(ctx));
        fz_drop_pixmap(ctx, pix);
        return nullptr;
    }
    int used, total;
    memMB(used, total);
    if (d->cookie.abort) {
        // an abort leaves a half-drawn page: throw it away
        appLog("pdf page %d: cancelled after %d ms", page, (int)(nowMs() - t0));
        fz_drop_pixmap(ctx, pix);
        return nullptr;
    }
    SDL_Surface* s = toSurface(ctx, pix);
    appLog("pdf page %d %s %dx%d: %d ms, memory %d/%d MB", page, whole ? "page" : "tile", fz_pixmap_width(ctx, pix),
           fz_pixmap_height(ctx, pix), (int)(nowMs() - t0), used, total);
    fz_drop_pixmap(ctx, pix);
    return s;
}

SDL_Surface* pdfRender(PdfDoc* d, int page, float scale) { return renderBox(d, page, scale, true, 0, 0, 0, 0); }

SDL_Surface* pdfRenderRegion(PdfDoc* d, int page, float scale, int x, int y, int w, int h) {
    return renderBox(d, page, scale, false, x, y, w, h);
}
