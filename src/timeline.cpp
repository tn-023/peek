// Zeitleiste (Ansicht 3): alle Bilder und Videos der Ansicht auf einem waagerechten Zeitstrahl, neueste links.
// Gruppen nach Tag, Monat oder Jahr (je nach Anzahl), die Zeitangabe riesig oben hinter der ersten Kachelzeile.
// Innerhalb einer Gruppe werden die Kacheln nach rechts kleiner; sehr grosse Gruppen liegen als eine Kachel da,
// ein Klick klappt sie auf. Spalten neben der Bildmitte werden leicht kleiner und dunkler (wie in der Wolke).
// Ziehen mit der Maus verschiebt ueberall die Ansicht (kein Ziehen von Dateien). Unten statt einer Bildlaufleiste
// eine Uebersicht: Menge je Zeitraum als Balken, darauf der gerade sichtbare Ausschnitt.
#include "app.h"
#include "core/lang.h"
#include "core/textutil.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace peek {

namespace {

constexpr float kTlSlider = 60;      // Hoehe der Uebersicht unten
constexpr float kTlTop = 50;         // Kacheln beginnen hier; die Zeitangabe liegt dahinter
constexpr float kTlGap = 6;          // Abstand der Kacheln
constexpr float kTlGroupGap = 72;    // Abstand der Gruppen
constexpr float kTlPad = 24;         // Rand links und rechts
constexpr float kTlBig = 52;         // Schriftgroesse der Zeitangabe (duenn)
constexpr float kTlBigMin = 26;
constexpr float kTlSmall = 15;       // Zusatz (Jahr bzw. Monat und Jahr)
constexpr DWRITE_FONT_WEIGHT kTlWeight = DWRITE_FONT_WEIGHT_LIGHT;
constexpr int kTlCollapse = 400;     // ab so vielen Elementen liegt eine Gruppe als eine Kachel da
constexpr int kTlMaxGroups = 48;     // feinste Stufe (Tag, Monat, Jahr) mit hoechstens so vielen Gruppen
constexpr int kTlRefine = 150;       // Gruppen mit mehr Elementen: feiner aufteilen (Jahr -> Monate -> Tage)
constexpr double kTlAnimMs = 520;    // Auf-/Zuklappen
constexpr double kTlStaggerMs = 4;   // Versatz je Kachel beim Aufklappen (hoechstens kTlStaggerMax)
constexpr double kTlStaggerMax = 280;

float EaseOutCubic(float u) { return 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u); }

struct Ymd { int y = 0, m = 0, d = 0; };

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

bool InRect(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

Ymd LocalYmd(int64_t unix) {
    Ymd r;
    if (unix <= 0) return r;
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(unix) * 10000000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart}, lft;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) return r;
    r.y = st.wYear;
    r.m = st.wMonth;
    r.d = st.wDay;
    return r;
}

int64_t GroupKey(const Ymd& d, int level) {
    if (level == 2) return d.y;
    if (level == 1) return d.y * 100 + d.m;
    return (d.y * 100 + d.m) * 100 + d.d;
}

}  // namespace

void App::TimelineSync() {
    auto& t = tl_;
    std::wstring key = path_ + (bookmarksView_ ? L"|b" : L"") + (collectionsView_ ? L"|c" : L"");
    if (key != t.folder) {
        t.folder = key;
        t.scroll = t.target = 0;
        t.expanded.clear();
        t.version = ~0ull;
    }
    const float vh = gridRc_.bottom - gridRc_.top;
    const bool same = t.version == visibleVersion_ && t.langEn == LangEn() && !t.dirty;
    if (same && std::fabs(t.viewH - vh) < 0.5f) return;
    // Hoehe aendert sich, waehrend das Fenster gezogen wird: nicht bei jedem Schritt neu anordnen (die Kacheln
    // sprangen und flimmerten), sondern die bisherige Anordnung mitskalieren (PaintTimeline); neu angeordnet
    // wird nach dem Loslassen
    if (same && inSizeMove_ && !t.items.empty()) return;

    // Lage merken: das Element am linken Rand bleibt nach dem Neuaufbau an derselben Stelle
    int anchorPos = -1;
    float anchorOff = 0;
    for (auto& it : t.items)
        if (it.x + it.s >= t.scroll) {
            anchorPos = it.pos;
            anchorOff = it.x - t.scroll;
            break;
        }
    std::wstring anchorPath = anchorPos >= 0 && anchorPos < static_cast<int>(visible_.size())
                                  ? entries_[visible_[anchorPos]].path
                                  : std::wstring();

    t.version = visibleVersion_;
    t.viewH = vh;
    t.langEn = LangEn();
    t.dirty = false;
    t.items.clear();
    t.groups.clear();
    t.cols.clear();
    t.groupOf.assign(visible_.size(), -1);

    // Bilder und Videos nach Datum, neueste zuerst
    std::vector<int> order;
    for (int p = 0; p < static_cast<int>(visible_.size()); ++p)
        if (entries_[visible_[p]].IsMedia()) order.push_back(p);
    const bool asc = settings_.timelineAsc;   // Sortierung: nur nach Aufnahmedatum, auf- oder absteigend
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        int64_t ta = entries_[visible_[a]].Time(), tb = entries_[visible_[b]].Time();
        return asc ? ta < tb : ta > tb;
    });
    std::vector<Ymd> ymd(order.size());
    for (size_t i = 0; i < order.size(); ++i) ymd[i] = LocalYmd(entries_[visible_[order[i]]].Time());

    // Stufe: Tage, wenn es nicht zu viele sind, sonst Monate, sonst Jahre
    int level = 2;
    for (int lv = 0; lv < 2; ++lv) {
        int groups = 0;
        int64_t last = -1;
        for (auto& d : ymd) {
            int64_t k = GroupKey(d, lv);
            if (k != last) { ++groups; last = k; }
            if (groups > kTlMaxGroups) break;
        }
        if (groups <= kTlMaxGroups) { level = lv; break; }
    }

    // Kachelgroesse: die erste Spalte einer Gruppe fasst zwei bis drei Kacheln
    const float availH = std::max(80.0f, vh - kTlSlider - kTlTop - 8);
    const int r0 = availH >= 420 ? 3 : 2;
    const float s0 = std::clamp((availH - (r0 - 1) * kTlGap) / r0, 64.0f, 260.0f);
    t.maxS = s0;
    IDWriteTextFormat* fBig = r_.Text(kTlBig, kTlWeight);
    IDWriteTextFormat* fSmall = r_.Text(kTlSmall, DWRITE_FONT_WEIGHT_NORMAL);

    // Abschnitte: grosse Gruppen feiner aufteilen (ein volles Jahr zeigt seine Monate, ein voller Monat seine Tage)
    struct Seg { size_t i, j; int level; };
    std::vector<Seg> segs;
    std::function<void(size_t, size_t, int)> split = [&](size_t a, size_t b, int lv) {
        size_t i = a;
        while (i < b) {
            const int64_t k = GroupKey(ymd[i], lv);
            size_t j = i;
            while (j < b && GroupKey(ymd[j], lv) == k) ++j;
            if (lv > 0 && j - i > static_cast<size_t>(kTlRefine) && ymd[i].y != 0) split(i, j, lv - 1);
            else segs.push_back({i, j, lv});
            i = j;
        }
    };
    split(0, order.size(), level);

    float x = kTlPad;
    for (const Seg& sg : segs) {
        const size_t i = sg.i, j = sg.j;
        const int lvl = sg.level;
        const int64_t gk = GroupKey(ymd[i], lvl);
        TimelineGroup g;
        g.key = gk;
        g.level = lvl;
        g.count = static_cast<int>(j - i);
        const Ymd& d = ymd[i];
        if (d.y == 0) {
            g.big = g.full = T(L"Ohne Datum");
        } else if (lvl == 2) {
            g.big = g.full = std::to_wstring(d.y);
        } else if (lvl == 1) {
            g.big = MonthName(static_cast<unsigned>(d.m));
            g.sub = std::to_wstring(d.y);
            g.full = g.big + L" " + g.sub;
        } else {
            g.big = std::to_wstring(d.d);
            g.sub = std::wstring(MonthName(static_cast<unsigned>(d.m))) + L" " + std::to_wstring(d.y);
            g.full = FormatDay(d.y, static_cast<unsigned>(d.m), static_cast<unsigned>(d.d));
        }
        // Diashow (aus der Zeitleiste): dieselbe Zeitangabe in Versalien ueber dem ersten Bild der Gruppe
        if (d.y != 0) {
            if (lvl == 0)
                g.caps = LangEn() ? std::wstring(MonthName(static_cast<unsigned>(d.m))) + L" " + std::to_wstring(d.d) + L", " +
                                        std::to_wstring(d.y)
                                  : std::to_wstring(d.d) + L". " + MonthName(static_cast<unsigned>(d.m)) + L" " +
                                        std::to_wstring(d.y);
            else
                g.caps = g.big + (g.sub.empty() ? L"" : L" " + g.sub);
            CharUpperBuffW(g.caps.data(), static_cast<DWORD>(g.caps.size()));
        }
        for (size_t k = i; k < j; ++k) t.groupOf[order[k]] = static_cast<int>(t.groups.size());
        g.x0 = x;
        g.firstItem = static_cast<int>(t.items.size());
        g.collapsed = g.count > kTlCollapse && !t.expanded.count(gk);
        float width = 0;
        if (g.collapsed) {
            // eine Kachel fuer die ganze Gruppe (Vorschau aus den ersten vier Bildern, Klick klappt auf)
            float s = std::min(availH, 2 * s0 + kTlGap);
            TimelineItem it;
            it.pos = order[i];
            it.x = x;
            it.y = kTlTop;
            it.s = s;
            it.group = static_cast<int>(t.groups.size());
            it.stack = true;
            it.col = static_cast<int>(t.cols.size());
            t.cols.push_back({x, s, static_cast<int>(t.items.size()), 1, it.group});
            t.items.push_back(it);
            for (size_t k = i; k < j && g.mosaic.size() < 4; ++k) g.mosaic.push_back(order[k]);
            width = s;
            t.maxS = std::max(t.maxS, s);
        } else {
            float cx = x;
            int col = 0;
            size_t k = i;
            while (k < j) {
                float s = std::round(s0 * (0.55f + 0.45f * std::exp(-col / 2.5f)));
                int rows = std::max(1, static_cast<int>((availH + kTlGap) / (s + kTlGap)));
                TimelineCol tc{cx, s, static_cast<int>(t.items.size()), 0, static_cast<int>(t.groups.size())};
                for (int r = 0; r < rows && k < j; ++r, ++k) {
                    TimelineItem it;
                    it.col = static_cast<int>(t.cols.size());
                    ++tc.count;
                    it.pos = order[k];
                    it.x = cx;
                    it.y = kTlTop + r * (s + kTlGap);
                    it.s = s;
                    it.group = static_cast<int>(t.groups.size());
                    t.items.push_back(it);
                }
                t.cols.push_back(tc);
                cx += s + kTlGap;
                ++col;
            }
            width = cx - kTlGap - x;
        }
        g.itemCount = static_cast<int>(t.items.size()) - g.firstItem;
        g.firstCol = g.itemCount > 0 ? t.items[g.firstItem].col : 0;
        g.lastCol = static_cast<int>(t.cols.size()) - 1;
        // Zeitangabe: so gross wie moeglich, hoechstens so breit wie die Gruppe (aber nicht kleiner als kTlBigMin)
        g.bigW = r_.MeasureText(g.big, fBig);
        g.subW = g.sub.empty() ? 0 : r_.MeasureText(g.sub, fSmall);
        float need = g.bigW + (g.sub.empty() ? 0 : 10 + g.subW);
        g.fs = kTlBig;
        if (need > width && need > 0) g.fs = std::max(kTlBigMin, std::floor(kTlBig * (width - (g.sub.empty() ? 0 : 10 + g.subW)) /
                                                                            std::max(1.0f, g.bigW)));
        g.labelW = g.bigW * g.fs / kTlBig + (g.sub.empty() ? 0 : 10 + g.subW);
        g.x1 = x + std::max(width, g.labelW);
        x = g.x1 + kTlGroupGap;
        t.groups.push_back(std::move(g));
    }
    t.contentW = t.items.empty() ? 0 : x - kTlGroupGap + kTlPad;

    // Lage wiederherstellen
    if (!anchorPath.empty()) {
        for (auto& it : t.items)
            if (entries_[visible_[it.pos]].path == anchorPath) {
                t.scroll = t.target = it.x - anchorOff;
                break;
            }
    }
    float minScroll = TimelineScrollMin(), maxScroll = TimelineScrollMax();
    t.scroll = std::clamp(t.scroll, minScroll, maxScroll);
    t.target = std::clamp(t.target, minScroll, maxScroll);
    t.colX.clear();
    TimelineMapColumns();
}

// Bildlauf: am Anfang und Ende so weit, dass die erste bzw. letzte Spalte in der Mitte steht (dort ist sie am groessten)
float App::TimelineScrollMin() const {
    const auto& t = tl_;
    if (t.cols.empty()) return 0;
    const float vw = gridRc_.right - gridRc_.left;
    return std::min(0.0f, t.cols.front().x + t.cols.front().s / 2 - vw / 2);
}

float App::TimelineScrollMax() const {
    const auto& t = tl_;
    if (t.cols.empty()) return 0;
    const float vw = gridRc_.right - gridRc_.left;
    return std::max({TimelineScrollMin(), t.contentW - vw, t.cols.back().x + t.cols.back().s / 2 - vw / 2});
}

// Spalten neben der Bildmitte schrumpfen (um ihre Ecke oben links); die Abstaende zwischen den Spalten bleiben
// dabei gleich: von der Spalte in der Mitte aus wird nach rechts und links neu aneinandergereiht
void App::TimelineMapColumns() {
    auto& t = tl_;
    const size_t n = t.cols.size();
    t.colX.assign(n, 0);
    t.colK.assign(n, 1);
    t.colFog.assign(n, 0);
    if (n == 0) return;
    const float center = t.scroll + (gridRc_.right - gridRc_.left) / 2;
    size_t lo = 0, hi = n;   // letzte Spalte, die links der Mitte beginnt
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (t.cols[mid].x <= center) lo = mid + 1;
        else hi = mid;
    }
    const size_t c0 = lo > 0 ? lo - 1 : 0;
    auto shrink = [&](size_t c, float x) {
        float fog = 0;
        t.colK[c] = TimelineShrink(x, t.cols[c].s, fog);
        t.colFog[c] = fog;
    };
    t.colX[c0] = t.cols[c0].x;
    shrink(c0, t.colX[c0]);
    for (size_t c = c0 + 1; c < n; ++c) {
        float gap = t.cols[c].x - (t.cols[c - 1].x + t.cols[c - 1].s);
        t.colX[c] = t.colX[c - 1] + t.cols[c - 1].s * t.colK[c - 1] + gap;
        shrink(c, t.colX[c]);
    }
    for (size_t c = c0; c-- > 0;) {
        float gap = t.cols[c + 1].x - (t.cols[c].x + t.cols[c].s);
        float est = t.colX[c + 1] - gap - t.cols[c].s;   // Lage ungefaehr (fuer die Staerke), dann genau
        shrink(c, est);
        t.colX[c] = t.colX[c + 1] - gap - t.cols[c].s * t.colK[c];
    }
}

// Lage einer Kachel nach dem Aneinanderreihen (Welt: x ab Beginn des Zeitstrahls, y ab Oberkante)
D2D1_RECT_F App::TimelineItemRect(const TimelineItem& it) const {
    const auto& t = tl_;
    if (it.col < 0 || it.col >= static_cast<int>(t.colX.size())) return D2D1::RectF(it.x, it.y, it.x + it.s, it.y + it.s);
    float k = t.colK[it.col];
    float x = t.colX[it.col];
    float top = kTlTop + (it.y - kTlTop) * k;
    return D2D1::RectF(x, top, x + it.s * k, top + it.s * k);
}

// Index des ersten Elements, dessen Spalte bis x reicht (Elemente sind nach x geordnet)
int App::TimelineFirstAt(float wx) const {
    const auto& it = tl_.items;
    int lo = 0, hi = static_cast<int>(it.size());
    float limit = wx - tl_.maxS;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (it[mid].x < limit) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Spalten neben der Bildmitte: etwas kleiner (um die Ecke oben links) und abgedunkelt; k = Groesse, fog = Abdunklung
// Die ganze Spalte schrumpft um ihre Ecke oben links (Abstaende eingeschlossen), die Kacheln darin gleich stark
float App::TimelineShrink(float x, float s, float& fog) const {
    const float vw = std::max(1.0f, gridRc_.right - gridRc_.left);
    const float cx = x - tl_.scroll + s / 2;   // Mitte der Spalte, vom linken Rand
    float d = std::clamp(std::fabs(cx - vw / 2) / (vw / 2), 0.0f, 1.0f);
    float f = d * d;
    fog = (dark_ ? 0.66f : 0.56f) * f;
    return 1.0f - 0.19f * f;
}



// Element unter dem Mauszeiger: Index in tl_.items oder -1
int App::TimelineItemAt(float x, float y) const {
    const auto& t = tl_;
    if (y >= gridRc_.bottom - kTlSlider) return -1;
    float wx = x - gridRc_.left + t.scroll, wy = y - gridRc_.top;
    for (size_t c = 0; c < t.colX.size() && c < t.cols.size(); ++c) {
        if (t.colX[c] > wx) break;
        if (wx >= t.colX[c] + t.cols[c].s * t.colK[c]) continue;
        for (int i = t.cols[c].first; i < t.cols[c].first + t.cols[c].count; ++i) {
            D2D1_RECT_F r = TimelineItemRect(t.items[i]);
            if (wx >= r.left && wx < r.right && wy >= r.top && wy < r.bottom) return i;
        }
    }
    return -1;
}

// Position in visible_ oder -1 (zusammengefasste Gruppen zaehlen nicht)
int App::TimelinePick(float x, float y) const {
    int i = TimelineItemAt(x, y);
    return i < 0 || tl_.items[i].stack ? -1 : tl_.items[i].pos;
}

// Auf- bzw. Zuklappen, animiert: beim Aufklappen fliegen die Kacheln nacheinander aus der Gruppenkachel an ihren
// Platz (werden dabei kleiner und blenden ein), alle anderen Kacheln und Zeitangaben gleiten an ihre neue Stelle
void App::TimelineToggle(int group) {
    auto& t = tl_;
    if (group < 0 || group >= static_cast<int>(t.groups.size())) return;
    int64_t k = t.groups[group].key;
    const bool expanding = !t.expanded.count(k);
    std::unordered_map<int, D2D1_RECT_F> old;   // Position in visible_ -> bisherige Lage (Welt)
    std::unordered_map<int64_t, float> oldX0;
    D2D1_RECT_F stackRc{};
    bool haveStack = false;
    TimelineMapColumns();
    for (const auto& it : t.items) {
        D2D1_RECT_F r = TimelineItemRect(it);   // so, wie die Kachel gerade zu sehen ist
        old[it.pos] = r;
        if (it.stack && t.groups[it.group].key == k) { stackRc = r; haveStack = true; }
    }
    for (const auto& g : t.groups)
        oldX0[g.key] = g.firstCol < static_cast<int>(t.colX.size()) ? t.colX[g.firstCol] : g.x0;
    if (expanding) t.expanded.insert(k);
    else t.expanded.erase(k);
    t.dirty = true;
    TimelineSync();
    const size_t n = t.items.size();
    t.from.assign(n, D2D1_RECT_F{});
    t.delay.assign(n, 0.0f);
    int rank = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& it = t.items[i];
        D2D1_RECT_F to = TimelineItemRect(it);
        bool inGroup = t.groups[it.group].key == k;
        if (expanding && inGroup && haveStack) {
            t.from[i] = stackRc;
            t.delay[i] = static_cast<float>(std::min(kTlStaggerMax, rank * kTlStaggerMs));
            ++rank;
        } else {
            auto f = old.find(it.pos);
            t.from[i] = f != old.end() ? f->second : to;
        }
    }
    for (auto& g : t.groups) {
        auto f = oldX0.find(g.key);
        g.fromX0 = f != oldX0.end() ? f->second : (g.firstCol < static_cast<int>(t.colX.size()) ? t.colX[g.firstCol] : g.x0);
    }
    t.anim = true;
    t.animT0 = NowMs();
    StartAnimation();
    // nach dem Zuklappen den Anfang der Gruppe zeigen, falls er links aus dem Bild ist
    if (group < static_cast<int>(t.groups.size()) && t.groups[group].x0 < t.scroll) {
        t.target = std::max(0.0f, t.groups[group].x0 - kTlPad);
        StartAnimation();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Uebersicht unten: der angeklickte Zeitpunkt kommt in die Mitte (wie eine Bildlaufleiste, absolut)
void App::TimelineScrubAt(float x, bool immediate) {
    auto& t = tl_;
    float w = std::max(1.0f, t.chartW);   // Breite der Uebersicht
    float vw = gridRc_.right - gridRc_.left;
    float minScroll = TimelineScrollMin(), maxScroll = TimelineScrollMax();
    float wx = std::clamp((x - t.chartL) / w, 0.0f, 1.0f) * t.contentW;
    t.target = std::clamp(wx - vw / 2, minScroll, maxScroll);
    if (immediate) t.scroll = t.target;
    StartAnimation();
}

// Ziehen verschiebt ueberall die Ansicht; ein Klick ohne Ziehen waehlt aus (bzw. oeffnet, Einstellung "ein Klick")
bool App::TimelineMouseDown(float x, float y, WPARAM keys) {
    auto& t = tl_;
    if (!InRect(gridRc_, x, y) || t.items.empty()) return false;
    if (y >= gridRc_.bottom - kTlSlider) return true;   // Uebersicht unten: nur ihr Treffer reagiert
    t.pan = true;
    t.panMoved = false;
    t.panX = x;
    t.vel = 0;   // ein Griff haelt den Zeitstrahl an
    t.lastX = x;
    t.lastT = NowMs();
    t.target = t.scroll;
    t.panScroll = t.scroll;
    t.scrub = false;
    t.pressPos = TimelinePick(x, y);
    t.pressKeys = keys;
    return true;
}

bool App::TimelineMouseMove(float x, float /*y*/) {
    auto& t = tl_;
    if (t.scrub) {
        TimelineScrubAt(x, true);
        return true;
    }
    if (t.pan) {
        if (std::fabs(x - t.panX) > GetSystemMetrics(SM_CXDRAG)) t.panMoved = true;
        if (!t.panMoved) return true;
        float minScroll = TimelineScrollMin(), maxScroll = TimelineScrollMax();
        // Geschwindigkeit fuer das Weiterlaufen nach dem Loslassen (geglaettet, DIP je ms)
        double now = NowMs();
        double dtm = now - t.lastT;
        if (t.lastT > 0 && dtm > 0.5 && dtm < 100) {
            float inst = static_cast<float>(-(x - t.lastX) / dtm);
            float w = static_cast<float>(std::min(1.0, dtm / 40.0));
            t.vel = t.vel * (1 - w) + inst * w;
        }
        t.lastX = x;
        t.lastT = now;
        t.scroll = t.target = std::clamp(t.panScroll - (x - t.panX), minScroll, maxScroll);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return false;
}

bool App::TimelineMouseUp() {
    auto& t = tl_;
    if (t.scrub) {
        t.scrub = false;
        return true;
    }
    if (t.pan) {
        t.pan = false;
        // angeschoben: weiterlaufen lassen (nur, wenn die Maus bis zuletzt bewegt wurde)
        if (t.panMoved && NowMs() - t.lastT < 60 && std::fabs(t.vel) > 0.15f) {
            t.vel = std::clamp(t.vel, -12.0f, 12.0f);
            StartAnimation();
        } else {
            t.vel = 0;
        }
        t.lastT = 0;
        if (!t.panMoved) {
            int pos = t.pressPos;
            if (pos < 0 || pos >= static_cast<int>(visible_.size())) {
                selected_.clear();   // Klick ins Leere: Auswahl aufheben
            } else {
                uint32_t idx = visible_[pos];
                if (t.pressKeys & MK_CONTROL) {
                    if (selected_.count(idx)) selected_.erase(idx);
                    else selected_.insert(idx);
                } else {
                    selected_.clear();
                    selected_.insert(idx);
                }
                focus_ = anchor_ = pos;
                if (settings_.singleClick && !(t.pressKeys & (MK_CONTROL | MK_SHIFT))) {
                    noDblUntil_ = GetTickCount() + GetDoubleClickTime();
                    OpenPos(pos);
                    return true;
                }
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return false;
}

void App::TimelineWheel(int delta) {
    auto& t = tl_;
    float minScroll = TimelineScrollMin(), maxScroll = TimelineScrollMax();
    t.vel = 0;
    t.target = std::clamp(t.target - delta / 120.0f * 260.0f, minScroll, maxScroll);
    StartAnimation();
}

bool App::TimelineKey(WPARAM vk) {
    auto& t = tl_;
    float vw = gridRc_.right - gridRc_.left;
    float minScroll = TimelineScrollMin(), maxScroll = TimelineScrollMax();
    t.vel = 0;
    switch (vk) {
    case VK_LEFT: t.target -= vw * 0.5f; break;
    case VK_RIGHT: t.target += vw * 0.5f; break;
    case VK_PRIOR: t.target -= vw * 0.9f; break;
    case VK_NEXT: t.target += vw * 0.9f; break;
    case VK_HOME: t.target = minScroll; break;
    case VK_END: t.target = maxScroll; break;
    default: return false;
    }
    t.target = std::clamp(t.target, minScroll, maxScroll);
    StartAnimation();
    return true;
}

// Ein Animationsschritt: weiches Scrollen zum Ziel (Mausrad, Tasten, Klick in die Uebersicht)
bool App::TimelineStep(float dt) {
    auto& t = tl_;
    bool more = false;
    float vw = gridRc_.right - gridRc_.left;
    float minScroll = TimelineScrollMin(), maxScroll = TimelineScrollMax();
    if (!t.pan && t.vel != 0) {
        // Weiterlaufen nach dem Anschieben, sanft abbremsend
        t.scroll += t.vel * dt;
        t.vel *= std::exp(-dt / 380.0f);
        if (t.scroll <= minScroll || t.scroll >= maxScroll || std::fabs(t.vel) < 0.01f) t.vel = 0;
        t.scroll = std::clamp(t.scroll, minScroll, maxScroll);
        t.target = t.scroll;
        more = true;
    }
    if (t.anim) {
        if (NowMs() - t.animT0 > kTlAnimMs + kTlStaggerMax) t.anim = false;
        else more = true;
    }
    t.target = std::clamp(t.target, minScroll, maxScroll);
    if (!t.pan) {
        float d = t.target - t.scroll;
        if (std::fabs(d) < 0.5f) t.scroll = t.target;
        else {
            t.scroll += d * (1.0f - std::exp(-dt / 70.0f));
            more = true;
        }
    }
    return more;
}

void App::PaintTimeline(ID2D1DeviceContext* dc) {
    TimelineSync();
    TimelineMapColumns();
    auto& t = tl_;
    const D2D1_RECT_F area = gridRc_;
    const float vw = area.right - area.left;
    const float vh0 = area.bottom - area.top;
    const D2D1_COLOR_F text = dark_ ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.88f);
    const D2D1_COLOR_F text2 = dark_ ? D2D1::ColorF(1, 1, 1, 0.6f) : D2D1::ColorF(0, 0, 0, 0.55f);
    const D2D1_COLOR_F cardBg = dark_ ? D2D1::ColorF(0.24f, 0.24f, 0.25f, 1) : D2D1::ColorF(0.9f, 0.9f, 0.91f, 1);
    if (t.items.empty()) {
        IDWriteTextFormat* fw = r_.TextWrap(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        r_.DrawWrapped(T(L"Keine Bilder oder Videos in dieser Ansicht."), area.left + 20, area.top + 40, vw - 40, fw, text2);
        return;
    }
    const float ox = area.left - t.scroll, oy = area.top;
    // waehrend das Fenster in der Hoehe gezogen wird: bisherige Anordnung passend skaliert (s. TimelineSync)
    const float zoom = (t.viewH > kTlSlider + 1 && std::fabs(t.viewH - vh0) >= 0.5f)
                           ? std::max(0.2f, (vh0 - kTlSlider) / (t.viewH - kTlSlider))
                           : 1.0f;
    const bool moving = t.scrub || t.pan || t.scroll != t.target || t.anim || t.vel != 0 || autoScroll_;
    const double now = NowMs();
    const float uAll = t.anim ? EaseOutCubic(static_cast<float>(std::clamp((now - t.animT0) / kTlAnimMs, 0.0, 1.0))) : 1.0f;
    const D2D1_COLOR_F fogC = dark_ ? D2D1::ColorF(0.17f, 0.17f, 0.17f, 1) : D2D1::ColorF(0.975f, 0.975f, 0.975f, 1);
    const float rad = 6;
    dc->PushAxisAlignedClip(D2D1::RectF(area.left, area.top, area.right, area.bottom - kTlSlider + 6), D2D1_ANTIALIAS_MODE_ALIASED);
    const float viewW = vw / zoom;   // sichtbare Breite in Weltkoordinaten
    if (zoom != 1.0f) dc->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom, D2D1::Point2F(area.left, area.top)));

    // 1) Zeitangaben (hinter den Kacheln); bleiben am linken Rand stehen, bis die Gruppe vorbei ist
    IDWriteTextFormat* fSmall = r_.Text(kTlSmall, DWRITE_FONT_WEIGHT_NORMAL);
    for (int gi = 0; gi < static_cast<int>(t.groups.size()); ++gi) {
        const auto& g = t.groups[gi];
        if (g.firstCol >= static_cast<int>(t.colX.size()) || g.lastCol >= static_cast<int>(t.colX.size())) continue;
        const float mx0 = t.colX[g.firstCol];   // Beginn der Gruppe nach dem Aneinanderreihen
        const float mx1 = t.colX[g.lastCol] + t.cols[g.lastCol].s * t.colK[g.lastCol];
        const float gdx = t.anim ? (g.fromX0 - mx0) * (1.0f - uAll) : 0.0f;   // gleitet beim Auf-/Zuklappen mit
        const float gx0 = mx0 + gdx, gx1 = gx0 + std::max(mx1 - mx0, g.labelW);
        if (gx1 + ox < area.left || gx0 + ox > area.left + viewW) continue;
        float lx = std::clamp(area.left + kTlPad, gx0 + ox, std::max(gx0 + ox, gx1 + ox - g.labelW));
        IDWriteTextFormat* fb = r_.Text(g.fs, kTlWeight);
        float bw = g.bigW * g.fs / kTlBig;
        float top = oy + kTlTop - g.fs * 1.02f;   // unterer Teil der Ziffern verschwindet hinter den Kacheln
        DrawText(dc, g.big, D2D1::RectF(lx - 2, top, lx + bw + 40, top + g.fs * 1.4f), fb, text);
        if (!g.sub.empty())
            DrawText(dc, g.sub, D2D1::RectF(lx + bw + 10, oy + kTlTop - 28, lx + bw + 10 + g.subW + 40, oy + kTlTop - 6),
                     fSmall, text2);
        // aufgeklappte grosse Gruppe: Klick auf die Zeitangabe klappt sie wieder zu
        if (g.count > kTlCollapse && !g.collapsed) {
            D2D1_RECT_F hr = D2D1::RectF(lx - 6, oy + 2, lx + g.labelW + 30, oy + kTlTop - 4);
            int idx = static_cast<int>(hits_.size());
            hits_.push_back({hr, HitType::TimelineGroup, gi});
            wchar_t gc[2] = {0xE70E, 0};   // Pfeil nach oben: zuklappen
            float cx = lx + g.labelW + (g.sub.empty() ? 10 : 6);
            DrawText(dc, gc, D2D1::RectF(cx, oy + kTlTop - 28, cx + 20, oy + kTlTop - 6), r_.Icon(11),
                     hoverHit_ == idx ? accent_ : text2);
        }
    }

    // 2) Kacheln (nur die sichtbaren)
    ComPtr<ID2D1BitmapBrush1> brush;
    auto drawThumb = [&](const FileEntry& e, const D2D1_RECT_F& rc, float r, int prio) {
        EnsureThumb(e, prio, frame_);
        auto bi = bmps_.find(e.path);
        if (bi == bmps_.end() || !bi->second.bmp || e.offline) {
            FillRound(dc, rc, r, cardBg);
            return;
        }
        ID2D1Bitmap1* bmp = bi->second.bmp.Get();
        if (!brush) {
            dc->CreateBitmapBrush(bmp, D2D1::BitmapBrushProperties1(), D2D1::BrushProperties(), &brush);
            if (!brush) return;
        }
        brush->SetBitmap(bmp);
        brush->SetInterpolationMode1(moving ? D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR
                                            : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        // quadratischer Ausschnitt aus der Mitte
        D2D1_SIZE_F sz = bmp->GetSize();
        float side = std::max(1.0f, std::min(sz.width, sz.height));
        float k = (rc.right - rc.left) / side;
        brush->SetTransform(D2D1::Matrix3x2F::Translation(-(sz.width - side) / 2, -(sz.height - side) / 2) *
                            D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(rc.left, rc.top));
        dc->FillRoundedRectangle(D2D1::RoundedRect(rc, r, r), brush.Get());
    };
    const float wx0 = t.scroll, wx1 = t.scroll + viewW;
    const bool animItems = t.anim && t.from.size() == t.items.size();
    std::vector<int>& todo = t.drawList;
    todo.clear();
    if (animItems) {
        for (int i = 0; i < static_cast<int>(t.items.size()); ++i) todo.push_back(i);
    } else {
        // Spalten im Bild (nach dem Aneinanderreihen; colX steigt mit c)
        size_t lo = 0, hi = t.colX.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (t.colX[mid] + t.cols[mid].s < wx0) lo = mid + 1;
            else hi = mid;
        }
        for (size_t c = lo; c < t.colX.size() && t.colX[c] <= wx1; ++c)
            for (int i = t.cols[c].first; i < t.cols[c].first + t.cols[c].count; ++i) todo.push_back(i);
    }
    for (int i : todo) {
        const auto& it = t.items[i];
        D2D1_RECT_F wr = TimelineItemRect(it);
        float fog = it.col >= 0 && it.col < static_cast<int>(t.colFog.size()) ? t.colFog[it.col] : 0.0f;
        if (animItems) {
            // von der bisherigen Lage (bzw. der Gruppenkachel) zur neuen, je Kachel etwas versetzt
            float u = EaseOutCubic(static_cast<float>(std::clamp((now - t.animT0 - t.delay[i]) / kTlAnimMs, 0.0, 1.0)));
            const D2D1_RECT_F& f = t.from[i];
            wr = D2D1::RectF(f.left + (wr.left - f.left) * u, f.top + (wr.top - f.top) * u, f.right + (wr.right - f.right) * u,
                             f.bottom + (wr.bottom - f.bottom) * u);
            if (wr.right < wx0 || wr.left > wx1) continue;
        }
        D2D1_RECT_F rc = D2D1::RectF(wr.left + ox, wr.top + oy, wr.right + ox, wr.bottom + oy);
        const float ss = rc.right - rc.left;
        auto darken = [&] {
            if (fog > 0.01f) FillRound(dc, rc, rad, D2D1::ColorF(fogC.r, fogC.g, fogC.b, fog));
        };
        if (it.stack) {
            // zusammengefasste Gruppe: 2 x 2 Vorschaubilder, abgedunkelt, mit Anzahl
            const auto& g = t.groups[it.group];
            int idx = static_cast<int>(hits_.size());
            hits_.push_back({rc, HitType::TimelineGroup, it.group});
            float h = (ss - kTlGap) / 2;
            for (int m = 0; m < 4; ++m) {
                D2D1_RECT_F mr = D2D1::RectF(rc.left + (m % 2) * (h + kTlGap), rc.top + (m / 2) * (h + kTlGap), 0, 0);
                mr.right = mr.left + h;
                mr.bottom = mr.top + h;
                if (m < static_cast<int>(g.mosaic.size())) drawThumb(entries_[visible_[g.mosaic[m]]], mr, rad, 0);
                else FillRound(dc, mr, rad, cardBg);
            }
            FillRound(dc, rc, rad, D2D1::ColorF(0, 0, 0, 0.5f - 0.12f * Hov(idx)));
            IDWriteTextFormat* fc = r_.Text(std::round(std::clamp(ss * 0.16f, 22.0f, 48.0f)), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                            DWRITE_TEXT_ALIGNMENT_CENTER);
            IDWriteTextFormat* fh = r_.Text(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            float my = (rc.top + rc.bottom) / 2;
            DrawText(dc, FormatThousands(static_cast<uint64_t>(g.count)), D2D1::RectF(rc.left, my - 44, rc.right, my + 10), fc,
                     D2D1::ColorF(1, 1, 1, 1));
            DrawText(dc, T(L"Klicken zum Aufklappen"), D2D1::RectF(rc.left + 8, my + 12, rc.right - 8, my + 34), fh,
                     D2D1::ColorF(1, 1, 1, 0.85f));
            darken();
            continue;
        }
        const FileEntry& e = entries_[visible_[it.pos]];
        drawThumb(e, rc, rad, 0);
        {   // fuer das Oeffnen/Schliessen des Betrachters: Lage im Fenster (mit Zoom der Zeitleiste)
            D2D1::Matrix3x2F m;
            dc->GetTransform(&m);
            D2D1_POINT_2F a = m.TransformPoint(D2D1::Point2F(rc.left, rc.top)), b = m.TransformPoint(D2D1::Point2F(rc.right, rc.bottom));
            drawnThumbs_[it.pos] = D2D1::RectF(a.x, a.y, b.x, b.y);
        }
        if (e.kind == Kind::Video) {
            float c = std::clamp(ss * 0.09f, 9.0f, 15.0f);
            D2D1_POINT_2F p = D2D1::Point2F(rc.left + c + 6, rc.bottom - c - 6);
            dc->FillEllipse(D2D1::Ellipse(p, c, c), r_.Brush(D2D1::ColorF(0, 0, 0, 0.55f)));
            wchar_t g[2] = {0xE768, 0};
            DrawText(dc, g, D2D1::RectF(p.x - c, p.y - c, p.x + c, p.y + c), r_.Icon(c * 0.8f), D2D1::ColorF(1, 1, 1, 1));
        }
        bool sel = selected_.count(visible_[it.pos]) != 0;
        darken();
        // wie in der Wolke: ausgewaehlt ein Rahmen in Akzentfarbe, unter der Maus ein weisser Rahmen
        if (sel) {
            D2D1_RECT_F fr = D2D1::RectF(rc.left - 1.5f, rc.top - 1.5f, rc.right + 1.5f, rc.bottom + 1.5f);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(fr, rad + 1.5f, rad + 1.5f), r_.Brush(accent_), 2.5f);
        } else if (float ha = PosHov(it.pos); ha > 0) {   // unter der Maus (blendet mit Animationen schnell aus)
            D2D1_RECT_F hr = D2D1::RectF(rc.left - 1, rc.top - 1, rc.right + 1, rc.bottom + 1);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(hr, rad + 1, rad + 1), r_.Brush(D2D1::ColorF(1, 1, 1, 0.95f * ha)), 2.0f);
        } else if (it.pos == focus_ && !moving) {
            D2D1_RECT_F in = D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f, rc.right - 0.5f, rc.bottom - 0.5f);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(in, rad, rad), r_.Brush(text2), 1.0f);
        }
    }
    // Vorausladen: eine Fensterbreite links und rechts
    for (int i = TimelineFirstAt(wx0 - vw); i < static_cast<int>(t.items.size()); ++i) {
        const auto& it = t.items[i];
        if (it.x > wx1 + vw) break;
        if (it.stack || (it.x + it.s >= wx0 && it.x <= wx1)) continue;
        EnsureThumb(entries_[visible_[it.pos]], 1, frame_);
    }
    if (zoom != 1.0f) dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->PopAxisAlignedClip();

    // 3) Uebersicht statt Bildlaufleiste: je Zeitraum ein Balken (Breite wie auf dem Zeitstrahl, Hoehe nach Anzahl),
    //    darueber der sichtbare Ausschnitt; Klicken oder Ziehen springt dorthin. Darunter die Jahre.
    const float cl = area.left + 20, cr = area.right - 20, cw = std::max(1.0f, cr - cl);
    const float barTop = area.bottom - kTlSlider + 10, barBottom = area.bottom - 22;
    t.chartL = cl;   // linker Rand und Breite der Uebersicht (fuer TimelineScrubAt)
    t.chartW = cw;
    hits_.push_back({D2D1::RectF(cl - 6, barTop - 6, cr + 6, area.bottom), HitType::TimelineScrub, 0});
    const float kx = cw / std::max(1.0f, t.contentW);
    int maxCount = 1;
    for (const auto& g : t.groups) maxCount = std::max(maxCount, g.count);
    const float vx0 = cl + std::max(0.0f, t.scroll) * kx, vx1 = cl + std::clamp(t.scroll + vw, 0.0f, t.contentW) * kx;
    const D2D1_COLOR_F barC = dark_ ? D2D1::ColorF(1, 1, 1, 0.26f) : D2D1::ColorF(0, 0, 0, 0.2f);
    dc->FillRectangle(D2D1::RectF(cl, barBottom, cr, barBottom + 1), r_.Brush(barC));
    IDWriteTextFormat* fy = r_.Text(11, DWRITE_FONT_WEIGHT_NORMAL);
    int lastYear = -1;
    float lastLabel = -1e9f;
    for (const auto& g : t.groups) {
        float x0 = cl + g.x0 * kx, x1 = cl + g.x1 * kx;
        if (x1 - x0 < 1) x1 = x0 + 1;
        float h = std::max(2.0f, (barBottom - barTop) * std::sqrt(static_cast<float>(g.count) / maxCount));
        bool in = x1 >= vx0 && x0 <= vx1;
        D2D1_RECT_F br = D2D1::RectF(x0, barBottom - h, std::max(x0 + 1, x1 - (x1 - x0 > 4 ? 1.0f : 0.0f)), barBottom);
        dc->FillRectangle(br, r_.Brush(in ? WithAlpha(accent_, 0.85f) : barC));
        int year = static_cast<int>(g.level == 2 ? g.key : g.level == 1 ? g.key / 100 : g.key / 10000);
        if (year != lastYear && year > 0 && x0 - lastLabel > 40) {
            DrawText(dc, std::to_wstring(year), D2D1::RectF(x0, barBottom + 3, x0 + 60, area.bottom - 2), fy, text2);
            lastLabel = x0;
        }
        lastYear = year;
    }
    if (t.contentW > vw) {
        D2D1_RECT_F vr = D2D1::RectF(vx0, barTop - 4, std::max(vx0 + 6, vx1), barBottom + 2);
        FillRound(dc, vr, 3, WithAlpha(accent_, 0.16f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(vr, 3, 3), r_.Brush(WithAlpha(accent_, 0.9f)), 1.2f);
    }
    if (moving) InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace peek
