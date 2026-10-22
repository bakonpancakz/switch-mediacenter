#include "images.h"

#include <windows.h>
#include <wincodec.h>
#include <wincodecsdk.h>

#include <cmath>
#include <ctime>

#include "com.h"
#include "util.h"

namespace {

thread_local IWICImagingFactory* t_wic = nullptr;

IWICImagingFactory* wic() {
    if (!t_wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t_wic));
    }
    return t_wic;
}

// EXIF "YYYY:MM:DD HH:MM:SS" -> unix seconds (taken as UTC: it's only used to sort)
int64_t exifDate(const char* s) {
    int Y, M, D, h, m, sec;
    if (sscanf(s, "%d:%d:%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6 || Y < 1900) return 0;
    tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = sec;
    return (int64_t)_mkgmtime(&t);
}

// Orientation (1-8) and date taken from the frame's metadata, whatever the format.
void readMetadata(IWICBitmapFrameDecode* frame, int& orientation, int64_t& taken) {
    orientation = 1;
    taken = 0;
    Com<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(q->GetMetadataByName(L"System.Photo.Orientation", &v))) {
        if (v.vt == VT_UI2) orientation = v.uiVal;
        PropVariantClear(&v);
    }
    // the policy name doesn't reach into every JPEG's EXIF block: ask for the tag too
    if (orientation == 1 && SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=274}", &v))) {
        if (v.vt == VT_UI2) orientation = v.uiVal;
        PropVariantClear(&v);
    }
    if (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/exif/{ushort=36867}", &v))) {
        if (v.vt == VT_LPSTR && v.pszVal) taken = exifDate(v.pszVal);
        PropVariantClear(&v);
    }
    if (!taken && SUCCEEDED(q->GetMetadataByName(L"System.Photo.DateTaken", &v))) {
        if (v.vt == VT_FILETIME) {
            ULARGE_INTEGER u;
            u.LowPart = v.filetime.dwLowDateTime;
            u.HighPart = v.filetime.dwHighDateTime;
            taken = (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL;
        }
        PropVariantClear(&v);
    }
    if (orientation < 1 || orientation > 8) orientation = 1;
}

WICBitmapTransformOptions transformFor(int o) {
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

bool decodeFrom(IWICBitmapDecoder* dec, int maxSide, Image& out, ImageInfo* info) {
    Com<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    int orientation;
    int64_t taken;
    readMetadata(frame, orientation, taken);
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || !w || !h) return false;
    bool swap = orientation >= 5;
    if (info) {
        info->width = swap ? h : w;
        info->height = swap ? w : h;
        info->taken = taken;
    }
    IWICBitmapSource* src = frame;
    // scale first (the decoder can do part of it cheaply), then rotate the small picture
    Com<IWICBitmapScaler> scaler;
    UINT sw = w, sh = h;
    if (maxSide > 0 && (w > (UINT)maxSide || h > (UINT)maxSide)) {
        double k = (double)maxSide / (w > h ? w : h);
        sw = (UINT)std::max(1.0, std::round(w * k));
        sh = (UINT)std::max(1.0, std::round(h * k));
        if (FAILED(wic()->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(src, sw, sh, WICBitmapInterpolationModeFant)))
            return false;
        src = scaler;
    }
    Com<IWICBitmapFlipRotator> rot;
    if (orientation != 1) {
        if (FAILED(wic()->CreateBitmapFlipRotator(&rot)) || FAILED(rot->Initialize(src, transformFor(orientation)))) return false;
        src = rot;
    }
    Com<IWICFormatConverter> conv;
    if (FAILED(wic()->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT ow = 0, oh = 0;
    conv->GetSize(&ow, &oh);
    out.w = (int)ow;
    out.h = (int)oh;
    out.bgra.resize((size_t)ow * oh * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, ow * 4, (UINT)out.bgra.size(), out.bgra.data()));
}

}  // namespace

void imagesThreadInit() { wic(); }

void imagesThreadExit() {
    if (t_wic) {
        t_wic->Release();
        t_wic = nullptr;
        CoUninitialize();
    }
}

bool loadImageFile(const std::string& path, int maxSide, Image& out, ImageInfo* info) {
    if (!wic()) return false;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(wic()->CreateDecoderFromFilename(winPath(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    return decodeFrom(dec, maxSide, out, info);
}

bool loadImageMemory(const void* data, size_t len, int maxSide, Image& out, ImageInfo* info) {
    if (!wic() || !len) return false;
    Com<IWICStream> st;
    if (FAILED(wic()->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)data, (DWORD)len))) return false;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(wic()->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    return decodeFrom(dec, maxSide, out, info);
}

bool decodeBmp(const std::vector<uint8_t>& b, Image& out) {
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return false;
    auto u32 = [&](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; };
    uint32_t off = u32(10);
    int32_t w = (int32_t)u32(18), h = (int32_t)u32(22);
    int bpp = b[28] | b[29] << 8;
    uint32_t compression = u32(30);
    bool bottomUp = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || w > 30000 || h > 30000 || (bpp != 24 && bpp != 32) || (compression != 0 && compression != 3)) return false;
    size_t stride = ((size_t)w * (bpp / 8) + 3) & ~(size_t)3;
    if (off + stride * h > b.size()) return false;
    out.w = w;
    out.h = h;
    out.bgra.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = b.data() + off + stride * (bottomUp ? h - 1 - y : y);
        uint8_t* o = out.bgra.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            o[x * 4 + 0] = row[x * (bpp / 8) + 0];
            o[x * 4 + 1] = row[x * (bpp / 8) + 1];
            o[x * 4 + 2] = row[x * (bpp / 8) + 2];
            o[x * 4 + 3] = 255;
        }
    }
    return true;
}

Image scaleImage(const Image& in, int maxSide) {
    if (in.w <= maxSide && in.h <= maxSide) return in;
    Image out;
    Com<IWICBitmap> bmp;
    if (!wic() || FAILED(wic()->CreateBitmapFromMemory(in.w, in.h, GUID_WICPixelFormat32bppBGRA, in.w * 4, (UINT)in.bgra.size(),
                                                       (BYTE*)in.bgra.data(), &bmp)))
        return in;
    double k = (double)maxSide / std::max(in.w, in.h);
    UINT sw = (UINT)std::max(1.0, std::round(in.w * k)), sh = (UINT)std::max(1.0, std::round(in.h * k));
    Com<IWICBitmapScaler> sc;
    if (FAILED(wic()->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(bmp, sw, sh, WICBitmapInterpolationModeFant))) return in;
    out.w = (int)sw;
    out.h = (int)sh;
    out.bgra.resize((size_t)sw * sh * 4);
    if (FAILED(sc->CopyPixels(nullptr, sw * 4, (UINT)out.bgra.size(), out.bgra.data()))) return in;
    return out;
}

bool encodeJpeg(const Image& img, float quality, std::vector<uint8_t>& out) {
    if (!wic() || img.w <= 0 || img.h <= 0) return false;
    // transparent pictures go onto black, as the Switch draws them
    std::vector<uint8_t> bgr((size_t)img.w * img.h * 3);
    for (size_t i = 0, n = (size_t)img.w * img.h; i < n; i++) {
        unsigned a = img.bgra[i * 4 + 3];
        for (int c = 0; c < 3; c++) bgr[i * 3 + c] = (uint8_t)(img.bgra[i * 4 + c] * a / 255);
    }
    Com<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    Com<IWICBitmapEncoder> enc;
    if (FAILED(wic()->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) || FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache)))
        return false;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
    PROPBAG2 opt = {};
    opt.pstrName = (LPOLESTR)L"ImageQuality";
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = quality;
    props->Write(1, &opt, &v);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->Initialize(props)) || FAILED(frame->SetSize(img.w, img.h)) || FAILED(frame->SetPixelFormat(&fmt)) ||
        fmt != GUID_WICPixelFormat24bppBGR)
        return false;
    UINT stride = (UINT)img.w * 3;
    // rows padded to 4 bytes, as WIC expects for 24-bit formats
    UINT padded = (stride + 3) & ~3u;
    std::vector<uint8_t> rows((size_t)padded * img.h);
    for (int y = 0; y < img.h; y++) memcpy(rows.data() + (size_t)y * padded, bgr.data() + (size_t)y * stride, stride);
    if (FAILED(frame->WritePixels(img.h, padded, (UINT)rows.size(), rows.data())) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return false;
    HGLOBAL g = nullptr;
    if (FAILED(GetHGlobalFromStream(stream, &g))) return false;
    STATSTG st;
    stream->Stat(&st, STATFLAG_NONAME);
    size_t n = (size_t)st.cbSize.QuadPart;
    void* p = GlobalLock(g);
    if (!p) return false;
    out.assign((uint8_t*)p, (uint8_t*)p + n);
    GlobalUnlock(g);
    return !out.empty();
}

void imageStats(const Image& img, double& mean, double& sd) {
    double sum = 0, sum2 = 0;
    size_t n = 0;
    // every 7th pixel is plenty
    for (size_t i = 0; i < (size_t)img.w * img.h; i += 7, n++) {
        const uint8_t* p = &img.bgra[i * 4];
        double y = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
        sum += y;
        sum2 += y * y;
    }
    mean = n ? sum / n : 0;
    sd = n ? std::sqrt(std::max(0.0, sum2 / n - mean * mean)) : 0;
}
