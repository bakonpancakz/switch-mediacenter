#include "winmedia.h"

#include <windows.h>
#include <asyncinfo.h>
#include <inspectable.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <roapi.h>
#include <shcore.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winstring.h>

#include <cmath>

#include "com.h"
#include "util.h"

namespace {

thread_local bool t_mf = false;

}  // namespace

void winMediaThreadInit() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!t_mf) t_mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

void winMediaThreadExit() {
    if (t_mf) MFShutdown();
    t_mf = false;
    CoUninitialize();
}

// ---------------------------------------------------------------- Shell thumbnails

bool shellThumbnail(const std::string& path, int size, Image& out) {
    Com<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(winPath(path).c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    // a real thumbnail or nothing: no file-type icon standing in for one
    if (FAILED(f->GetImage(SIZE{size, size}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb) return false;
    BITMAP bm = {};
    GetObjectW(hb, sizeof(bm), &bm);
    bool ok = bm.bmWidth > 0 && bm.bmHeight > 0;
    if (ok) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.w = bm.bmWidth;
        out.h = bm.bmHeight;
        out.bgra.assign((size_t)out.w * out.h * 4, 0);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, hb, 0, out.h, out.bgra.data(), &bi, DIB_RGB_COLORS) == out.h;
        ReleaseDC(nullptr, dc);
        // Shell bitmaps are premultiplied when they have alpha (shown over black, the colours
        // are already right), and some have no alpha at all (every byte 0): opaque either way
        for (size_t i = 3; i < out.bgra.size(); i += 4) out.bgra[i] = 255;
    }
    DeleteObject(hb);
    if (!ok) return false;
    if (out.w > size || out.h > size) out = scaleImage(out, size);
    return true;
}

// ---------------------------------------------------------------- Media Foundation

bool mfProbe(const std::string& path, MediaInfo& out) {
    if (!t_mf) return false;
    Com<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(winPath(path).c_str(), nullptr, &r))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) && v.vt == VT_UI8)
        out.durationMs = (int64_t)(v.uhVal.QuadPart / 10000);
    PropVariantClear(&v);
    for (DWORD i = 0;; i++) {
        Com<IMFMediaType> mt;
        HRESULT hr = r->GetNativeMediaType(i, 0, &mt);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(hr)) continue;
        GUID major = {};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio) out.audio = true;
        else if (major == MFMediaType_Video && !out.video) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
            if (!w || !h) continue;  // cover art and such
            out.video = true;
            out.width = (int)w;
            out.height = (int)h;
            // phones record portrait video as landscape plus a rotation
            UINT32 rot = MFGetAttributeUINT32(mt, MF_MT_VIDEO_ROTATION, 0);
            if (rot == 90 || rot == 270) std::swap(out.width, out.height);
        }
    }
    return out.video || out.audio;
}

bool mfAudioPeaks(const std::string& path, int count, std::vector<uint8_t>& out, int64_t* durationMs, const std::atomic<bool>* cancel) {
    if (!t_mf || count <= 0) return false;
    Com<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(winPath(path).c_str(), nullptr, &r))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    uint64_t dur = 0;  // 100 ns units
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) && v.vt == VT_UI8) dur = v.uhVal.QuadPart;
    PropVariantClear(&v);
    if (!dur) return false;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) return false;
    // 16-bit PCM, at whatever rate and channels the file has
    Com<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want))) return false;
    Com<IMFMediaType> got;
    if (FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got))) return false;
    UINT32 channels = MFGetAttributeUINT32(got, MF_MT_AUDIO_NUM_CHANNELS, 0), rate = MFGetAttributeUINT32(got, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    UINT32 bits = MFGetAttributeUINT32(got, MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
    if (!channels || !rate || bits != 16) return false;

    std::vector<double> sum(count, 0.0);
    std::vector<uint64_t> n(count, 0);
    uint64_t frame = 0;  // frames decoded so far: where we are in the file
    double framesTotal = dur / 1e7 * rate;
    for (;;) {
        if (cancel && *cancel) return false;
        DWORD flags = 0;
        Com<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        Com<IMFMediaBuffer> buf;
        if (FAILED(s->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) continue;
        const int16_t* pcm = (const int16_t*)p;
        size_t frames = len / (2 * channels);
        for (size_t f = 0; f < frames; f++, frame++) {
            int b = (int)std::min<double>(count - 1, frame * count / std::max(1.0, framesTotal));
            double e = 0;
            for (UINT32 c = 0; c < channels; c++) {
                double x = pcm[f * channels + c];
                e += x * x;
            }
            sum[b] += e / channels;
            n[b]++;
        }
        buf->Unlock();
    }
    if (!frame) return false;
    std::vector<double> rms(count, 0.0);
    double peak = 0;
    for (int i = 0; i < count; i++) {
        rms[i] = n[i] ? std::sqrt(sum[i] / n[i]) : 0;
        peak = std::max(peak, rms[i]);
    }
    out.resize(count);
    for (int i = 0; i < count; i++) out[i] = peak > 0 ? (uint8_t)std::lround(255 * rms[i] / peak) : 0;
    if (durationMs) *durationMs = (int64_t)(dur / 10000);
    return true;
}

// ---------------------------------------------------------------- PDF (Windows.Data.Pdf)

// The few WinRT interfaces this needs, declared by hand: the toolchain has no
// windows.data.pdf.h. IIDs and method order are from Windows' own metadata
// (Windows.Data.Pdf.winmd). Not in an anonymous namespace: GCC would see that
// nothing here implements them and "devirtualize" the calls into garbage.
namespace pdfabi {

const IID IID_IPdfDocumentStatics = {0x433a0b5f, 0xc007, 0x4788, {0x90, 0xf2, 0x08, 0x14, 0x3d, 0x92, 0x25, 0x99}};
const IID IID_IPdfPageRenderOptions = {0x3c98056f, 0xb7cf, 0x4c29, {0x9a, 0x04, 0x52, 0xd9, 0x02, 0x67, 0xf4, 0x25}};
const IID IID_IRandomAccessStreamAbi = {0x905a0fe1, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};

struct IPdfPage;
struct IPdfDocument : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32 index, IPdfPage** page) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean* v) = 0;
};
// IAsyncOperation<PdfDocument>: only its GetResults is needed (waiting is done through IAsyncInfo)
struct IAsyncOperationPdfDocument : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IPdfDocument** result) = 0;
};
struct IAsyncActionAbi : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};
struct IPdfDocumentStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IUnknown* file, IAsyncOperationPdfDocument** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IUnknown* file, HSTRING password, IAsyncOperationPdfDocument** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IUnknown* stream, IAsyncOperationPdfDocument** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(IUnknown* stream, HSTRING password, IAsyncOperationPdfDocument** op) = 0;
};
struct IPdfPageRenderOptions : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(void* rect) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(double x, double y, double w, double h) = 0;  // a Rect is four floats; not used
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32 v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32 v) = 0;
};
struct IPdfPage : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IUnknown* stream, IAsyncActionAbi** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IUnknown* stream, IPdfPageRenderOptions* options, IAsyncActionAbi** op) = 0;
};

}  // namespace pdfabi

namespace {
using namespace pdfabi;

struct HStr {
    HSTRING h = nullptr;
    explicit HStr(const wchar_t* s) { WindowsCreateString(s, (UINT32)wcslen(s), &h); }
    ~HStr() { if (h) WindowsDeleteString(h); }
};

// Waits for a WinRT async operation to finish (30 s at most).
bool waitAsync(IUnknown* op) {
    Com<IAsyncInfo> info;
    if (FAILED(op->QueryInterface(__uuidof(IAsyncInfo), (void**)&info))) return false;
    for (int i = 0; i < 6000; i++) {
        AsyncStatus st = Started;
        if (FAILED(info->get_Status(&st))) return false;
        if (st == Completed) return true;
        if (st != Started) return false;
        Sleep(5);
    }
    info->Cancel();
    return false;
}

}  // namespace

bool pdfFirstPage(const std::string& path, int height, Image& out) {
    RoInitialize(RO_INIT_MULTITHREADED);  // the thread is already in the MTA: this just makes sure
    Com<IPdfDocumentStatics> statics;
    HStr docClass(L"Windows.Data.Pdf.PdfDocument");
    if (FAILED(RoGetActivationFactory(docClass.h, IID_IPdfDocumentStatics, (void**)&statics))) return false;
    Com<IUnknown> in;
    if (FAILED(CreateRandomAccessStreamOnFile(winPath(path).c_str(), 0 /* read */, IID_IRandomAccessStreamAbi, (void**)&in))) return false;
    Com<IAsyncOperationPdfDocument> load;
    if (FAILED(statics->LoadFromStreamAsync(in, &load)) || !waitAsync(load)) return false;
    Com<IPdfDocument> doc;
    if (FAILED(load->GetResults(&doc)) || !doc) return false;
    Com<IPdfPage> page;
    if (FAILED(doc->GetPage(0, &page)) || !page) return false;

    HStr optClass(L"Windows.Data.Pdf.PdfPageRenderOptions");
    Com<IInspectable> optInsp;
    Com<IPdfPageRenderOptions> opts;
    if (FAILED(RoActivateInstance(optClass.h, &optInsp)) || FAILED(optInsp->QueryInterface(IID_IPdfPageRenderOptions, (void**)&opts))) return false;
    opts->put_DestinationHeight((UINT32)height);
    HStr memClass(L"Windows.Storage.Streams.InMemoryRandomAccessStream");
    Com<IInspectable> memInsp;
    Com<IUnknown> mem;
    if (FAILED(RoActivateInstance(memClass.h, &memInsp)) || FAILED(memInsp->QueryInterface(IID_IRandomAccessStreamAbi, (void**)&mem))) return false;
    Com<IAsyncActionAbi> render;
    if (FAILED(page->RenderWithOptionsToStreamAsync(mem, opts, &render)) || !waitAsync(render) || FAILED(render->GetResults())) return false;

    // the page as PNG in memory: read it back through a plain IStream
    Com<IStream> st;
    if (FAILED(CreateStreamOverRandomAccessStream(mem, IID_PPV_ARGS(&st)))) return false;
    LARGE_INTEGER zero = {};
    ULARGE_INTEGER end = {};
    st->Seek(zero, STREAM_SEEK_END, &end);
    st->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (!end.QuadPart || end.QuadPart > (256u << 20)) return false;
    std::vector<uint8_t> png((size_t)end.QuadPart);
    ULONG got = 0;
    if (FAILED(st->Read(png.data(), (ULONG)png.size(), &got)) || got != png.size()) return false;
    return loadImageMemory(png.data(), png.size(), 0, out);
}
