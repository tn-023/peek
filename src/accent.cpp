// Akzentfarbe (Einstellungen > Darstellung): die von Windows (Standard), eine der Farben von Windows, eine der zuletzt
// verwendeten eigenen Farben oder eine eigene Farbe (Farbfeld, Helligkeit, Hex-Wert, RGB bzw. HSV). Beim Waehlen
// faerbt sich peek sofort mit; "Abbrechen" (auch Esc oder Klick ausserhalb) stellt die vorige Farbe wieder her.
#include "app.h"
#include "popup.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace peek {

namespace {

// Farben der Windows-Einstellungen (Personalisierung > Farben), in derselben Reihenfolge
const uint32_t kPalette[48] = {
    0xFFB900, 0xFF8C00, 0xF7630C, 0xCA5010, 0xDA3B01, 0xEF6950, 0xD13438, 0xFF4343, 0xE74856,
    0xE81123, 0xEA005E, 0xC30052, 0xE3008C, 0xBF0077, 0xC239B3, 0x9A0089, 0x0078D7, 0x0063B1,
    0x8E8CD8, 0x6B69D6, 0x8764B8, 0x744DA9, 0xB146C2, 0x881798, 0x0099BC, 0x2D7D9A, 0x00B7C3,
    0x038387, 0x00B294, 0x018574, 0x00CC6A, 0x10893E, 0x7A7574, 0x5D5A58, 0x68768A, 0x515C6B,
    0x567C73, 0x486860, 0x498205, 0x107C10, 0x767676, 0x4C4A48, 0x69797E, 0x4A5459, 0x647C64,
    0x525E54, 0x847545, 0x7E735F};

D2D1_COLOR_F Rgb(uint32_t c) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f);
}

float Luminance(const D2D1_COLOR_F& c) {   // relative Helligkeit (sRGB)
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

// HSV (h 0..360, s und v 0..1) <-> RGB
void HsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    h = std::fmod(std::max(0.0f, h), 360.0f) / 60.0f;
    const int i = static_cast<int>(h);
    const float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

void RgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) return;   // Grau: Farbton bleibt
    if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
}

uint32_t Pack(float r, float g, float b) {
    auto c = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

std::wstring Hex(uint32_t c) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%06X", c & 0xFFFFFF);
    return buf;
}

void DrawStr(ID2D1DeviceContext* dc, Renderer& r, const std::wstring& s, const D2D1_RECT_F& rc, IDWriteTextFormat* f,
             const D2D1_COLOR_F& c) {
    if (f && !s.empty()) dc->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, rc, r.Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Farbfeld mit Rand; gewaehlt: weisses Eck mit Haken oben rechts (wie in den Windows-Einstellungen)
void Swatch(ID2D1DeviceContext* dc, Renderer& r, const D2D1_RECT_F& rc, uint32_t c, bool hover, bool selected) {
    dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4), r.Brush(Rgb(c)));
    if (selected || hover) {
        D2D1_RECT_F in = D2D1::RectF(rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(in, 3.5f, 3.5f), r.Brush(D2D1::ColorF(1, 1, 1, selected ? 1.0f : 0.7f)), 2.0f);
    }
    if (selected) {
        D2D1_RECT_F ck = D2D1::RectF(rc.right - 18, rc.top + 2, rc.right - 2, rc.top + 18);
        dc->FillRectangle(ck, r.Brush(D2D1::ColorF(1, 1, 1, 1)));
        wchar_t g[2] = {0xE73E, 0};
        DrawStr(dc, r, g, ck, r.Icon(10), D2D1::ColorF(0.1f, 0.1f, 0.1f, 1));
    }
}

}  // namespace

// accent_ fuer die Oberflaeche: die gewaehlte Farbe; die von Windows wie bisher (im dunklen Modus etwas heller).
// Sehr dunkle Farben im dunklen bzw. sehr helle im hellen Modus werden so weit angepasst, dass sie sichtbar bleiben.
void App::ApplyAccent() {
    D2D1_COLOR_F c = D2D1::ColorF(0, 0.47f, 0.84f, 1);   // falls Windows keine Farbe nennt
    if (settings_.accent >= 0) {
        c = Rgb(static_cast<uint32_t>(settings_.accent));
        accentBase_ = c;
    } else {
        DWORD accent = 0, sz = sizeof(accent);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                         &accent, &sz) == ERROR_SUCCESS)   // 0xAABBGGRR
            c = D2D1::ColorF((accent & 0xFF) / 255.0f, ((accent >> 8) & 0xFF) / 255.0f, ((accent >> 16) & 0xFF) / 255.0f, 1.0f);
        accentBase_ = c;
        if (dark_) {   // im dunklen Modus etwas aufhellen, wie Windows
            c.r += (1 - c.r) * 0.25f;
            c.g += (1 - c.g) * 0.25f;
            c.b += (1 - c.b) * 0.25f;
        }
    }
    for (int i = 0; i < 20; ++i) {   // Lesbarkeit auf dem Hintergrund
        const float l = Luminance(c);
        if (dark_ && l < 0.09f) {
            c.r += (1 - c.r) * 0.12f;
            c.g += (1 - c.g) * 0.12f;
            c.b += (1 - c.b) * 0.12f;
        } else if (!dark_ && l > 0.62f) {
            c.r *= 0.9f;
            c.g *= 0.9f;
            c.b *= 0.9f;
        } else {
            break;
        }
    }
    accent_ = c;
    // Text auf der Akzentfarbe: der staerkere Kontrast von Weiss und Schwarz
    const float l = Luminance(c);
    g_onAccent = (1.05f / (l + 0.05f)) >= ((l + 0.05f) / 0.05f) ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, 0.9f);
}

void App::SetAccent(int rgb, bool save) {
    settings_.accent = rgb < 0 ? -1 : (rgb & 0xFFFFFF);
    ApplyAccent();
    if (save) settings_.Save();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);   // Ansicht dahinter sofort in der neuen Farbe
}

// ---------------------------------------------------------------------------
// Auswahl: Windows (Standard), Farben von Windows, zuletzt verwendet, Eigene Farbe …

class AccentPanel : public PopupPanel {
public:
    explicit AccentPanel(App& app) : app_(app) {}
    bool custom = false;   // "Eigene Farbe …" gewaehlt

    D2D1_SIZE_F Measure(Renderer&) override {
        float h = kPad + kRow + 6 + kRowsP * kCell + 8;
        if (!app_.settings_.recentAccents.empty()) h += 26 + kCell + 4;
        h += 6 + kRow + kPad;
        return D2D1::SizeF(kPad * 2 + kCols * kCell, h);
    }

    void Paint(Renderer& r, float width, float) override {
        ID2D1DeviceContext* dc = r.dc();
        hits_.clear();
        const int cur = app_.settings_.accent;
        auto row = [&](float y, int id, wchar_t glyph, const std::wstring& text, bool checked) {
            D2D1_RECT_F rc = D2D1::RectF(kPad, y, width - kPad, y + kRow);
            hits_.push_back({rc, id, 0});
            if (hover_ == id) dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4), r.Brush(PopupHover()));
            wchar_t g[2] = {glyph, 0};
            if (glyph) DrawStr(dc, r, g, D2D1::RectF(rc.left + 8, rc.top, rc.left + 30, rc.bottom), r.Icon(13), PopupText());
            DrawStr(dc, r, text, D2D1::RectF(rc.left + 36, rc.top, rc.right - 36, rc.bottom), r.Text(13), PopupText());
            if (checked) {
                wchar_t ck[2] = {0xE73E, 0};
                DrawStr(dc, r, ck, D2D1::RectF(rc.right - 30, rc.top, rc.right - 8, rc.bottom), r.Icon(12), app_.accent_);
            }
        };
        float y = kPad;
        row(y, 1, 0xE770, T(L"Windows-Akzentfarbe (Standard)"), cur < 0);
        y += kRow + 6;
        for (int i = 0; i < 48; ++i) {
            D2D1_RECT_F rc = Cell(i % kCols, y + (i / kCols) * kCell);
            hits_.push_back({rc, 10, i});
            Swatch(dc, r, rc, kPalette[i], hover_ == 10 && hoverIdx_ == i, cur == static_cast<int>(kPalette[i]));
        }
        y += kRowsP * kCell + 8;
        const auto& rec = app_.settings_.recentAccents;
        if (!rec.empty()) {
            DrawStr(dc, r, T(L"Zuletzt verwendet"), D2D1::RectF(kPad + 4, y, width - kPad, y + 24), r.Text(12), PopupDim());
            y += 26;
            for (size_t i = 0; i < rec.size(); ++i) {
                D2D1_RECT_F rc = Cell(static_cast<int>(i), y);
                hits_.push_back({rc, 11, static_cast<int>(i)});
                Swatch(dc, r, rc, rec[i], hover_ == 11 && hoverIdx_ == static_cast<int>(i), cur == static_cast<int>(rec[i]));
            }
            y += kCell + 4;
        }
        float ly = std::round(y * r.scale()) / r.scale() + 0.5f;
        dc->DrawLine(D2D1::Point2F(8, ly), D2D1::Point2F(width - 8, ly), r.Brush(PopupLine()), 1.0f);
        y += 6;
        row(y, 2, 0xE790, T(L"Eigene Farbe …"), false);
    }

    void OnMouse(UINT msg, float x, float y) override {
        int type = 0, idx = -1;
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (x >= it->rc.left && x < it->rc.right && y >= it->rc.top && y < it->rc.bottom) { type = it->type; idx = it->a; break; }
        if (msg == WM_MOUSEMOVE) {
            if (type != hover_ || idx != hoverIdx_) { hover_ = type; hoverIdx_ = idx; dirty = true; }
            return;
        }
        if (msg != WM_LBUTTONUP) return;
        switch (type) {
        case 1: app_.SetAccent(-1, true); close = true; break;
        case 10: app_.SetAccent(static_cast<int>(kPalette[idx]), true); close = true; break;
        case 11:
            if (idx >= 0 && idx < static_cast<int>(app_.settings_.recentAccents.size())) {
                app_.SetAccent(static_cast<int>(app_.settings_.recentAccents[static_cast<size_t>(idx)]), true);
                close = true;
            }
            break;
        case 2: custom = true; close = true; break;
        default: break;
        }
    }

private:
    static constexpr float kPad = 6, kRow = 36, kCell = 44;
    static constexpr int kCols = 9, kRowsP = 6;
    struct HitRc { D2D1_RECT_F rc; int type; int a; };
    D2D1_RECT_F Cell(int col, float top) const {
        return D2D1::RectF(kPad + col * kCell + 2, top + 2, kPad + (col + 1) * kCell - 2, top + kCell - 2);
    }
    App& app_;
    std::vector<HitRc> hits_;
    int hover_ = 0, hoverIdx_ = -1;
};

// ---------------------------------------------------------------------------
// Eigene Farbe: Farbfeld (Farbton waagerecht, nach unten heller), Vorschau, Helligkeit, Hex-Wert, RGB bzw. HSV

class ColorPickerPanel : public PopupPanel {
public:
    explicit ColorPickerPanel(App& app) : app_(app) {
        const uint32_t start = app_.settings_.accent >= 0 ? static_cast<uint32_t>(app_.settings_.accent)
                                                          : Pack(app_.accentBase_.r, app_.accentBase_.g, app_.accentBase_.b);
        SetRgb(start, false);
    }
    bool done = false;   // "Fertig"
    uint32_t color = 0;

    bool WantsText() const override { return true; }

    D2D1_SIZE_F Measure(Renderer&) override { return D2D1::SizeF(kW, kH); }

    void Paint(Renderer& r, float, float) override {
        ID2D1DeviceContext* dc = r.dc();
        hits_.clear();
        // Farbfeld: einmal als Bild (Farbton waagerecht, Saettigung nach unten abnehmend, volle Helligkeit)
        if (!spectrum_) {
            const UINT n = static_cast<UINT>(std::ceil(kSpec * r.scale()));
            Pixels px;
            px.w = px.srcW = n;
            px.h = px.srcH = n;
            px.data.resize(size_t(n) * n * 4);
            for (UINT y = 0; y < n; ++y)
                for (UINT x = 0; x < n; ++x) {
                    float rr, gg, bb;
                    HsvToRgb(360.0f * x / n, 1.0f - static_cast<float>(y) / (n - 1), 1.0f, rr, gg, bb);
                    uint8_t* p = &px.data[(size_t(y) * n + x) * 4];
                    p[0] = static_cast<uint8_t>(std::lround(bb * 255));
                    p[1] = static_cast<uint8_t>(std::lround(gg * 255));
                    p[2] = static_cast<uint8_t>(std::lround(rr * 255));
                    p[3] = 255;
                }
            spectrum_ = r.CreateBitmap(px);
        }
        const D2D1_RECT_F spec = SpecRect();
        hits_.push_back({spec, 1, 0});
        if (spectrum_) {
            dc->PushAxisAlignedClip(spec, D2D1_ANTIALIAS_MODE_ALIASED);
            dc->DrawBitmap(spectrum_.Get(), spec, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
            dc->PopAxisAlignedClip();
        }
        {
            const float mx = spec.left + h_ / 360.0f * kSpec, my = spec.top + (1 - s_) * kSpec;
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), 6, 6), r.Brush(D2D1::ColorF(0, 0, 0, 0.5f)), 3.0f);
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), 6, 6), r.Brush(D2D1::ColorF(1, 1, 1, 1)), 1.5f);
        }
        // Vorschau
        const D2D1_RECT_F prev = D2D1::RectF(spec.right + 12, spec.top, spec.right + 56, spec.bottom);
        dc->FillRoundedRectangle(D2D1::RoundedRect(prev, 4, 4), r.Brush(Rgb(color)));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(prev, 4, 4), r.Brush(PopupLine()), 1.0f);
        // Helligkeit: senkrecht, oben die volle Farbe, unten Schwarz
        const D2D1_RECT_F val = ValRect();
        hits_.push_back({D2D1::RectF(val.left - 10, val.top - 6, val.right + 10, val.bottom + 6), 2, 0});
        {
            float rr, gg, bb;
            HsvToRgb(h_, s_, 1.0f, rr, gg, bb);
            D2D1_GRADIENT_STOP stops[2] = {{0.0f, D2D1::ColorF(rr, gg, bb, 1)}, {1.0f, D2D1::ColorF(0, 0, 0, 1)}};
            ComPtr<ID2D1GradientStopCollection> sc;
            ComPtr<ID2D1LinearGradientBrush> lb;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, val.top), D2D1::Point2F(0, val.bottom)), sc.Get(), &lb)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(val, 4, 4), lb.Get());
            const float ky = val.top + (1 - v_) * (val.bottom - val.top);
            const D2D1_POINT_2F kc = D2D1::Point2F((val.left + val.right) / 2, ky);
            dc->FillEllipse(D2D1::Ellipse(kc, 7, 7), r.Brush(D2D1::ColorF(1, 1, 1, 1)));
            dc->DrawEllipse(D2D1::Ellipse(kc, 7, 7), r.Brush(D2D1::ColorF(0, 0, 0, 0.35f)), 1.0f);
        }
        // rechts: Hex, RGB/HSV, drei Werte
        const float fx = val.right + 24, fw = 120;
        auto field = [&](int id, float y, const std::wstring& text, const std::wstring& label) {
            D2D1_RECT_F rc = D2D1::RectF(fx, y, fx + fw, y + 32);
            hits_.push_back({rc, 10 + id, 0});
            const bool foc = focus_ == id;
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4),
                                     r.Brush(D2D1::ColorF(1, 1, 1, foc ? 0.04f : (hover_ == 10 + id ? 0.1f : 0.07f))));
            dc->FillRectangle(D2D1::RectF(rc.left + 2, rc.bottom - (foc ? 2.0f : 1.0f), rc.right - 2, rc.bottom),
                              r.Brush(foc ? app_.accent_ : D2D1::ColorF(1, 1, 1, 0.3f)));
            const std::wstring shown = foc ? edit_ : text;
            IDWriteTextFormat* f = r.Text(13);
            DrawStr(dc, r, shown, D2D1::RectF(rc.left + 10, rc.top, rc.right - 6, rc.bottom - 1), f, PopupText());
            if (foc) {   // Schreibmarke am Ende
                const float cx = rc.left + 10 + r.MeasureText(shown, f) + 1;
                dc->FillRectangle(D2D1::RectF(cx, rc.top + 8, cx + 1, rc.bottom - 9), r.Brush(PopupText()));
            }
            if (!label.empty())
                DrawStr(dc, r, label, D2D1::RectF(rc.right + 10, rc.top, kW - kPad, rc.bottom), r.Text(13), PopupText());
        };
        float y = spec.top;
        field(0, y, Hex(color), L"");
        y += 46;
        {   // RGB bzw. HSV (Umschalter)
            D2D1_RECT_F rc = D2D1::RectF(fx, y, fx + fw, y + 32);
            hits_.push_back({rc, 3, 0});
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4), r.Brush(D2D1::ColorF(1, 1, 1, hover_ == 3 ? 0.1f : 0.07f)));
            DrawStr(dc, r, hsv_ ? L"HSV" : L"RGB", D2D1::RectF(rc.left + 10, rc.top, rc.right - 30, rc.bottom), r.Text(13),   // lang:ok
                    PopupText());
            wchar_t g[2] = {0xE70D, 0};
            DrawStr(dc, r, g, D2D1::RectF(rc.right - 28, rc.top, rc.right - 6, rc.bottom), r.Icon(10), PopupDim());
        }
        y += 46;
        for (int i = 0; i < 3; ++i) {
            field(1 + i, y, std::to_wstring(Component(i)), Label(i));
            y += 46;
        }
        // Fertig, Abbrechen
        const float by = kH - kPad - 32, bw = (kW - 3 * kPad) / 2;
        auto button = [&](int id, float x, const std::wstring& text, bool primary) {
            D2D1_RECT_F rc = D2D1::RectF(x, by, x + bw, by + 32);
            hits_.push_back({rc, id, 0});
            D2D1_COLOR_F bg = primary ? app_.accent_ : D2D1::ColorF(1, 1, 1, 0.08f);
            if (hover_ == id) bg = primary ? D2D1::ColorF(std::min(1.0f, bg.r * 1.1f + 0.04f), std::min(1.0f, bg.g * 1.1f + 0.04f),
                                                          std::min(1.0f, bg.b * 1.1f + 0.04f), 1)
                                           : D2D1::ColorF(1, 1, 1, 0.13f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4), r.Brush(bg));
            DrawStr(dc, r, text, rc, r.Text(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER),
                    primary ? g_onAccent : PopupText());
        };
        button(4, kPad, T(L"Fertig"), true);
        button(5, kPad * 2 + bw, T(L"Abbrechen"), false);
    }

    void OnMouse(UINT msg, float x, float y) override {
        if (msg == WM_MOUSEMOVE && drag_) {
            Drag(x, y);
            return;
        }
        int type = 0;
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (x >= it->rc.left && x < it->rc.right && y >= it->rc.top && y < it->rc.bottom) { type = it->type; break; }
        if (msg == WM_MOUSEMOVE) {
            if (type != hover_) { hover_ = type; dirty = true; }
            return;
        }
        if (msg == WM_LBUTTONUP) {
            drag_ = 0;
            if (type == 4 && press_ == 4) Finish();
            else if (type == 5 && press_ == 5) close = true;
            press_ = 0;
            return;
        }
        // WM_LBUTTONDOWN
        press_ = type;
        if (type == 1 || type == 2) {
            Commit();
            drag_ = type;
            Drag(x, y);
        } else if (type >= 10 && type <= 13) {
            Focus(type - 10);
        } else if (type == 3) {
            Commit();
            hsv_ = !hsv_;
            focus_ = -1;
        } else {
            Commit();
        }
        dirty = true;
    }

    void OnKey(WPARAM vk) override {
        if (vk == VK_RETURN) {
            if (focus_ >= 0) { Commit(); focus_ = -1; }
            else Finish();
        } else if (vk == VK_TAB) {
            const int next = focus_ < 0 ? 0 : (focus_ + ((GetKeyState(VK_SHIFT) & 0x8000) ? 3 : 1)) % 4;
            Commit();
            Focus(next);
        } else if (vk == VK_BACK && focus_ >= 0) {
            if (replace_) edit_.clear();
            else if (!edit_.empty()) edit_.pop_back();
            replace_ = false;
            Live();
        } else if ((vk == VK_UP || vk == VK_DOWN) && focus_ >= 1) {   // Wert um 1 aendern
            Commit();
            int v = Component(focus_ - 1) + (vk == VK_UP ? 1 : -1);
            SetComponent(focus_ - 1, v);
            edit_ = std::to_wstring(Component(focus_ - 1));
            replace_ = true;
        }
        dirty = true;
    }

    void OnChar(wchar_t ch) override {
        if (focus_ < 0 || ch < 32) return;
        const bool hexField = focus_ == 0;
        if (hexField ? !(std::iswxdigit(ch) || ch == L'#') : !std::iswdigit(ch)) return;
        if (replace_) edit_.clear();
        replace_ = false;
        if (hexField && ch == L'#') { if (edit_.empty()) edit_ = L"#"; }
        else if (edit_.size() < (hexField ? 7u : 3u)) edit_ += static_cast<wchar_t>(std::towupper(ch));
        Live();
        dirty = true;
    }

private:
    static constexpr float kPad = 16, kSpec = 256, kW = 16 + 256 + 12 + 44 + 16 + 8 + 24 + 120 + 90, kH = 16 + 256 + 16 + 32 + 16;
    struct HitRc { D2D1_RECT_F rc; int type; int a; };

    D2D1_RECT_F SpecRect() const { return D2D1::RectF(kPad, kPad, kPad + kSpec, kPad + kSpec); }
    D2D1_RECT_F ValRect() const {
        const float x = kPad + kSpec + 12 + 44 + 16;
        return D2D1::RectF(x, kPad + 4, x + 8, kPad + kSpec - 4);
    }
    std::wstring Label(int i) const {
        static const wchar_t* rgb[3] = {N_(L"Rot"), N_(L"Grün"), N_(L"Blau")};
        static const wchar_t* hsv[3] = {N_(L"Farbton"), N_(L"Sättigung"), N_(L"Hellwert")};
        return T(hsv_ ? hsv[i] : rgb[i]);
    }
    int Component(int i) const {
        if (hsv_) return i == 0 ? static_cast<int>(std::lround(h_)) % 360 : static_cast<int>(std::lround((i == 1 ? s_ : v_) * 100));
        return static_cast<int>((color >> (16 - 8 * i)) & 0xFF);
    }
    void SetComponent(int i, int v) {
        if (hsv_) {
            if (i == 0) h_ = static_cast<float>(std::clamp(v, 0, 359));
            else if (i == 1) s_ = std::clamp(v, 0, 100) / 100.0f;
            else v_ = std::clamp(v, 0, 100) / 100.0f;
            FromHsv();
        } else {
            const int sh = 16 - 8 * i;
            uint32_t c = (color & ~(0xFFu << sh)) | (static_cast<uint32_t>(std::clamp(v, 0, 255)) << sh);
            SetRgb(c, true);
        }
    }
    void SetRgb(uint32_t c, bool live) {
        color = c & 0xFFFFFF;
        RgbToHsv(((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f, h_, s_, v_);
        if (live) app_.SetAccent(static_cast<int>(color), false);
    }
    void FromHsv() {
        float r, g, b;
        HsvToRgb(h_, s_, v_, r, g, b);
        color = Pack(r, g, b);
        app_.SetAccent(static_cast<int>(color), false);
    }
    void Drag(float x, float y) {
        const D2D1_RECT_F sp = SpecRect(), va = ValRect();
        if (drag_ == 1) {
            h_ = std::clamp((x - sp.left) / kSpec, 0.0f, 0.9999f) * 360.0f;
            s_ = 1.0f - std::clamp((y - sp.top) / kSpec, 0.0f, 1.0f);
        } else {
            v_ = 1.0f - std::clamp((y - va.top) / (va.bottom - va.top), 0.0f, 1.0f);
        }
        FromHsv();
        focus_ = -1;
        dirty = true;
    }
    void Focus(int f) {
        focus_ = f;
        edit_ = f == 0 ? Hex(color) : std::to_wstring(Component(f - 1));
        replace_ = true;   // erste Eingabe ersetzt den Wert
    }
    // waehrend der Eingabe: gueltige Werte sofort uebernehmen
    void Live() {
        if (focus_ == 0) {
            std::wstring h = edit_;
            if (!h.empty() && h[0] == L'#') h.erase(0, 1);
            if (h.size() == 6) SetRgb(static_cast<uint32_t>(wcstoul(h.c_str(), nullptr, 16)), true);
            else if (h.size() == 3) {   // Kurzform #RGB
                uint32_t v = static_cast<uint32_t>(wcstoul(h.c_str(), nullptr, 16));
                uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
                SetRgb((r * 17) << 16 | (g * 17) << 8 | (b * 17), true);
            }
        } else if (focus_ >= 1 && !edit_.empty()) {
            SetComponent(focus_ - 1, _wtoi(edit_.c_str()));
        }
    }
    void Commit() {
        if (focus_ >= 0) Live();
        focus_ = -1;
    }
    void Finish() {
        Commit();
        done = true;
        close = true;
    }

    App& app_;
    ComPtr<ID2D1Bitmap1> spectrum_;
    std::vector<HitRc> hits_;
    float h_ = 0, s_ = 0, v_ = 0;
    bool hsv_ = false;
    int hover_ = 0, press_ = 0, drag_ = 0;
    int focus_ = -1;          // 0 Hex, 1..3 Werte
    std::wstring edit_;
    bool replace_ = false;
};

void App::ShowAccentPanel(const D2D1_RECT_F& rc) {
    const float s = r_.scale();
    POINT pt = {static_cast<LONG>(rc.left * s), static_cast<LONG>((rc.bottom + 4) * s)};
    ContentToScreen(pt);
    AccentPanel panel(*this);
    ShowPopupPanel(hwnd_, panel, pt, accent_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (panel.custom) ShowColorPicker(pt);
}

void App::ShowColorPicker(POINT screen) {
    const int before = settings_.accent;
    ColorPickerPanel panel(*this);
    ShowPopupPanel(hwnd_, panel, screen, accent_);
    if (panel.done) {
        const uint32_t c = panel.color;
        auto& rec = settings_.recentAccents;   // zuletzt verwendet: neueste zuerst, ohne Doppelte und ohne Farben von Windows
        rec.erase(std::remove(rec.begin(), rec.end(), c), rec.end());
        if (std::find(std::begin(kPalette), std::end(kPalette), c) == std::end(kPalette)) rec.insert(rec.begin(), c);
        if (rec.size() > 5) rec.resize(5);
        SetAccent(static_cast<int>(c), true);
    } else {
        SetAccent(before, false);   // Abbrechen: vorige Farbe
    }
}

}  // namespace peek
