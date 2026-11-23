#include "renderer.h"
#include <cmath>

namespace peek {

static bool FontExists(IDWriteFactory* dw, const wchar_t* family) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(dw->GetSystemFontCollection(&fonts))) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    fonts->FindFamilyName(family, &idx, &exists);
    return exists != FALSE;
}

bool Renderer::Init(HWND hwnd) {
    hwnd_ = hwnd;
    D2D1_FACTORY_OPTIONS opts = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    if (FontExists(dwrite_.Get(), L"Segoe UI Variable Text")) textFamily_ = L"Segoe UI Variable Text";
    if (FontExists(dwrite_.Get(), L"Segoe Fluent Icons")) iconFamily_ = L"Segoe Fluent Icons";
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd));
    if (dpi_ <= 0) dpi_ = 96.0f;
    UpdateTextScale();
    return CreateDevice();
}

bool Renderer::CreateDevice() {
    svgDocs_.clear();
    dc_.Reset(); target_.Reset(); swap_.Reset(); d2dDevice_.Reset(); visual_.Reset(); dcompTarget_.Reset();
    dcomp_.Reset(); dxgiDevice_.Reset(); d3d_.Reset(); brush_.Reset();
    if (frameWait_) { CloseHandle(frameWait_); frameWait_ = nullptr; }
    swapFlags_ = 0;
    waited_ = true;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &d3d_, &got, nullptr);
    if (FAILED(hr))  // Software-Rueckfallebene
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &d3d_, &got, nullptr);
    if (FAILED(hr)) return false;
    maxBitmap_ = got >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
    if (FAILED(d3d_.As(&dxgiDevice_))) return false;
    {
        // nur ein Bild in der Warteschlange: Present taktet genau auf die Bildwiederholung, geringe Verzoegerung
        ComPtr<IDXGIDevice1> d1;
        if (SUCCEEDED(dxgiDevice_.As(&d1))) d1->SetMaximumFrameLatency(1);
    }

    if (FAILED(factory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = std::max<UINT>(1, rc.right - rc.left);
    height_ = std::max<UINT>(1, rc.bottom - rc.top);

    ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory)))) return false;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Scaling = DXGI_SCALING_STRETCH;
    // Wartbare Swapchain: vor dem Zeichnen auf den Takt des Bildschirms warten statt in Present zu blockieren.
    // Eingaben und Animationszeit werden so direkt vor dem Zeichnen gelesen (gleichmaessiger, weniger Verzoegerung).
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (SUCCEEDED(dxgiFactory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr, &swap_))) {
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(swap_.As(&sc2)) && SUCCEEDED(sc2->SetMaximumFrameLatency(1))) frameWait_ = sc2->GetFrameLatencyWaitableObject();
        if (frameWait_) swapFlags_ = desc.Flags;
        else swap_.Reset();
    }
    if (!swap_) {   // Rueckfall: Present wartet wie frueher selbst
        desc.Flags = 0;
        if (FAILED(dxgiFactory->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr, &swap_))) return false;
    }

    if (FAILED(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
    if (FAILED(dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_))) return false;
    if (FAILED(dcomp_->CreateVisual(&visual_))) return false;
    visual_->SetContent(swap_.Get());
    dcompTarget_->SetRoot(visual_.Get());
    dcomp_->Commit();

    dc_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_);
    return CreateTarget();
}

bool Renderer::CreateTarget() {
    dc_->SetTarget(nullptr);
    target_.Reset();
    ComPtr<IDXGISurface> surface;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi(), dpi());
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_))) return false;
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi(), dpi());
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

void Renderer::SetOffset(float xPx, float yPx) {
    if (!visual_ || !dcomp_) return;
    visual_->SetOffsetX(xPx);
    visual_->SetOffsetY(yPx);
    dcomp_->Commit();
}

void Renderer::Resize() {
    if (!swap_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    UINT w = std::max<UINT>(1, rc.right - rc.left), h = std::max<UINT>(1, rc.bottom - rc.top);
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    dc_->SetTarget(nullptr);
    target_.Reset();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags_);
    if (FAILED(hr)) {
        lost_ = true;
        CreateDevice();
        return;
    }
    CreateTarget();
}

void Renderer::SetDpi(float d) {
    dpi_ = d;
    if (dc_) dc_->SetDpi(dpi(), dpi());
    if (swap_) CreateTarget();
}

// Windows-Einstellung "Textgroesse" (100-225 %), wie sie auch Explorer und die Einstellungen-App verwenden
bool Renderer::UpdateTextScale() {
    DWORD v = 100, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor", RRF_RT_REG_DWORD,
                     nullptr, &v, &size) != ERROR_SUCCESS)
        v = 100;
    float f = std::clamp(static_cast<float>(v) / 100.0f, 1.0f, 2.25f);
    if (std::fabs(f - textScale_) < 0.001f) return false;
    textScale_ = f;
    if (dc_) {
        dc_->SetDpi(dpi(), dpi());
        if (swap_) CreateTarget();
    }
    return true;
}

bool Renderer::BeginDraw() {
    if (!dc_ || !target_) {
        if (!CreateDevice()) return false;
        lost_ = true;
    }
    // hoechstens kurz warten: bleibt der Takt aus (Fenster verdeckt), drosselt der Aufrufer selbst
    waited_ = frameWait_ ? WaitForSingleObjectEx(frameWait_, 40, FALSE) == WAIT_OBJECT_0 : true;
    dc_->BeginDraw();
    return true;
}

void Renderer::EndDraw() {
    HRESULT hr = dc_->EndDraw();
    if (SUCCEEDED(hr)) hr = swap_->Present(1, 0);
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        lost_ = true;
        CreateDevice();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

ComPtr<ID2D1Bitmap1> Renderer::CreateBitmap(const Pixels& px) {
    ComPtr<ID2D1Bitmap1> bmp;
    if (!dc_ || !px.Valid() || px.w > maxBitmap_ || px.h > maxBitmap_) return bmp;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc_->CreateBitmap(D2D1::SizeU(px.w, px.h), px.data.data(), px.w * 4, &props, &bmp);
    return bmp;
}

ID2D1SolidColorBrush* Renderer::Brush(const D2D1_COLOR_F& c) {
    brush_->SetColor(c);
    return brush_.Get();
}

IDWriteTextFormat* Renderer::Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a,
                                     bool wrap) {
    uint64_t key = (static_cast<uint64_t>(size * 4) << 32) | (static_cast<uint64_t>(w) << 8) |
                   (static_cast<uint64_t>(a) << 4) | (wrap ? 2u : 0u) | (family == iconFamily_.c_str() ? 1u : 0u);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dwrite_->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         size, L"de-de", &f)))
        return nullptr;
    f->SetTextAlignment(a);
    if (wrap) {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
    } else {
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    formats_[key] = f;
    return f.Get();
}

IDWriteTextFormat* Renderer::TextWrap(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    return Format(textFamily_.c_str(), size, weight, align, true);
}

float Renderer::MeasureWrapped(const std::wstring& s, IDWriteTextFormat* fmt, float width) {
    if (!fmt || s.empty() || width <= 1) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, width, 100000.0f, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

float Renderer::DrawWrapped(const std::wstring& s, float x, float y, float width, IDWriteTextFormat* fmt,
                            const D2D1_COLOR_F& color) {
    if (!fmt || s.empty() || width <= 1) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, width, 100000.0f, &layout)))
        return 0;
    dc_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), Brush(color));
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

float Renderer::TextWidthFast(const std::wstring& s, float size) {
    float w = 0;
    for (wchar_t c : s) w += Advance(c, size);
    return w;
}

float Renderer::Advance(wchar_t c, float size) {
    if (c < 0x3000) {
        if (advFast_.empty()) advFast_.assign(0x3000, -1.0f);
        if (advFast_[c] >= 0) return advFast_[c] * size;
    }
    auto it = advances_.find(c);
    if (it != advances_.end()) return it->second * size;
    float adv = 0.55f;   // Rueckfall (Zeichen aus Ersatzschriften)
    if (!face_) {
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 idx = 0;
        BOOL exists = FALSE;
        ComPtr<IDWriteFontFamily> fam;
        ComPtr<IDWriteFont> font;
        if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts)) &&
            SUCCEEDED(fonts->FindFamilyName(textFamily_.c_str(), &idx, &exists)) && exists &&
            SUCCEEDED(fonts->GetFontFamily(idx, &fam)) &&
            SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                DWRITE_FONT_STYLE_NORMAL, &font)) &&
            SUCCEEDED(font->CreateFontFace(&face_))) {
            DWRITE_FONT_METRICS fm{};
            face_->GetMetrics(&fm);
            unitsPerEm_ = fm.designUnitsPerEm ? fm.designUnitsPerEm : 2048;
        }
    }
    if (face_) {
        UINT32 cp = static_cast<UINT32>(c);
        UINT16 gi = 0;
        if (SUCCEEDED(face_->GetGlyphIndices(&cp, 1, &gi)) && gi != 0) {
            DWRITE_GLYPH_METRICS gm{};
            if (SUCCEEDED(face_->GetDesignGlyphMetrics(&gi, 1, &gm, FALSE)))
                adv = static_cast<float>(gm.advanceWidth) / unitsPerEm_;
        } else if (c >= 0x2E80) {
            adv = 1.0f;   // CJK: volle Breite
        }
    }
    advances_[c] = adv;
    if (c < 0x3000) advFast_[c] = adv;
    return adv * size;
}

void Renderer::BreakLines(const std::wstring& s, float width, float size,
                          std::vector<std::pair<uint32_t, uint32_t>>& lines) {
    lines.clear();
    const float limit = width * 0.96f;   // Sicherheitsabstand gegenueber der echten Textdarstellung
    size_t n = s.size(), start = 0;
    while (start < n) {
        float w = 0;
        size_t i = start, lastBreak = std::wstring::npos;
        while (i < n) {
            float a = Advance(s[i], size);
            if (w + a > limit && i > start) break;
            w += a;
            wchar_t c = s[i];
            if (c == L' ' || c == L'_' || c == L'-' || c == L'.' || c == L',' || c == L')' || c == L']')
                lastBreak = i + 1;   // nach diesen Zeichen umbrechen
            ++i;
        }
        size_t end = i;
        if (i < n && lastBreak != std::wstring::npos && lastBreak > start) end = lastBreak;
        size_t len = end - start;
        // Leerzeichen am Zeilenende nicht mitzaehlen
        size_t trimmed = len;
        while (trimmed > 0 && s[start + trimmed - 1] == L' ') --trimmed;
        lines.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(trimmed ? trimmed : len)});
        start = end;
        while (start < n && s[start] == L' ') ++start;
    }
    if (lines.empty()) lines.push_back({0, 0});
}

namespace {
// SVG fuer Direct2D vorbereiten: CSS-Klassen (.cls-1{fill:...}) als fill-Attribute einsetzen, <style>, <title> und
// data-*-Attribute entfernen (Direct2D kennt keine Stylesheets)
std::string PrepareSvg(std::string s) {
    std::vector<std::pair<std::string, std::string>> cls;
    size_t st = s.find("<style");
    if (st != std::string::npos) {
        size_t en = s.find("</style>", st);
        std::string css = s.substr(st, en == std::string::npos ? std::string::npos : en - st);
        size_t p = 0;
        while ((p = css.find('.', p)) != std::string::npos) {
            size_t ob = css.find('{', p), cb = css.find('}', p);
            if (ob == std::string::npos || cb == std::string::npos || cb < ob) break;
            std::string name = css.substr(p + 1, ob - p - 1);
            std::string body = css.substr(ob + 1, cb - ob - 1);
            size_t f = body.find("fill:");
            if (f != std::string::npos) {
                std::string v = body.substr(f + 5);
                size_t semi = v.find(';');
                if (semi != std::string::npos) v.resize(semi);
                while (!name.empty() && name.back() == ' ') name.pop_back();
                cls.push_back({name, v});
            }
            p = cb + 1;
        }
        if (en != std::string::npos) s.erase(st, en + 8 - st);
    }
    for (auto& c : cls) {
        std::string from = "class=\"" + c.first + "\"", to = "fill=\"" + c.second + "\"";
        for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
    }
    for (const char* tag : {"title"}) {
        std::string o = std::string("<") + tag + ">", e = std::string("</") + tag + ">";
        size_t p = s.find(o);
        if (p != std::string::npos) {
            size_t q = s.find(e, p);
            if (q != std::string::npos) s.erase(p, q + e.size() - p);
        }
    }
    for (size_t p = s.find(" data-"); p != std::string::npos; p = s.find(" data-", p)) {
        size_t q1 = s.find('"', p), q2 = q1 == std::string::npos ? q1 : s.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        s.erase(p, q2 + 1 - p);
    }
    return s;
}
}  // namespace

// SVG aus den Programmressourcen (RCDATA) in rc einpassen (Seitenverhaeltnis bleibt)
void Renderer::DrawSvgResource(int id, const D2D1_RECT_F& rc) {
    if (!dc_) return;
    ComPtr<ID2D1DeviceContext5> dc5;
    if (FAILED(dc_.As(&dc5))) return;
    D2D1_SIZE_F size = D2D1::SizeF(rc.right - rc.left, rc.bottom - rc.top);
    auto& doc = svgDocs_[id];
    if (!doc) {
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
        const char* data = g ? static_cast<const char*>(LockResource(g)) : nullptr;
        if (!data) return;
        std::string svg = PrepareSvg(std::string(data, SizeofResource(nullptr, r)));
        ComPtr<IStream> st;
        st.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(svg.data()), static_cast<UINT>(svg.size())));
        if (!st || FAILED(dc5->CreateSvgDocument(st.Get(), size, &doc))) {
            doc.Reset();
            return;
        }
    } else {
        doc->SetViewportSize(size);
    }
    D2D1_MATRIX_3X2_F old;
    dc_->GetTransform(&old);
    dc_->SetTransform(D2D1::Matrix3x2F::Translation(rc.left, rc.top) * old);
    dc5->DrawSvgDocument(doc.Get());
    dc_->SetTransform(old);
}

IDWriteTextFormat* Renderer::Text(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    return Format(textFamily_.c_str(), size, weight, align);
}

IDWriteTextFormat* Renderer::Icon(float size, DWRITE_TEXT_ALIGNMENT align) {
    return Format(iconFamily_.c_str(), size, DWRITE_FONT_WEIGHT_NORMAL, align);
}

float Renderer::MeasureText(const std::wstring& s, IDWriteTextFormat* fmt) {
    if (!fmt || s.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 10000.0f, 100.0f, &layout)))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float Renderer::Baseline(const std::wstring& s, IDWriteTextFormat* fmt) {
    if (!fmt || s.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt, 10000.0f, 1000.0f, &layout)))
        return 0;
    DWRITE_LINE_METRICS lm{};
    UINT32 n = 0;
    if (FAILED(layout->GetLineMetrics(&lm, 1, &n)) || n == 0) return 0;
    return lm.baseline;
}

} // namespace peek
