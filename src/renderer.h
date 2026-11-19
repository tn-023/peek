#pragma once
// GPU-Rendering: Direct3D 11 + Direct2D + DirectComposition (transparente Swapchain fuer Mica).
#include "common.h"

#include <map>

namespace peek {

// Bild mit runden Ecken ohne Zwischenebene zeichnen (hochwertig gefiltert)
void DrawRoundedBitmap(ID2D1DeviceContext* dc, ID2D1Bitmap1* bmp, const D2D1_RECT_F& rc, float rad);
// Glyphenlaeufe eines Text-Layouts: einmal aufzeichnen, danach ohne erneuten Durchlauf des Layouts zeichnen
struct GlyphRuns {
    struct Run {
        ComPtr<IDWriteFontFace> face;
        float em = 0;
        std::vector<UINT16> idx;
        std::vector<float> adv;
        std::vector<DWRITE_GLYPH_OFFSET> off;
        BOOL sideways = FALSE;
        UINT32 bidi = 0;
        float x = 0, y = 0;   // Grundlinie relativ zum Layout (nicht auf Pixel eingerastet)
        DWRITE_MEASURING_MODE mode = DWRITE_MEASURING_MODE_NATURAL;
    };
    std::vector<Run> runs;
};
void RecordGlyphRuns(IDWriteTextLayout* layout, GlyphRuns& out);
// snap: 0 = nicht einrasten, 1 = Grundlinie senkrecht, 2 = waagerecht und senkrecht auf ganze Pixel (Geraeteraum)
void DrawGlyphRuns(ID2D1DeviceContext* dc, const GlyphRuns& g, ID2D1Brush* brush, int snap);
// Pruefmodus --render-check: schnelle Zeichenwege mit den bisherigen vergleichen (0 = gleich)
int RunRenderCheck(const std::wstring& report);

class Renderer {
public:
    bool Init(HWND hwnd);
    void Resize();
    void SetDpi(float dpi);
    bool UpdateTextScale();   // Windows-Textskalierung neu lesen; true = geaendert
    bool BeginDraw();
    // true: der Bildschirm hat das vorige Bild abgenommen (Takt der Bildwiederholung). false: kein Takt verfuegbar
    // (Fenster verdeckt) oder keine Takt-Abfrage moeglich (dann framePaced() == false)
    bool frameReady() const { return waited_; }
    bool framePaced() const { return frameWait_ != nullptr; }
    void EndDraw();

    ID2D1DeviceContext* dc() const { return dc_.Get(); }
    IDWriteFactory* dwrite() const { return dwrite_.Get(); }
    ID2D1Factory1* factory() const { return factory_.Get(); }
    // wirksame DPI: Bildschirm-DPI x Windows-Textskalierung (Bedienungshilfen > Textgroesse)
    float dpi() const { return dpi_ * textScale_; }
    float scale() const { return dpi_ * textScale_ / 96.0f; }
    float widthDip() const { return width_ / scale(); }
    float heightDip() const { return height_ / scale(); }
    bool deviceLost() { bool l = lost_; lost_ = false; return l; }
    uint32_t maxBitmapSize() const { return maxBitmap_; }

    ComPtr<ID2D1Bitmap1> CreateBitmap(const Pixels& px);

    ID2D1SolidColorBrush* Brush(const D2D1_COLOR_F& c);
    IDWriteTextFormat* Text(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteTextFormat* Icon(float size, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_CENTER);
    // Mehrzeiliger Text: umbricht statt abzuschneiden (oben ausgerichtet)
    IDWriteTextFormat* TextWrap(float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                                DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float MeasureText(const std::wstring& s, IDWriteTextFormat* fmt);
    float MeasureWrapped(const std::wstring& s, IDWriteTextFormat* fmt, float width);
    float DrawWrapped(const std::wstring& s, float x, float y, float width, IDWriteTextFormat* fmt,
                      const D2D1_COLOR_F& color);
    // Schneller Zeilenumbruch fuer viele Dateinamen (eigene Zeichenbreiten, ohne Text-Layout)
    float TextWidthFast(const std::wstring& s, float size);   // dieselbe Summe wie in BreakLines
    void BreakLines(const std::wstring& s, float width, float size, std::vector<std::pair<uint32_t, uint32_t>>& lines);
    // App-Symbol "p" (dunkelgraue abgerundete Flaeche, Windows-11-Blau)
    void DrawSvgResource(int id, const D2D1_RECT_F& rc);   // SVG aus RCDATA, eingepasst

private:
    bool CreateDevice();
    bool CreateTarget();
    IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a,
                              bool wrap = false);
    float Advance(wchar_t c, float size);
    ComPtr<IDWriteFontFace> face_;
    UINT16 unitsPerEm_ = 2048;
    std::unordered_map<wchar_t, float> advances_;   // in Einheiten von 1 DIP Schriftgroesse
    std::vector<float> advFast_;                    // dasselbe als Tabelle fuer haeufige Zeichen (< 0x3000), -1 = unbekannt
    HANDLE frameWait_ = nullptr;                    // Takt der Swapchain (wartbar), sonst nullptr
    UINT swapFlags_ = 0;
    bool waited_ = true;

    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice> dxgiDevice_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID2D1Factory1> factory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<ID2D1Bitmap1> target_;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> visual_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<uint64_t, ComPtr<IDWriteTextFormat>> formats_;
    std::wstring textFamily_ = L"Segoe UI";
    std::wstring iconFamily_ = L"Segoe MDL2 Assets";
    UINT width_ = 1, height_ = 1;
    float dpi_ = 96.0f;
    float textScale_ = 1.0f;
    bool lost_ = false;
    std::unordered_map<int, ComPtr<ID2D1SvgDocument>> svgDocs_;
    uint32_t maxBitmap_ = 16384;
};

} // namespace peek
