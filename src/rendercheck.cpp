// Pruefmodus (--render-check <bericht.txt>): vergleicht die schnellen Zeichenwege mit den bisherigen.
//  - Wolke, nahe Karte: Fuellung mit Bitmap-Pinsel und runden Ecken  <->  Ebene mit Maske + DrawBitmap
//  - Betrachter, ruhende Ansicht: einmal gefiltertes Bild 1:1 kopiert  <->  direkt gefiltert
// Beide muessen dieselben Pixel liefern (innen exakt bzw. hoechstens Rundung, am geglaetteten Rand kleine Abweichung).
#include "common.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace peek {

void DrawRoundedBitmap(ID2D1DeviceContext* dc, ID2D1Bitmap1* bmp, const D2D1_RECT_F& rc, float rad) {
    D2D1_SIZE_F bs = bmp->GetSize();
    if (bs.width <= 0 || bs.height <= 0) return;
    D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale((rc.right - rc.left) / bs.width, (rc.bottom - rc.top) / bs.height) *
                          D2D1::Matrix3x2F::Translation(rc.left, rc.top);
    ComPtr<ID2D1BitmapBrush1> brush;
    dc->CreateBitmapBrush(bmp,
                          D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC),
                          D2D1::BrushProperties(1.0f, m), &brush);
    if (brush) dc->FillRoundedRectangle(D2D1::RoundedRect(rc, rad, rad), brush.Get());
}

namespace {
// Zeichnet nichts, sondern haelt die Glyphenlaeufe fest (Text-Layout -> GlyphRuns)
class RunRecorder final : public IDWriteTextRenderer {
public:
    explicit RunRecorder(GlyphRuns& out) : out_(out) {}
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *ppv = static_cast<IDWriteTextRenderer*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 1; }    // lebt auf dem Stapel
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = TRUE; return S_OK; }
    IFACEMETHODIMP GetCurrentTransform(void*, DWRITE_MATRIX* m) override { *m = {1, 0, 0, 1, 0, 0}; return S_OK; }
    IFACEMETHODIMP GetPixelsPerDip(void*, FLOAT* ppd) override { *ppd = 1.0f; return S_OK; }
    IFACEMETHODIMP DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        GlyphRuns::Run r;
        r.face = run->fontFace;
        r.em = run->fontEmSize;
        r.idx.assign(run->glyphIndices, run->glyphIndices + run->glyphCount);
        if (run->glyphAdvances) r.adv.assign(run->glyphAdvances, run->glyphAdvances + run->glyphCount);
        if (run->glyphOffsets) r.off.assign(run->glyphOffsets, run->glyphOffsets + run->glyphCount);
        r.sideways = run->isSideways;
        r.bidi = run->bidiLevel;
        r.x = x;
        r.y = y;
        r.mode = mode;
        out_.runs.push_back(std::move(r));
        return S_OK;
    }
    IFACEMETHODIMP DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    IFACEMETHODIMP DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    IFACEMETHODIMP DrawInlineObject(void* ctx, FLOAT x, FLOAT y, IDWriteInlineObject* obj, BOOL sideways, BOOL rtl,
                                    IUnknown* effect) override {
        return obj ? obj->Draw(ctx, this, x, y, sideways, rtl, effect) : S_OK;   // Auslassungszeichen: zeichnet sich ueber uns
    }

private:
    GlyphRuns& out_;
};
}  // namespace

void RecordGlyphRuns(IDWriteTextLayout* layout, GlyphRuns& out) {
    out.runs.clear();
    if (!layout) return;
    RunRecorder rec(out);
    layout->Draw(nullptr, &rec, 0, 0);
}

void DrawGlyphRuns(ID2D1DeviceContext* dc, const GlyphRuns& g, ID2D1Brush* brush, int snap) {
    D2D1_MATRIX_3X2_F m;
    float dx = 96, dy = 96;
    if (snap) {
        dc->GetTransform(&m);
        dc->GetDpi(&dx, &dy);
    }
    for (const auto& r : g.runs) {
        DWRITE_GLYPH_RUN run{};
        run.fontFace = r.face.Get();
        run.fontEmSize = r.em;
        run.glyphCount = static_cast<UINT32>(r.idx.size());
        run.glyphIndices = r.idx.data();
        run.glyphAdvances = r.adv.empty() ? nullptr : r.adv.data();
        run.glyphOffsets = r.off.empty() ? nullptr : r.off.data();
        run.isSideways = r.sideways;
        run.bidiLevel = r.bidi;
        D2D1_POINT_2F p = D2D1::Point2F(r.x, r.y);
        if (snap && m._12 == 0 && m._21 == 0 && m._11 > 0 && m._22 > 0) {
            // Grundlinie im Geraeteraum auf ganze Pixel legen und zurueckrechnen
            float sx = m._11 * dx / 96.0f, sy = m._22 * dy / 96.0f;
            if (snap == 2) p.x = (std::round((p.x * m._11 + m._31) * dx / 96.0f) - m._31 * dx / 96.0f) / sx;
            p.y = (std::round((p.y * m._22 + m._32) * dy / 96.0f) - m._32 * dy / 96.0f) / sy;
        }
        dc->DrawGlyphRun(p, &run, brush, r.mode);
    }
}

namespace {

struct Img { UINT w = 0, h = 0; std::vector<uint8_t> px; };

ComPtr<ID2D1Bitmap1> MakeSource(ID2D1DeviceContext* dc, UINT w, UINT h) {
    // feines Muster mit harten Kanten und Rauschen: empfindlich gegen jede Abweichung beim Filtern
    std::vector<uint8_t> px(size_t(w) * h * 4);
    uint32_t seed = 12345;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            uint8_t n = static_cast<uint8_t>(seed >> 24);
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            bool check = ((x / 3) + (y / 5)) & 1;
            p[0] = static_cast<uint8_t>(check ? 230 : 25);
            p[1] = static_cast<uint8_t>((x * 255) / w);
            p[2] = static_cast<uint8_t>((n >> 1) + ((y * 127) / h));
            p[3] = 255;
        }
    ComPtr<ID2D1Bitmap1> bmp;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, &bmp);
    return bmp;
}

ComPtr<ID2D1Bitmap1> MakeTarget(ID2D1DeviceContext* dc, UINT w, UINT h) {
    ComPtr<ID2D1Bitmap1> bmp;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &bp, &bmp);
    return bmp;
}

bool ReadBack(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, Img& out) {
    D2D1_SIZE_U s = target->GetPixelSize();
    ComPtr<ID2D1Bitmap1> cpu;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(dc->CreateBitmap(s, nullptr, 0, &bp, &cpu))) return false;
    if (FAILED(cpu->CopyFromBitmap(nullptr, target, nullptr))) return false;
    D2D1_MAPPED_RECT mr{};
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mr))) return false;
    out.w = s.width;
    out.h = s.height;
    out.px.resize(size_t(s.width) * s.height * 4);
    for (UINT y = 0; y < s.height; ++y) memcpy(&out.px[size_t(y) * s.width * 4], mr.bits + size_t(y) * mr.pitch, s.width * 4);
    cpu->Unmap();
    return true;
}

// groesste Abweichung je Farbkanal: innen (mindestens 3 px vom Rand der Karte entfernt) und insgesamt
void Compare(const Img& a, const Img& b, const D2D1_RECT_F& rc, int& inner, int& all, int& innerCount) {
    inner = all = innerCount = 0;
    for (UINT y = 0; y < a.h; ++y)
        for (UINT x = 0; x < a.w; ++x) {
            const uint8_t* p = &a.px[(size_t(y) * a.w + x) * 4];
            const uint8_t* q = &b.px[(size_t(y) * a.w + x) * 4];
            int d = 0;
            for (int ch = 0; ch < 4; ++ch) d = std::max(d, std::abs(int(p[ch]) - int(q[ch])));
            all = std::max(all, d);
            bool in = x >= rc.left + 8 && x + 1 <= rc.right - 8 && y >= rc.top + 8 && y + 1 <= rc.bottom - 8;
            if (in) {
                inner = std::max(inner, d);
                if (d > 0) ++innerCount;
            }
        }
}

}  // namespace

int RunRenderCheck(const std::wstring& report) {
    FILE* f = _wfopen(report.c_str(), L"w");
    if (!f) return 2;
    auto fail = [&](const char* what) {
        fprintf(f, "FEHLER: %s\n", what);
        fclose(f);
        return 2;
    };
    ComPtr<ID3D11Device> d3d;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const char* driver = "Hardware";
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d,
                                   nullptr, nullptr);
    if (FAILED(hr)) {
        driver = "WARP (Software)";
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr,
                               nullptr);
    }
    if (FAILED(hr)) return fail("kein Direct3D-Geraet");
    ComPtr<IDXGIDevice> dxgi;
    d3d.As(&dxgi);
    ComPtr<ID2D1Factory1> fac;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, &fac)))
        return fail("keine Direct2D-Factory");
    ComPtr<ID2D1Device> dev;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(fac->CreateDevice(dxgi.Get(), &dev)) || FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))
        return fail("kein Direct2D-Geraet");
    dc->SetDpi(96, 96);
    fprintf(f, "Zeichenwege vergleichen (%s)\n", driver);

    const UINT TW = 720, TH = 540;
    ComPtr<ID2D1Bitmap1> big = MakeSource(dc.Get(), 640, 480), tiny = MakeSource(dc.Get(), 200, 150);
    ComPtr<ID2D1Bitmap1> ta = MakeTarget(dc.Get(), TW, TH), tb = MakeTarget(dc.Get(), TW, TH);
    if (!big || !tiny || !ta || !tb) return fail("Testbilder");
    ComPtr<ID2D1SolidColorBrush> solid;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &solid);
    const D2D1_COLOR_F bg = D2D1::ColorF(0.17f, 0.17f, 0.17f, 1);
    bool ok = true;

    struct Case { const char* name; ID2D1Bitmap1* src; D2D1_RECT_F rc; float fog = 0; };
    const D2D1_COLOR_F fogC = D2D1::ColorF(0.17f, 0.17f, 0.17f, 1);
    const Case cases[] = {
        {"Karte stark verkleinert (0,31)", big.Get(), D2D1::RectF(10.3f, 7.6f, 10.3f + 200, 7.6f + 150)},
        {"Karte verkleinert (0,62)", big.Get(), D2D1::RectF(5.5f, 5.25f, 5.5f + 400, 5.25f + 300)},
        {"Karte fast 1:1 (0,95)", big.Get(), D2D1::RectF(3.2f, 4.7f, 3.2f + 608, 4.7f + 456)},
        {"Karte 1:1", big.Get(), D2D1::RectF(20, 20, 660, 500)},
        {"Karte vergroessert (2,6)", tiny.Get(), D2D1::RectF(12.4f, 9.1f, 12.4f + 520, 9.1f + 390)},
        {"Karte abgedunkelt (0,62)", big.Get(), D2D1::RectF(5.5f, 5.25f, 5.5f + 400, 5.25f + 300), 0.45f},
    };
    for (const Case& cs : cases) {
        float rad = std::min(5.0f, 0.05f * std::min(cs.rc.right - cs.rc.left, cs.rc.bottom - cs.rc.top));
        // A: bisheriger Weg (Ebene mit runder Maske)
        dc->SetTarget(ta.Get());
        dc->BeginDraw();
        dc->Clear(bg);
        {
            ComPtr<ID2D1RoundedRectangleGeometry> geo;
            fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(cs.rc, rad, rad), &geo);
            dc->PushLayer(D2D1::LayerParameters1(cs.rc, geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f),
                          nullptr);
            dc->DrawBitmap(cs.src, cs.rc, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            if (cs.fog > 0) {
                solid->SetColor(D2D1::ColorF(fogC.r, fogC.g, fogC.b, cs.fog));
                dc->FillRectangle(cs.rc, solid.Get());
            }
            dc->PopLayer();
        }
        if (FAILED(dc->EndDraw())) return fail("Zeichnen A");
        // B: neuer Weg (Pinsel)
        dc->SetTarget(tb.Get());
        dc->BeginDraw();
        dc->Clear(bg);
        DrawRoundedBitmap(dc.Get(), cs.src, cs.rc, rad);
        if (cs.fog > 0) {
            solid->SetColor(D2D1::ColorF(fogC.r, fogC.g, fogC.b, cs.fog));
            dc->FillRoundedRectangle(D2D1::RoundedRect(cs.rc, rad, rad), solid.Get());
        }
        if (FAILED(dc->EndDraw())) return fail("Zeichnen B");
        dc->SetTarget(nullptr);
        Img a, b;
        if (!ReadBack(dc.Get(), ta.Get(), a) || !ReadBack(dc.Get(), tb.Get(), b)) return fail("Auslesen");
        int inner, all, cnt;
        Compare(a, b, cs.rc, inner, all, cnt);
        bool good = inner <= 1 && all <= 80;   // Bildflaeche gleich; der geglaettete Rand darf abweichen (siehe cloud.cpp)
        ok = ok && good;
        fprintf(f, "  %-34s innen max %3d (%d Pixel), Rand max %3d  %s\n", cs.name, inner, cnt, all, good ? "ok" : "ABWEICHUNG");
    }

    // Betrachter: Zwischenspeicher der ruhenden Ansicht
    {
        D2D1_RECT_F dst = D2D1::RectF(33.4f, 21.7f, 33.4f + 512, 21.7f + 384);
        const D2D1_COLOR_F overlay = D2D1::ColorF(0.03f, 0.03f, 0.03f, 1);
        dc->SetTarget(ta.Get());
        dc->BeginDraw();
        dc->Clear(overlay);
        dc->DrawBitmap(big.Get(), dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        if (FAILED(dc->EndDraw())) return fail("Zeichnen Betrachter A");
        int x0 = 0, y0 = 0;   // vom Ursprung an, wie im Betrachter
        int x1 = static_cast<int>(std::ceil(dst.right)), y1 = static_cast<int>(std::ceil(dst.bottom));
        ComPtr<ID2D1Bitmap1> cache = MakeTarget(dc.Get(), static_cast<UINT>(x1 - x0), static_cast<UINT>(y1 - y0));
        if (!cache) return fail("Zwischenspeicher");
        dc->SetTarget(cache.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(-x0), static_cast<float>(-y0)));
        dc->DrawBitmap(big.Get(), dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        if (FAILED(dc->EndDraw())) return fail("Zeichnen Zwischenspeicher");
        dc->SetTarget(tb.Get());
        dc->BeginDraw();
        dc->Clear(overlay);
        dc->DrawBitmap(cache.Get(),
                       D2D1::RectF(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1)),
                       1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        if (FAILED(dc->EndDraw())) return fail("Zeichnen Betrachter B");
        dc->SetTarget(nullptr);
        Img a, b;
        if (!ReadBack(dc.Get(), ta.Get(), a) || !ReadBack(dc.Get(), tb.Get(), b)) return fail("Auslesen");
        int inner, all, cnt;
        Compare(a, b, dst, inner, all, cnt);
        bool good = all <= 1;
        ok = ok && good;
        fprintf(f, "  %-34s innen max %3d (%d Pixel), Rand max %3d  %s\n", "Betrachter, ruhende Ansicht", inner, cnt, all,
                good ? "ok" : "ABWEICHUNG");
    }

    // Text: Layout zeichnen  <->  aufgezeichnete Glyphenlaeufe (mit verschiedenem Einrasten der Grundlinie)
    {
        ComPtr<IDWriteFactory> dw;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
        ComPtr<IDWriteTextFormat> tf;
        if (dw) dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"de-de", &tf);
        ComPtr<IDWriteTextLayout> lay;
        const wchar_t* name = L"Quartalsbericht_2024_final_Entwurf_v3 (Kopie).docx";
        if (tf) {
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            tf->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
            dw->CreateTextLayout(name, static_cast<UINT32>(wcslen(name)), tf.Get(), 120.0f, 33.0f, &lay);
        }
        if (!lay) return fail("Text-Layout");
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> sign;
        dw->CreateEllipsisTrimmingSign(tf.Get(), &sign);
        lay->SetTrimming(&trim, sign.Get());
        GlyphRuns runs;
        RecordGlyphRuns(lay.Get(), runs);
        fprintf(f, "Text: %d Glyphenlaeufe\n", static_cast<int>(runs.runs.size()));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        struct TCase { float scale, x, y; };
        const TCase tcs[] = {{0.75f, 103.37f, 57.62f}, {0.75f, 40.0f, 21.5f}, {1.0f, 20.5f, 33.25f}, {1.21f, 50.1f, 80.9f}, {1.3333f, 77.77f, 12.34f}};
        const D2D1_COLOR_F tc = D2D1::ColorF(1, 1, 1, 0.9f);
        int worst[3] = {0, 0, 0};
        for (const TCase& t : tcs) {
            D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(t.scale, t.scale) * D2D1::Matrix3x2F::Translation(t.x, t.y);
            dc->SetTarget(ta.Get());
            dc->BeginDraw();
            dc->Clear(bg);
            dc->SetTransform(m);
            solid->SetColor(tc);
            dc->DrawTextLayout(D2D1::Point2F(0, 0), lay.Get(), solid.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            if (FAILED(dc->EndDraw())) return fail("Text A");
            dc->SetTarget(nullptr);
            Img a;
            if (!ReadBack(dc.Get(), ta.Get(), a)) return fail("Auslesen");
            int d[3];
            for (int snap = 0; snap < 3; ++snap) {
                dc->SetTarget(tb.Get());
                dc->BeginDraw();
                dc->Clear(bg);
                dc->SetTransform(m);
                DrawGlyphRuns(dc.Get(), runs, solid.Get(), snap);
                dc->SetTransform(D2D1::Matrix3x2F::Identity());
                if (FAILED(dc->EndDraw())) return fail("Text B");
                dc->SetTarget(nullptr);
                Img b;
                if (!ReadBack(dc.Get(), tb.Get(), b)) return fail("Auslesen");
                int inner, cnt;
                Compare(a, b, D2D1::RectF(0, 0, 0, 0), inner, d[snap], cnt);
                worst[snap] = std::max(worst[snap], d[snap]);
            }
            fprintf(f, "  Text x%.2f bei (%.2f, %.2f): ohne Einrasten %3d, senkrecht %3d, beide %3d\n", t.scale, t.x, t.y, d[0], d[1], d[2]);
        }
        fprintf(f, "  Text gesamt: ohne %d, senkrecht %d, beide %d\n", worst[0], worst[1], worst[2]);

        // Zeitbedarf je Zeichenbefehl (nur der Aufruf, ohne Abschluss)
        const int N = 3000;
        ComPtr<ID2D1Bitmap1> icon = MakeSource(dc.Get(), 256, 256);
        auto bench = [&](const char* what, const std::function<void(int)>& fn) {
            dc->SetTarget(ta.Get());
            dc->BeginDraw();
            dc->Clear(bg);
            double t0 = NowMs();
            for (int i = 0; i < N; ++i) fn(i);
            double t1 = NowMs();
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->EndDraw();
            double t2 = NowMs();
            dc->SetTarget(nullptr);
            fprintf(f, "  %-38s %6.2f us je Aufruf, Abschluss %6.2f us\n", what, (t1 - t0) * 1000.0 / N, (t2 - t1) * 1000.0 / N);
        };
        auto pos = [](int i) { return D2D1::Point2F(10.3f + (i % 40) * 14.37f, 8.6f + (i / 40 % 30) * 15.21f); };
        solid->SetColor(tc);
        bench("Text-Layout (skaliert)", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(0.75f, 0.75f) * D2D1::Matrix3x2F::Translation(p.x, p.y));
            dc->DrawTextLayout(D2D1::Point2F(0, 0), lay.Get(), solid.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        });
        bench("Glyphenlaeufe (skaliert)", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(0.75f, 0.75f) * D2D1::Matrix3x2F::Translation(p.x, p.y));
            DrawGlyphRuns(dc.Get(), runs, solid.Get(), 2);
        });
        bench("Flaeche mit runden Ecken", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p.x, p.y, p.x + 90, p.y + 70), 4, 4), solid.Get());
        });
        bench("Rechteck", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->FillRectangle(D2D1::RectF(p.x, p.y, p.x + 90, p.y + 70), solid.Get());
        });
        bench("Symbol hochwertig verkleinert", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->DrawBitmap(icon.Get(), D2D1::RectF(p.x, p.y, p.x + 41.3f, p.y + 41.3f), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        });
        bench("Symbol linear", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->DrawBitmap(icon.Get(), D2D1::RectF(p.x, p.y, p.x + 41.3f, p.y + 41.3f), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        });
        bench("Begrenzung setzen und aufheben", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->PushAxisAlignedClip(D2D1::RectF(p.x, p.y, p.x + 90, p.y + 70), D2D1_ANTIALIAS_MODE_ALIASED);
            dc->PopAxisAlignedClip();
        });
        bench("Begrenzung + Rechteck", [&](int i) {
            D2D1_POINT_2F p = pos(i);
            dc->PushAxisAlignedClip(D2D1::RectF(p.x, p.y, p.x + 90, p.y + 70), D2D1_ANTIALIAS_MODE_ALIASED);
            dc->FillRectangle(D2D1::RectF(p.x + 5, p.y + 5, p.x + 40, p.y + 40), solid.Get());
            dc->PopAxisAlignedClip();
        });
    }
    fprintf(f, ok ? "ERGEBNIS: gleich\n" : "ERGEBNIS: Abweichung\n");
    fclose(f);
    return ok ? 0 : 1;
}

}  // namespace peek
