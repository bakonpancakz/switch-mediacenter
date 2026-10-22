// PDF (and XPS/OXPS) pages rendered with MuPDF. A PdfDoc owns its own MuPDF
// context, so different documents can be used from different threads, but one
// PdfDoc must only be used by one thread at a time.
#pragma once
#include <SDL2/SDL.h>
#include <string>

struct PdfDoc;

PdfDoc* pdfOpen(const std::string& path, std::string& err);
void pdfClose(PdfDoc* d);
int pdfPageCount(PdfDoc* d);
// Page size in points (1/72 inch); false if the page can't be loaded.
bool pdfPageSize(PdfDoc* d, int page, float& w, float& h);
// RGBA32 on white, scale = pixels per point. nullptr on failure.
SDL_Surface* pdfRender(PdfDoc* d, int page, float scale);
// Part of the page rendered at `scale`: the rectangle (x, y, w, h) in pixels of
// the page drawn at that scale. Used to keep zoomed-in text sharp.
SDL_Surface* pdfRenderRegion(PdfDoc* d, int page, float scale, int x, int y, int w, int h);

// Cancelling a render: pdfAbort may be called from any thread and makes the
// current render stop early and return nullptr. The flag stays set until
// pdfClearAbort, which the rendering thread calls before each new job.
void pdfAbort(PdfDoc* d);
void pdfClearAbort(PdfDoc* d);
bool pdfAborted(PdfDoc* d);
