// Animationen (Einstellung "Animationen", Standard aus; aus: alles wie bisher)
//  - Hereingleiten: in Kacheln und Liste gleiten die sichtbaren Elemente nacheinander an ihren Platz
//    (Kacheln von links, Zeilen von unten) und werden dabei eingeblendet; nach hoechstens ~350 ms steht alles.
//  - Hover: verlaesst die Maus ein Element, blendet sein Hover-Effekt in ~110 ms aus (erscheinen: sofort).
#include "app.h"

#include <algorithm>
#include <cmath>

namespace peek {

namespace {
constexpr DWORD kHoverFadeMs = 110;
constexpr float kEnterMs = 220, kEnterSpreadMs = 130;   // Dauer je Element, groesster Versatz
constexpr float kEnterStaggerMs = 12;

float FadeAlpha(DWORD t0, DWORD now) {
    float t = static_cast<float>(now - t0) / kHoverFadeMs;
    if (t >= 1) return 0;
    float u = 1 - t;
    return u * u;   // schnell weg, sanft am Ende
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

}  // namespace peek
