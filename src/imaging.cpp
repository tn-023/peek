#include "imaging.h"
#include "core/aimeta.h"
#include "svgimaging.h"
#include "fileinput.h"
#include "core/tga.h"
#include "core/heif.h"
#include "core/textutil.h"

#include <commoncontrols.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <webp/decode.h>
#include <avif/avif.h>
#include <wincodecsdk.h>

namespace peek {

IWICImagingFactory* Wic() {
    thread_local ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    }
    return factory.Get();
}

namespace {

void FitSize(uint32_t w, uint32_t h, uint32_t maxEdge, uint32_t& tw, uint32_t& th) {
    if (maxEdge == 0 || (w <= maxEdge && h <= maxEdge)) { tw = w; th = h; return; }
    if (w >= h) {
        tw = maxEdge;
        th = std::max<uint32_t>(1, static_cast<uint32_t>((uint64_t(h) * maxEdge + w / 2) / w));
    } else {
        th = maxEdge;
        tw = std::max<uint32_t>(1, static_cast<uint32_t>((uint64_t(w) * maxEdge + h / 2) / h));
    }
}

bool CopyToPixels(IWICBitmapSource* src, Pixels& out) {
    auto f = Wic();
    if (!f || !src) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 32768 || h > 32768 || uint64_t(w) * h > kMaxDecodePixels) return false;
    out.w = w;
    out.h = h;
    out.data.resize(size_t(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.data.size()), out.data.data()));
}

UINT BitsPerPixel(const WICPixelFormatGUID& fmt) {
    ComPtr<IWICComponentInfo> ci;
    if (FAILED(Wic()->CreateComponentInfo(fmt, &ci))) return 0;
    ComPtr<IWICPixelFormatInfo> pi;
    if (FAILED(ci.As(&pi))) return 0;
    UINT b = 0;
    pi->GetBitsPerPixel(&b);
    return b;
}

// EXIF-Ausrichtung (1..8)
UINT ReadOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return 1;
    const wchar_t* names[] = {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    for (auto n : names) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(n, &v))) {
            UINT o = 1;
            if (v.vt == VT_UI2) o = v.uiVal;
            else if (v.vt == VT_UI4) o = v.ulVal;
            PropVariantClear(&v);
            if (o >= 1 && o <= 8) return o;
        }
        PropVariantClear(&v);
    }
    return 1;
}

WICBitmapTransformOptions OrientationTransform(UINT o) {
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}

// AVIF ohne Systemcodec (Windows braucht dafuer sonst die "AV1 Video Extension"): libavif mit dav1d
bool DecodeAvifFallback(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    std::vector<uint8_t> data;
    if (!ReadInputAll(path, 512ull * 1024 * 1024, data)) return false;
    avifDecoder* dec = avifDecoderCreate();
    if (!dec) return false;
    dec->maxThreads = 2;
    dec->strictFlags = AVIF_STRICT_DISABLED;
    dec->imageSizeLimit = static_cast<uint32_t>(kMaxDecodePixels);   // Schutz vor riesigen Angaben im Dateikopf
    bool ok = false;
    Pixels full;
    if (avifDecoderSetIOMemory(dec, data.data(), data.size()) == AVIF_RESULT_OK && avifDecoderParse(dec) == AVIF_RESULT_OK &&
        avifDecoderNextImage(dec) == AVIF_RESULT_OK) {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, dec->image);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.alphaPremultiplied = AVIF_TRUE;
        if (avifRGBImageAllocatePixels(&rgb) == AVIF_RESULT_OK) {
            if (avifImageYUVToRGB(dec->image, &rgb) == AVIF_RESULT_OK && rgb.width && rgb.height) {
                full.w = rgb.width;
                full.h = rgb.height;
                full.data.resize(size_t(rgb.width) * rgb.height * 4);
                for (uint32_t y = 0; y < rgb.height; ++y)
                    memcpy(full.data.data() + size_t(y) * rgb.width * 4, rgb.pixels + size_t(y) * rgb.rowBytes,
                           size_t(rgb.width) * 4);
                ok = true;
            }
            avifRGBImageFreePixels(&rgb);
        }
    }
    avifDecoderDestroy(dec);
    if (!ok) return false;
    full.srcW = full.w;
    full.srcH = full.h;
    if (maxEdge && (full.w > maxEdge || full.h > maxEdge)) return ScalePixels(full, maxEdge, out);
    out = std::move(full);
    return true;
}

// TGA: Windows hat keinen Dekoder dafuer; eigener, gepruefter Dekoder (core/tga.cpp)
bool IsHeifPath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const wchar_t* e = path.c_str() + dot;
    return _wcsicmp(e, L".heic") == 0 || _wcsicmp(e, L".heif") == 0 || _wcsicmp(e, L".hif") == 0;
}

bool ReadStreamRange(IStream* in, uint64_t off, size_t n, std::vector<uint8_t>& out) {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    out.clear();
    if (FAILED(in->Seek(li, STREAM_SEEK_SET, nullptr))) return false;
    out.resize(n);
    size_t got = 0;
    while (got < n) {
        ULONG r = 0;
        HRESULT hr = in->Read(out.data() + got, static_cast<ULONG>(std::min<size_t>(n - got, 1u << 20)), &r);
        if (FAILED(hr) || r == 0) break;
        got += r;
    }
    out.resize(got);
    return got > 0;
}

// HEIF/HEIC: Groesse, Drehung und EXIF liest peek selbst aus dem Dateikopf (core/heif.cpp), die Bilddaten
// dekodiert der HEIF-Codec von Windows. So sind Aufnahmedatum und Abmessungen auch ohne Codec bekannt, und die
// Angaben haengen nicht davon ab, welche Metadaten der Codec weiterreicht.
struct HeifHead {
    HeifInfo info;
    ExifInfo exif;
    bool ok = false, haveExif = false;
};

HeifHead ReadHeifHead(const std::wstring& path) {
    HeifHead h;
    ComPtr<IStream> in = OpenInputStream(path);
    if (!in) return h;
    std::vector<uint8_t> buf;
    if (!ReadStreamRange(in.Get(), 0, 256 * 1024, buf)) return h;
    h.ok = HeifParse(buf.data(), buf.size(), h.info);
    if (!h.ok && h.info.metaEnd > buf.size() && h.info.metaEnd <= 8u * 1024 * 1024 &&
        ReadStreamRange(in.Get(), 0, static_cast<size_t>(h.info.metaEnd), buf))
        h.ok = HeifParse(buf.data(), buf.size(), h.info);
    if (h.ok && h.info.exifLength > 0 && h.info.exifLength <= 1024 * 1024) {
        std::vector<uint8_t> ex;
        if (ReadStreamRange(in.Get(), h.info.exifOffset, static_cast<size_t>(h.info.exifLength), ex) &&
            ex.size() == h.info.exifLength)
            h.haveExif = HeifExifParse(ex.data(), ex.size(), h.exif);
    }
    return h;
}

// Drehung eines HEIF-Bildes fuer die Pixel, die der Codec liefert. In HEIF ist sie Teil des Containers (irot/imir),
// EXIF ist dort nur Information. Hat der Codec bei 90/270 Grad schon gedreht, sind Breite und Hoehe vertauscht.
UINT HeifFrameOrientation(const HeifHead& h, UINT fw, UINT fh) {
    if (!h.ok) return 1;
    UINT o = static_cast<UINT>(h.info.orientation);
    if (o >= 5 && h.info.width && h.info.height && h.info.width != h.info.height && fw == h.info.height &&
        fh == h.info.width)
        return 1;
    return o;
}

bool IsTgaPath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".tga") == 0;
}

bool DecodeTgaFile(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    std::vector<uint8_t> data;
    if (!ReadInputAll(path, 1024ull * 1024 * 1024, data)) return false;
    Pixels full;
    if (!TgaDecode(data.data(), data.size(), kMaxDecodePixels, full.data, full.w, full.h)) return false;
    full.srcW = full.w;
    full.srcH = full.h;
    if (maxEdge && (full.w > maxEdge || full.h > maxEdge)) return ScalePixels(full, maxEdge, out);
    out = std::move(full);
    return true;
}

// nur Abmessungen einer AVIF-Datei (ohne Dekodieren der Bilddaten)
bool AvifSize(const std::wstring& path, uint32_t& w, uint32_t& h) {
    std::vector<uint8_t> data;
    if (!ReadInputAll(path, 512ull * 1024 * 1024, data)) return false;
    avifDecoder* dec = avifDecoderCreate();
    if (!dec) return false;
    dec->strictFlags = AVIF_STRICT_DISABLED;
    bool ok = avifDecoderSetIOMemory(dec, data.data(), data.size()) == AVIF_RESULT_OK && avifDecoderParse(dec) == AVIF_RESULT_OK;
    if (ok) {
        w = dec->image->width;
        h = dec->image->height;
    }
    avifDecoderDestroy(dec);
    return ok && w && h;
}

bool DecodeWebPFallback(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    std::vector<uint8_t> data;
    if (!ReadInputAll(path, 512ull * 1024 * 1024, data)) return false;
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) return false;
    if (WebPGetFeatures(data.data(), data.size(), &cfg.input) != VP8_STATUS_OK) return false;
    const int w = cfg.input.width, h = cfg.input.height;
    if (w <= 0 || h <= 0 || uint64_t(w) * h > kMaxDecodePixels) return false;
    // Fuer Vorschaubilder direkt im Dekoder verkleinern: kein Puffer in voller Groesse
    uint32_t tw, th;
    FitSize(static_cast<uint32_t>(w), static_cast<uint32_t>(h), maxEdge, tw, th);
    if (tw < static_cast<uint32_t>(w) || th < static_cast<uint32_t>(h)) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = static_cast<int>(tw);
        cfg.options.scaled_height = static_cast<int>(th);
    }
    Pixels px;
    px.w = tw;
    px.h = th;
    px.srcW = static_cast<uint32_t>(w);
    px.srcH = static_cast<uint32_t>(h);
    px.data.resize(size_t(tw) * th * 4);
    cfg.output.colorspace = MODE_bgrA;   // BGRA mit vormultipliertem Alpha
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = px.data.data();
    cfg.output.u.RGBA.stride = static_cast<int>(tw * 4);
    cfg.output.u.RGBA.size = px.data.size();
    VP8StatusCode st = WebPDecode(data.data(), data.size(), &cfg);
    WebPFreeDecBuffer(&cfg.output);
    if (st != VP8_STATUS_OK) return false;
    out = std::move(px);
    return true;
}

// orientation: 0 = aus EXIF lesen, sonst vorgegeben (HEIF)
bool DecodeFrame(IWICBitmapFrameDecode* frame, uint32_t maxEdge, Pixels& out, UINT orientation = 0) {
    auto f = Wic();
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    if (orientation < 1 || orientation > 8) orientation = ReadOrientation(frame);
    bool swap = orientation >= 5;

    ComPtr<IWICBitmapSource> src = frame;
    uint32_t tw, th;
    FitSize(w, h, maxEdge, tw, th);

    if (tw < w) {
        // 1) Skalierte Dekodierung direkt im Decoder (JPEG: 1/2, 1/4, 1/8)
        ComPtr<IWICBitmapSourceTransform> xf;
        if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&xf)))) {
            UINT cw = tw, ch = th;
            if (SUCCEEDED(xf->GetClosestSize(&cw, &ch)) && cw < w && cw >= tw / 2) {
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                xf->GetClosestPixelFormat(&fmt);
                UINT bpp = BitsPerPixel(fmt);
                if (bpp >= 8) {
                    UINT stride = (cw * bpp + 7) / 8;
                    stride = (stride + 3) & ~3u;
                    std::vector<uint8_t> buf(size_t(stride) * ch);
                    if (SUCCEEDED(xf->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride,
                                                 static_cast<UINT>(buf.size()), buf.data()))) {
                        ComPtr<IWICBitmap> mem;
                        if (SUCCEEDED(f->CreateBitmapFromMemory(cw, ch, fmt, stride, static_cast<UINT>(buf.size()),
                                                                buf.data(), &mem))) {
                            src = mem;
                        }
                    }
                }
            }
        }
        // 2) Rest per Fant-Filter
        UINT sw = 0, sh = 0;
        src->GetSize(&sw, &sh);
        if (sw != tw || sh != th) {
            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(src.Get(), tw, th, WICBitmapInterpolationModeFant)))
                src = scaler;
        }
    }

    if (orientation != 1) {
        // Drehen benoetigt eine vollstaendig dekodierte Quelle
        ComPtr<IWICBitmap> cached;
        if (SUCCEEDED(f->CreateBitmapFromSource(src.Get(), WICBitmapCacheOnLoad, &cached))) {
            ComPtr<IWICBitmapFlipRotator> rot;
            if (SUCCEEDED(f->CreateBitmapFlipRotator(&rot)) &&
                SUCCEEDED(rot->Initialize(cached.Get(), OrientationTransform(orientation))))
                src = rot;
            else
                src = cached;
        }
    }

    if (!CopyToPixels(src.Get(), out)) return false;
    out.srcW = swap ? h : w;
    out.srcH = swap ? w : h;
    return true;
}

} // namespace

bool SaveCroppedImage(const std::wstring& srcPath, int viewRot, double x0, double y0, double x1, double y1,
                      const std::wstring& dst, const GUID& container, uint32_t* outW, uint32_t* outH) {
    auto f = Wic();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(f->CreateDecoderFromFilename(LongPath(srcPath).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                            &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;
    ComPtr<IWICBitmapSource> src = frame;
    // 1) EXIF-Ausrichtung (HEIF: aus dem Container), 2) Drehung im Betrachter
    UINT o = ReadOrientation(frame.Get());
    if (IsHeifPath(srcPath)) {
        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        o = HeifFrameOrientation(ReadHeifHead(srcPath), fw, fh);
    }
    auto rotate = [&](WICBitmapTransformOptions opt) {
        ComPtr<IWICBitmap> cached;
        ComPtr<IWICBitmapFlipRotator> rot;
        if (FAILED(f->CreateBitmapFromSource(src.Get(), WICBitmapCacheOnLoad, &cached)) ||
            FAILED(f->CreateBitmapFlipRotator(&rot)) || FAILED(rot->Initialize(cached.Get(), opt)))
            return false;
        src = rot;
        return true;
    };
    if (o != 1 && !rotate(OrientationTransform(o))) return false;
    static const WICBitmapTransformOptions kRot[4] = {WICBitmapTransformRotate0, WICBitmapTransformRotate90,
                                                      WICBitmapTransformRotate180, WICBitmapTransformRotate270};
    viewRot = ((viewRot % 4) + 4) % 4;
    if (viewRot && !rotate(kRot[viewRot])) return false;

    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    auto px = [](double v, UINT n) { return static_cast<INT>(std::clamp(std::lround(v * n), 0L, static_cast<long>(n))); };
    INT l = px(std::min(x0, x1), w), t = px(std::min(y0, y1), h), r = px(std::max(x0, x1), w), b = px(std::max(y0, y1), h);
    if (r - l < 1 || b - t < 1) return false;
    WICRect rc = {l, t, r - l, b - t};
    ComPtr<IWICBitmapClipper> clip;
    if (FAILED(f->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(src.Get(), &rc))) return false;

    bool jpeg = IsEqualGUID(container, GUID_ContainerFormatJpeg) != 0;
    bool opaque = jpeg || IsEqualGUID(container, GUID_ContainerFormatBmp);
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICStream> stream;
    if (FAILED(f->CreateEncoder(container, nullptr, &enc)) || FAILED(f->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(LongPath(dst).c_str(), GENERIC_WRITE)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> fe;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&fe, &props))) return false;
    if (jpeg && props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.95f;
        props->Write(1, &opt, &v);
    }
    if (FAILED(fe->Initialize(props.Get())) || FAILED(fe->SetSize(rc.Width, rc.Height))) return false;
    WICPixelFormatGUID fmt = opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    if (FAILED(fe->SetPixelFormat(&fmt))) return false;
    // Metadaten (EXIF, Aufnahmedatum, Kamera) uebernehmen, Ausrichtung auf "normal" setzen
    if (jpeg || IsEqualGUID(container, GUID_ContainerFormatTiff)) {
        ComPtr<IWICMetadataBlockReader> br;
        ComPtr<IWICMetadataBlockWriter> bw;
        if (SUCCEEDED(frame.As(&br)) && SUCCEEDED(fe.As(&bw))) bw->InitializeFromBlockReader(br.Get());
        ComPtr<IWICMetadataQueryWriter> qw;
        if (SUCCEEDED(fe->GetMetadataQueryWriter(&qw))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_UI2;
            pv.uiVal = 1;
            qw->SetMetadataByName(jpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}", &pv);
            qw->RemoveMetadataByName(jpeg ? L"/app1/thumb" : L"/ifd/thumb");   // altes Vorschaubild passt nicht mehr
        }
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(clip.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    if (FAILED(fe->WriteSource(conv.Get(), nullptr)) || FAILED(fe->Commit()) || FAILED(enc->Commit())) return false;
    if (outW) *outW = static_cast<uint32_t>(rc.Width);
    if (outH) *outH = static_cast<uint32_t>(rc.Height);
    return true;
}

static bool InvokeFn(const std::function<bool()>* fn) { return (*fn)(); }

// Welche Ausnahmen aus Codecs abgefangen werden. Nur Faelle, nach denen der Prozess sicher weiterlaufen kann:
//  - C++-Ausnahmen (z. B. std::bad_alloc bei zu wenig Speicher),
//  - Lesefehler bei Dateien, die waehrend des Lesens verschwinden (Wechseldatentraeger),
//  - Lese-/Schreibzugriffe auf die ersten 64 KB (Nullzeiger; dort ist nie Speicher eingeblendet),
//  - Ganzzahl-Division durch null.
// Alle anderen Zugriffsverletzungen deuten auf beschaedigten Speicher hin: dann beendet sich peek, statt mit
// einem moeglicherweise manipulierten Zustand weiterzulaufen (sonst koennte ein Angreifer beliebig oft probieren).
static int CodecFilter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep ? ep->ExceptionRecord : nullptr;
    if (!r) return EXCEPTION_CONTINUE_SEARCH;
    switch (r->ExceptionCode) {
        case 0xE06D7363:   // C++-Ausnahme (MSVC)
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return EXCEPTION_EXECUTE_HANDLER;
        case EXCEPTION_ACCESS_VIOLATION:
            if (r->NumberParameters >= 2 && r->ExceptionInformation[0] != 8 &&   // 8 = Ausfuehrung (DEP)
                r->ExceptionInformation[1] < 0x10000)
                return EXCEPTION_EXECUTE_HANDLER;
            return EXCEPTION_CONTINUE_SEARCH;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
}

bool GuardedCall(const std::function<bool()>& fn) {
    __try {
        return InvokeFn(&fn);
    } __except (CodecFilter(GetExceptionInformation())) {
        return false;
    }
}

bool ScalePixels(const Pixels& in, uint32_t maxEdge, Pixels& out) {
    auto f = Wic();
    if (!f || !in.Valid()) return false;
    uint32_t tw, th;
    FitSize(in.w, in.h, maxEdge, tw, th);
    ComPtr<IWICBitmap> mem;
    if (FAILED(f->CreateBitmapFromMemory(in.w, in.h, GUID_WICPixelFormat32bppPBGRA, in.w * 4,
                                         static_cast<UINT>(in.data.size()), const_cast<BYTE*>(in.data.data()), &mem)))
        return false;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(mem.Get(), tw, th, WICBitmapInterpolationModeFant)))
        return false;
    out.srcW = in.srcW ? in.srcW : in.w;
    out.srcH = in.srcH ? in.srcH : in.h;
    return CopyToPixels(scaler.Get(), out);
}

bool DecodeImageFile(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    if (IsSvgPath(path)) return DecodeSvg(path, maxEdge, out);   // Vektorgrafik: resvg
    if (IsTgaPath(path)) return DecodeTgaFile(path, maxEdge, out);
    auto f = Wic();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IStream> in = OpenInputStream(path);
    if (!in) return false;
    HRESULT hr = f->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(dec->GetFrame(0, &frame))) {
            UINT o = 0;
            if (IsHeifPath(path)) {
                UINT fw = 0, fh = 0;
                frame->GetSize(&fw, &fh);
                o = HeifFrameOrientation(ReadHeifHead(path), fw, fh);
            }
            if (DecodeFrame(frame.Get(), maxEdge, out, o)) return true;
        }
    }
    // WebP und AVIF ohne Systemcodec
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".webp") == 0)
        return DecodeWebPFallback(path, maxEdge, out);
    if (dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".avif") == 0)
        return DecodeAvifFallback(path, maxEdge, out);
    return false;
}

bool DecodeEmbeddedThumb(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    if (IsSvgPath(path) || IsTgaPath(path)) return false;
    auto f = Wic();
    if (!f) return false;
    ComPtr<IStream> in = OpenInputStream(path);
    if (!in) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;
    ComPtr<IWICBitmapSource> src;
    if (FAILED(frame->GetThumbnail(&src)) || !src) return false;
    UINT tw = 0, th = 0;
    if (FAILED(src->GetSize(&tw, &th)) || tw < 32 || th < 32) return false;
    uint32_t fw, fh;
    FitSize(tw, th, maxEdge, fw, fh);
    if (fw < tw) {
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(src.Get(), fw, fh, WICBitmapInterpolationModeFant)))
            src = scaler;
    }
    // das Vorschaubild ist wie das Bild selbst gespeichert (HEIF: Drehung aus dem Container)
    UINT orientation = IsHeifPath(path) ? HeifFrameOrientation(ReadHeifHead(path), w, h) : ReadOrientation(frame.Get());
    if (orientation != 1) {
        ComPtr<IWICBitmap> cached;
        if (SUCCEEDED(f->CreateBitmapFromSource(src.Get(), WICBitmapCacheOnLoad, &cached))) {
            ComPtr<IWICBitmapFlipRotator> rot;
            if (SUCCEEDED(f->CreateBitmapFlipRotator(&rot)) &&
                SUCCEEDED(rot->Initialize(cached.Get(), OrientationTransform(orientation))))
                src = rot;
            else
                src = cached;
        }
    }
    if (!CopyToPixels(src.Get(), out)) return false;
    bool swap = orientation >= 5;
    out.srcW = swap ? h : w;
    out.srcH = swap ? w : h;
    return true;
}

namespace {
// EXIF-Werte als Zahl: Gleitkomma, (vorzeichenbehafteter) Bruch als VT_UI8/VT_I8 (Zaehler unten, Nenner oben), Ganzzahl
bool PropNumber(const PROPVARIANT& v, double& out) {
    switch (v.vt) {
    case VT_R8: out = v.dblVal; return true;
    case VT_R4: out = v.fltVal; return true;
    case VT_UI2: out = v.uiVal; return true;
    case VT_UI4: out = v.ulVal; return true;
    case VT_I4: out = v.lVal; return true;
    case VT_UI8: {
        uint32_t num = v.uhVal.LowPart, den = v.uhVal.HighPart;
        if (!den) return false;
        out = double(num) / den;
        return true;
    }
    case VT_I8: {
        int32_t num = static_cast<int32_t>(v.hVal.LowPart), den = static_cast<int32_t>(v.hVal.HighPart);
        if (!den) return false;
        out = double(num) / den;
        return true;
    }
    case VT_VECTOR | VT_UI2: if (v.caui.cElems) { out = v.caui.pElems[0]; return true; } return false;
    default: return false;
    }
}

bool PropText(const PROPVARIANT& v, std::wstring& out) {
    if (v.vt == VT_LPWSTR && v.pwszVal) { out = v.pwszVal; return !out.empty(); }
    if (v.vt == VT_LPSTR && v.pszVal) {
        int n = MultiByteToWideChar(CP_UTF8, 0, v.pszVal, -1, nullptr, 0);
        UINT cp = CP_UTF8;
        if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(cp, 0, v.pszVal, -1, nullptr, 0); }
        if (n <= 1) return false;
        out.resize(static_cast<size_t>(n));
        MultiByteToWideChar(cp, 0, v.pszVal, -1, out.data(), n);
        out.resize(static_cast<size_t>(n - 1));
        return !out.empty();
    }
    return false;
}

template <typename F>
bool QueryFirst(IWICMetadataQueryReader* q, std::initializer_list<const wchar_t*> names, F&& take) {
    for (const wchar_t* n : names) {
        PROPVARIANT v;
        PropVariantInit(&v);
        bool ok = SUCCEEDED(q->GetMetadataByName(n, &v)) && take(v);
        PropVariantClear(&v);
        if (ok) return true;
    }
    return false;
}
}  // namespace

bool HeifCodecInstalled() {
    static int known = -1;
    if (known >= 0) return known == 1;
    known = 0;
    auto f = Wic();
    ComPtr<IEnumUnknown> en;
    if (!f || FAILED(f->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &en))) return false;
    ComPtr<IUnknown> u;
    ULONG got = 0;
    while (en->Next(1, &u, &got) == S_OK && got == 1) {
        ComPtr<IWICBitmapDecoderInfo> di;
        if (SUCCEEDED(u.As(&di))) {
            wchar_t ext[512] = {};
            UINT len = 0;
            if (SUCCEEDED(di->GetFileExtensions(511, ext, &len))) {
                std::wstring e = ToLower(ext);
                if (e.find(L".heic") != std::wstring::npos || e.find(L".heif") != std::wstring::npos) known = 1;
            }
        }
        u.Reset();
        if (known == 1) break;
    }
    return known == 1;
}

bool ReadPhotoDetails(const std::wstring& path, std::wstring& out) {
    out.clear();
    if (IsSvgPath(path) || IsTgaPath(path)) return false;
    if (IsHeifPath(path)) {   // HEIF: EXIF selbst lesen (geht auch ohne Codec)
        HeifHead hh = ReadHeifHead(path);
        if (!hh.haveExif) return false;
        std::wstring lens = Utf8ToWide(hh.exif.lens);
        if (lens.size() > 120) lens.resize(120);
        for (auto& c : lens) if (c == 0x1f || c < 0x20) c = L' ';
        auto num = [](double d) { wchar_t b[32]; std::swprintf(b, 32, L"%.6g", d); return std::wstring(b); };
        const ExifInfo& x = hh.exif;
        out = lens + L'\x1f' + (x.fnumber > 0 ? num(x.fnumber) : L"") + L'\x1f' + (x.exposure > 0 ? num(x.exposure) : L"") +
              L'\x1f' + (x.iso > 0 ? num(x.iso) : L"");
        return !lens.empty() || x.fnumber > 0 || x.exposure > 0 || x.iso > 0;
    }
    auto f = Wic();
    if (!f) return false;
    ComPtr<IStream> in = OpenInputStream(path);
    if (!in) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return false;
    std::wstring lens;
    double fnum = 0, expo = 0, iso = 0;
    QueryFirst(q.Get(), {L"System.Photo.LensModel", L"/app1/ifd/exif/{ushort=42036}", L"/ifd/exif/{ushort=42036}"},
               [&](const PROPVARIANT& v) { return PropText(v, lens); });
    QueryFirst(q.Get(), {L"System.Photo.FNumber", L"/app1/ifd/exif/{ushort=33437}", L"/ifd/exif/{ushort=33437}"},
               [&](const PROPVARIANT& v) { return PropNumber(v, fnum) && fnum > 0; });
    QueryFirst(q.Get(), {L"System.Photo.ExposureTime", L"/app1/ifd/exif/{ushort=33434}", L"/ifd/exif/{ushort=33434}"},
               [&](const PROPVARIANT& v) { return PropNumber(v, expo) && expo > 0; });
    QueryFirst(q.Get(), {L"System.Photo.ISOSpeed", L"/app1/ifd/exif/{ushort=34855}", L"/ifd/exif/{ushort=34855}"},
               [&](const PROPVARIANT& v) { return PropNumber(v, iso) && iso > 0; });
    while (!lens.empty() && (lens.back() == L' ' || lens.back() == L'\0')) lens.pop_back();
    if (lens.size() > 120) lens.resize(120);
    for (auto& c : lens) if (c == 0x1f || c < 0x20) c = L' ';
    auto num = [](double d) { wchar_t b[32]; std::swprintf(b, 32, L"%.6g", d); return std::wstring(b); };
    out = lens + L'\x1f' + (fnum > 0 ? num(fnum) : L"") + L'\x1f' + (expo > 0 ? num(expo) : L"") + L'\x1f' +
          (iso > 0 ? num(iso) : L"");
    return !lens.empty() || fnum > 0 || expo > 0 || iso > 0;
}

bool DecodeImageMemory(const uint8_t* data, size_t size, Pixels& out) {
    auto f = Wic();
    if (!f || !data || !size) return false;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(data, static_cast<UINT>(size)));
    if (!stream) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    if (!CopyToPixels(frame.Get(), out)) return false;
    out.srcW = out.w;
    out.srcH = out.h;
    return true;
}

bool EncodeJpegOnWhite(const Pixels& in, float quality, std::vector<uint8_t>& out) {
    auto f = Wic();
    if (!f || !in.Valid()) return false;
    Pixels px = in;
    for (size_t i = 0; i < px.data.size(); i += 4) {   // vormultipliert: Farbe + (255 - Alpha) = auf Weiss
        uint8_t a = px.data[i + 3];
        if (a != 255) {
            px.data[i] = static_cast<uint8_t>(std::min(255, px.data[i] + 255 - a));
            px.data[i + 1] = static_cast<uint8_t>(std::min(255, px.data[i + 1] + 255 - a));
            px.data[i + 2] = static_cast<uint8_t>(std::min(255, px.data[i + 2] + 255 - a));
            px.data[i + 3] = 255;
        }
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (!stream || FAILED(f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, &props)))
        return false;
    if (props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = quality;
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(px.w, px.h))) return false;
    ComPtr<IWICBitmap> mem;
    if (FAILED(f->CreateBitmapFromMemory(px.w, px.h, GUID_WICPixelFormat32bppPBGRA, px.w * 4,
                                         static_cast<UINT>(px.data.size()), px.data.data(), &mem)))
        return false;
    WICPixelFormatGUID target = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&target);
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(mem.Get(), target, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(conv.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return false;
    ULARGE_INTEGER size{};
    if (FAILED(IStream_Size(stream.Get(), &size)) || size.QuadPart == 0) return false;
    out.resize(static_cast<size_t>(size.QuadPart));
    IStream_Reset(stream.Get());
    return SUCCEEDED(IStream_Read(stream.Get(), out.data(), static_cast<ULONG>(out.size())));
}

bool EncodeThumb(const Pixels& px, std::vector<uint8_t>& out) {
    auto f = Wic();
    if (!f || !px.Valid()) return false;
    bool alpha = false;
    for (size_t i = 3; i < px.data.size(); i += 4)
        if (px.data[i] != 255) { alpha = true; break; }

    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    if (!stream) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(alpha ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
    if (!alpha && props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.85f;
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    frame->SetSize(px.w, px.h);

    ComPtr<IWICBitmap> mem;
    if (FAILED(f->CreateBitmapFromMemory(px.w, px.h, GUID_WICPixelFormat32bppPBGRA, px.w * 4,
                                         static_cast<UINT>(px.data.size()), const_cast<BYTE*>(px.data.data()), &mem)))
        return false;
    WICPixelFormatGUID target = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&target);
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(mem.Get(), target, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    if (FAILED(frame->WriteSource(conv.Get(), nullptr))) return false;
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return false;

    ULARGE_INTEGER size{};
    if (FAILED(IStream_Size(stream.Get(), &size)) || size.QuadPart == 0 || size.QuadPart > 64ull * 1024 * 1024)
        return false;
    out.resize(static_cast<size_t>(size.QuadPart));
    IStream_Reset(stream.Get());
    return SUCCEEDED(IStream_Read(stream.Get(), out.data(), static_cast<ULONG>(out.size())));
}

namespace {

int64_t ParseExifDate(const char* s) {
    // "YYYY:MM:DD HH:MM:SS"
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (sscanf_s(s, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
    if (y < 1900 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(mo);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(se);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return FileTimeToUnix(ft);
}

// Kamera fuer die Anzeige: Hersteller nur voranstellen, wenn er nicht schon im Modell steht
std::wstring CameraName(std::wstring make, std::wstring model) {
    auto trim = [](std::wstring& s) {
        while (!s.empty() && (s.back() == L' ' || s.back() == 0)) s.pop_back();
    };
    trim(make);
    trim(model);
    if (!make.empty() && !model.empty() && _wcsnicmp(model.c_str(), make.c_str(), std::min<size_t>(make.size(), 5)) != 0) {
        size_t sp = make.find(L' ');
        return (sp == std::wstring::npos ? make : make.substr(0, sp)) + L" " + model;
    }
    return model.empty() ? make : model;
}

std::wstring PropString(const PROPVARIANT& v) {
    if (v.vt == VT_LPWSTR && v.pwszVal) return v.pwszVal;
    if (v.vt == VT_LPSTR && v.pszVal) return Utf8ToWide(v.pszVal);
    return {};
}

} // namespace

// PNG: Textbloecke (tEXt, unkomprimiertes iTXt) mit Angaben zur Bilderzeugung; Bilddaten werden uebersprungen
static void ReadPngAi(const std::wstring& path, MetaRow& m) {
    ComPtr<IStream> in = OpenInputStream(path);
    if (!in) return;
    auto readAll = [&](void* buf, ULONG n) {
        ULONG got = 0;
        return SUCCEEDED(in->Read(buf, n, &got)) && got == n;
    };
    uint8_t sig[8];
    if (!readAll(sig, 8) || sig[0] != 0x89 || sig[1] != 'P' || sig[2] != 'N' || sig[3] != 'G') return;
    AiMeta ai;
    uint64_t textBytes = 0;
    for (int chunks = 0; chunks < 100000; ++chunks) {
        uint8_t h[8];
        if (!readAll(h, 8)) break;
        const uint32_t len = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) | (uint32_t(h[2]) << 8) | h[3];
        if (len > 0x7FFFFFFFu) break;
        const bool tEXt = memcmp(h + 4, "tEXt", 4) == 0, iTXt = memcmp(h + 4, "iTXt", 4) == 0;
        if (memcmp(h + 4, "IEND", 4) == 0) break;
        if ((tEXt || iTXt) && len <= (16u << 20) && textBytes + len <= (32u << 20)) {
            std::string body(len, '\0');
            if (len && !readAll(body.data(), len)) break;
            textBytes += len;
            size_t z = body.find('\0');
            if (z != std::string::npos && z < 80) {
                std::string key = body.substr(0, z);
                if (tEXt) ParseAiText(key, std::string_view(body).substr(z + 1), ai);
                else if (z + 2 < body.size() && body[z + 1] == 0) {   // iTXt ohne Kompression: Sprache\0 Schluessel\0 Text
                    size_t lang = body.find('\0', z + 3);
                    size_t tk = lang == std::string::npos ? std::string::npos : body.find('\0', lang + 1);
                    if (tk != std::string::npos) ParseAiText(key, std::string_view(body).substr(tk + 1), ai);
                }
            }
            LARGE_INTEGER crc;
            crc.QuadPart = 4;
            if (FAILED(in->Seek(crc, STREAM_SEEK_CUR, nullptr))) break;
        } else {
            LARGE_INTEGER skip;
            skip.QuadPart = static_cast<LONGLONG>(len) + 4;
            if (FAILED(in->Seek(skip, STREAM_SEEK_CUR, nullptr))) break;
        }
    }
    m.ai = EncodeAi(ai);
}

bool ReadImageMeta(const std::wstring& path, MetaRow& m) {
    if (IsSvgPath(path)) {   // SVG: Groesse aus width/height bzw. viewBox
        float w = 0, h = 0;
        if (!SvgSize(path, w, h)) return false;
        m.width = static_cast<uint32_t>(std::lround(w));
        m.height = static_cast<uint32_t>(std::lround(h));
        return true;
    }
    if (IsTgaPath(path)) {   // TGA: Abmessungen aus dem Dateikopf
        std::vector<uint8_t> head;
        TgaInfo ti;
        if (!ReadInputHead(path, 18, head) || !TgaReadInfo(head.data(), head.size(), ti)) return false;
        m.width = ti.width;
        m.height = ti.height;
        return true;
    }
    if (IsHeifPath(path)) {
        // HEIF: Groesse, Aufnahmedatum und Kamera aus dem eigenen Kopf-Leser (auch ohne HEIF-Codec)
        HeifHead hh = ReadHeifHead(path);
        if (!hh.ok || !hh.info.width || !hh.info.height) return false;
        bool swap = hh.info.orientation >= 5;
        m.width = swap ? hh.info.height : hh.info.width;
        m.height = swap ? hh.info.width : hh.info.height;
        if (hh.haveExif) {
            if (!hh.exif.dateTaken.empty()) m.taken = ParseExifDate(hh.exif.dateTaken.c_str());
            m.camera = CameraName(Utf8ToWide(hh.exif.make), Utf8ToWide(hh.exif.model));
            if (hh.exif.hasGps) {
                m.hasGeo = true;
                m.lat = hh.exif.lat;
                m.lon = hh.exif.lon;
            }
        }
        return true;
    }
    auto f = Wic();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IStream> in = OpenInputStream(path);
    if (!in) return false;
    if (FAILED(f->CreateDecoderFromStream(in.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
        // AVIF-Rueckfallebene: nur Abmessungen
        size_t dot = path.find_last_of(L'.');
        if (dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".avif") == 0) {
            uint32_t aw = 0, ah = 0;
            if (!AvifSize(path, aw, ah)) return false;
            m.width = aw;
            m.height = ah;
            return true;
        }
        // WebP-Rueckfallebene: nur Abmessungen
        std::vector<uint8_t> head;
        if (!ReadInputHead(path, 64 * 1024, head)) return false;
        int w = 0, hh = 0;
        if (head.size() > 30 && WebPGetInfo(head.data(), head.size(), &w, &hh)) {
            m.width = static_cast<uint32_t>(w);
            m.height = static_cast<uint32_t>(hh);
            return true;
        }
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    UINT o = ReadOrientation(frame.Get());
    m.width = o >= 5 ? h : w;
    m.height = o >= 5 ? w : h;

    ComPtr<IWICMetadataQueryReader> q;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&q))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"System.Photo.DateTaken", &v)) && v.vt == VT_FILETIME) {
            m.taken = FileTimeToUnix(v.filetime);
        }
        PropVariantClear(&v);
        if (!m.taken) {
            const wchar_t* paths[] = {L"/app1/ifd/exif/{ushort=36867}", L"/ifd/exif/{ushort=36867}",
                                      L"/app1/ifd/{ushort=306}"};
            for (auto p : paths) {
                PropVariantInit(&v);
                if (SUCCEEDED(q->GetMetadataByName(p, &v)) && v.vt == VT_LPSTR && v.pszVal) m.taken = ParseExifDate(v.pszVal);
                PropVariantClear(&v);
                if (m.taken) break;
            }
        }
        std::wstring make, model;
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"System.Photo.CameraModel", &v))) model = PropString(v);
        PropVariantClear(&v);
        if (model.empty()) {
            PropVariantInit(&v);
            if (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=272}", &v))) model = PropString(v);
            PropVariantClear(&v);
        }
        PropVariantInit(&v);
        if (SUCCEEDED(q->GetMetadataByName(L"System.Photo.CameraManufacturer", &v))) make = PropString(v);
        PropVariantClear(&v);
        if (make.empty()) {
            PropVariantInit(&v);
            if (SUCCEEDED(q->GetMetadataByName(L"/app1/ifd/{ushort=271}", &v))) make = PropString(v);
            PropVariantClear(&v);
        }
        m.camera = CameraName(make, model);
    }
    // Aufnahmeort: JPEG (APP1) und TIFF mit dem eigenen EXIF-Leser (nur der Dateianfang)
    std::vector<uint8_t> head;
    if (ReadInputHead(path, 256 * 1024, head) && head.size() >= 8) {
        ExifInfo ex;
        size_t off = 0, len = 0;
        bool ok = false;
        if (JpegFindExif(head.data(), head.size(), off, len)) ok = ExifParse(head.data() + off, len, ex);
        else if ((head[0] == 'I' && head[1] == 'I') || (head[0] == 'M' && head[1] == 'M'))
            ok = ExifParse(head.data(), head.size(), ex);
        if (ok && ex.hasGps) {
            m.hasGeo = true;
            m.lat = ex.lat;
            m.lon = ex.lon;
        }
    }
    {
        size_t dot = path.find_last_of(L'.');
        if (dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".png") == 0) ReadPngAi(path, m);
    }
    return true;
}

namespace {

struct VideoReader {
    ComPtr<IMFMediaSource> mediaSource;
    ComPtr<IMFSourceReader> reader;
    ~VideoReader() {
        reader.Reset();
        if (mediaSource) mediaSource->Shutdown();
    }
    uint32_t w = 0, h = 0;
    LONG stride = 0;
    uint64_t duration = 0;  // 100 ns

    bool Open(const std::wstring& path, bool forFrames) {
        ComPtr<IMFAttributes> attr;
        if (FAILED(MFCreateAttributes(&attr, 2))) return false;
        if (forFrames) attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        // Ueber einen Datenstrom statt der Adresse: im Dekoder-Prozess gibt es nur das uebergebene Handle.
        // Der Dateiname dient dem Quellen-Aufloeser nur als Hinweis auf das Format.
        ComPtr<IStream> in = OpenInputStream(path);
        ComPtr<IMFByteStream> bs;
        if (!in || FAILED(MFCreateMFByteStreamOnStream(in.Get(), &bs))) return false;
        ComPtr<IMFSourceResolver> resolver;
        if (FAILED(MFCreateSourceResolver(&resolver))) return false;
        MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
        ComPtr<IUnknown> obj;
        size_t slash = path.find_last_of(L"\\/");
        std::wstring hint = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (FAILED(resolver->CreateObjectFromByteStream(
                bs.Get(), hint.c_str(),
                MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE |
                    MF_RESOLUTION_READ,
                nullptr, &type, &obj)) ||
            type != MF_OBJECT_MEDIASOURCE)
            return false;
        ComPtr<IMFMediaSource> source;
        if (FAILED(obj.As(&source))) return false;
        mediaSource = source;
        if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), attr.Get(), &reader))) return false;
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE))) return false;
        if (forFrames) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(MFCreateMediaType(&mt))) return false;
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, mt.Get())))
                return false;
        }
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
            var.vt == VT_UI8)
            duration = var.uhVal.QuadPart;
        PropVariantClear(&var);
        return UpdateType();
    }

    bool UpdateType() {
        ComPtr<IMFMediaType> cur;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur))) return false;
        UINT32 fw = 0, fh = 0;
        if (FAILED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &fw, &fh)) || !fw || !fh) return false;
        w = fw;
        h = fh;
        UINT32 s = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = static_cast<LONG>(s);
        else stride = static_cast<LONG>(w * 4);
        return true;
    }
};

} // namespace

bool ReadVideoMeta(const std::wstring& path, MetaRow& m) {
    VideoReader vr;
    if (!vr.Open(path, false)) return false;
    m.width = vr.w;
    m.height = vr.h;
    m.durationMs = static_cast<uint32_t>(vr.duration / 10000);
    return true;
}

bool VideoThumbnail(const std::wstring& path, uint32_t maxEdge, Pixels& out, MetaRow* meta) {
    VideoReader vr;
    if (!vr.Open(path, true)) return false;
    if (meta) {
        meta->width = vr.w;
        meta->height = vr.h;
        meta->durationMs = static_cast<uint32_t>(vr.duration / 10000);
    }
    if (vr.duration > 0) {
        uint64_t pos = std::min<uint64_t>(vr.duration / 10, 60ull * 10000000);  // max. 60 s
        PROPVARIANT p;
        PropVariantInit(&p);
        p.vt = VT_I8;
        p.hVal.QuadPart = static_cast<LONGLONG>(pos);
        vr.reader->SetCurrentPosition(GUID_NULL, p);
        PropVariantClear(&p);
    }
    ComPtr<IMFSample> sample;
    for (int i = 0; i < 40 && !sample; ++i) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        DWORD stream = 0;
        if (FAILED(vr.reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts,
                                         &sample)))
            return false;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) vr.UpdateType();
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!sample) return false;

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
    Pixels frame;
    frame.w = vr.w;
    frame.h = vr.h;
    frame.data.resize(size_t(vr.w) * vr.h * 4);

    bool copied = false;
    ComPtr<IMF2DBuffer> b2d;
    if (SUCCEEDED(buf.As(&b2d))) {
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(b2d->Lock2D(&scan0, &pitch))) {
            for (uint32_t y = 0; y < vr.h; ++y)
                memcpy(frame.data.data() + size_t(y) * vr.w * 4, scan0 + static_cast<ptrdiff_t>(y) * pitch, size_t(vr.w) * 4);
            b2d->Unlock2D();
            copied = true;
        }
    }
    if (!copied) {
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
        LONG stride = vr.stride ? vr.stride : static_cast<LONG>(vr.w * 4);
        size_t absStride = static_cast<size_t>(stride < 0 ? -stride : stride);
        if (len >= absStride * vr.h) {
            for (uint32_t y = 0; y < vr.h; ++y) {
                size_t srcRow = stride < 0 ? (vr.h - 1 - y) : y;
                memcpy(frame.data.data() + size_t(y) * vr.w * 4, p + srcRow * absStride, size_t(vr.w) * 4);
            }
            copied = true;
        }
        buf->Unlock();
    }
    if (!copied) return false;
    for (size_t i = 3; i < frame.data.size(); i += 4) frame.data[i] = 255;  // RGB32 hat kein Alpha
    frame.srcW = vr.w;
    frame.srcH = vr.h;
    if (maxEdge && (frame.w > maxEdge || frame.h > maxEdge)) return ScalePixels(frame, maxEdge, out);
    out = std::move(frame);
    return true;
}

bool ShellIcon(const std::wstring& pathOrName, bool isDir, bool perFile, Pixels& out) {
    SHFILEINFOW sfi{};
    UINT flags = SHGFI_SYSICONINDEX;
    if (!perFile) flags |= SHGFI_USEFILEATTRIBUTES;
    DWORD attrs = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!SHGetFileInfoW(pathOrName.c_str(), attrs, &sfi, sizeof(sfi), flags)) return false;

    ComPtr<IImageList> list;
    if (FAILED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&list)))) return false;
    HICON icon = nullptr;
    if (FAILED(list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon)) || !icon) return false;
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = Wic()->CreateBitmapFromHICON(icon, &bmp);
    DestroyIcon(icon);
    if (FAILED(hr)) return false;
    Pixels px;
    if (!CopyToPixels(bmp.Get(), px)) return false;

    // Kleine Symbole liegen oben links im 256er-Raster: auf den Inhalt zuschneiden
    uint32_t minX = px.w, minY = px.h, maxX = 0, maxY = 0;
    for (uint32_t y = 0; y < px.h; ++y)
        for (uint32_t x = 0; x < px.w; ++x)
            if (px.data[(size_t(y) * px.w + x) * 4 + 3] > 8) {
                minX = std::min(minX, x); minY = std::min(minY, y);
                maxX = std::max(maxX, x); maxY = std::max(maxY, y);
            }
    if (maxX < minX) return false;
    uint32_t cw = maxX - minX + 1, ch = maxY - minY + 1;
    if (std::max(cw, ch) <= 96 && (minX < 8 && minY < 8)) {
        uint32_t side = std::max(cw, ch);
        Pixels crop;
        crop.w = crop.h = side;
        crop.data.assign(size_t(side) * side * 4, 0);
        uint32_t ox = (side - cw) / 2, oy = (side - ch) / 2;
        for (uint32_t y = 0; y < ch; ++y)
            memcpy(crop.data.data() + (size_t(y + oy) * side + ox) * 4,
                   px.data.data() + (size_t(y + minY) * px.w + minX) * 4, size_t(cw) * 4);
        crop.srcW = crop.srcH = side;
        out = std::move(crop);
        return true;
    }
    px.srcW = px.w;
    px.srcH = px.h;
    out = std::move(px);
    return true;
}

} // namespace peek
