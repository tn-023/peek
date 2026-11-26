// Bilder vergleichen: zwei Bilder im Betrachter-Stil, nebeneinander (Zoom und Verschiebung synchron) oder exakt
// uebereinander mit einer Trennlinie am Mauszeiger. Beide Bilder werden in dieselbe Flaeche eingepasst; Zoom 1 =
// eingepasst, so liegen auch Bilder unterschiedlicher Aufloesung deckungsgleich.
#include "app.h"
#include "dragdrop.h"
#include "core/sheetlayout.h"
#include "core/textutil.h"

#include <cmath>

namespace peek {

namespace {
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
constexpr float kBarH = 52;
}  // namespace

void App::OpenCompare(const std::vector<std::wstring>& paths) {
    if (paths.size() != 2) return;
    auto a = byPath_.find(paths[0]), b = byPath_.find(paths[1]);
    if (a == byPath_.end() || b == byPath_.end()) return;
    const FileEntry ea = entries_[a->second], eb = entries_[b->second];
    OpenCompareEntries(ea, eb);
}

void App::OpenCompareEntries(const FileEntry& ea, const FileEntry& eb) {
    auto& c = compare_;
    c.group.clear();
    c.groupPos = -1;
    for (int i = 0; i < 2; ++i) {
        const FileEntry& e = i == 0 ? ea : eb;
        c.path[i] = e.path;
        c.name[i] = e.name;
        c.info[i].clear();
        EnsureThumb(e, 0, frame_);
        if (!fullBmps_.count(e.path)) {   // volle Aufloesung laden
            fullBmps_[e.path].requested = true;
            ThumbRequest r;
            r.key = e.path;
            r.path = e.path;
            r.kind = e.kind;
            r.job = ThumbJob::Full;
            r.prio = 0;
            r.gen = 0;
            thumbs_.Request(std::move(r));
        }
    }
    c.z = c.tz = 1;
    c.pan = c.tpan = D2D1::Point2F(0, 0);
    c.splitX = -1;
    c.dragging = false;
    c.tick = NowMs();
    c.open = true;
    c.mode = 0;
    searchFocus_ = false;
    c.ownFullscreen = false;
    if (settings_.viewerFullscreen) {   // wie der Bildbetrachter: Vollbild oder im Fenster ohne Titelleiste
        if (!fullscreen_) {
            ToggleFullscreen();
            c.ownFullscreen = fullscreen_;
        }
    } else {
        SetCaptionless(true);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseCompare() {
    auto& c = compare_;
    if (!c.open) return;
    c.open = false;
    c.group.clear();
    c.groupPos = -1;
    if (!viewer_.open)
        for (auto& p : c.path) fullBmps_.erase(p);
    if (c.ownFullscreen && fullscreen_) ToggleFullscreen();
    c.ownFullscreen = false;
    if (!viewer_.open) SetCaptionless(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Flaeche, in die Bild i eingepasst wird (nebeneinander: je eine Haelfte, uebereinander: ganzes Fenster)
D2D1_RECT_F App::ComparePane(int i) const {
    if (compare_.mode == 1) return D2D1::RectF(0, kBarH, W_, H_);
    float half = W_ / 2;
    return D2D1::RectF(i * half, kBarH, (i + 1) * half, H_);
}

void App::PaintCompare(ID2D1DeviceContext* dc) {
    auto& c = compare_;
    dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(D2D1::ColorF(0.03f, 0.03f, 0.03f, 1)));

    // weiche Animation von Zoom und Verschiebung
    double now = NowMs();
    float dt = static_cast<float>(std::clamp(now - c.tick, 0.0, 50.0));
    c.tick = now;
    float k = 1.0f - std::exp(-dt / 55.0f);
    bool moving = false;
    auto approach = [&](float& v, float t, float eps) {
        if (std::fabs(t - v) < eps) v = t;
        else { v += (t - v) * k; moving = true; }
    };
    approach(c.z, c.tz, 0.0005f * c.tz);
    approach(c.pan.x, c.tpan.x, 0.05f);
    approach(c.pan.y, c.tpan.y, 0.05f);
    if (moving) InvalidateRect(hwnd_, nullptr, FALSE);

    const float px = r_.scale();
    if (c.splitX < 0) c.splitX = W_ / 2;
    for (int i = 0; i < 2; ++i) {
        Bmp* b = nullptr;
        auto fi = fullBmps_.find(c.path[i]);
        if (fi != fullBmps_.end() && fi->second.bmp) b = &fi->second;
        else {
            auto ti = bmps_.find(c.path[i]);
            if (ti != bmps_.end() && ti->second.bmp) b = &ti->second;
        }
        D2D1_RECT_F pane = ComparePane(i);
        D2D1_RECT_F clip = pane;
        if (c.mode == 1) {   // uebereinander: links Bild 1, rechts Bild 2
            if (i == 0) clip.right = c.splitX;
            else clip.left = c.splitX;
        }
        if (clip.right <= clip.left) continue;
        dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        if (b) {
            D2D1_SIZE_U ps = b->bmp->GetPixelSize();
            // Groesse des Bildes: Originalgroesse, falls bekannt (Vorschaubild hat dasselbe Seitenverhaeltnis)
            float iw = static_cast<float>(b->srcW ? b->srcW : ps.width), ih = static_cast<float>(b->srcH ? b->srcH : ps.height);
            float pw = pane.right - pane.left - 16, ph = pane.bottom - pane.top - 16;
            float fit = std::min(pw / iw, ph / ih);
            float s = fit * c.z;
            float cx = (pane.left + pane.right) / 2 + c.pan.x, cy = (pane.top + pane.bottom) / 2 + c.pan.y;
            D2D1_RECT_F dst = D2D1::RectF(cx - iw * s / 2, cy - ih * s / 2, cx + iw * s / 2, cy + ih * s / 2);
            float screenScale = (dst.right - dst.left) * px / std::max(1u, ps.width);
            D2D1_INTERPOLATION_MODE im = screenScale < 1.0f || settings_.viewerSmooth
                                             ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                             : (screenScale >= 3.0f ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                                                    : D2D1_INTERPOLATION_MODE_LINEAR);
            dc->DrawBitmap(b->bmp.Get(), dst, 1.0f, im);
        } else {
            bool failed = fi != fullBmps_.end() && fi->second.failed;
            DrawText(dc, failed ? T(L"Bild kann nicht angezeigt werden.") : T(L"Wird geladen …"), pane,
                     r_.Text(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::ColorF(1, 1, 1, 0.6f));
        }
        dc->PopAxisAlignedClip();
    }
    if (c.mode == 0) {
        dc->DrawLine(D2D1::Point2F(W_ / 2, kBarH), D2D1::Point2F(W_ / 2, H_), r_.Brush(D2D1::ColorF(1, 1, 1, 0.12f)), 1.0f);
    } else {
        // Trennlinie am Mauszeiger mit Griff
        float x = std::round(c.splitX) + 0.5f;
        dc->DrawLine(D2D1::Point2F(x, kBarH), D2D1::Point2F(x, H_), r_.Brush(D2D1::ColorF(1, 1, 1, 0.9f)), 1.5f);
        float my = (kBarH + H_) / 2;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, my), 15, 15), r_.Brush(D2D1::ColorF(1, 1, 1, 0.92f)));
        wchar_t g[3] = {0xE76B, 0xE76C, 0};
        DrawText(dc, g, D2D1::RectF(x - 15, my - 15, x + 15, my + 15), r_.Icon(9), D2D1::ColorF(0, 0, 0, 0.8f));
    }

    // Leiste oben: je Bild Name und Angaben, rechts Umschalter und Schliessen
    dc->FillRectangle(D2D1::RectF(0, 0, W_, kBarH), r_.Brush(D2D1::ColorF(0, 0, 0, 0.75f)));
    IDWriteTextFormat* fn = r_.Text(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fi = r_.Text(12);
    IDWriteTextFormat* fnR = r_.Text(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    IDWriteTextFormat* fiR = r_.Text(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    const float btnW = 48, rightEnd = W_ - 2 * btnW - 12;
    D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 0.95f), dim = D2D1::ColorF(1, 1, 1, 0.6f);
    auto label = [&](int i, float l, float r, bool trailing) {
        if (r - l < 40) return;
        {   // Angaben: Aufloesung (aus den Metadaten bzw. dem geladenen Bild), Groesse, Typ
            auto it = byPath_.find(c.path[i]);
            std::wstring info;
            uint32_t w = 0, h = 0;
            if (it != byPath_.end()) { w = entries_[it->second].width; h = entries_[it->second].height; }
            auto fb = fullBmps_.find(c.path[i]);
            if ((!w || !h) && fb != fullBmps_.end() && fb->second.bmp) { w = fb->second.srcW; h = fb->second.srcH; }
            if (w && h) info = std::to_wstring(w) + L" \u00d7 " + std::to_wstring(h) + L"  \u00b7  ";
            if (it != byPath_.end()) info += FormatSize(entries_[it->second].size) + L"  \u00b7  " + TypeName(entries_[it->second]);
            c.info[i] = info;
        }
        auto measure = [&](const std::wstring& t) { return r_.MeasureText(t, fn); };
        DrawText(dc, MiddleEllipsis(c.name[i], r - l, measure), D2D1::RectF(l, 7, r, 28), trailing ? fnR : fn, white);
        DrawText(dc, c.info[i], D2D1::RectF(l, 27, r, 46), trailing ? fiR : fi, dim);
    };
    if (c.mode == 0) {
        label(0, 16, W_ / 2 - 16, false);
        label(1, W_ / 2 + 16, rightEnd, false);
    } else {
        label(0, 16, W_ / 2 - 16, false);
        label(1, W_ / 2 + 16, rightEnd, true);
    }
    auto topButton = [&](HitType type, wchar_t glyph, float left, bool danger) {
        D2D1_RECT_F rc = D2D1::RectF(left, 0, left + btnW, 44);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, type, 0});
        if (hoverHit_ == idx) dc->FillRectangle(rc, r_.Brush(danger ? D2D1::ColorF(0.77f, 0.17f, 0.11f, 1) : D2D1::ColorF(1, 1, 1, 0.14f)));
        wchar_t g[2] = {glyph, 0};
        DrawText(dc, g, rc, r_.Icon(13), D2D1::ColorF(1, 1, 1, 1));
    };
    // Symbol zeigt den jeweils anderen Modus: nebeneinander -> uebereinander (Ebenen), sonst -> nebeneinander (Spalten)
    topButton(HitType::CompareMode, c.mode == 0 ? 0xE81E : 0xE8A0, W_ - 2 * btnW, false);
    topButton(HitType::CompareClose, 0xE8BB, W_ - btnW, true);
}

void App::CompareMouseDown(float x, float y) {
    auto& c = compare_;
    if (const Hit* h = HitAt(x, y)) {
        if (h->type == HitType::CompareClose) { CloseCompare(); return; }
        if (h->type == HitType::CompareMode) {
            c.mode = 1 - c.mode;
            c.splitX = W_ / 2;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    if (y < kBarH) return;
    c.dragging = true;
    c.last = D2D1::Point2F(x, y);
}

void App::CompareMouseMove(float x, float y) {
    auto& c = compare_;
    if (c.dragging) {   // verschieben (synchron)
        c.tpan.x += x - c.last.x;
        c.tpan.y += y - c.last.y;
        c.pan = c.tpan;
        c.last = D2D1::Point2F(x, y);
    }
    if (c.mode == 1) c.splitX = std::clamp(x, 0.0f, W_);   // Trennlinie haengt am Mauszeiger
    int hh = -1;
    for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i)
        if (hits_[i].type != HitType::None && Inside(hits_[i].rc, x, y)) { hh = i; break; }
    hoverHit_ = hh;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CompareMouseUp() { compare_.dragging = false; }

void App::CompareZoom(float x, float y, float factor) {
    auto& c = compare_;
    float z1 = std::clamp(c.tz * factor, 0.1f, 64.0f);
    // Punkt unter dem Mauszeiger bleibt stehen (in der Flaeche, in der die Maus steht)
    int pane = c.mode == 0 && x >= W_ / 2 ? 1 : 0;
    D2D1_RECT_F p = ComparePane(pane);
    float ax = x - (p.left + p.right) / 2, ay = y - (p.top + p.bottom) / 2;
    float r = z1 / c.tz;
    c.tpan.x = ax - (ax - c.tpan.x) * r;
    c.tpan.y = ay - (ay - c.tpan.y) * r;
    c.tz = z1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CompareReset() {
    auto& c = compare_;
    c.tz = 1;
    c.tpan = D2D1::Point2F(0, 0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::CompareKey(WPARAM vk) {
    auto& c = compare_;
    switch (vk) {
    case VK_ESCAPE:
    case VK_BACK: CloseCompare(); return true;
    case VK_APPS: {   // Kontextmenue-Taste: Menue fuer das linke bzw. das Bild links der Trennlinie
        POINT pt = {static_cast<LONG>(W_ / 4 * r_.scale()), static_cast<LONG>(H_ / 2 * r_.scale())};
        ContentToScreen(pt);
        ShowCompareMenu(W_ / 4, H_ / 2, pt);
        return true;
    }
    case VK_LEFT:
    case VK_RIGHT:   // aus der Duplikatsuche: rechts durch die Gruppe blaettern
        if (!c.group.empty()) CompareStepGroup(vk == VK_LEFT ? -1 : 1);
        return true;
    case VK_TAB:
    case 'M':
        c.mode = 1 - c.mode;
        c.splitX = W_ / 2;
        break;
    case '0':
    case VK_NUMPAD0: CompareReset(); break;
    case VK_RETURN:
        if (GetKeyState(VK_MENU) < 0) {   // Alt+Enter: Eigenschaften beider Bilder
            ShowProperties({c.path[0], c.path[1]});
            return true;
        }
        break;
    case VK_ADD:
    case VK_OEM_PLUS: CompareZoom(W_ / 2, (kBarH + H_) / 2, 1.25f); break;
    case VK_SUBTRACT:
    case VK_OEM_MINUS: CompareZoom(W_ / 2, (kBarH + H_) / 2, 0.8f); break;
    default: return vk != VK_F4;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

// Kontextmenue fuer das Bild unter dem Mauszeiger (nebeneinander: Haelfte, uebereinander: Seite der Trennlinie)
void App::ShowCompareMenu(float x, float y, POINT screen) {
    auto& c = compare_;
    (void)y;
    int i = c.mode == 0 ? (x >= W_ / 2 ? 1 : 0) : (x >= c.splitX ? 1 : 0);
    auto it = byPath_.find(c.path[i]);
    if (it == byPath_.end()) return;
    const FileEntry e = entries_[it->second];
    auto item = [](UINT id, const std::wstring& text, const std::wstring& right = {}, wchar_t glyph = 0) {
        MenuItem m;
        m.id = id;
        m.text = text;
        m.right = right;
        m.glyph = glyph;
        return m;
    };
    auto openWith = std::make_shared<std::vector<OpenWithApp>>();
    std::vector<MenuItem> items;
    items.push_back(item(2, T(L"Mit Standard-App \u00f6ffnen")));
    items.push_back(OpenWithMenuItem(e, openWith));
    items.push_back(item(3, T(L"Im Explorer zeigen")));
    items.push_back(MenuItem::Sep());
    items.push_back(item(4, T(L"Kopieren")));
    items.push_back(item(5, T(L"Pfad kopieren")));
    items.push_back(MenuItem::Sep());
    items.push_back(item(10, c.mode == 0 ? T(L"\u00dcbereinander vergleichen") : T(L"Nebeneinander vergleichen"), L"Tab"));
    items.push_back(item(11, T(L"Einpassen"), L"0"));
    MenuItem smooth = item(13, T(L"Beim Vergr\u00f6\u00dfern gl\u00e4tten"));
    smooth.checked = settings_.viewerSmooth;
    items.push_back(smooth);
    items.push_back(MenuItem::Sep());
    bool marked = IsBookmarked(e.path);
    items.push_back(item(8, marked ? T(L"Von Merkliste entfernen") : T(L"Zur Merkliste hinzuf\u00fcgen"), L"", marked ? 0xE735 : 0xE734));
    items.push_back(item(6, T(L"In den Papierkorb"), L"", 0xE74D));
    items.push_back(MenuItem::Sep());
    items.push_back(item(14, T(L"Eigenschaften"), L"Alt+Enter"));
    UINT cmd = ShowPopupMenu(hwnd_, items, screen, accent_);
    switch (cmd) {
    case 2: OpenWithDefaultApp(hwnd_, e.path); break;
    case 3: ShowInExplorer(e.path); break;
    case 4: if (CopyFilesToClipboard({e.path})) SetStatus(T(L"In die Zwischenablage kopiert.")); break;
    case 5: CopyTextToClipboard(hwnd_, e.path); SetStatus(T(L"Pfad kopiert.")); break;
    case 6:   // Bild loeschen: Vergleich endet
        if (RecycleFiles(hwnd_, {e.path})) {
            CloseCompare();
            DupForget({e.path});   // aus der Duplikatsuche geoeffnet: dort ebenfalls entfernen
            QuietReload();
        }
        break;
    case 8: SetBookmarked({e.path}, !marked); break;
    case 10:
        c.mode = 1 - c.mode;
        c.splitX = W_ / 2;
        break;
    case 11: CompareReset(); break;
    case 13: settings_.viewerSmooth = !settings_.viewerSmooth; break;
    case 14: ShowProperties({e.path}); break;
    default:
        if (cmd == 999 || cmd >= 1000) RunOpenWith(e, *openWith, cmd, true);
        break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

} // namespace peek
