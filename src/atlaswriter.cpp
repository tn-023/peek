// Kacheltextur: PNG-Sequenz pixelgenau zu einer PNG-Datei zusammensetzen (WIC).
// Ablauf: Einzelbilder im eigenen Pixelformat dekodieren, Zeilen per Speicherkopie an ihre Stelle legen und die
// Textur streifenweise schreiben. Pro Kachelzeile sind nur deren Einzelbilder geoeffnet.
#include "atlaswriter.h"
#include "core/lang.h"
#include "fs.h"
#include "imaging.h"

#include <cstring>

namespace peek {

namespace {

bool IsIndexed(const GUID& f) {
    return f == GUID_WICPixelFormat1bppIndexed || f == GUID_WICPixelFormat2bppIndexed ||
           f == GUID_WICPixelFormat4bppIndexed || f == GUID_WICPixelFormat8bppIndexed;
}

// Pixelformat in der Textur: Palette -> RGBA 8 Bit (gleiche Farben), Graustufen unter 8 Bit -> 8 Bit Graustufen
GUID TargetFormat(const GUID& f) {
    if (IsIndexed(f)) return GUID_WICPixelFormat32bppBGRA;
    if (f == GUID_WICPixelFormatBlackWhite || f == GUID_WICPixelFormat2bppGray || f == GUID_WICPixelFormat4bppGray)
        return GUID_WICPixelFormat8bppGray;
    return f;
}

std::wstring FormatLabel(const GUID& f, uint32_t bpp) {
    if (f == GUID_WICPixelFormat24bppBGR || f == GUID_WICPixelFormat24bppRGB) return T(L"RGB 8 Bit");
    if (f == GUID_WICPixelFormat32bppBGRA || f == GUID_WICPixelFormat32bppRGBA) return T(L"RGBA 8 Bit");
    if (f == GUID_WICPixelFormat48bppRGB || f == GUID_WICPixelFormat48bppBGR) return T(L"RGB 16 Bit");
    if (f == GUID_WICPixelFormat64bppRGBA || f == GUID_WICPixelFormat64bppBGRA) return T(L"RGBA 16 Bit");
    if (f == GUID_WICPixelFormat8bppGray) return T(L"Graustufen 8 Bit");
    if (f == GUID_WICPixelFormat16bppGray) return T(L"Graustufen 16 Bit");
    return std::to_wstring(bpp) + T(L" Bit pro Pixel");
}

bool OpenFrame(IWICImagingFactory* wic, const std::wstring& path, ComPtr<IWICBitmapFrameDecode>& frame) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(LongPath(path).c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    GUID container{};
    if (FAILED(dec->GetContainerFormat(&container)) || container != GUID_ContainerFormatPng) return false;
    return SUCCEEDED(dec->GetFrame(0, &frame));
}

// Quelle im Zielformat (ohne Umrechnung, ausser Palette/Graustufen unter 8 Bit; beides verlustfrei)
bool OpenSource(IWICImagingFactory* wic, const std::wstring& path, const GUID& target, uint32_t w, uint32_t h,
                ComPtr<IWICBitmapSource>& src, std::wstring& err) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!OpenFrame(wic, path, frame)) {
        err = T(L"Bild kann nicht gelesen werden: ") + FileNameOf(path);
        return false;
    }
    UINT fw = 0, fh = 0;
    GUID f{};
    frame->GetSize(&fw, &fh);
    frame->GetPixelFormat(&f);
    if (fw != w || fh != h || TargetFormat(f) != target) {
        err = T(L"Bild wurde seit dem Öffnen verändert: ") + FileNameOf(path);
        return false;
    }
    if (f == target) {
        src = frame;
        return true;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), target, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        err = T(L"Pixelformat kann nicht übernommen werden: ") + FileNameOf(path);
        return false;
    }
    src = conv;
    return true;
}

}  // namespace

bool InspectAtlas(const std::vector<std::wstring>& files, AtlasInfo& info, std::wstring& err) {
    info = {};
    if (files.empty()) {
        err = T(L"Der Ordner enthält keine PNG-Dateien.");
        return false;
    }
    IWICImagingFactory* wic = Wic();
    if (!wic) {
        err = T(L"WIC nicht verfügbar.");
        return false;
    }
    GUID nativeFirst{};
    for (size_t i = 0; i < files.size(); ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        if (!OpenFrame(wic, files[i], frame)) {
            err = T(L"Keine gültige PNG-Datei: ") + FileNameOf(files[i]);
            return false;
        }
        UINT w = 0, h = 0;
        GUID f{};
        frame->GetSize(&w, &h);
        frame->GetPixelFormat(&f);
        GUID t = TargetFormat(f);
        if (i == 0) {
            info.w = w;
            info.h = h;
            info.format = t;
            nativeFirst = f;
            continue;
        }
        if (w != info.w || h != info.h) {
            err = TF(L"Abweichende Größe: {0} hat {1}, {2} hat {3}",
                     {FileNameOf(files[i]), std::to_wstring(w) + L" × " + std::to_wstring(h) + L" px", FileNameOf(files[0]),
                      std::to_wstring(info.w) + L" × " + std::to_wstring(info.h) + L" px"}) + L".";
            return false;
        }
        if (t != info.format) {
            err = TF(L"Abweichendes Pixelformat: {0} passt nicht zu {1}", {FileNameOf(files[i]), FileNameOf(files[0])}) + L".";
            return false;
        }
    }
    ComPtr<IWICComponentInfo> ci;
    ComPtr<IWICPixelFormatInfo2> pfi;
    UINT bpp = 0;
    if (SUCCEEDED(wic->CreateComponentInfo(info.format, &ci)) && SUCCEEDED(ci.As(&pfi))) {
        pfi->GetBitsPerPixel(&bpp);
        BOOL tr = FALSE;
        pfi->SupportsTransparency(&tr);
        info.alpha = tr != FALSE;
    }
    if (bpp == 0 || bpp % 8 != 0) {
        err = T(L"Dieses Pixelformat wird nicht unterstützt.");
        return false;
    }
    info.bpp = bpp;
    info.label = FormatLabel(info.format, bpp);
    if (IsIndexed(nativeFirst)) info.label += T(L" (aus Palette)");
    return true;
}

bool WriteAtlas(const std::vector<std::wstring>& files, const AtlasInfo& info, int cols, int rows,
                const std::wstring& output, const std::function<bool(float)>& progress, std::wstring& err) {
    IWICImagingFactory* wic = Wic();
    if (!wic || files.empty() || cols < 1 || rows < 1 || info.bpp == 0) {
        err = T(L"Kacheltextur konnte nicht erstellt werden.");
        return false;
    }
    const uint64_t W = static_cast<uint64_t>(cols) * info.w, H = static_cast<uint64_t>(rows) * info.h;
    const uint64_t Bpp = info.bpp / 8;
    const uint64_t stride64 = W * Bpp;
    if (W > 0x7FFFFFFF || H > 0x7FFFFFFF || stride64 > 0x7FFFFFFF) {
        err = T(L"Die Textur ist zu groß für eine PNG-Datei.");
        return false;
    }
    const UINT stride = static_cast<UINT>(stride64);
    const UINT tileBytes = static_cast<UINT>(static_cast<uint64_t>(info.w) * Bpp);
    // Streifen von hoechstens ca. 64 MB
    const UINT band = static_cast<UINT>(std::clamp<uint64_t>((64ull << 20) / stride64, 1, info.h));
    std::vector<uint8_t> buf;
    try {
        buf.resize(static_cast<size_t>(band) * stride);
    } catch (...) {
        err = T(L"Nicht genug Arbeitsspeicher.");
        return false;
    }

    std::wstring tmp = output + L".peek-tmp";
    bool ok = false;
    {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fe;
        ComPtr<IPropertyBag2> props;
        if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(LongPath(tmp).c_str(), GENERIC_WRITE)) ||
            FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
            FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&fe, &props)) ||
            FAILED(fe->Initialize(props.Get())) || FAILED(fe->SetSize(static_cast<UINT>(W), static_cast<UINT>(H)))) {
            err = T(L"Datei kann nicht geschrieben werden: ") + FileNameOf(output);
        } else {
            GUID pf = info.format;
            if (FAILED(fe->SetPixelFormat(&pf)) || pf != info.format) {
                err = T(L"Dieses Pixelformat kann nicht als PNG gespeichert werden.");
            } else {
                // Aufloesung und Farbprofil des ersten Bildes uebernehmen (Pixelwerte bleiben unveraendert)
                ComPtr<IWICBitmapFrameDecode> first;
                if (OpenFrame(wic, files.front(), first)) {
                    double dx = 0, dy = 0;
                    if (SUCCEEDED(first->GetResolution(&dx, &dy)) && dx > 0 && dy > 0) fe->SetResolution(dx, dy);
                    UINT nctx = 0;
                    if (SUCCEEDED(first->GetColorContexts(0, nullptr, &nctx)) && nctx > 0) {
                        std::vector<ComPtr<IWICColorContext>> holders(nctx);
                        std::vector<IWICColorContext*> raw(nctx);
                        bool made = true;
                        for (UINT i = 0; i < nctx; ++i) {
                            if (FAILED(wic->CreateColorContext(&holders[i]))) { made = false; break; }
                            raw[i] = holders[i].Get();
                        }
                        if (made && SUCCEEDED(first->GetColorContexts(nctx, raw.data(), &nctx))) {
                            // nur eingebettete Profile (ICC) schreiben
                            std::vector<IWICColorContext*> icc;
                            for (UINT i = 0; i < nctx; ++i) {
                                WICColorContextType ty{};
                                if (SUCCEEDED(raw[i]->GetType(&ty)) && ty == WICColorContextProfile) icc.push_back(raw[i]);
                            }
                            if (!icc.empty()) fe->SetColorContexts(static_cast<UINT>(icc.size()), icc.data());
                        }
                    }
                }
                ok = true;
                const int n = static_cast<int>(files.size());
                const uint64_t totalLines = H;
                uint64_t doneLines = 0;
                for (int r = 0; r < rows && ok; ++r) {
                    // Quellen dieser Kachelzeile
                    std::vector<ComPtr<IWICBitmapSource>> src;
                    for (int c = 0; c < cols; ++c) {
                        int i = r * cols + c;
                        if (i >= n) break;
                        ComPtr<IWICBitmapSource> s;
                        if (!OpenSource(wic, files[i], info.format, info.w, info.h, s, err)) { ok = false; break; }
                        src.push_back(s);
                    }
                    for (UINT y0 = 0; ok && y0 < info.h; y0 += band) {
                        UINT bh = std::min(band, info.h - y0);
                        std::memset(buf.data(), 0, static_cast<size_t>(bh) * stride);
                        for (size_t c = 0; c < src.size(); ++c) {
                            WICRect rc = {0, static_cast<INT>(y0), static_cast<INT>(info.w), static_cast<INT>(bh)};
                            size_t off = c * tileBytes;
                            UINT size = static_cast<UINT>(static_cast<size_t>(bh - 1) * stride + tileBytes);
                            if (FAILED(src[c]->CopyPixels(&rc, stride, size, buf.data() + off))) {
                                err = T(L"Bild kann nicht gelesen werden: ") + FileNameOf(files[r * cols + c]);
                                ok = false;
                                break;
                            }
                        }
                        if (!ok) break;
                        if (FAILED(fe->WritePixels(bh, stride, bh * stride, buf.data()))) {
                            err = T(L"Datei kann nicht geschrieben werden (Datenträger voll?).");
                            ok = false;
                            break;
                        }
                        doneLines += bh;
                        if (progress && !progress(static_cast<float>(doneLines) / static_cast<float>(totalLines))) {
                            err.clear();   // abgebrochen
                            ok = false;
                        }
                    }
                }
                if (ok && (FAILED(fe->Commit()) || FAILED(enc->Commit()))) {
                    err = T(L"Datei kann nicht geschrieben werden (Datenträger voll?).");
                    ok = false;
                }
            }
        }
    }   // Stream schliessen
    if (ok && !MoveFileExW(LongPath(tmp).c_str(), LongPath(output).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = T(L"Datei kann nicht gespeichert werden: ") + FileNameOf(output);
        ok = false;
    }
    if (!ok) DeleteFileW(LongPath(tmp).c_str());
    return ok;
}

} // namespace peek
