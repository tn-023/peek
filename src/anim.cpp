// Animationen (Einstellung "Animationen", Standard aus; aus: alles wie bisher)
//  - Hereingleiten: in Kacheln und Liste gleiten die sichtbaren Elemente nacheinander an ihren Platz
//    (Kacheln von links, Zeilen von unten) und werden dabei eingeblendet; nach hoechstens ~350 ms steht alles.
//  - Hover: verlaesst die Maus ein Element, blendet sein Hover-Effekt in ~110 ms aus (erscheinen: sofort).
#include "app.h"

#include <algorithm>
#include <cmath>

namespace peek {

namespace {
constexpr DWORD kHoverFadeMs = 140;
constexpr double kZoomOpenMs = 200, kZoomCloseMs = 180;
constexpr float kEnterMs = 220, kEnterSpreadMs = 130;   // Dauer je Element, groesster Versatz
constexpr float kEnterStaggerMs = 12;

float FadeAlpha(DWORD t0, DWORD now) {
    float t = static_cast<float>(now - t0) / kHoverFadeMs;
    if (t >= 1) return 0;
    float u = 1 - t;
    return u * u * u;   // steil abfallend: auf langen Listen nur eine kurze Spur, die sanft verklingt
}

float EaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float u = 1 - t;
    return 1 - u * u * u;
}

D2D1_RECT_F LerpRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float k) {
    return D2D1::RectF(a.left + (b.left - a.left) * k, a.top + (b.top - a.top) * k, a.right + (b.right - a.right) * k,
                       a.bottom + (b.bottom - a.bottom) * k);
}

bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return std::fabs(a.left - b.left) < 1.5f && std::fabs(a.top - b.top) < 1.5f && std::fabs(a.right - b.right) < 1.5f &&
           std::fabs(a.bottom - b.bottom) < 1.5f;
}
}  // namespace

float App::Hov(int idx) const {
    if (idx < 0) return 0;
    if (idx == hoverHit_) return 1;
    if (!settings_.animations || hoverFades_.empty() || idx >= static_cast<int>(hits_.size())) return 0;
    const Hit& h = hits_[static_cast<size_t>(idx)];
    const DWORD now = GetTickCount();
    float best = 0;
    for (auto& f : hoverFades_)
        if (f.type == h.type && f.a == h.a && SameRect(f.rc, h.rc)) best = std::max(best, FadeAlpha(f.t0, now));
    return best;
}

float App::PosHov(int pos) const {
    if (pos < 0) return 0;
    if (pos == hoverPos_) return 1;
    if (!settings_.animations) return 0;
    const DWORD now = GetTickCount();
    for (auto& [p, t0] : posFades_)
        if (p == pos) return FadeAlpha(t0, now);
    return 0;
}

// zu Beginn jedes Bildes (hits_ ist noch das vorige): hat das Element unter der Maus gewechselt?
void App::TrackHoverFades() {
    const DWORD now = GetTickCount();
    if (!settings_.animations) {
        hoverFades_.clear();
        posFades_.clear();
        lastHovValid_ = false;
        lastHoverPos_ = hoverPos_;
        return;
    }
    const bool valid = hoverHit_ >= 0 && hoverHit_ < static_cast<int>(hits_.size()) && hits_[hoverHit_].type != HitType::None;
    bool changed = valid != lastHovValid_;
    if (valid && lastHovValid_) {
        const Hit& h = hits_[static_cast<size_t>(hoverHit_)];
        changed = h.type != lastHovType_ || h.a != lastHovA_ || !SameRect(h.rc, lastHovRc_);
    }
    if (changed) {
        if (lastHovValid_) {
            hoverFades_.push_back({lastHovType_, lastHovA_, lastHovRc_, now});
            StartAnimation();
        }
        lastHovValid_ = valid;
        if (valid) {
            const Hit& h = hits_[static_cast<size_t>(hoverHit_)];
            lastHovType_ = h.type;
            lastHovA_ = h.a;
            lastHovRc_ = h.rc;
        }
    }
    if (hoverPos_ != lastHoverPos_) {
        if (lastHoverPos_ >= 0) {
            posFades_.push_back({lastHoverPos_, now});
            StartAnimation();
        }
        lastHoverPos_ = hoverPos_;
    }
    if (hoverFades_.size() > 64) hoverFades_.erase(hoverFades_.begin(), hoverFades_.end() - 64);
    if (posFades_.size() > 64) posFades_.erase(posFades_.begin(), posFades_.end() - 64);
}

// im Animationsschritt: abgelaufene Ausblendungen entfernen, Hereingleiten beenden; true = weiter zeichnen
bool App::AnimFadesStep() {
    const DWORD now = GetTickCount();
    hoverFades_.erase(std::remove_if(hoverFades_.begin(), hoverFades_.end(), [&](const HoverFade& f) { return now - f.t0 >= kHoverFadeMs; }),
                      hoverFades_.end());
    posFades_.erase(std::remove_if(posFades_.begin(), posFades_.end(), [&](const auto& f) { return now - f.second >= kHoverFadeMs; }),
                    posFades_.end());
    bool more = !hoverFades_.empty() || !posFades_.empty();
    if (zoom_.active) {
        if (NowMs() - zoom_.t0 >= (zoom_.closing ? kZoomCloseMs : kZoomOpenMs)) {
            zoom_ = ZoomAnim{};
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else {
            more = true;
        }
    }
    if (enterActive_) {
        if (now - enterT0_ > static_cast<DWORD>(kEnterMs + kEnterSpreadMs + 40)) enterActive_ = false;
        else more = true;
    }
    return more;
}

// Hereingleiten beim naechsten Bild mit Elementen (Ordner geoeffnet, Ansicht gewechselt, neues Suchergebnis)
void App::RequestEnter() {
    if (!settings_.animations || (settings_.viewMode != 0 && settings_.viewMode != 1)) return;
    enterPending_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

float App::EnterProgress(int pos) const {
    if (!enterActive_) return 1;
    const int k = std::max(0, pos - enterFirst_);
    const float stagger = enterCount_ > 1 ? std::min(kEnterStaggerMs, kEnterSpreadMs / (enterCount_ - 1)) : 0;
    const float delay = std::min(k * stagger, kEnterSpreadMs);   // spaeter geladene Elemente: nicht laenger warten
    const float t = (static_cast<float>(GetTickCount() - enterT0_) - delay) / kEnterMs;
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const float u = 1 - t;
    return 1 - u * u * u;   // ease-out
}


// ---------------------------------------------------------------------------
// Bildbetrachter: Vorschaubild <-> Vollbild

D2D1_RECT_F App::DipToScreen(const D2D1_RECT_F& r) const {
    POINT o{0, 0};
    ClientToScreen(hwnd_, &o);
    const float s = r_.scale() > 0 ? r_.scale() : 1.0f;
    return D2D1::RectF(o.x + r.left * s, o.y + r.top * s, o.x + r.right * s, o.y + r.bottom * s);
}

D2D1_RECT_F App::ScreenToDip(const D2D1_RECT_F& r) const {
    POINT o{0, 0};
    ClientToScreen(hwnd_, &o);
    const float s = r_.scale() > 0 ? r_.scale() : 1.0f;
    return D2D1::RectF((r.left - o.x) / s, (r.top - o.y) / s, (r.right - o.x) / s, (r.bottom - o.y) / s);
}

// Oeffnen aus Kacheln oder Liste: erst die Ansicht im naechsten Bild festhalten, dann oeffnen (WM_APP_ZOOMOPEN)
bool App::TryZoomOpen(int pos) {
    if (!settings_.animations || viewer_.open || settingsOpen_ || (settings_.viewMode != 0 && settings_.viewMode != 1)) return false;
    if (pos < 0 || pos >= static_cast<int>(visible_.size())) return false;
    auto it = drawnThumbs_.find(pos);
    if (it == drawnThumbs_.end()) return false;
    const FileEntry& e = entries_[visible_[pos]];
    auto b = bmps_.find(e.path);
    if (b == bmps_.end() || !b->second.bmp) return false;   // ohne Vorschaubild: sofort oeffnen
    zoom_ = ZoomAnim{};
    zoom_.pos = pos;
    zoom_.fromScr = DipToScreen(it->second);
    viewerOpenReq_ = pos;
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

// am Ende eines Bildes der Ordneransicht: Inhalt kopieren
void App::SnapForZoom(ID2D1DeviceContext* dc) {
    if (viewerOpenReq_ < 0) return;
    const int pos = viewerOpenReq_;
    viewerOpenReq_ = -1;
    ComPtr<ID2D1Image> target;
    dc->GetTarget(&target);
    ComPtr<ID2D1Bitmap1> tb;
    if (target) target.As(&tb);
    if (tb) {
        D2D1_SIZE_U px = tb->GetPixelSize();
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), r_.dpi(), r_.dpi());
        if (SUCCEEDED(dc->CreateBitmap(px, nullptr, 0, bp, &zoom_.snap)) && zoom_.snap) {
            dc->Flush();
            if (FAILED(zoom_.snap->CopyFromBitmap(nullptr, tb.Get(), nullptr))) zoom_.snap.Reset();
        }
    }
    zoom_.snapOrigin = POINT{0, 0};
    ClientToScreen(hwnd_, &zoom_.snapOrigin);
    PostMessageW(hwnd_, WM_APP_ZOOMOPEN, static_cast<WPARAM>(pos), 0);
}

void App::ZoomOpenNow(int pos) {
    if (pos < 0 || pos >= static_cast<int>(visible_.size()) || viewer_.open) {
        zoom_ = ZoomAnim{};
        return;
    }
    ZoomAnim keep = zoom_;
    OpenViewer(pos);
    zoom_ = keep;
    zoom_.active = true;
    zoom_.closing = false;
    zoom_.t0 = NowMs();
    StartAnimation();
}

// Oeffnen: festgehaltene Ansicht, darueber Schwarz (blendet ein), das Bild waechst vom Vorschaubild auf seine Groesse
void App::PaintZoomOpen(ID2D1DeviceContext* dc) {
    const float k = EaseOut(static_cast<float>((NowMs() - zoom_.t0) / kZoomOpenMs));
    const float s = r_.scale() > 0 ? r_.scale() : 1.0f;
    if (zoom_.snap) {
        POINT o{0, 0};
        ClientToScreen(hwnd_, &o);
        D2D1_SIZE_U px = zoom_.snap->GetPixelSize();
        float x = (zoom_.snapOrigin.x - o.x) / s, y = (zoom_.snapOrigin.y - o.y) / s;
        dc->DrawBitmap(zoom_.snap.Get(), D2D1::RectF(x, y, x + px.width / s, y + px.height / s), 1.0f,
                       D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(D2D1::ColorF(0, 0, 0, k)));
    if (viewer_.pos < 0 || viewer_.pos >= static_cast<int>(visible_.size())) return;
    const FileEntry& e = entries_[visible_[viewer_.pos]];
    float iw, ih;
    const float sc = ViewerScale(e, iw, ih);
    const float cx = ViewerAreaW() / 2, cy = H_ / 2;
    D2D1_RECT_F to = D2D1::RectF(cx - iw * sc / 2, cy - ih * sc / 2, cx + iw * sc / 2, cy + ih * sc / 2);
    D2D1_RECT_F from = ScreenToDip(zoom_.fromScr);
    ID2D1Bitmap* b = nullptr;
    auto fb = fullBmps_.find(e.path);
    if (fb != fullBmps_.end() && fb->second.bmp) b = fb->second.bmp.Get();
    else if (auto tb = bmps_.find(e.path); tb != bmps_.end() && tb->second.bmp) b = tb->second.bmp.Get();
    if (b) dc->DrawBitmap(b, LerpRect(from, to, k), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
}

// Schliessen (vor dem eigentlichen Schliessen): wo das Bild steht und welches Bild es ist
void App::ZoomCloseBegin() {
    zoom_ = ZoomAnim{};
    if (!settings_.animations || viewer_.slide || viewer_.shell || viewer_.crop || (settings_.viewMode != 0 && settings_.viewMode != 1)) return;
    if (viewer_.pos < 0 || viewer_.pos >= static_cast<int>(visible_.size())) return;
    if (viewer_.rotation != 0) return;
    const FileEntry& e = entries_[visible_[viewer_.pos]];
    auto fb = fullBmps_.find(e.path);
    if (fb != fullBmps_.end() && fb->second.bmp) zoom_.bmp = fb->second.bmp;
    else if (auto tb = bmps_.find(e.path); tb != bmps_.end() && tb->second.bmp) zoom_.bmp = tb->second.bmp;
    if (!zoom_.bmp) return;
    D2D1_RECT_F box = viewer_.imgBox;
    if (box.right - box.left < 2) return;
    zoom_.fromScr = DipToScreen(box);
    zoom_.pos = viewer_.pos;
    zoom_.closing = true;
    zoom_.active = true;
    zoom_.t0 = NowMs();
    StartAnimation();
}

// Schliessen: ueber der Ordneransicht blendet Schwarz aus, das Bild schrumpft auf sein Vorschaubild
void App::PaintZoomOverlay(ID2D1DeviceContext* dc) {
    if (!zoom_.active || !zoom_.closing) return;
    const float k = EaseOut(static_cast<float>((NowMs() - zoom_.t0) / kZoomCloseMs));
    dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(D2D1::ColorF(0, 0, 0, 1 - k)));
    if (!zoom_.bmp) return;
    D2D1_RECT_F from = ScreenToDip(zoom_.fromScr);
    auto it = drawnThumbs_.find(zoom_.pos);
    if (it == drawnThumbs_.end()) {   // Vorschaubild nicht sichtbar: nur ausblenden
        dc->DrawBitmap(zoom_.bmp.Get(), from, 1 - k, D2D1_INTERPOLATION_MODE_LINEAR);
        return;
    }
    dc->DrawBitmap(zoom_.bmp.Get(), LerpRect(from, it->second, k), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
}

}  // namespace peek
