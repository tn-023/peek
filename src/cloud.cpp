// Bilderwolke: alle Elemente der aktuellen Ansicht als raeumlicher Haufen.
// Jede Karte liegt flach (parallel zum Bildschirm) in einer eigenen Tiefe; die Sortierung bestimmt, was oben liegt.
// Die Projektion ist echte Perspektive (Kamera mit Brennweite), gezeichnet wird mit Direct2D auf der GPU:
// Vorschaubilder liegen in Atlas-Texturen (64 px und 16 px je Karte) und werden als Sprite-Batches gezeichnet,
// nahe Karten direkt aus dem Vorschaubild. Abdunklung nach Tiefe, Schatten und Platzhalter sind ebenfalls Sprites.
#include "app.h"
#include "core/sheetlayout.h"
#include "core/textutil.h"

#include <cmath>

namespace peek {

namespace {

constexpr float kTanHalfFov = 0.41421356f;   // 45 Grad Bildwinkel
constexpr UINT kPage = 2048;
constexpr UINT kCellS = 64, kCellXS = 16;
constexpr int kRowS = kPage / kCellS, kRowXS = kPage / kCellXS;
constexpr int kPerS = kRowS * kRowS - 2, kPerXS = kRowXS * kRowXS - 2;   // Zellen 0 und 1: Weiss und Schatten
constexpr float kNear = 0.08f;
constexpr float kRound = 0.05f;    // Eckenradius relativ zur Kartenseite (hoechstens 5 DIP wie die Buttons)

bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

uint64_t Hash(const std::wstring& s) {
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(towlower(c));
        h *= 1099511628211ull;
    }
    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 32;
    return h;
}

D2D1_RECT_U CellRect(int cellIndex, UINT cell, int perRow, UINT inset) {
    UINT cx = static_cast<UINT>(cellIndex % perRow) * cell, cy = static_cast<UINT>(cellIndex / perRow) * cell;
    return D2D1::RectU(cx + inset, cy + inset, cx + cell - inset, cy + cell - inset);
}
D2D1_RECT_F ToF(const D2D1_RECT_U& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom));
}

std::wstring DateText(int64_t unix) {
    if (unix <= 0) return {};
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(unix) * 10000000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart}, lft;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    return FormatDayTime(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

float Approach(float& v, float target, float k, float eps) {
    float d = target - v;
    if (std::fabs(d) < eps) { v = target; return 0; }
    v += d * k;
    return 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Ansicht wechseln (Seitenleiste und Infobereich werden je Ansicht gemerkt)

void App::SetViewMode(int mode) {
    mode = std::clamp(mode, 0, 3);
    int old = settings_.viewMode;
    if (mode == old) return;
    settings_.sidebarView[old] = settings_.sidebar;
    settings_.infoView[old] = settings_.infoPanel;
    settings_.sidebar = settings_.sidebarView[mode];
    settings_.infoPanel = settings_.infoView[mode];
    if (mode == 3) {
        tl_.version = ~0ull;
        tl_.scrub = tl_.pan = false;
    }
    if (mode == 2) {
        cloud_.version = ~0ull;
        cloud_.camInit = false;
        cloud_.spread = 1;   // immer mit dem Standardabstand beginnen
    } else if (old == 2) {
        CloudReleaseGpu();
        cloud_.cards.clear();
        cloud_.index.clear();
        cloud_.order.clear();
        cloud_.drawn.clear();
        cloud_.freeSlots.clear();
        cloud_.nextSlot = 0;
        cloud_.sel = cloud_.hover = cloud_.block = -1;
        cloud_.version = ~0ull;
    }
    settings_.viewMode = mode;
    hoverPos_ = -1;
    Layout();
    if (mode != 2 && focus_ >= 0) ScrollToPos(focus_);
    if (mode == 3 || old == 3) Refilter();   // Zeitleiste braucht die Aufnahmedaten (NeedsMeta)
    StartAnimation();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Anordnung

static float CardAspect(const FileEntry* e, const Bmp* b) {
    float a = 0;
    if (b && b->bmp && b->srcW && b->srcH) a = static_cast<float>(b->srcW) / b->srcH;
    else if (e && e->IsMedia() && e->width && e->height) a = static_cast<float>(e->width) / e->height;
    else if (e && e->HasThumb() && !e->offline) a = 1.5f;
    else a = 0.78f;   // Karte mit Dateisymbol
    return std::clamp(a, 0.3f, 3.2f);
}

void App::CloudTargets() {
    auto& c = cloud_;
    c.allActive = true;   // neue Ziele: alle Karten einmal pruefen
    c.grid.valid = false;
    const int n = static_cast<int>(c.order.size());
    float vw = std::max(50.0f, c.view.right - c.view.left), vh = std::max(50.0f, c.view.bottom - c.view.top);
    float va = std::clamp(vw / vh, 1.0f, 2.4f);
    c.va = vw / vh;
    c.R = 0.38f * std::sqrt(static_cast<float>(std::max(1, n)));
    c.D = 0.3f + 0.5f * c.R;
    c.strips.clear();
    if (c.groupKey >= 0 && n > 0) {
        // Gruppiert: nebeneinander liegende senkrechte Streifen, Breite nach Anzahl (gleiche Dichte wie die runde
        // Wolke). Jeder Streifen beginnt vorne mit dem ersten Element seiner Gruppe.
        std::vector<CloudGroup> groups = CloudGroups();
        const float kA = 0.4536f;   // Flaeche je Karte wie in der runden Wolke (pi * 0.38^2)
        const float gap = kCloudGap, wMin = 1.8f;
        const int g = static_cast<int>(groups.size());
        const float A = kA * n, G = gap * (g - 1);
        const float sa = std::clamp(vw / vh, 1.0f, 3.0f);
        float H = std::max(1.8f, (G + std::sqrt(G * G + 4 * sa * A)) / (2 * sa));
        // Mindestbreite: Titel und Anzahl muessen in der Gesamtansicht ganz lesbar sein. Die Breite in DIP haengt
        // vom Kameraabstand ab, der wiederum von der Gesamtbreite: einige Durchgaenge genuegen.
        IDWriteTextFormat* gf = r_.Text(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        IDWriteTextFormat* cf = r_.Text(12);
        std::vector<float> needPx(g), ws(g), wNeed(g, wMin);
        for (int j = 0; j < g; ++j)
            needPx[j] = r_.MeasureText(groups[j].label, gf) + 10 +
                        r_.MeasureText(FormatThousands(static_cast<uint64_t>(groups[j].count)), cf) + 40;
        const float fl = (vh / 2) / kTanHalfFov;
        float W = G;
        for (int it = 0; it < 8; ++it) {
            W = G;
            for (int j = 0; j < g; ++j) {
                ws[j] = std::max(wNeed[j], kA * groups[j].count / H);
                W += ws[j];
            }
            float fit = std::max((H / 2 + 0.7f) * fl / (vh / 2 * 0.94f), (W / 2 + 0.7f) * fl / (vw / 2 * 0.94f));
            float px = fl / fit;   // DIP je Welteinheit in der Gesamtansicht
            bool grown = false;
            for (int j = 0; j < g; ++j) {
                float w = needPx[j] / px - gap;
                if (w > ws[j] + 0.01f) { wNeed[j] = w; grown = true; }
            }
            if (!grown) break;
        }
        float x = -W / 2;
        c.D = 0.3f;
        for (int j = 0; j < g; ++j) {
            CloudStrip st;
            st.label = groups[j].label;
            st.count = groups[j].count;
            st.x0 = x;
            st.x1 = x + ws[j];
            x = st.x1 + gap;
            const int m = groups[j].count;
            const float Dg = 0.3f + 0.19f * std::sqrt(static_cast<float>(m));
            c.D = std::max(c.D, Dg);
            for (int q = 0; q < m; ++q) {
                int i = groups[j].start + q;
                if (i < 0 || i >= n) break;
                CloudCard& k = c.cards[c.order[i]];
                uint64_t h = Hash(k.path);
                float u1 = static_cast<float>(h & 0xFFFFFF) / 16777216.0f;
                float u2 = static_cast<float>((h >> 24) & 0xFFFFFF) / 16777216.0f;
                float u3 = static_cast<float>((h >> 48) & 0xFFFF) / 65536.0f;
                auto it = byPath_.find(k.path);
                const FileEntry* e = it != byPath_.end() ? &entries_[it->second] : nullptr;
                auto bi = bmps_.find(k.path);
                float a = CardAspect(e, bi != bmps_.end() ? &bi->second : nullptr);
                k.tw = std::sqrt(a);
                k.th = 1.0f / std::sqrt(a);
                float mx = std::min(k.tw / 2, ws[j] / 2), my = std::min(k.th / 2, H / 2);
                k.tx = st.x0 + mx + u1 * (ws[j] - 2 * mx);
                k.ty = -H / 2 + my + u2 * (H - 2 * my);
                float step = m > 1 ? Dg / (m - 1) : 0;
                k.tz = m > 1 ? Dg * q / (m - 1) + (u3 - 0.5f) * step * 0.6f : 0;
            }
            c.strips.push_back(std::move(st));
        }
        c.stripH = H;
        c.R = std::max(W, H) / 2;
        float f = (vh / 2) / kTanHalfFov;
        c.fit = std::max((H / 2 + 0.7f) * f / (vh / 2 * 0.94f), (W / 2 + 0.7f) * f / (vw / 2 * 0.94f));
        ApplySpread();
        return;
    }
    float sva = std::sqrt(va);
    for (int i = 0; i < n; ++i) {
        CloudCard& k = c.cards[c.order[i]];
        uint64_t h = Hash(k.path);
        float u1 = static_cast<float>(h & 0xFFFFFF) / 16777216.0f;
        float u2 = static_cast<float>((h >> 24) & 0xFFFFFF) / 16777216.0f;
        float u3 = static_cast<float>((h >> 48) & 0xFFFF) / 65536.0f;
        float r = c.R * std::sqrt(u1), th = 6.2831853f * u2;
        k.tx = r * std::cos(th) * sva;
        k.ty = r * std::sin(th) / sva;
        float step = n > 1 ? c.D / (n - 1) : 0;
        k.tz = n > 1 ? c.D * i / (n - 1) + (u3 - 0.5f) * step * 0.6f : 0;
        auto it = byPath_.find(k.path);
        const FileEntry* e = it != byPath_.end() ? &entries_[it->second] : nullptr;
        auto bi = bmps_.find(k.path);
        float a = CardAspect(e, bi != bmps_.end() ? &bi->second : nullptr);
        k.tw = std::sqrt(a);
        k.th = 1.0f / std::sqrt(a);
    }
    // Abstand, bei dem der ganze Haufen in die Flaeche passt
    float halfW = c.R * sva + 0.7f, halfH = c.R / sva + 0.7f;
    float f = (vh / 2) / kTanHalfFov;
    c.fit = std::max(halfH * f / (vh / 2 * 0.94f), halfW * f / (vw / 2 * 0.94f));
    ApplySpread();
}

// Abstand der Karten: alle Lagen (auch die Tiefe) und die Masse des Haufens strecken, die Karten selbst bleiben
// gleich gross. Aus Sicht der Kamera werden so die Luecken groesser bzw. kleiner.
void App::ApplySpread() {
    auto& c = cloud_;
    const float s = c.spread;
    if (s == 1.0f) return;
    for (int ci : c.order) {
        CloudCard& k = c.cards[ci];
        k.tx *= s;
        k.ty *= s;
        k.tz *= s;
    }
    for (auto& st : c.strips) {
        st.x0 *= s;
        st.x1 *= s;
    }
    c.stripH *= s;
    c.R *= s;
    c.D *= s;
    c.fit *= s;
}

void App::CloudSetSpread(float spread) {
    auto& c = cloud_;
    spread = std::clamp(spread, 0.7f, 4.0f);
    if (std::fabs(spread - c.spread) < 1e-4f || c.order.empty()) return;
    const float r = spread / c.spread;
    c.spread = spread;
    CloudTargets();
    // Kamera mitstrecken: der Blick bleibt auf derselben Stelle des Haufens
    c.tcx *= r;
    c.tcy *= r;
    c.tcz *= r;
    c.tau = 230;   // wie die Karten
    StartAnimation();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Pfeiltasten: Kamera seitlich bzw. senkrecht verschieben, um einen Anteil der sichtbaren Breite
void App::CloudPan(float fx, float fy) {
    auto& c = cloud_;
    float vw = std::max(50.0f, c.view.right - c.view.left), vh = std::max(50.0f, c.view.bottom - c.view.top);
    float f = (vh / 2) / kTanHalfFov;
    float d = std::max(1.0f, c.focusZ - c.tcz);   // Abstand zur betrachteten Ebene
    float halfW = vw / 2 / f * d, halfH = vh / 2 / f * d;
    float lim = c.R + 1.0f;
    c.tcx = std::clamp(c.tcx + fx * halfW * 0.35f, -lim, lim);
    c.tcy = std::clamp(c.tcy + fy * halfH * 0.35f, -lim, lim);
    c.tau = 120;
    c.camUser = true;
    c.haveBefore = false;
    StartAnimation();
}

void App::CloudFit(bool immediate) {
    auto& c = cloud_;
    c.tcx = c.tcy = 0;
    c.tcz = -c.fit;
    if (pendingCloudZoom_ > 0) {   // nur Dokumentation (--cloud-zoom): Kamera naeher am Haufen
        c.tcz = -c.fit * pendingCloudZoom_;
        c.tcx = c.R * 0.15f;
    }
    c.tau = 220;
    if (immediate) {
        c.cx = c.tcx;
        c.cy = c.tcy;
        c.cz = c.tcz;
    }
}

void App::CloudSync() {
    auto& c = cloud_;
    c.view = gridRc_;
    const int gk = CloudGroupKey();
    if (gk >= 0) c.view.top += 8 + kGroupH;   // oben: Leiste mit den Gruppentiteln (Abstand wie im Raster)
    std::wstring key = path_ + (bookmarksView_ ? L"|b" : L"") + (collectionsView_ ? L"|c" : L"");
    if (key != c.folder) {
        // anderer Ordner: neu aufbauen (die Karten fliegen von hinten herein)
        c.folder = key;
        c.cards.clear();
        c.index.clear();
        c.order.clear();
        c.drawn.clear();
        c.freeSlots.clear();
        c.nextSlot = 0;
        c.sel = c.hover = c.block = -1;
        c.haveBefore = false;
        c.camInit = false;
        c.camUser = false;
        c.version = ~0ull;
    }
    float vw = c.view.right - c.view.left, vh = std::max(1.0f, c.view.bottom - c.view.top);
    bool resized = c.va > 0 && std::fabs(vw / vh - c.va) > 0.04f;
    const bool regroup = gk != c.groupKey;
    if (c.version == visibleVersion_ && !resized && !regroup) return;
    c.version = visibleVersion_;
    c.groupKey = gk;

    std::vector<char> seen(c.cards.size(), 0);
    std::vector<int> fresh;
    const size_t prevCount = c.order.size();
    c.order.assign(visible_.size(), -1);
    for (size_t p = 0; p < visible_.size(); ++p) {
        const FileEntry& e = entries_[visible_[p]];
        auto it = c.index.find(e.path);
        int idx;
        if (it == c.index.end()) {
            idx = static_cast<int>(c.cards.size());
            CloudCard k;
            k.path = e.path;
            if (!c.freeSlots.empty()) { k.slot = c.freeSlots.back(); c.freeSlots.pop_back(); }
            else k.slot = c.nextSlot++;
            c.cards.push_back(std::move(k));
            seen.push_back(1);
            c.index[e.path] = idx;
            fresh.push_back(idx);
        } else {
            idx = it->second;
            seen[idx] = 1;
        }
        CloudCard& k = c.cards[idx];
        k.rank = static_cast<int>(p);
        k.leaving = false;
        k.ta = 1;
        c.order[p] = idx;
    }
    CloudTargets();
    for (size_t i = 0; i < c.cards.size(); ++i) {
        CloudCard& k = c.cards[i];
        if (seen[i]) continue;
        if (!k.leaving) {   // herausgefiltert: nach hinten sinken und ausblenden
            k.leaving = true;
            k.ta = 0;
            k.tz = k.z + 0.8f + c.D * 0.15f;
        }
    }
    for (int idx : fresh) {   // neu: von hinten heranfliegen
        CloudCard& k = c.cards[idx];
        k.x = k.tx;
        k.y = k.ty;
        k.z = k.tz + c.D * 0.35f + 0.8f;
        k.w = k.tw;
        k.h = k.th;
        k.a = 0;
    }
    if (c.sel >= 0 && (c.sel >= static_cast<int>(c.cards.size()) || c.cards[c.sel].leaving)) {
        c.sel = -1;
        c.haveBefore = false;
    }
    if (!c.camInit) {
        CloudFit(true);
        c.camInit = true;
        c.camUser = false;
        c.focusZ = 0;
    } else if (visible_.size() != prevCount || regroup) {
        // Filter oder Suche haben die Anzahl geaendert: Kamera wieder auf den ganzen Haufen
        c.sel = -1;
        c.haveBefore = false;
        c.camUser = false;
        CloudFit(false);
    } else if (!c.camUser && c.sel < 0) {
        CloudFit(false);
    }
    c.needRepack = true;   // neue Reihenfolge: Atlas bei Bedarf passend umsortieren
    StartAnimation();
}

// ---------------------------------------------------------------------------
// Animation

// ---------------------------------------------------------------------------
// Vorfuehrung (--cloud-tour): feste Kamerafahrt fuer die Aufnahme im README.
// Querfahrt (Parallaxe), leichte Fahrt hinein, ein vorderes Bild waehlen und wieder abwaehlen,
// ein weiter hinten liegendes Bild waehlen und wieder abwaehlen.

int App::CloudTourPick(float zFrom, float zTo) const {
    const auto& c = cloud_;
    int best = -1;
    float bestD = 1e30f;
    for (int ci : c.order) {
        const CloudCard& k = c.cards[ci];
        if (k.leaving || k.tz < c.D * zFrom || k.tz > c.D * zTo) continue;
        float dx = k.tx - c.tcx, dy = k.ty - c.tcy;
        float d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = ci; }
    }
    return best;
}

bool App::CloudTourStep(float dt) {
    auto& c = cloud_;
    if (!cloudTour_ || c.order.empty()) return false;
    if (cloudTourT_ < 0) {
        // erst warten, bis die Karten eingeflogen und die Vorschaubilder geladen sind
        cloudTourWait_ += dt;
        if (cloudTourWait_ < 4000) return true;
        cloudTourT_ = 0;
        cloudTourPose_[0] = -0.5f * c.R;
        cloudTourPose_[1] = 0.04f * c.R;
        cloudTourPose_[2] = -0.62f * c.fit;
        c.cx = c.tcx = cloudTourPose_[0];
        c.cy = c.tcy = cloudTourPose_[1];
        c.cz = c.tcz = cloudTourPose_[2];
        c.camUser = true;
        c.sel = -1;
        c.haveBefore = false;
        if (!cloudTourMark_.empty()) {   // bereit: Markerdatei anlegen, dann auf "<Marker>.go" warten (Aufnahme laeuft)
            HANDLE h = CreateFileW(cloudTourMark_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        return true;
    }
    if (cloudTourT_ == 0 && !cloudTourMark_.empty() &&
        GetFileAttributesW((cloudTourMark_ + L".go").c_str()) == INVALID_FILE_ATTRIBUTES) {
        cloudTourWait_ += dt;
        if (cloudTourWait_ < 64000) return true;   // spaetestens nach einer Minute ohne Startzeichen weiter
    }
    if (cloudTourStart_ < 0) cloudTourStart_ = NowMs();
    const float t0 = cloudTourT_;
    cloudTourT_ = static_cast<float>(NowMs() - cloudTourStart_);
    const float t = cloudTourT_;
    auto at = [&](float ms) { return t0 < ms && t >= ms; };
    auto ease = [](float u) { u = std::clamp(u, 0.0f, 1.0f); return u * u * (3 - 2 * u); };
    const float x0 = cloudTourPose_[0], y0 = cloudTourPose_[1], z0 = cloudTourPose_[2];
    const float x1 = 0.42f * c.R, z1 = -0.36f * c.fit, x2 = 0.32f * c.R;
    if (t < 4600) {
        // Querfahrt: Kamera folgt einer weichen Bahn, nahe Karten ziehen schneller vorbei als ferne
        float u = ease((t - 600) / 4000.0f);
        c.tcx = x0 + (x1 - x0) * u;
        c.tcy = y0;
        c.tcz = z0;
        c.tau = 45;
    } else if (t < 6400) {
        // leichte Fahrt hinein
        float u = ease((t - 4600) / 1800.0f);
        c.tcx = x1 + (x2 - x1) * u;
        c.tcz = z0 + (z1 - z0) * u;
        c.tau = 60;
    }
    if (at(6900)) CloudSelect(CloudTourPick(0.0f, 0.22f));
    if (at(9700)) CloudDeselect();
    if (at(11400)) CloudSelect(CloudTourPick(0.45f, 0.72f));
    if (at(14600)) CloudDeselect();
    if (t > 16800) {
        cloudTour_ = false;
        return false;
    }
    return true;
}

bool App::CloudStep(float dt) {
    auto& c = cloud_;
    bool more = CloudTourStep(dt);
    float kc = 1.0f - std::exp(-dt / c.tau);
    const float tol = 0.0002f;   // fest: tief im Haufen sind nahe Karten gross, jedes Einrasten waere sichtbar
    if (Approach(c.cx, c.tcx, kc, tol)) more = true;
    if (Approach(c.cy, c.tcy, kc, tol)) more = true;
    if (Approach(c.cz, c.tcz, kc, tol)) more = true;
    float fz = c.sel >= 0 ? c.cards[c.sel].tz : 0.0f;
    if (Approach(c.focusZ, fz, kc, 0.001f)) more = true;
    {
        float ks = 1.0f - std::exp(-dt / 90.0f);
        if (Approach(c.selAnim, c.sel >= 0 ? 1.0f : 0.0f, ks, 0.002f)) more = true;
    }

    // Neue Ziele oder geaenderte Auswahl: alle Karten pruefen; sonst nur die, die sich noch bewegen
    if (c.sel != c.stepSel) {
        c.stepSel = c.sel;
        c.allActive = true;
    }
    // Raster, sobald keine Karte mehr ihre Lage aendert (Ein-/Ausblenden und Ausweichen vor der Auswahl duerfen
    // weiterlaufen: sie verschieben nichts im Raster)
    if (!c.grid.valid && !c.allActive && !c.posMoving) CloudBuildGrid();
    const bool fast = c.grid.valid;

    // Karte in der Bildmitte (vorderste, die die Mitte ueberdeckt; mit Beharrung gegen Flackern):
    // sie weicht nie aus, die Kamera stoesst dagegen
    const float vw = std::max(50.0f, c.view.right - c.view.left), vh = std::max(50.0f, c.view.bottom - c.view.top);
    {
        auto covers = [&](int ci, float margin) {
            const CloudCard& k = c.cards[ci];
            float d = k.z - c.cz;
            if (d < kNear || k.leaving || k.a < 0.5f) return false;
            float lx = std::fabs(k.x - c.cx), ly = std::fabs(k.y - c.cy);
            return lx < k.w / 2 * margin && ly < k.h / 2 * margin;
        };
        int best = -1;
        float bestD = 1e30f;
        if (fast) {
            // nur die Zellen um den Kamerastrahl; je Zelle nach Tiefe sortiert: die erste ueberdeckende ist die vorderste
            const auto& g = c.grid;
            int gx0 = std::clamp(static_cast<int>(std::floor((c.cx - g.maxHalf - g.x0) / g.cs)), 0, g.nx - 1);
            int gx1 = std::clamp(static_cast<int>(std::floor((c.cx + g.maxHalf - g.x0) / g.cs)), 0, g.nx - 1);
            int gy0 = std::clamp(static_cast<int>(std::floor((c.cy - g.maxHalf - g.y0) / g.cs)), 0, g.ny - 1);
            int gy1 = std::clamp(static_cast<int>(std::floor((c.cy + g.maxHalf - g.y0) / g.cs)), 0, g.ny - 1);
            for (int gy = gy0; gy <= gy1; ++gy)
                for (int gx = gx0; gx <= gx1; ++gx) {
                    int cell = gy * g.nx + gx, s0 = g.start[cell], s1 = g.start[cell + 1];
                    int i = static_cast<int>(std::lower_bound(g.itemZ.begin() + s0, g.itemZ.begin() + s1, c.cz + kNear) - g.itemZ.begin());
                    for (; i < s1; ++i) {
                        float d = g.itemZ[i] - c.cz;
                        if (d >= bestD) break;
                        if (covers(g.items[i], 1.0f)) { bestD = d; best = g.items[i]; break; }
                    }
                }
        } else {
            for (int ci : c.order) {
                if (!covers(ci, 1.0f)) continue;
                float d = c.cards[ci].z - c.cz;
                if (d < bestD) { bestD = d; best = ci; }
            }
        }
        if (c.block >= 0 && c.block < static_cast<int>(c.cards.size()) && covers(c.block, 1.25f) &&
            (best < 0 || c.cards[c.block].z - c.cz <= bestD + 0.1f))
            best = c.block;
        c.block = best;
    }
    // Ausweichen als perspektivische Spreizung: nahe der Kamera ruecken Karten proportional zu ihrem seitlichen
    // Abstand vom Kamerastrahl nach aussen. In der Bildmitte bewegt sich praktisch nichts, am Rand gleiten die Karten
    // unauffaellig zur Seite. Kein Umschalten zwischen "Mitte" und "Rand", keine Verzoegerung.
    auto evade = [&](int ci) {
        CloudCard& k = c.cards[ci];
        float d = k.z - c.cz;
        float lx = k.x - c.cx, ly = k.y - c.cy;
        float size = std::max(k.w, k.h);
        float start = size * 5.5f;
        float pr = std::clamp((start - d) / std::max(0.01f, start - kNear), 0.0f, 1.0f);
        float g = 2.0f * pr * pr;   // weich zunehmend
        // kurz vor der Kamera zusaetzlich spreizen, damit nichts an der Nahgrenze ploetzlich verschwindet
        float nearP = std::clamp(1.0f - (d - kNear) / (0.7f * size), 0.0f, 1.0f);
        g += 6.0f * nearP * nearP;
        float ex = lx * g, ey = ly * g;
        // vor dem ausgewaehlten Bild: aus dem Bild gleiten (gesteuert ueber fade, das weich animiert wird)
        if (k.fade < 1.0f && ci != c.sel) {
            if (k.evAng < 0) k.evAng = static_cast<float>(Hash(k.path) >> 40) / 16777216.0f * 6.2831853f;
            float lat = std::sqrt(lx * lx + ly * ly);
            float dx = lat > 0.05f ? lx / lat : std::cos(k.evAng), dy = lat > 0.05f ? ly / lat : std::sin(k.evAng);
            float out = std::max(0.0f, d) * kTanHalfFov * std::max(1.0f, vw / vh) + size * 0.6f;
            float push = std::max(0.0f, out - lat) * (1.0f - k.fade);
            ex += dx * push;
            ey += dy * push;
        }
        k.ex = ex;
        k.ey = ey;
    };
    if (fast && c.sel < 0 && !c.fading) {
        // Ruhezustand ohne Auswahl: der Versatz ist nur nahe der Kamera ungleich null (bis 5,5 Kartengroessen davor).
        // Nur diese Tiefenscheibe rechnen; Karten, die sie verlassen haben, auf null setzen.
        const auto& g = c.grid;
        float zLo = c.cz - 2.5f * g.maxSize, zHi = c.cz + 5.5f * g.maxSize + 0.01f;
        size_t i0 = std::lower_bound(g.byZz.begin(), g.byZz.end(), zLo) - g.byZz.begin();
        size_t i1 = std::upper_bound(g.byZz.begin(), g.byZz.end(), zHi) - g.byZz.begin();
        ++c.stamp;
        if (c.visStamp.size() != c.cards.size()) c.visStamp.assign(c.cards.size(), 0);
        std::vector<int> now;
        now.reserve(i1 - i0);
        for (size_t i = i0; i < i1; ++i) {
            int ci = g.byZ[i];
            evade(ci);
            c.visStamp[ci] = c.stamp;
            if (c.cards[ci].ex != 0 || c.cards[ci].ey != 0) now.push_back(ci);
        }
        for (int ci : c.evaded)
            if (ci >= 0 && ci < static_cast<int>(c.cards.size()) && c.visStamp[ci] != c.stamp) c.cards[ci].ex = c.cards[ci].ey = 0;
        c.evaded.swap(now);
    } else {
        c.evaded.clear();
        for (size_t ci = 0; ci < c.cards.size(); ++ci) {
            evade(static_cast<int>(ci));
            if (c.cards[ci].ex != 0 || c.cards[ci].ey != 0) c.evaded.push_back(static_cast<int>(ci));
        }
    }
    float kp = 1.0f - std::exp(-dt / 230.0f), ka = 1.0f - std::exp(-dt / 160.0f);
    float selZ = c.sel >= 0 ? c.cards[c.sel].tz : 0.0f;
    bool dead = false;
    bool posMove = false, fading = false;
    auto approach = [&](CloudCard& k) {
        float any = 0;
        any += Approach(k.x, k.tx, kp, 0.0001f);
        any += Approach(k.y, k.ty, kp, 0.0001f);
        any += Approach(k.z, k.tz, kp, 0.0001f);
        any += Approach(k.w, k.tw, kp, 0.0001f);
        any += Approach(k.h, k.th, kp, 0.0001f);
        if (any > 0) posMove = true;
        any += Approach(k.a, k.ta, ka, 0.004f);
        // vor dem ausgewaehlten Bild liegende Karten weichen aus (fade 0 = ganz zur Seite)
        float tf = (c.sel >= 0 && k.tz < selZ - 1e-5f) ? 0.0f : 1.0f;
        any += Approach(k.fade, tf, ka, 0.004f);
        if (k.fade < 1.0f) fading = true;
        if (k.leaving && k.a <= 0.0f) dead = true;
        return any > 0 || k.leaving;
    };
    // nur Karten, die sich noch bewegen (nach neuen Zielen einmal alle)
    std::vector<int> still;
    if (c.allActive) {
        c.allActive = false;
        for (size_t ci = 0; ci < c.cards.size(); ++ci)
            if (approach(c.cards[ci])) still.push_back(static_cast<int>(ci));
    } else {
        for (int ci : c.active)
            if (ci >= 0 && ci < static_cast<int>(c.cards.size()) && approach(c.cards[ci])) still.push_back(ci);
    }
    c.active.swap(still);
    c.posMoving = posMove;
    c.fading = fading;
    if (posMove) c.grid.valid = false;
    if (!c.active.empty()) more = true;
    if (dead) {
        // ausgeblendete Karten entfernen (Atlasplatz wird frei)
        std::vector<int> remap(c.cards.size(), -1);
        std::vector<CloudCard> keep;
        keep.reserve(c.cards.size());
        for (size_t i = 0; i < c.cards.size(); ++i) {
            CloudCard& k = c.cards[i];
            if (k.leaving && k.a <= 0.0f) {
                if (k.slot >= 0) c.freeSlots.push_back(k.slot);
                continue;
            }
            remap[i] = static_cast<int>(keep.size());
            keep.push_back(std::move(k));
        }
        c.cards = std::move(keep);
        c.index.clear();
        for (size_t i = 0; i < c.cards.size(); ++i) c.index[c.cards[i].path] = static_cast<int>(i);
        for (auto& o : c.order) o = o >= 0 ? remap[o] : -1;
        c.order.erase(std::remove(c.order.begin(), c.order.end(), -1), c.order.end());
        c.sel = c.sel >= 0 ? remap[c.sel] : -1;
        c.hover = c.hover >= 0 ? remap[c.hover] : -1;
        c.block = c.block >= 0 ? remap[c.block] : -1;
        c.drawn.clear();
        c.stepSel = c.sel;
        c.allActive = true;   // Nummern haben sich verschoben
        c.evaded.clear();
        c.grid.valid = false;
        more = true;
    }
    return more;
}

// Raster fuer die ruhende Wolke: Zellen ueber x/y, je Zelle die Karten nach Tiefe. Damit muessen Zeichnen,
// Ausweichen und die Suche nach der Karte in der Bildmitte nicht mehr alle Karten durchlaufen.
void App::CloudBuildGrid() {
    auto& c = cloud_;
    auto& g = c.grid;
    g.valid = false;
    const int n = static_cast<int>(c.cards.size());
    if (n == 0) return;
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    g.maxHalf = 0;
    g.maxSize = 0;
    for (const auto& k : c.cards) {
        x0 = std::min(x0, k.x);
        x1 = std::max(x1, k.x);
        y0 = std::min(y0, k.y);
        y1 = std::max(y1, k.y);
        g.maxHalf = std::max(g.maxHalf, std::max(k.w, k.h) / 2);
        g.maxSize = std::max(g.maxSize, std::max(k.w, k.h));
    }
    // etwa 8 Karten je Zelle, hoechstens 256 x 256 Zellen
    float area = std::max(1e-3f, (x1 - x0) * (y1 - y0));
    g.cs = std::max({0.5f, std::sqrt(area * 8.0f / n), (x1 - x0) / 256.0f, (y1 - y0) / 256.0f});
    g.x0 = x0;
    g.y0 = y0;
    g.nx = std::clamp(static_cast<int>((x1 - x0) / g.cs) + 1, 1, 257);
    g.ny = std::clamp(static_cast<int>((y1 - y0) / g.cs) + 1, 1, 257);
    const int cells = g.nx * g.ny;
    std::vector<int> cellOf(n);
    g.start.assign(cells + 1, 0);
    for (int i = 0; i < n; ++i) {
        const auto& k = c.cards[i];
        int gx = std::clamp(static_cast<int>((k.x - g.x0) / g.cs), 0, g.nx - 1);
        int gy = std::clamp(static_cast<int>((k.y - g.y0) / g.cs), 0, g.ny - 1);
        cellOf[i] = gy * g.nx + gx;
        ++g.start[cellOf[i] + 1];
    }
    for (int i = 0; i < cells; ++i) g.start[i + 1] += g.start[i];
    g.items.assign(n, 0);
    std::vector<int> fill(g.start.begin(), g.start.end() - 1);
    for (int i = 0; i < n; ++i) g.items[fill[cellOf[i]]++] = i;
    for (int cell = 0; cell < cells; ++cell)
        std::sort(g.items.begin() + g.start[cell], g.items.begin() + g.start[cell + 1],
                  [&](int a, int b) { return c.cards[a].z < c.cards[b].z; });
    g.itemZ.resize(n);
    for (int i = 0; i < n; ++i) g.itemZ[i] = c.cards[g.items[i]].z;
    g.byZ.resize(n);
    for (int i = 0; i < n; ++i) g.byZ[i] = i;
    std::sort(g.byZ.begin(), g.byZ.end(), [&](int a, int b) { return c.cards[a].z < c.cards[b].z; });
    g.byZz.resize(n);
    for (int i = 0; i < n; ++i) g.byZz[i] = c.cards[g.byZ[i]].z;
    g.valid = true;
}

// Ruhezustand: Karten markieren, die im Blickfeld liegen koennen. Je Zelle ist das ein Abschnitt ab einer
// Mindesttiefe: weiter seitlich liegende Zellen werden erst in groesserer Entfernung sichtbar (der Blickkegel
// wird nach hinten breiter). Bewusst grosszuegig; die genaue Pruefung macht das Zeichnen selbst.
void App::CloudMarkVisible() {
    auto& c = cloud_;
    const auto& g = c.grid;
    ++c.stamp;
    if (c.visStamp.size() != c.cards.size()) c.visStamp.assign(c.cards.size(), 0);
    const D2D1_RECT_F& v = c.view;
    const float hx = (v.right - v.left) / 2 + 48, hy = (v.bottom - v.top) / 2 + 48;   // wie beim Zeichnen: 40 px Rand
    const float f = std::max(1.0f, c.f);
    for (int gy = 0; gy < g.ny; ++gy) {
        float cy0 = g.y0 + gy * g.cs, cy1 = cy0 + g.cs;
        float dy = std::max({0.0f, cy0 - c.cy, c.cy - cy1}) - g.maxHalf;
        for (int gx = 0; gx < g.nx; ++gx) {
            int cell = gy * g.nx + gx, s0 = g.start[cell], s1 = g.start[cell + 1];
            if (s0 == s1) continue;
            float cx0 = g.x0 + gx * g.cs, cx1 = cx0 + g.cs;
            float dx = std::max({0.0f, cx0 - c.cx, c.cx - cx1}) - g.maxHalf;
            // sichtbar ab der Tiefe, in der der seitliche Abstand in den Bildausschnitt passt
            float dmin = std::max({kNear, dx > 0 ? dx * f / hx : 0.0f, dy > 0 ? dy * f / hy : 0.0f});
            int i = static_cast<int>(std::lower_bound(g.itemZ.begin() + s0, g.itemZ.begin() + s1, c.cz + dmin) - g.itemZ.begin());
            for (; i < s1; ++i) c.visStamp[g.items[i]] = c.stamp;
        }
    }
}

// ---------------------------------------------------------------------------
// Atlas

void App::CloudReleaseGpu() {
    auto& c = cloud_;
    c.pagesS.clear();
    c.pagesXS.clear();
    c.batch.Reset();
    c.icons.clear();
    c.iconBytes = 0;
    for (auto& k : c.cards) {
        k.face = 0;
        k.xs = false;
    }
}

bool App::CloudEnsurePage(ID2D1DeviceContext* dc, int slot) {
    auto& c = cloud_;
    auto make = [&](std::vector<ComPtr<ID2D1Bitmap1>>& pages, size_t need, UINT cell, int perRow) {
        while (pages.size() < need) {
            ComPtr<ID2D1Bitmap1> bmp;
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(kPage, kPage), nullptr, 0, bp, &bmp))) return false;
            dc->SetTarget(bmp.Get());
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            // Zelle 0: weisse Karte mit abgerundeten Ecken (Platzhalter und Abdunklung)
            D2D1_RECT_F w = ToF(CellRect(0, cell, perRow, 0));
            float rad = kRound * cell;
            dc->FillRoundedRectangle(D2D1::RoundedRect(w, rad, rad), r_.Brush(D2D1::ColorF(1, 1, 1, 1)));
            pages.push_back(bmp);
        }
        return true;
    };
    return make(c.pagesS, static_cast<size_t>(slot / kPerS) + 1, kCellS, kRowS) &&
           make(c.pagesXS, static_cast<size_t>(slot / kPerXS) + 1, kCellXS, kRowXS);
}

// Inhalt einer Karte in ein Rechteck zeichnen (Atlaszelle oder direkt auf den Bildschirm). Die Zelle wird spaeter
// auf das Seitenverhaeltnis der Karte gestreckt; Symbole werden deshalb entsprechend vorverzerrt.
void App::CloudDrawFace(ID2D1DeviceContext* dc, const CloudCard& k, const D2D1_RECT_F& rc, bool large, float rad) {
    auto it = byPath_.find(k.path);
    const FileEntry* e = it != byPath_.end() ? &entries_[it->second] : nullptr;
    D2D1_COLOR_F cardBg = dark_ ? D2D1::ColorF(0.24f, 0.24f, 0.25f, 1) : D2D1::ColorF(0.93f, 0.93f, 0.94f, 1);
    auto bi = bmps_.find(k.path);
    if (e && e->HasThumb() && !e->offline && bi != bmps_.end() && bi->second.bmp) {
        bi->second.lastUse = frame_;
        ID2D1Bitmap1* bmp = bi->second.bmp.Get();
        if (large) {   // ganz nah: Bild in voller Aufloesung, sobald geladen
            auto fi = fullBmps_.find(k.path);
            if (fi != fullBmps_.end() && fi->second.bmp) bmp = fi->second.bmp.Get();
        }
        dc->DrawBitmap(bmp, rc, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        if (large && e->kind == Kind::Video) {
            float r = std::min(18.0f, (rc.bottom - rc.top) * 0.1f);
            D2D1_POINT_2F p = D2D1::Point2F(rc.right - r - 8, rc.bottom - r - 8);
            dc->FillEllipse(D2D1::Ellipse(p, r, r), r_.Brush(D2D1::ColorF(0, 0, 0, 0.55f)));
            wchar_t g[2] = {0xE768, 0};
            DrawText(dc, g, D2D1::RectF(p.x - r, p.y - r, p.x + r, p.y + r), r_.Icon(r * 0.9f), D2D1::ColorF(1, 1, 1, 1));
        }
        return;
    }
    if (rad > 0) dc->FillRoundedRectangle(D2D1::RoundedRect(rc, rad, rad), r_.Brush(cardBg));
    else dc->FillRectangle(rc, r_.Brush(cardBg));
    if (!e) return;
    auto ii = bmps_.find(IconKey(*e));
    if (ii == bmps_.end() || !ii->second.bmp) return;
    ii->second.lastUse = frame_;
    // ohne Zwischenebene: Symbol und Name auf die Karte begrenzen (sie reichen nie bis in die runden Ecken)
    struct Clip {
        ID2D1DeviceContext* dc;
        bool on;
        ~Clip() { if (on) dc->PopAxisAlignedClip(); }
    } clip{dc, rad > 0};
    if (clip.on) dc->PushAxisAlignedClip(rc, D2D1_ANTIALIAS_MODE_ALIASED);
    // Symbol quadratisch auf der Karte (Karte: k.tw x k.th)
    float rw = rc.right - rc.left, rh = rc.bottom - rc.top;
    float side = 0.55f * std::min(k.tw, k.th);
    float sx = rw * side / k.tw, sy = rh * side / k.th;
    D2D1_SIZE_F is = ii->second.bmp->GetSize();
    if (is.width > 0 && is.height > 0) {
        float ia = is.width / is.height;
        if (ia > 1) sy /= ia;
        else sx *= ia;
    }
    if (large && is.width > 0) {   // Symbole nicht ueber das 1,5-Fache ihrer Groesse vergroessern (unscharf)
        float lim = std::max(is.width, is.height) * 1.5f, m = std::max(sx, sy);
        if (m > lim) { sx *= lim / m; sy *= lim / m; }
    }
    float cxr = (rc.left + rc.right) / 2, cyr = (rc.top + rc.bottom) / 2;
    // Dateiname erst auf nahen Karten (ab 90 px Kartenhoehe), weich eingeblendet bis 125 px; das Symbol rueckt dabei
    // gleitend nach oben
    constexpr float kNameFrom = 90.0f, kNameFull = 125.0f;
    if (large && rh >= kNameFrom) {
        const float vis = std::clamp((rh - kNameFrom) / (kNameFull - kNameFrom), 0.0f, 1.0f);
        // Ordner und Dateien ohne Vorschau: Symbol etwas hoeher, Name darunter (hoechstens zwei Zeilen).
        // Das Text-Layout entsteht einmal in einer Bezugsgroesse und wird nur skaliert gezeichnet: beim Zoomen
        // bleibt es gleich, es wird also nicht in jedem Bild neu gemessen.
        const float fsRef = 12.0f;
        float fs = std::clamp(rh * 0.075f, 9.0f, 16.0f);
        float scale = fs / fsRef;
        const float avail = rw * 0.86f / scale;   // verfuegbare Zeilenbreite in Bezugsgroesse
        k.nameUse = frame_;
        // Umbrueche nur in festen Stufen (je 25 % breiter): beim Zoomen aendern sie sich selten, und passt der Name in
        // eine Zeile, nie mehr. Breiter wird erst gewechselt, wenn mehr als eine Stufe Platz gewonnen ist (kein Hin
        // und Her an der Grenze); schmaler sofort, sobald die alte Zeile nicht mehr passt.
        if (k.nameNat < 0) k.nameNat = r_.TextWidthFast(e->name, fsRef) * 1.08f + 4.0f;   // halbfett etwas breiter
        constexpr int kSingle = 1000;
        auto stepW = [](int L) { return 20.0f * std::pow(1.25f, static_cast<float>(L)); };
        int want;
        float wantW;
        if (k.nameNat <= avail) {
            want = kSingle;
            wantW = k.nameNat;
        } else {
            want = avail > 20 ? std::clamp(static_cast<int>(std::floor(std::log(avail / 20.0f) / std::log(1.25f))), 0, 40) : 0;
            wantW = stepW(want);
        }
        bool fits = k.nameLay && k.nameW <= avail + 0.5f;
        if (fits && want != k.nameBucket && avail < k.nameW * 1.3f) {   // bisherige Umbrueche behalten
            want = k.nameBucket;
            wantW = k.nameW;
        }
        if (want != k.nameBucket || !k.nameLay) {
            // Zeitbudget: hoechstens etwa 1,5 ms je Bild fuer neue Umbrueche (mindestens 4), der Rest im naechsten Bild
            if (cloud_.nameMade < 4 || NowMs() - cloud_.nameT0 < 1.5) {
                ++cloud_.nameMade;
                k.nameBucket = want;
                k.nameW = wantW;
                k.nameLay.Reset();
                k.nameH = 0;
                IDWriteTextFormat* tf = r_.TextWrap(fsRef, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
                if (tf && SUCCEEDED(r_.dwrite()->CreateTextLayout(e->name.c_str(), static_cast<UINT32>(e->name.size()), tf,
                                                                  wantW, fsRef * 2.75f, &k.nameLay))) {
                    k.nameLay->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);   // lange Namen ohne Leerzeichen brechen
                    DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                    if (!cloud_.ellipsis) r_.dwrite()->CreateEllipsisTrimmingSign(tf, &cloud_.ellipsis);
                    k.nameLay->SetTrimming(&trim, cloud_.ellipsis.Get());
                    DWRITE_TEXT_METRICS tm{};
                    k.nameLay->GetMetrics(&tm);
                    k.nameH = tm.height;
                }
                RecordGlyphRuns(k.nameLay.Get(), k.nameRuns);
            } else {
                cloud_.namePending = true;
                // bis dahin die bisherigen Umbrueche, falls noetig etwas kleiner, damit sie auf die Karte passen
                if (k.nameLay && k.nameW > avail) scale *= avail / k.nameW;
            }
        }
        const float wRef = k.nameW;
        if (IDWriteTextLayout* lay = k.nameLay.Get()) {
            // Texthoehe weich angleichen: wechselt der Name beim Zoomen zwischen ein und zwei Zeilen, gleitet das
            // Symbol an seine neue Hoehe statt zu springen
            float target = std::min(k.nameH, fsRef * 2.75f);
            if (k.nameHs < 0 || std::fabs(target - k.nameHs) < 0.05f) {
                k.nameHs = target;
            } else {
                k.nameHs += (target - k.nameHs) * cloud_.nameK;
                cloud_.nameMoving = true;
            }
            float th = k.nameHs * scale;
            cyr -= th * 0.55f * vis;
            float ty = cyr + sy / 2 + fs * 0.45f;
            D2D1_MATRIX_3X2_F old;
            dc->GetTransform(&old);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(cxr - wRef * scale / 2, ty) * old);
            // aufgezeichnete Glyphenlaeufe statt DrawTextLayout: dieselben Pixel (siehe --render-check), aber ohne
            // das Layout in jedem Bild erneut zu durchlaufen
            (void)lay;
            DrawGlyphRuns(dc, k.nameRuns, r_.Brush(dark_ ? D2D1::ColorF(1, 1, 1, 0.9f * vis) : D2D1::ColorF(0, 0, 0, 0.85f * vis)), 1);
            dc->SetTransform(old);
        }
    }

    D2D1_RECT_F ir = D2D1::RectF(cxr - sx / 2, cyr - sy / 2, cxr + sx / 2, cyr + sy / 2);
    if (large) {
        // Symbol in Zielgroesse aus dem Zwischenspeicher (kubisch an die genaue Lage gelegt). Fehlt es noch, wird es
        // dieses eine Mal direkt hochwertig gezeichnet und vor dem naechsten Bild angelegt.
        const float px = r_.scale();
        int w = static_cast<int>(std::lround(sx * px)), h = static_cast<int>(std::lround(sy * px));
        D2D1_SIZE_U sp = ii->second.bmp->GetPixelSize();
        if (w >= 4 && h >= 4 && w <= 512 && h <= 512 && static_cast<UINT>(w) < sp.width && static_cast<UINT>(h) < sp.height) {
            auto& ic = cloud_.icons[std::make_tuple(ii->second.bmp.Get(), w, h)];
            ic.lastUse = frame_;
            if (!ic.src) ic.src = ii->second.bmp;
            if (ic.bmp) {
                dc->DrawBitmap(ic.bmp.Get(), ir, 1.0f, D2D1_INTERPOLATION_MODE_CUBIC);
                return;
            }
        }
    }
    dc->DrawBitmap(ii->second.bmp.Get(), ir, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
}

// Angeforderte Symbolgroessen anlegen (vor dem Zeichnen der Wolke: dabei wechselt das Zeichenziel)
void App::CloudFillIcons(ID2D1DeviceContext* dc) {
    auto& c = cloud_;
    if (c.icons.empty()) return;
    // nicht mehr verwendete Groessen freigeben (beim Zoomen entstehen laufend neue)
    if (c.iconBytes > 48u * 1024 * 1024 || c.icons.size() > 4000) {
        for (auto it = c.icons.begin(); it != c.icons.end();) {
            if (it->second.lastUse + 2 < frame_) {
                if (it->second.bmp) c.iconBytes -= size_t(std::get<1>(it->first)) * std::get<2>(it->first) * 4;
                it = c.icons.erase(it);
            } else {
                ++it;
            }
        }
    }
    int made = 0;
    bool switched = false;
    ComPtr<ID2D1Image> oldTarget;
    D2D1_MATRIX_3X2_F oldT{};
    float dpiX = 96, dpiY = 96;
    for (auto& [key, ic] : c.icons) {
        if (ic.bmp || !ic.src || ic.lastUse + 1 < frame_) continue;
        if (made >= 48) { InvalidateRect(hwnd_, nullptr, FALSE); break; }   // Rest im naechsten Bild
        if (!switched) {
            switched = true;
            dc->GetTarget(&oldTarget);
            dc->GetTransform(&oldT);
            dc->GetDpi(&dpiX, &dpiY);
            dc->SetDpi(96, 96);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        const int w = std::get<1>(key), h = std::get<2>(key);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(static_cast<UINT>(w), static_cast<UINT>(h)), nullptr, 0, bp, &ic.bmp))) {
            ic.src.Reset();   // nicht erneut versuchen
            continue;
        }
        dc->SetTarget(ic.bmp.Get());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->DrawBitmap(ic.src.Get(), D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), 1.0f,
                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        c.iconBytes += size_t(w) * h * 4;
        ++made;
    }
    if (switched) {
        dc->SetTarget(oldTarget.Get());
        dc->SetDpi(dpiX, dpiY);
        dc->SetTransform(oldT);
    }
}

void App::CloudFillAtlas(ID2D1DeviceContext* dc) {
    auto& c = cloud_;
    // Vorschaubilder anfordern und fertige in den Atlas zeichnen (vorne zuerst, begrenzt pro Bild)
    struct Job { int card; int face; };
    std::vector<Job> jobs;
    const size_t maxJobs = 160;
    if (c.fillStamp.size() != c.cards.size()) c.fillStamp.assign(c.cards.size(), 0);
    ++c.fillMark;
    auto consider = [&](int ci, int prio) {
        if (ci < 0 || ci >= static_cast<int>(c.cards.size()) || c.fillStamp[ci] == c.fillMark) return;
        c.fillStamp[ci] = c.fillMark;
        CloudCard& k = c.cards[ci];
        if (k.leaving || k.face == 2) return;
        auto it = byPath_.find(k.path);
        if (it == byPath_.end()) return;
        const FileEntry& e = entries_[it->second];
        int face = 0;
        if (e.HasThumb() && !e.offline) {
            EnsureThumb(e, prio, frame_);
            Bmp& b = bmps_[e.path];
            if (b.bmp) face = 2;
            else if (b.failed || e.Doc()) {   // kein Vorschaubild (endgueltig) bzw. Dokument, bis es da ist
                EnsureIcon(e, prio);
                if (bmps_[IconKey(e)].bmp) face = b.failed ? 2 : 1;
            }
        } else {
            EnsureIcon(e, prio);
            if (bmps_[IconKey(e)].bmp) face = 2;   // Dateisymbol ist endgueltig
        }
        if (face > k.face && jobs.size() < maxJobs) jobs.push_back({ci, face});
    };
    for (auto it = c.drawn.rbegin(); it != c.drawn.rend(); ++it) consider(it->card, 1);
    // gerade eingetroffene Vorschaubilder sofort uebernehmen (bevor der Speicher sie wieder verdraengt)
    size_t ai = 0;
    for (; ai < c.arrived.size() && jobs.size() < maxJobs; ++ai) {
        auto it = c.index.find(c.arrived[ai]);
        if (it != c.index.end()) consider(it->second, 2);
    }
    c.arrived.erase(c.arrived.begin(), c.arrived.begin() + static_cast<std::ptrdiff_t>(ai));
    // Karten ausserhalb des Bildes: je Bild nur ein Abschnitt (reihum). Jede Pruefung sucht ueber den Dateipfad;
    // bei 100.000 Karten waere ein voller Durchlauf je Bild der groesste Posten.
    const size_t no = c.order.size();
    if (no) {
        if (c.fillCursor >= no) c.fillCursor = 0;
        size_t scan = std::min<size_t>(no, 3000);
        for (size_t i = 0; i < scan && jobs.size() < maxJobs; ++i) {
            consider(c.order[c.fillCursor], 2);
            if (++c.fillCursor >= no) c.fillCursor = 0;
        }
    }
    bool repack = c.needRepack && GetTickCount() - c.lastRepack > 1500;
    if (jobs.empty() && !repack) return;

    ComPtr<ID2D1Image> oldTarget;
    dc->GetTarget(&oldTarget);
    D2D1_MATRIX_3X2_F oldT;
    dc->GetTransform(&oldT);
    float dpiX = 96, dpiY = 96;
    dc->GetDpi(&dpiX, &dpiY);
    dc->SetDpi(96, 96);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());

    if (repack) {
        CloudRepack(dc);
        c.needRepack = false;
    }

    if (!jobs.empty()) {
        int maxSlot = 0;
        for (auto& j : jobs) maxSlot = std::max(maxSlot, c.cards[j.card].slot);
        if (CloudEnsurePage(dc, maxSlot)) {
            std::sort(jobs.begin(), jobs.end(), [&](const Job& a, const Job& b) { return c.cards[a.card].slot < c.cards[b.card].slot; });
            // 64-px-Zellen (mit abgerundeten Ecken)
            ComPtr<ID2D1Factory> fac;
            dc->GetFactory(&fac);
            ID2D1Bitmap1* cur = nullptr;
            for (auto& j : jobs) {
                CloudCard& k = c.cards[j.card];
                ID2D1Bitmap1* page = c.pagesS[k.slot / kPerS].Get();
                if (page != cur) { dc->SetTarget(page); cur = page; }
                D2D1_RECT_F cell = ToF(CellRect(k.slot % kPerS + 2, kCellS, kRowS, 0));
                dc->PushAxisAlignedClip(cell, D2D1_ANTIALIAS_MODE_ALIASED);
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                if (fac) fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(cell, kRound * kCellS, kRound * kCellS), &geo);
                if (geo) dc->PushLayer(D2D1::LayerParameters1(cell, geo.Get()), nullptr);
                if (j.face == 2) {   // Seitenverhaeltnis des Vorschaubilds uebernehmen
                    auto it = byPath_.find(k.path);
                    auto bi = bmps_.find(k.path);
                    float a = CardAspect(it != byPath_.end() ? &entries_[it->second] : nullptr, bi != bmps_.end() ? &bi->second : nullptr);
                    float tw = std::sqrt(a), th = 1.0f / std::sqrt(a);
                    if (tw != k.tw || th != k.th) {   // Karte nimmt die neue Form an: wieder in Bewegung
                        k.tw = tw;
                        k.th = th;
                        if (c.allActive || c.active.size() > 4096) c.allActive = true;
                        else if (std::find(c.active.begin(), c.active.end(), j.card) == c.active.end()) c.active.push_back(j.card);
                        c.grid.valid = false;
                        StartAnimation();
                    }
                }
                CloudDrawFace(dc, k, cell, false);
                if (geo) dc->PopLayer();
                dc->PopAxisAlignedClip();
                k.face = j.face;
            }
            // 16-px-Zellen aus den 64-px-Zellen (hochwertig verkleinert, ruhiger bei kleinen Karten)
            cur = nullptr;
            std::sort(jobs.begin(), jobs.end(), [&](const Job& a, const Job& b) { return c.cards[a.card].slot / kPerXS < c.cards[b.card].slot / kPerXS; });
            for (auto& j : jobs) {
                CloudCard& k = c.cards[j.card];
                ID2D1Bitmap1* page = c.pagesXS[k.slot / kPerXS].Get();
                if (page != cur) { dc->SetTarget(page); cur = page; }
                D2D1_RECT_F cell = ToF(CellRect(k.slot % kPerXS + 2, kCellXS, kRowXS, 0));
                D2D1_RECT_F src = ToF(CellRect(k.slot % kPerS + 2, kCellS, kRowS, 0));
                dc->PushAxisAlignedClip(cell, D2D1_ANTIALIAS_MODE_ALIASED);
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                dc->DrawBitmap(c.pagesS[k.slot / kPerS].Get(), cell, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src);
                dc->PopAxisAlignedClip();
                k.xs = true;
            }
            StartAnimation();   // Seitenverhaeltnisse koennen sich geaendert haben
        }
    }
    dc->SetTarget(oldTarget.Get());
    dc->SetDpi(dpiX, dpiY);
    dc->SetTransform(oldT);
}

// Atlas nach Rang neu ordnen, damit benachbarte Karten in derselben Textur liegen (wenige Zeichenaufrufe)
void App::CloudRepack(ID2D1DeviceContext* dc) {
    auto& c = cloud_;
    c.lastRepack = GetTickCount();
    std::vector<int> seq = c.order;
    if (!c.strips.empty())   // Streifen: in Zeichenreihenfolge (nach Tiefe), damit wenige Texturwechsel noetig sind
        std::stable_sort(seq.begin(), seq.end(), [&](int a, int b) { return c.cards[a].tz > c.cards[b].tz; });
    for (size_t i = 0; i < c.cards.size(); ++i)
        if (c.cards[i].leaving) seq.push_back(static_cast<int>(i));
    bool sorted = true;
    for (size_t i = 0; i < seq.size(); ++i)
        if (c.cards[seq[i]].slot != static_cast<int>(i)) { sorted = false; break; }
    if (sorted) return;
    auto oldS = std::move(c.pagesS);
    auto oldXS = std::move(c.pagesXS);
    c.pagesS.clear();
    c.pagesXS.clear();
    if (!seq.empty() && !CloudEnsurePage(dc, static_cast<int>(seq.size()) - 1)) {
        CloudReleaseGpu();
        return;
    }
    // Das Vorbereiten der neuen Seiten (Loeschen) ist noch nicht ausgefuehrt: erst abschliessen, sonst ueberschreibt
    // es die folgenden Kopien (Karten waeren leer)
    dc->Flush();
    for (size_t i = 0; i < seq.size(); ++i) {
        CloudCard& k = c.cards[seq[i]];
        int ns = static_cast<int>(i);
        if (k.face > 0 && k.slot >= 0 && k.slot / kPerS < static_cast<int>(oldS.size())) {
            D2D1_RECT_U src = CellRect(k.slot % kPerS + 2, kCellS, kRowS, 0);
            D2D1_RECT_U dst = CellRect(ns % kPerS + 2, kCellS, kRowS, 0);
            D2D1_POINT_2U p = D2D1::Point2U(dst.left, dst.top);
            c.pagesS[ns / kPerS]->CopyFromBitmap(&p, oldS[k.slot / kPerS].Get(), &src);
            if (k.xs && k.slot / kPerXS < static_cast<int>(oldXS.size())) {
                D2D1_RECT_U sx = CellRect(k.slot % kPerXS + 2, kCellXS, kRowXS, 0);
                D2D1_RECT_U dx = CellRect(ns % kPerXS + 2, kCellXS, kRowXS, 0);
                D2D1_POINT_2U px = D2D1::Point2U(dx.left, dx.top);
                c.pagesXS[ns / kPerXS]->CopyFromBitmap(&px, oldXS[k.slot / kPerXS].Get(), &sx);
            } else {
                k.xs = false;
            }
        } else {
            k.face = 0;
            k.xs = false;
        }
        k.slot = ns;
    }
    c.nextSlot = static_cast<int>(seq.size());
    c.freeSlots.clear();
}

// ---------------------------------------------------------------------------
// Zeichnen

void App::PaintCloud(ID2D1DeviceContext* dc) {
    auto& c = cloud_;
    CloudSync();
    if (!pendingCloudSel_.empty()) {   // --cloud-select (Dokumentation)
        auto it = c.index.find(pendingCloudSel_);
        if (it != c.index.end()) CloudSelect(it->second);
        pendingCloudSel_.clear();
    }
    const double pt0 = perfHud_ ? NowMs() : 0;
    c.nameMoving = false;
    c.nameK = 1.0f - std::exp(-static_cast<float>(RefreshPeriodMs()) / 60.0f);
    CloudFillAtlas(dc);
    CloudFillIcons(dc);
    const double pt1 = perfHud_ ? NowMs() : 0;
    int pImg = 0, pText = 0, pLayer = 0;

    const D2D1_RECT_F v = c.view;
    const float vw = v.right - v.left, vh = v.bottom - v.top;
    if (vw < 10 || vh < 10) return;
    const float f = (vh / 2) / kTanHalfFov;
    c.f = f;
    const float vcx = (v.left + v.right) / 2, vcy = (v.top + v.bottom) / 2;
    const float px = r_.scale();
    const D2D1_COLOR_F fogC = dark_ ? D2D1::ColorF(0.17f, 0.17f, 0.17f, 1) : D2D1::ColorF(0.975f, 0.975f, 0.975f, 1);
    const D2D1_COLOR_F cardBg = dark_ ? D2D1::ColorF(0.24f, 0.24f, 0.25f, 1) : D2D1::ColorF(0.93f, 0.93f, 0.94f, 1);
    const float fogMax = dark_ ? 0.72f : 0.62f;
    const float dRef = std::max(0.15f, c.focusZ - c.cz);
    const float fogSpan = c.D * 1.15f + 0.3f;   // relativ zur Tiefe des Haufens: kleine wie grosse Wolken gleich abgestuft

    dc->PushAxisAlignedClip(v, D2D1_ANTIALIAS_MODE_ALIASED);

    struct Seg { int start = 0, count = 0; ID2D1Bitmap1* bmp = nullptr; int direct = -1; D2D1_RECT_F rc{}; float alpha = 1, fog = 0; };
    // Listen bleiben ueber die Bilder erhalten (bei 100.000 Karten sonst mehrere MB Speicheranforderung je Bild)
    std::vector<D2D1_RECT_F>& dst = c.sDst;
    std::vector<D2D1_RECT_U>& src = c.sSrc;
    std::vector<D2D1_COLOR_F>& col = c.sCol;
    dst.clear();
    src.clear();
    col.clear();
    std::vector<Seg> segs;
    c.drawn.clear();
    // Dateinamen: Zeitbudget fuer neue Umbrueche in diesem Bild
    c.nameT0 = NowMs();
    c.nameMade = 0;
    c.namePending = false;
    // Ruhezustand: nur Karten anfassen, die im Blickfeld liegen koennen
    if (!c.grid.valid && !c.allActive && !c.posMoving) CloudBuildGrid();
    const bool cull = c.grid.valid;
    if (cull) CloudMarkVisible();
    std::unordered_set<std::wstring> wantFull;

    auto addSprite = [&](ID2D1Bitmap1* bmp, const D2D1_RECT_F& d, const D2D1_RECT_U& s, const D2D1_COLOR_F& cc) {
        if (segs.empty() || segs.back().direct >= 0 || segs.back().bmp != bmp) {
            Seg sg;
            sg.start = static_cast<int>(dst.size());
            sg.bmp = bmp;
            segs.push_back(sg);
        }
        dst.push_back(d);
        src.push_back(s);
        col.push_back(cc);
        ++segs.back().count;
    };

    auto drawCard = [&](int ci) {
        CloudCard& k = c.cards[ci];
        float d = k.z - c.cz;
        if (d < kNear) return;
        float alpha = k.a;   // nur Ein-/Ausblenden beim Filtern, sonst nie durchsichtig
        float kx = k.x + k.ex, ky = k.y + k.ey;   // mit Ausweichversatz (geglaettet in CloudStep)
        float s = f / d;
        float sx = vcx + (kx - c.cx) * s, sy = vcy + (ky - c.cy) * s;
        if (alpha < 0.01f) return;
        float hw = k.w * s / 2, hh = k.h * s / 2;
        D2D1_RECT_F rc = D2D1::RectF(sx - hw, sy - hh, sx + hw, sy + hh);
        if (rc.right < v.left - 40 || rc.left > v.right + 40 || rc.bottom < v.top - 40 || rc.top > v.bottom + 40) return;
        float sizePx = std::max(hw, hh) * 2 * px;
        float fx = std::clamp((d - dRef) / fogSpan, 0.0f, 1.0f);
        float fog = fx * std::sqrt(fx) * fogMax;   // nahe Karten (relativ zur Kamera) hell, erst weiter hinten dunkler
        if (c.sel >= 0 && ci != c.sel)   // ausgewaehltes Bild hervorheben: alles andere deutlich zuruecknehmen
            fog = std::max(fog, std::min(0.8f, 0.45f + std::clamp((d - dRef) / (0.6f + c.D * 0.3f), 0.0f, 1.0f) * 0.35f));
        c.drawn.push_back({rc, ci});
        if (k.slot < 0) return;

        // direkt zeichnen: nahe Karten in voller Vorschauqualitaet, Ordner (mit Namen) ab mittlerer Groesse
        bool canDirect = false;
        if (sizePx > 56) {
            auto it = byPath_.find(k.path);
            if (it != byPath_.end()) {
                const FileEntry& e = entries_[it->second];
                auto tb = bmps_.find(k.path);
                bool noThumb = !(e.HasThumb() && !e.offline) || (tb != bmps_.end() && tb->second.failed);
                if (noThumb) {
                    canDirect = k.face > 0;   // ohne Vorschaubild: Karte mit Symbol und Dateinamen
                } else if (sizePx > 150) {
                    {
                        EnsureThumb(e, 0, frame_);
                        auto bi = bmps_.find(k.path);
                        canDirect = bi != bmps_.end() && bi->second.bmp;
                        // groesser als das Vorschaubild: volle Aufloesung nachladen
                        D2D1_SIZE_U tp = canDirect ? bi->second.bmp->GetPixelSize() : D2D1::SizeU(0, 0);
                        if (canDirect && e.kind == Kind::Image && sizePx > std::max(tp.width, tp.height) * 1.1f) {
                            wantFull.insert(k.path);
                            if (!fullBmps_.count(k.path)) {
                                fullBmps_[k.path].requested = true;
                                ThumbRequest r;
                                r.key = k.path;
                                r.path = k.path;
                                r.kind = e.kind;
                                r.job = ThumbJob::Full;
                                r.prio = 0;
                                r.gen = gen_;
                                thumbs_.Request(std::move(r));
                            }
                        }
                    }
                }
            }
        }
        if (canDirect) {
            Seg sg;
            sg.direct = ci;
            sg.rc = rc;
            sg.alpha = alpha;
            sg.fog = fog;
            segs.push_back(sg);
            return;
        }
        bool useXS = sizePx < 28 && k.xs;
        int pi = useXS ? k.slot / kPerXS : k.slot / kPerS;
        auto& pages = useXS ? c.pagesXS : c.pagesS;
        if (pi >= static_cast<int>(pages.size())) return;
        ID2D1Bitmap1* page = pages[pi].Get();
        UINT cell = useXS ? kCellXS : kCellS;
        int perRow = useXS ? kRowXS : kRowS;
        int ix = (useXS ? k.slot % kPerXS : k.slot % kPerS) + 2;
        // ganze Zelle verwenden: die Ecken sind in der Zelle bereits transparent abgerundet
        if (k.face > 0) addSprite(page, rc, CellRect(ix, cell, perRow, 0), D2D1::ColorF(1, 1, 1, alpha));
        else addSprite(page, rc, CellRect(0, cell, perRow, 0), D2D1::ColorF(cardBg.r, cardBg.g, cardBg.b, alpha));
        if (fog > 0.01f) addSprite(page, rc, CellRect(0, cell, perRow, 0), D2D1::ColorF(fogC.r, fogC.g, fogC.b, fog * alpha));
    };

    // hinten zuerst: ausblendende Karten, dann vom untersten Rang nach oben
    for (size_t i = 0; i < c.cards.size(); ++i)
        if (c.cards[i].leaving) drawCard(static_cast<int>(i));
    if (c.strips.empty()) {
        for (auto it = c.order.rbegin(); it != c.order.rend(); ++it)
            if (!cull || c.visStamp[*it] == c.stamp) drawCard(*it);
    } else {
        // Streifen: Rang und Tiefe laufen je Gruppe neu, deshalb nach der aktuellen Tiefe zeichnen (hinten zuerst)
        if (cull) {   // Raster: Karten liegen schon nach Tiefe sortiert vor
            for (auto it = c.grid.byZ.rbegin(); it != c.grid.byZ.rend(); ++it)
                if (c.visStamp[*it] == c.stamp) drawCard(*it);
        } else {
            std::vector<int> seq = c.order;
            std::stable_sort(seq.begin(), seq.end(), [&](int a, int b) { return c.cards[a].z > c.cards[b].z; });
            for (int ci : seq) drawCard(ci);
        }
    }

    ComPtr<ID2D1DeviceContext3> dc3;
    dc->QueryInterface(IID_PPV_ARGS(&dc3));
    if (dc3 && !c.batch) dc3->CreateSpriteBatch(&c.batch);
    const double pt2 = perfHud_ ? NowMs() : 0;
    if (c.batch) {
        c.batch->Clear();
        if (!dst.empty())
            c.batch->AddSprites(static_cast<UINT32>(dst.size()), dst.data(), src.data(), col.data(), nullptr,
                                sizeof(D2D1_RECT_F), sizeof(D2D1_RECT_U), sizeof(D2D1_COLOR_F), 0);
    }
    if (!c.strips.empty()) PaintCloudHeaders(dc, false);   // feine Trennlinien hinter den Karten
    ComPtr<ID2D1Factory> fac;
    dc->GetFactory(&fac);
    int runs = 0;
    for (auto& sg : segs) {
        if (sg.direct >= 0) {
            CloudCard& k = c.cards[sg.direct];
            dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            float rad = std::min(5.0f, kRound * std::min(sg.rc.right - sg.rc.left, sg.rc.bottom - sg.rc.top));
            // Haeufigster Fall (Bild, voll deckend): direkt mit runden Ecken fuellen, Abdunklung als zweite Fuellung.
            // Eine Zwischenebene je Karte unterbricht das gebuendelte Zeichnen und kostet eine eigene Zeichenflaeche.
            // Die Bildflaeche ist dieselbe (siehe --render-check); nur der 1 px breite geglaettete Rand faellt
            // etwas klarer aus, weil er nicht mehr doppelt (Bildkante und Maske) abgeschwaecht wird.
            if (sg.alpha >= 0.999f) {
                auto it = byPath_.find(k.path);
                auto bi = bmps_.find(k.path);
                if (it != byPath_.end() && bi != bmps_.end() && bi->second.bmp) {
                    const FileEntry& e = entries_[it->second];
                    if (e.HasThumb() && !e.offline && e.kind != Kind::Video) {
                        bi->second.lastUse = frame_;
                        ID2D1Bitmap1* bmp = bi->second.bmp.Get();
                        auto fi = fullBmps_.find(k.path);
                        if (fi != fullBmps_.end() && fi->second.bmp) bmp = fi->second.bmp.Get();
                        ++pImg;
                        DrawRoundedBitmap(dc, bmp, sg.rc, rad);
                        if (sg.fog > 0.01f)
                            dc->FillRoundedRectangle(D2D1::RoundedRect(sg.rc, rad, rad),
                                                     r_.Brush(D2D1::ColorF(fogC.r, fogC.g, fogC.b, sg.fog)));
                        continue;
                    }
                }
                // Karte ohne Vorschaubild (Symbol und Dateiname): ebenfalls ohne Zwischenebene. Bei vielen
                // solchen Karten war eine Ebene je Karte der groesste Posten.
                bool thumb = false;
                if (it != byPath_.end() && bi != bmps_.end() && bi->second.bmp) {
                    const FileEntry& e = entries_[it->second];
                    thumb = e.HasThumb() && !e.offline;
                }
                if (!thumb) {
                    ++pText;
                    CloudDrawFace(dc, k, sg.rc, true, rad);
                    if (sg.fog > 0.01f)
                        dc->FillRoundedRectangle(D2D1::RoundedRect(sg.rc, rad, rad),
                                                 r_.Brush(D2D1::ColorF(fogC.r, fogC.g, fogC.b, sg.fog)));
                    continue;
                }
            }
            ++pLayer;
            ComPtr<ID2D1RoundedRectangleGeometry> geo;
            if (fac) fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(sg.rc, rad, rad), &geo);
            dc->PushLayer(D2D1::LayerParameters1(sg.rc, geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), sg.alpha),
                          nullptr);
            CloudDrawFace(dc, k, sg.rc, true);
            if (sg.fog > 0.01f) dc->FillRectangle(sg.rc, r_.Brush(D2D1::ColorF(fogC.r, fogC.g, fogC.b, sg.fog)));
            dc->PopLayer();
            continue;
        }
        ++runs;
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        if (c.batch && dc3) {
            dc3->DrawSpriteBatch(c.batch.Get(), static_cast<UINT32>(sg.start), static_cast<UINT32>(sg.count), sg.bmp,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, D2D1_SPRITE_OPTIONS_NONE);
        } else {
            // Ersatz ohne Sprite-Batches: einzeln zeichnen (Farbe nur als Deckkraft)
            for (int i = sg.start; i < sg.start + sg.count; ++i) {
                D2D1_RECT_F sf = ToF(src[i]);
                dc->DrawBitmap(sg.bmp, dst[i], col[i].a, D2D1_INTERPOLATION_MODE_LINEAR, &sf);
            }
        }
    }
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (perfHud_) {
        double pt3 = NowMs();
        perfNow_[0] = static_cast<float>(pt1 - pt0);
        perfNow_[1] = static_cast<float>(pt2 - pt1);
        perfNow_[2] = static_cast<float>(pt3 - pt2);
        perfCount_[0] = static_cast<int>(c.drawn.size());
        perfCount_[1] = pImg;
        perfCount_[2] = pText;
        perfCount_[3] = pLayer;
        perfCount_[4] = static_cast<int>(dst.size());
    }
    if (c.nameMoving) StartAnimation();   // Symbole gleiten noch an ihre Hoehe
    if (c.namePending) InvalidateRect(hwnd_, nullptr, FALSE);   // restliche Umbrueche im naechsten Bild
    // Dateinamen ferner Karten freigeben (Speicher waechst sonst bei einem Flug durch grosse Wolken stetig)
    if ((frame_ & 63) == 0)
        for (auto& k : c.cards)
            if (k.nameLay && k.nameUse + 240 < frame_) {
                k.nameLay.Reset();
                GlyphRuns().runs.swap(k.nameRuns.runs);
                k.nameBucket = -1;
                k.nameHs = -1;
            }
    if (runs > 48) c.needRepack = true;

    // nicht mehr benoetigte Bilder in voller Aufloesung freigeben (der Betrachter verwaltet seine eigenen)
    if (!viewer_.open) {
        for (auto it = c.full.begin(); it != c.full.end();) {
            if (!wantFull.count(*it)) { fullBmps_.erase(*it); it = c.full.erase(it); }
            else ++it;
        }
        for (auto& p : wantFull) c.full.insert(p);
    }
    // Hervorhebung unter der Maus (nicht, solange ein Bild ausgewaehlt ist)
    if (c.hover >= 0 && c.sel < 0) {
        for (auto& dw : c.drawn)
            if (dw.card == c.hover) {
                float rad = std::min(5.0f, kRound * std::min(dw.rc.right - dw.rc.left, dw.rc.bottom - dw.rc.top));
                D2D1_RECT_F hr = D2D1::RectF(dw.rc.left - 1, dw.rc.top - 1, dw.rc.right + 1, dw.rc.bottom + 1);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(hr, rad + 1, rad + 1), r_.Brush(accent_), 2.0f);
                break;
            }
    }
    // Ausgewaehltes Bild: Metadaten darunter
    if (c.sel >= 0) {
        const CloudDrawn* sd = nullptr;
        for (auto& dw : c.drawn)
            if (dw.card == c.sel) sd = &dw;
        auto it = byPath_.find(c.cards[c.sel].path);
        if (sd && it != byPath_.end()) {
            const FileEntry& e = entries_[it->second];
            std::wstring name = e.name;
            if (e.kind != Kind::Folder) {   // ohne Dateiendung
                size_t dot = name.find_last_of(L'.');
                if (dot != std::wstring::npos && dot > 0) name.resize(dot);
            }
            D2D1_COLOR_F tx = dark_ ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.9f);
            D2D1_COLOR_F tx2 = dark_ ? D2D1::ColorF(1, 1, 1, 0.66f) : D2D1::ColorF(0, 0, 0, 0.62f);
            std::wstring l2 = TypeName(e);
            if (e.kind != Kind::Folder) l2 += L"  \u00b7  " + FormatSize(e.size);
            if (e.width && e.height) l2 += L"  \u00b7  " + std::to_wstring(e.width) + L" \u00d7 " + std::to_wstring(e.height);
            if (e.kind == Kind::Video && e.durationMs) l2 += L"  \u00b7  " + FormatDuration(e.durationMs);
            std::wstring dt = DateText(e.Time());
            if (!dt.empty()) l2 += L"  \u00b7  " + dt;
            IDWriteTextFormat* f1 = r_.Text(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            IDWriteTextFormat* f2 = r_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            // Info-Feld an der Unterkante des Bildes angedockt; faehrt beim Auswaehlen nach unten heraus
            float cw = sd->rc.right - sd->rc.left;
            float w = std::max(cw, std::max(r_.MeasureText(name, f1), r_.MeasureText(l2, f2)) + 36);
            w = std::min(w, vw - 24);
            const float h = 62, rad = 6;
            float cxl = (sd->rc.left + sd->rc.right) / 2;
            float ea = c.selAnim * c.selAnim * (3 - 2 * c.selAnim);
            float top = sd->rc.bottom - rad - (1.0f - ea) * (h + rad);
            D2D1_RECT_F box = D2D1::RectF(cxl - w / 2, top, cxl + w / 2, top + h + rad);
            dc->PushAxisAlignedClip(D2D1::RectF(v.left, sd->rc.bottom, v.right, v.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
            FillRound(dc, box, rad, dark_ ? D2D1::ColorF(0.13f, 0.13f, 0.13f, 0.94f) : D2D1::ColorF(1, 1, 1, 0.96f));
            float ty = top + rad + 8;
            DrawText(dc, MiddleEllipsis(name, w - 36, [&](const std::wstring& t) { return r_.MeasureText(t, f1); }),
                     D2D1::RectF(box.left + 12, ty, box.right - 12, ty + 24), f1, tx);
            DrawText(dc, l2, D2D1::RectF(box.left + 12, ty + 24, box.right - 12, ty + 46), f2, tx2);
            dc->PopAxisAlignedClip();
            // weisser Rahmen um das ausgewaehlte Bild
            float rr = std::min(5.0f, kRound * std::min(cw, sd->rc.bottom - sd->rc.top));
            D2D1_RECT_F fr = D2D1::RectF(sd->rc.left - 1.5f, sd->rc.top - 1.5f, sd->rc.right + 1.5f, sd->rc.bottom + 1.5f);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(fr, rr + 1.5f, rr + 1.5f), r_.Brush(D2D1::ColorF(1, 1, 1, 0.95f)), 2.5f);
        }
    }
    dc->PopAxisAlignedClip();
    if (!c.strips.empty()) PaintCloudHeaders(dc, true);   // Gruppentitel: immer oben, nur waagerecht mitbewegt
}

// ---------------------------------------------------------------------------
// Eingabe

int App::CloudPick(float x, float y) const {
    const auto& c = cloud_;
    if (!Inside(c.view, x, y)) return -1;
    for (auto it = c.drawn.rbegin(); it != c.drawn.rend(); ++it) {
        const CloudCard& k = c.cards[it->card];
        if (k.leaving || k.fade < 0.5f) continue;
        if (Inside(it->rc, x, y)) return it->card;
    }
    return -1;
}

int App::CloudPosOf(int card) const {
    if (card < 0 || card >= static_cast<int>(cloud_.cards.size())) return -1;
    int r = cloud_.cards[card].rank;
    if (r >= 0 && r < static_cast<int>(visible_.size()) && entries_[visible_[r]].path == cloud_.cards[card].path) return r;
    auto it = byPath_.find(cloud_.cards[card].path);
    if (it == byPath_.end()) return -1;
    for (size_t p = 0; p < visible_.size(); ++p)
        if (visible_[p] == it->second) return static_cast<int>(p);
    return -1;
}

void App::CloudSelect(int card) {
    auto& c = cloud_;
    if (card < 0 || card >= static_cast<int>(c.cards.size())) return;
    if (!c.haveBefore) {
        c.before[0] = c.tcx;
        c.before[1] = c.tcy;
        c.before[2] = c.tcz;
        c.haveBefore = true;
    }
    c.sel = card;
    c.selAnim = 0;
    c.hover = -1;
    const CloudCard& k = c.cards[card];
    float vh = std::max(50.0f, c.view.bottom - c.view.top);
    c.f = (vh / 2) / kTanHalfFov;   // auch vor dem ersten Zeichnen gueltig
    float dist = std::max(0.3f, c.f * std::max(k.th, k.tw * 0.75f) / (0.56f * vh));
    c.tcx = k.tx;
    c.tcy = k.ty + k.th * 0.18f;
    c.tcz = k.tz - dist;
    c.tau = 170;
    c.camUser = true;
    int pos = CloudPosOf(card);
    if (pos >= 0) {
        selected_.clear();
        selected_.insert(visible_[pos]);
        focus_ = anchor_ = pos;
    }
    StartAnimation();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloudDeselect() {
    auto& c = cloud_;
    if (c.sel < 0) return;
    c.sel = -1;
    if (c.haveBefore) {
        c.tcx = c.before[0];
        c.tcy = c.before[1];
        c.tcz = c.before[2];
        c.haveBefore = false;
    }
    c.tau = 170;
    StartAnimation();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloudFocusStrip(int i) {
    auto& c = cloud_;
    if (i < 0 || i >= static_cast<int>(c.strips.size())) return;
    const CloudStrip& st = c.strips[i];
    float vw = std::max(50.0f, c.view.right - c.view.left), vh = std::max(50.0f, c.view.bottom - c.view.top);
    float f = (vh / 2) / kTanHalfFov;
    float fit = std::max((c.stripH / 2 + 0.7f) * f / (vh / 2 * 0.94f), ((st.x1 - st.x0) / 2 + 0.7f) * f / (vw / 2 * 0.94f));
    c.sel = -1;
    c.haveBefore = false;
    c.tcx = (st.x0 + st.x1) / 2;
    c.tcy = 0;
    c.tcz = -fit;
    c.tau = 170;
    c.camUser = true;
    StartAnimation();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloudMouseDown(float x, float y) {
    auto& c = cloud_;
    c.down = true;
    c.dragging = false;
    c.downPt = c.lastPt = D2D1::Point2F(x, y);
    int card = CloudPick(x, y);
    c.dragZ = card >= 0 ? c.cards[card].z : (c.cz < 0 ? c.D * 0.3f : c.cz + 2.0f);
}

void App::CloudMouseMove(float x, float y) {
    auto& c = cloud_;
    if (c.down) {
        if (!c.dragging && (std::fabs(x - c.downPt.x) > 4 || std::fabs(y - c.downPt.y) > 4)) c.dragging = true;
        if (c.dragging) {
            float k = std::max(0.5f, c.dragZ - c.cz) / std::max(1.0f, c.f);
            c.tcx -= (x - c.lastPt.x) * k;
            c.tcy -= (y - c.lastPt.y) * k;
            float lim = c.R * 1.6f + 3;
            c.tcx = std::clamp(c.tcx, -lim, lim);
            c.tcy = std::clamp(c.tcy, -lim, lim);
            c.lastPt = D2D1::Point2F(x, y);
            c.tau = 55;
            c.camUser = true;
            c.haveBefore = false;   // Bewegen loest die Auswahl auf, die Kamera bleibt, wo sie ist
            c.sel = -1;
            StartAnimation();
            return;
        }
    }
    int h = c.sel >= 0 ? -1 : CloudPick(x, y);   // waehrend einer Auswahl keine Hervorhebung anderer Bilder
    if (h != c.hover) {
        c.hover = h;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::CloudMouseUp(float x, float y) {
    auto& c = cloud_;
    if (!c.down) return;
    bool click = !c.dragging;
    c.down = false;
    c.dragging = false;
    if (!click) return;
    if (!c.strips.empty() && y >= gridRc_.top && y < c.view.top) {   // Gruppentitel: Kamera auf diesen Streifen
        for (int i = 0; i < static_cast<int>(c.strips.size()); ++i)
            if (x >= c.strips[i].sl && x < c.strips[i].sr) { CloudFocusStrip(i); break; }
        return;
    }
    int card = CloudPick(x, y);
    if (c.sel >= 0) {
        if (card == c.sel) {
            int pos = CloudPosOf(card);
            if (pos >= 0) OpenPos(pos);   // zweiter Klick: oeffnen
        } else {
            CloudDeselect();              // Klick ausserhalb: Auswahl aufheben
        }
    } else if (card >= 0) {
        CloudSelect(card);
    }
}

void App::CloudWheel(float x, float y, int delta) {
    auto& c = cloud_;
    float steps = delta / 120.0f;
    float dx = (x - (c.view.left + c.view.right) / 2) / std::max(1.0f, c.f);
    float dy = (y - (c.view.top + c.view.bottom) / 2) / std::max(1.0f, c.f);
    // Schrittweite: ausserhalb des Haufens anteilig zum Abstand, darin gleichmaessig (man kommt bis nach hinten)
    float base = std::max(0.3f + 0.015f * c.D, -c.tcz * 0.18f);
    float nz = std::clamp(c.tcz + steps * base, -c.fit * 2.5f, c.D - 0.1f);
    if (steps > 0 && c.block >= 0 && c.block < static_cast<int>(c.cards.size())) {
        // gegen die Karte in der Bildmitte stossen: nicht naeher als etwa bildschirmfuellend
        const CloudCard& b = c.cards[c.block];
        float limit = b.z - 1.0f * std::max(b.w, b.h);
        nz = std::min(nz, std::max(c.tcz, limit));
    }
    float t = nz - c.tcz;
    c.tcx += dx * t;
    c.tcy += dy * t;
    c.tcz = nz;
    c.tau = 120;
    c.camUser = true;
    c.haveBefore = false;
    c.sel = -1;   // Bewegen loest die Auswahl auf
    StartAnimation();
}

bool App::CloudKey(WPARAM vk) {
    auto& c = cloud_;
    switch (vk) {
    case VK_ESCAPE:
        if (c.sel < 0) return false;
        CloudDeselect();
        return true;
    case VK_RETURN:
        if (c.sel < 0) return false;
        if (int pos = CloudPosOf(c.sel); pos >= 0) OpenPos(pos);
        return true;
    case VK_HOME:
        c.sel = -1;
        c.haveBefore = false;
        c.camUser = false;
        CloudFit(false);
        StartAnimation();
        return true;
    case VK_LEFT:
    case VK_RIGHT: {
        if (c.order.empty()) return true;
        if (c.sel < 0) {   // ohne Auswahl: bewegen
            CloudPan(vk == VK_RIGHT ? 1.0f : -1.0f, 0);
            return true;
        }
        int r = c.sel >= 0 ? c.cards[c.sel].rank : -1;
        r = vk == VK_RIGHT ? r + 1 : (r < 0 ? 0 : r - 1);
        r = std::clamp(r, 0, static_cast<int>(c.order.size()) - 1);
        CloudSelect(c.order[r]);
        return true;
    }
    case VK_ADD:
    case VK_OEM_PLUS:
    case VK_SUBTRACT:
    case VK_OEM_MINUS: {
        bool in = vk == VK_ADD || vk == VK_OEM_PLUS;
        CloudWheel((c.view.left + c.view.right) / 2, (c.view.top + c.view.bottom) / 2, in ? 120 : -120);
        return true;
    }
    case VK_UP:
    case VK_DOWN:
        if (c.sel >= 0) CloudDeselect();
        CloudPan(0, vk == VK_DOWN ? 1.0f : -1.0f);
        return true;
    case VK_PRIOR:
    case VK_NEXT:
    case VK_END:
        return true;   // in der Wolke ohne Bedeutung
    default:
        return false;
    }
}

void App::CloudContextMenu(float x, float y) {
    int card = CloudPick(x, y);
    int pos = CloudPosOf(card);
    if (pos < 0) return;
    if (!selected_.count(visible_[pos])) {
        selected_.clear();
        selected_.insert(visible_[pos]);
    }
    focus_ = anchor_ = pos;
    POINT screen;
    GetCursorPos(&screen);
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowContextMenu(pos, screen);
}

} // namespace peek
