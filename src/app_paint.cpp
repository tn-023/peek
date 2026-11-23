#include "core/query.h"
#include <unordered_map>
// App: Darstellung (Direct2D)
#include "app.h"
#include "core/sheetlayout.h"
#include "imaging.h"
#include "sandbox.h"
#include "core/textutil.h"
#include "core/atlaslayout.h"

#include <cmath>

namespace peek {

namespace {

struct Theme {
    D2D1_COLOR_F bg, layer, status, text, text2, text3, hover, press, stroke, chip, chipHover, overlay, danger, onAccent;
};

Theme MakeTheme(bool dark, bool backdrop) {
    Theme t;
    if (dark) {
        t.bg = backdrop ? D2D1::ColorF(0, 0, 0, 0) : D2D1::ColorF(0.125f, 0.125f, 0.125f, 1);
        t.layer = D2D1::ColorF(0.17f, 0.17f, 0.17f, backdrop ? 0.6f : 1.0f);
        t.status = D2D1::ColorF(0.13f, 0.13f, 0.13f, backdrop ? 0.5f : 1.0f);
        t.text = D2D1::ColorF(1, 1, 1, 0.92f);
        t.text2 = D2D1::ColorF(1, 1, 1, 0.66f);
        t.text3 = D2D1::ColorF(1, 1, 1, 0.42f);
        t.hover = D2D1::ColorF(1, 1, 1, 0.06f);
        t.press = D2D1::ColorF(1, 1, 1, 0.1f);
        t.stroke = D2D1::ColorF(1, 1, 1, 0.08f);
        t.chip = D2D1::ColorF(1, 1, 1, 0.06f);
        t.chipHover = D2D1::ColorF(1, 1, 1, 0.11f);
        t.danger = D2D1::ColorF(1.0f, 0.6f, 0.55f, 1);
    } else {
        t.bg = backdrop ? D2D1::ColorF(0, 0, 0, 0) : D2D1::ColorF(0.95f, 0.95f, 0.95f, 1);
        t.layer = D2D1::ColorF(1, 1, 1, backdrop ? 0.65f : 1.0f);
        t.status = D2D1::ColorF(0.97f, 0.97f, 0.97f, backdrop ? 0.55f : 1.0f);
        t.text = D2D1::ColorF(0, 0, 0, 0.9f);
        t.text2 = D2D1::ColorF(0, 0, 0, 0.62f);
        t.text3 = D2D1::ColorF(0, 0, 0, 0.4f);
        t.hover = D2D1::ColorF(0, 0, 0, 0.04f);
        t.press = D2D1::ColorF(0, 0, 0, 0.07f);
        t.stroke = D2D1::ColorF(0, 0, 0, 0.08f);
        t.chip = D2D1::ColorF(1, 1, 1, 0.75f);
        t.chipHover = D2D1::ColorF(0.95f, 0.95f, 0.95f, 0.95f);
        t.danger = D2D1::ColorF(0.77f, 0.17f, 0.11f, 1);
    }
    t.onAccent = D2D1::ColorF(1, 1, 1, 1);   // Text auf ausgewaehlten Pillen: immer weiss
    t.overlay = D2D1::ColorF(0.03f, 0.03f, 0.03f, 1.0f);
    return t;
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

std::wstring Thousands(uint64_t n) { return FormatThousands(n); }

float Ease(float t) { return t * t * (3 - 2 * t); }

std::wstring FormatDate(int64_t unix) {
    if (unix <= 0) return {};
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(unix) * 10000000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart}, lft;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    return FormatDayTime(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

// Spalten der Detailansicht: Anzeige-Reihenfolge -> Sortierschluessel
const int kColSort[5] = {0, 1, 4, 2, 3};   // Name, Datum, Typ, Groesse, Aufloesung
const wchar_t* kColName[5] = {N_(L"Name"), N_(L"Datum"), N_(L"Typ"), N_(L"Größe"), N_(L"Auflösung")};

} // namespace

float App::SidebarWidth() const { return sidebarW_ * Ease(std::clamp(sidebarAnim_, 0.0f, 1.0f)); }
float App::InfoWidth() const { return 300.0f * Ease(std::clamp(infoAnim_, 0.0f, 1.0f)); }
float App::ViewerAreaW() const { return W_ - InfoWidth(); }

void App::FillRound(ID2D1DeviceContext* dc, const D2D1_RECT_F& rc, float r, const D2D1_COLOR_F& c) {
    if (r <= 0) dc->FillRectangle(rc, r_.Brush(c));
    else dc->FillRoundedRectangle(D2D1::RoundedRect(rc, r, r), r_.Brush(c));
}

void App::DrawText(ID2D1DeviceContext* dc, const std::wstring& s, const D2D1_RECT_F& rc, IDWriteTextFormat* f,
                   const D2D1_COLOR_F& c) {
    if (!f || s.empty() || rc.right <= rc.left) return;
    dc->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, rc, r_.Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

bool App::DrawBitmapFit(ID2D1DeviceContext* dc, Bmp* b, const D2D1_RECT_F& box, float maxScale, float opacity,
                        D2D1_RECT_F* drawn) {
    if (!b || !b->bmp) return false;
    D2D1_SIZE_U px = b->bmp->GetPixelSize();
    if (!px.width || !px.height) return false;
    float bw = box.right - box.left, bh = box.bottom - box.top;
    float sc = std::min({bw / px.width, bh / px.height, maxScale});
    float w = px.width * sc, h = px.height * sc;
    D2D1_RECT_F dst = {box.left + (bw - w) / 2, box.top + (bh - h) / 2, 0, 0};
    dst.right = dst.left + w;
    dst.bottom = dst.top + h;
    float s = r_.scale();
    dst.left = std::round(dst.left * s) / s;
    dst.top = std::round(dst.top * s) / s;
    dst.right = std::round(dst.right * s) / s;
    dst.bottom = std::round(dst.bottom * s) / s;
    dc->DrawBitmap(b->bmp.Get(), dst, opacity,
                   sc * s < 1.0f ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC : D2D1_INTERPOLATION_MODE_LINEAR);
    if (drawn) *drawn = dst;
    return true;
}

// ---------------------------------------------------------------------------
// Layout und Zeilengeometrie
// ---------------------------------------------------------------------------

void App::Layout() {
    W_ = r_.widthDip();
    H_ = r_.heightDip();
    gridRc_ = D2D1::RectF(SidebarWidth(), topH_ + chipsH_, W_ - InfoWidth(), H_ - statusH_);
    bool list = settings_.viewMode == 1;
    if (bookmarksView_) {
        bookmarkBarRc_ = D2D1::RectF(gridRc_.left, gridRc_.top, gridRc_.right, gridRc_.top + 42);
        gridRc_.top += 42;
    }
    if (list) {
        headerRc_ = D2D1::RectF(gridRc_.left, gridRc_.top, gridRc_.right, gridRc_.top + 30);
        gridRc_.top += 30;
    }
    tileS_ = static_cast<float>(settings_.tileSize);
    float gw = std::max(1.0f, gridRc_.right - gridRc_.left - 16 - 12);
    float minCell = tileS_ + 20;
    cols_ = list ? 1 : std::max(1, static_cast<int>(gw / minCell));
    cellW_ = gw / cols_;
    cellH_ = list ? 34.0f : tileS_ + 26 + kLineH;
    RebuildRows();
    contentH_ = rowTop_.back() + 16;
    float maxScroll = std::max(0.0f, contentH_ - (gridRc_.bottom - gridRc_.top));
    scrollTarget_ = std::clamp(scrollTarget_, 0.0f, maxScroll);
    scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
}

void App::RebuildRows() {
    bool list = settings_.viewMode == 1;
    float nameW = list ? ListNameW() - 48.0f : cellW_ - 6 - 8;
    int nameWi = static_cast<int>(nameW);
    int groupKey = settings_.group ? (settings_.sortKey * 2 + (settings_.sortDesc ? 1 : 0)) : -1;
    if (rowsVersion_ == visibleVersion_ && rowsMode_ == settings_.viewMode && rowsCols_ == cols_ &&
        rowsNameW_ == nameWi && rowsTile_ == tileS_ && rowsGroupKey_ == groupKey && rowTop_.size() >= 1)
        return;
    rowsVersion_ = visibleVersion_;
    rowsMode_ = settings_.viewMode;
    rowsCols_ = cols_;
    rowsNameW_ = nameWi;
    rowsTile_ = tileS_;
    rowsGroupKey_ = groupKey;

    int n = static_cast<int>(visible_.size());
    rowTop_.clear();
    rowFirst_.clear();
    rowHeader_.clear();
    // Gruppen und natuerliche Namensbreiten haengen nur vom Inhalt ab, nicht von der Breite: einmal bestimmen.
    // (Beim Ein- und Ausfahren der Seitenleiste aendert sich die Breite in jedem Bild.)
    if (grpVersion_ != visibleVersion_ || grpKey_ != groupKey) {
        grpVersion_ = visibleVersion_;
        grpKey_ = groupKey;
        grpStart_.clear();
        groupLabels_.clear();
        groupCounts_.clear();
        if (groupKey >= 0) {
            ComputeDateBounds();
            for (int p = 0; p < n;) {
                std::wstring label = GroupLabel(entries_[visible_[p]]);
                int end = p + 1;
                while (end < n && GroupLabel(entries_[visible_[end]]) == label) ++end;
                grpStart_.push_back(p);
                groupLabels_.push_back(std::move(label));
                groupCounts_.push_back(static_cast<uint32_t>(end - p));
                p = end;
            }
        }
    }
    if (natVersion_ != visibleVersion_) {
        natVersion_ = visibleVersion_;
        nameNatural_.resize(n);
        for (int p = 0; p < n; ++p) nameNatural_[p] = r_.TextWidthFast(entries_[visible_[p]].name, kNameSize);
    }
    const float oneLine = nameW * 0.96f;   // dieselbe Grenze wie in BreakLines
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    float y = 0;
    auto addItemRow = [&](int first, int last) {
        size_t maxLines = 1;
        for (int p = first; p < last; ++p) {
            if (nameNatural_[p] <= oneLine) continue;   // passt in eine Zeile
            r_.BreakLines(entries_[visible_[p]].name, nameW, kNameSize, lines);
            maxLines = std::max(maxLines, lines.size());
        }
        rowTop_.push_back(y);
        rowFirst_.push_back(first);
        rowHeader_.push_back(-1);
        y += list ? std::max(34.0f, maxLines * kLineH + 16) : tileS_ + 26 + maxLines * kLineH;
    };
    int p = 0;
    int gi = 0;
    while (p < n) {
        // naechste Gruppe: alle folgenden Elemente mit gleicher Beschriftung
        int end = n;
        if (groupKey >= 0) {
            end = p + static_cast<int>(groupCounts_[gi]);
            rowTop_.push_back(y);
            rowFirst_.push_back(p);
            rowHeader_.push_back(gi);
            ++gi;
            y += kGroupH;
        }
        int step = list ? 1 : cols_;
        for (int q = p; q < end; q += step) addItemRow(q, std::min(end, q + step));
        p = end;
    }
    rowTop_.push_back(y);
    rowFirst_.push_back(n);
}

// Beschriftung der Gruppe eines Elements, passend zum Sortierkriterium (Ordner bilden immer eine eigene Gruppe)
std::wstring App::GroupLabel(const FileEntry& e) const {
    if (e.kind == Kind::Folder) return T(L"Ordner");
    auto dateGroup = [&](int64_t t) -> std::wstring {
        if (t <= 0) return T(L"Ohne Datum");
        if (t >= todayStart_ + 86400) return T(L"In der Zukunft");
        if (t >= todayStart_) return T(L"Heute");
        if (t >= todayStart_ - 86400) return T(L"Gestern");
        int64_t local = t + tzOffset_;
        int y; unsigned m, d;
        CivilFromDays(local / 86400, y, m, d);
        int ny; unsigned nm, nd;
        CivilFromDays((now_ + tzOffset_) / 86400, ny, nm, nd);
        if (y == ny && m == nm) return T(L"Früher in diesem Monat");
        if (y == ny) return MonthName(m);
        return std::wstring(MonthName(m)) + L" " + std::to_wstring(y);
    };
    switch (settings_.sortKey) {
    case 0: {   // Name: Anfangsbuchstabe
        wchar_t c = e.name.empty() ? L'#' : e.name[0];
        wchar_t up[2] = {c, 0};
        CharUpperBuffW(up, 1);
        c = up[0];
        if (c >= L'0' && c <= L'9') return L"0–9";
        if (c == L'Ä') return L"A";
        if (c == L'Ö') return L"O";
        if (c == L'Ü') return L"U";
        if (IsCharAlphaW(c)) return std::wstring(1, c);
        return L"#";
    }
    case 1: return dateGroup(e.Time());
    case 5: return e.taken ? dateGroup(e.taken) : std::wstring(T(L"Ohne Aufnahmedatum"));
    case 2: {
        const uint64_t KB = 1024, MB = KB * 1024, GB = MB * 1024;
        if (e.size == 0) return T(L"Leer");
        if (e.size < 16 * KB) return T(L"Sehr klein (< 16 KB)");
        if (e.size < MB) return T(L"Klein (16 KB – 1 MB)");
        if (e.size < 128 * MB) return T(L"Mittel (1 – 128 MB)");
        if (e.size < GB) return T(L"Groß (128 MB – 1 GB)");
        return T(L"Sehr groß (> 1 GB)");
    }
    case 3: {
        double mp = double(e.width) * e.height;
        if (mp <= 0) return T(L"Ohne Auflösung");
        if (mp < 1280.0 * 720) return T(L"Unter HD");
        if (mp < 1920.0 * 1080) return L"HD";
        if (mp < 3840.0 * 2160) return L"Full HD";
        if (mp < 20e6) return L"4K";
        return T(L"Über 20 Megapixel");
    }
    case 4: return const_cast<App*>(this)->TypeName(e);
    default: return {};
    }
}

// Gruppierung fuer die Wolke (-1 = keine Streifen)
int App::CloudGroupKey() const {
    if (!settings_.group) return -1;
    return (settings_.sortKey * 2 + (settings_.sortDesc ? 1 : 0)) * 4 + std::clamp(settings_.language, 0, 3);
}

// Gruppen der Wolke, hoechstens 6 Streifen. Ausgangspunkt sind die Gruppen des Rasters:
// - mehr als 6: benachbarte Gruppen zusammenfassen, Grenzen gleichen die Anzahl aus;
// - weniger als 6: grosse Gruppen nach dem Sortierwert weiter teilen (Namensanfaenge, Zeitraeume, Groessen,
//   Aufloesungen), Titel dann als Bereich. Sondergruppen (Ordner, ohne Datum ...) bleiben eigene Streifen.
std::vector<App::CloudGroup> App::CloudGroups() {
    std::vector<CloudGroup> out;
    const int n = static_cast<int>(visible_.size());
    if (n == 0) return out;
    ComputeDateBounds();
    const int key = settings_.sortKey;
    struct Run { std::wstring label; int start, count; bool special; int64_t t; };
    std::vector<Run> rs;
    const std::wstring special[] = {T(L"Ordner"), T(L"Ohne Datum"), T(L"Ohne Aufnahmedatum"), T(L"In der Zukunft"),
                                    T(L"Leer"), T(L"Ohne Auflösung")};
    auto timeOf = [&](const FileEntry& e) { return key == 5 ? e.taken : e.Time(); };
    for (int p = 0; p < n; ++p) {
        const FileEntry& e = entries_[visible_[p]];
        std::wstring label = GroupLabel(e);
        if (!rs.empty() && label == rs.back().label) { ++rs.back().count; continue; }
        bool sp = std::find(std::begin(special), std::end(special), label) != std::end(special);
        rs.push_back({std::move(label), p, 1, sp, timeOf(e)});
    }
    constexpr int kMax = 6;
    std::vector<std::pair<int, int>> buckets;   // Bereiche von Gruppen [a, b)
    const int nr = static_cast<int>(rs.size());
    if (nr <= kMax) {
        for (int i = 0; i < nr; ++i) buckets.push_back({i, i + 1});
    } else {
        struct Seg { int a, b; int64_t cnt; int k; };
        std::vector<Seg> segs;
        int nSpecial = 0;
        for (int i = 0; i < nr;) {
            if (rs[i].special) { ++nSpecial; ++i; continue; }
            int j = i;
            int64_t cnt = 0;
            while (j < nr && !rs[j].special) cnt += rs[j++].count;
            segs.push_back({i, j, cnt, 1});
            i = j;
        }
        int slots = kMax - nSpecial;
        if (slots < static_cast<int>(segs.size()) || slots <= 0) {   // zu viele Sondergruppen: alles gleich behandeln
            for (auto& r : rs) r.special = false;
            segs.assign(1, Seg{0, nr, n, 1});
            slots = kMax;
        }
        int used = static_cast<int>(segs.size());
        while (used < slots) {   // freie Streifen an die groessten Abschnitte verteilen
            int best = -1;
            double bv = 0;
            for (int s = 0; s < static_cast<int>(segs.size()); ++s) {
                if (segs[s].k >= segs[s].b - segs[s].a) continue;
                double v = double(segs[s].cnt) / (segs[s].k + 1);
                if (v > bv) { bv = v; best = s; }
            }
            if (best < 0) break;
            ++segs[best].k;
            ++used;
        }
        size_t si = 0;
        for (int i = 0; i < nr;) {
            if (rs[i].special) { buckets.push_back({i, i + 1}); ++i; continue; }
            const Seg& sg = segs[si++];
            int a = sg.a;
            int64_t done = 0;
            for (int part = 0; part < sg.k; ++part) {
                int left = sg.k - part;
                if (left == 1) { buckets.push_back({a, sg.b}); break; }
                double target = double(sg.cnt) * (part + 1) / sg.k;
                int b = a + 1;
                int64_t cum = done + rs[a].count;
                while (b < sg.b - (left - 1)) {   // genug Gruppen fuer die restlichen Streifen lassen
                    int64_t next = cum + rs[b].count;
                    if (std::fabs(double(next) - target) > std::fabs(double(cum) - target)) break;
                    cum = next;
                    ++b;
                }
                buckets.push_back({a, b});
                done = cum;
                a = b;
            }
            i = sg.b;
        }
    }
    int ny;
    unsigned nm, nd;
    CivilFromDays((now_ + tzOffset_) / 86400, ny, nm, nd);
    std::vector<char> isSpecial;
    for (auto [a, b] : buckets) {
        CloudGroup g;
        g.start = rs[a].start;
        g.count = rs[b - 1].start + rs[b - 1].count - rs[a].start;
        const std::wstring& la = rs[a].label;
        const std::wstring& lb = rs[b - 1].label;
        if (b - a == 1) {
            g.label = la;
        } else if (key == 0) {   // Name: "A–C" (aus "0–9" wird je nach Seite "0" bzw. "9")
            size_t pa = la.find(L'–'), pb = lb.find(L'–');
            g.label = (pa == std::wstring::npos ? la : la.substr(0, pa)) + L"–" +
                      (pb == std::wstring::npos ? lb : lb.substr(pb + 1));
        } else if ((key == 1 || key == 5) && rs[a].t > 0 && rs[b - 1].t > 0) {
            int ya, yb;
            unsigned ma, mb, d;
            CivilFromDays((rs[a].t + tzOffset_) / 86400, ya, ma, d);
            CivilFromDays((rs[b - 1].t + tzOffset_) / 86400, yb, mb, d);
            if (ya != yb) g.label = std::to_wstring(ya) + L" – " + std::to_wstring(yb);
            else if (ya == ny) g.label = la + L" – " + lb;
            else g.label = std::wstring(MonthName(ma)) + L" – " + MonthName(mb) + L" " + std::to_wstring(ya);
        } else {
            g.label = la + L" – " + lb;
        }
        isSpecial.push_back(b - a == 1 && rs[a].special);
        out.push_back(std::move(g));
    }

    // Verfeinern: zu wenige Streifen -> grosse Gruppen weiter teilen (nicht nach Dateityp: keine Reihenfolge darin)
    if (key == 4 || static_cast<int>(out.size()) >= kMax) return out;
    const int minPart = std::max(4, n / 24);
    // Namen: nur am ersten Zeichen teilen (Titel wie "A" oder "A–C"), also hoechstens so viele Teile wie Anfangszeichen
    auto firstChar = [&](int p) -> wchar_t {
        const std::wstring& nm = entries_[visible_[p]].name;
        wchar_t up[2] = {nm.empty() ? L'#' : nm[0], 0};
        CharUpperBuffW(up, 1);
        wchar_t ch = up[0];
        if (ch == L'\u00c4') return L'A';
        if (ch == L'\u00d6') return L'O';
        if (ch == L'\u00dc') return L'U';
        return (ch >= L'0' && ch <= L'9') || IsCharAlphaW(ch) ? ch : L'#';
    };
    struct CharRun { wchar_t ch; int start, count; };
    std::vector<std::vector<CharRun>> charRuns(out.size());
    if (key == 0)
        for (size_t i = 0; i < out.size(); ++i)
            for (int p = out[i].start; p < out[i].start + out[i].count; ++p) {
                wchar_t ch = firstChar(p);
                if (!charRuns[i].empty() && charRuns[i].back().ch == ch) ++charRuns[i].back().count;
                else charRuns[i].push_back({ch, p, 1});
            }
    std::vector<int> parts(out.size(), 1);
    int used = static_cast<int>(out.size());
    while (used < kMax) {
        int best = -1;
        double bv = 0;
        for (size_t i = 0; i < out.size(); ++i) {
            if (isSpecial[i]) continue;
            if (key == 0 && parts[i] >= static_cast<int>(charRuns[i].size())) continue;
            double v = double(out[i].count) / (parts[i] + 1);
            if (v >= minPart && v > bv) { bv = v; best = static_cast<int>(i); }
        }
        if (best < 0) break;
        ++parts[best];
        ++used;
    }
    if (used == static_cast<int>(out.size())) return out;

    auto decimal = [](double v) {
        wchar_t buf[32];
        std::swprintf(buf, 32, v < 10 ? L"%.1f" : L"%.0f", v);
        std::wstring r = buf;
        if (!LangEn()) for (auto& ch : r) if (ch == L'.') ch = L',';
        return r;
    };
    auto rangeLabel = [&](int a, int b) -> std::wstring {   // Elemente [a, b)
        const FileEntry& e0 = entries_[visible_[a]];
        const FileEntry& e1 = entries_[visible_[b - 1]];
        switch (key) {
        case 1:
        case 5: {
            int64_t t0 = timeOf(e0), t1 = timeOf(e1);
            if (t0 <= 0 || t1 <= 0) break;
            int y0, y1;
            unsigned m0, m1, d0, d1;
            CivilFromDays((t0 + tzOffset_) / 86400, y0, m0, d0);
            CivilFromDays((t1 + tzOffset_) / 86400, y1, m1, d1);
            if (y0 != y1) return std::to_wstring(y0) + L" – " + std::to_wstring(y1);
            if (m0 != m1) return std::wstring(MonthName(m0)) + L" – " + MonthName(m1) + L" " + std::to_wstring(y0);
            if (d0 != d1)
                return TF(L"{0}. – {1}. {2} {3}", {std::to_wstring(d0), std::to_wstring(d1), MonthName(m0), std::to_wstring(y0)});
            auto hm = [&](int64_t t) {
                int64_t sec = ((t + tzOffset_) % 86400 + 86400) % 86400;
                wchar_t buf[16];
                std::swprintf(buf, 16, L"%02d:%02d", static_cast<int>(sec / 3600), static_cast<int>(sec / 60 % 60));
                return std::wstring(buf);
            };
            return FormatDay(y0, m0, d0) + L", " + hm(t0) + L" – " + hm(t1);
        }
        case 2: return FormatSize(e0.size) + L" – " + FormatSize(e1.size);
        case 3: {
            double p0 = double(e0.width) * e0.height / 1e6, p1 = double(e1.width) * e1.height / 1e6;
            return TF(L"{0} – {1} MP", {decimal(p0), decimal(p1)});
        }
        default: break;
        }
        return {};
    };
    std::vector<CloudGroup> ref;
    for (size_t i = 0; i < out.size(); ++i) {
        if (parts[i] == 1) { ref.push_back(out[i]); continue; }
        if (key == 0) {   // Anfangszeichen ausgeglichen nach Anzahl zu parts[i] Streifen zusammenfassen
            const auto& cr = charRuns[i];
            const int m = static_cast<int>(cr.size()), k = parts[i];
            int a = 0;
            int64_t done = 0;
            for (int part = 0; part < k && a < m; ++part) {
                int b = m;
                if (k - part > 1) {
                    double target = double(out[i].count) * (part + 1) / k;
                    b = a + 1;
                    int64_t cum = done + cr[a].count;
                    while (b < m - (k - part - 1)) {
                        int64_t next = cum + cr[b].count;
                        if (std::fabs(double(next) - target) > std::fabs(double(cum) - target)) break;
                        cum = next;
                        ++b;
                    }
                    done = cum;
                }
                CloudGroup g;
                g.start = cr[a].start;
                g.count = cr[b - 1].start + cr[b - 1].count - cr[a].start;
                g.label = std::wstring(1, cr[a].ch);
                if (b - a > 1) g.label += L"\u2013" + std::wstring(1, cr[b - 1].ch);
                ref.push_back(std::move(g));
                a = b;
            }
            continue;
        }
        for (int j = 0; j < parts[i]; ++j) {
            int a = out[i].start + static_cast<int>(int64_t(out[i].count) * j / parts[i]);
            int b = out[i].start + static_cast<int>(int64_t(out[i].count) * (j + 1) / parts[i]);
            CloudGroup g;
            g.start = a;
            g.count = b - a;
            g.label = rangeLabel(a, b);
            if (g.label.empty()) g.label = out[i].label;
            ref.push_back(std::move(g));
        }
    }
    return ref;
}

// Wolke mit Streifen: Trennlinien (hinter den Karten) bzw. Gruppentitel in der Leiste oben, gestaltet wie die
// Gruppenkoepfe im Raster. Waagerecht folgen beide der Kamera (Ebene der vordersten bzw. ausgewaehlten Karten).
void App::PaintCloudHeaders(ID2D1DeviceContext* dc, bool titles) {
    auto& c = cloud_;
    const int ns = static_cast<int>(c.strips.size());
    if (ns == 0) return;
    Theme t = MakeTheme(dark_, backdrop_);
    const D2D1_RECT_F& v = c.view;
    const D2D1_RECT_F band = D2D1::RectF(gridRc_.left, gridRc_.top, gridRc_.right, v.top);
    const float vcx = (v.left + v.right) / 2;
    const float sc = r_.scale();
    const float k = c.f / std::max(0.15f, c.focusZ - c.cz);
    auto sx = [&](float X) { return vcx + (X - c.cx) * k; };
    const float top = band.top + 8;   // wie der erste Gruppenkopf im Raster
    if (!titles) {
        const float half = kCloudGap / 2;   // halber Abstand zwischen zwei Streifen (Welt)
        for (int i = 0; i < ns; ++i) {
            CloudStrip& st = c.strips[i];
            st.sl = sx(i == 0 ? st.x0 - half : (c.strips[i - 1].x1 + st.x0) / 2);
            st.sr = sx(i == ns - 1 ? st.x1 + half : (st.x1 + c.strips[i + 1].x0) / 2);
        }
        dc->PushAxisAlignedClip(D2D1::RectF(gridRc_.left, gridRc_.top, gridRc_.right, gridRc_.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 1; i < ns; ++i) {
            float x = std::round(c.strips[i].sl * sc) / sc + 0.5f;
            if (x < v.left || x > v.right) continue;
            dc->DrawLine(D2D1::Point2F(x, top + 10), D2D1::Point2F(x, v.bottom), r_.Brush(t.stroke), 1.0f);
        }
        dc->PopAxisAlignedClip();
        return;
    }
    IDWriteTextFormat* gf = r_.Text(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* cf = r_.Text(12);
    for (int i = 1; i < ns; ++i) {   // Trennlinien auch in der Leiste: trennen die Titel
        float x = std::round(c.strips[i].sl * sc) / sc + 0.5f;
        if (x < band.left || x > band.right) continue;
        dc->DrawLine(D2D1::Point2F(x, top + 10), D2D1::Point2F(x, band.bottom), r_.Brush(t.stroke), 1.0f);
    }
    for (int i = 0; i < ns; ++i) {
        const CloudStrip& st = c.strips[i];
        float l = std::max(st.sl, band.left), r = std::min(st.sr, band.right);
        if (r - l < 24) continue;
        std::wstring cnt = Thousands(static_cast<uint64_t>(st.count));
        float gw = r_.MeasureText(st.label, gf), cw = r_.MeasureText(cnt, cf);
        float need = gw + 10 + cw;
        // Titel bleibt sichtbar, solange ein Teil des Streifens zu sehen ist
        float lx = std::max(st.sl + 16, band.left + 16);
        if (lx + need > st.sr - 16) lx = std::max(st.sl + 16, st.sr - 16 - need);
        dc->PushAxisAlignedClip(D2D1::RectF(l, band.top, r, band.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        DrawText(dc, st.label, D2D1::RectF(lx, top + 10, lx + gw + 4, top + kGroupH - 4), gf, t.text);
        DrawText(dc, cnt, D2D1::RectF(lx + gw + 10, top + 10, lx + gw + 14 + cw, top + kGroupH - 4), cf, t.text3);
        dc->PopAxisAlignedClip();
    }
}

int App::RowOfPos(int pos) const {
    if (rowFirst_.size() < 2) return 0;
    auto it = std::upper_bound(rowFirst_.begin(), rowFirst_.end() - 1, pos);
    return std::max(0, static_cast<int>(it - rowFirst_.begin()) - 1);
}
int App::FirstPosOfRow(int row) const {
    if (row < 0 || row >= static_cast<int>(rowFirst_.size())) return 0;
    return rowFirst_[row];
}

// Element in der Zeile darueber/darunter (Gruppenkoepfe werden uebersprungen, Spalte bleibt moeglichst erhalten)
int App::VerticalNeighbor(int pos, int dir) const {
    int rows = static_cast<int>(rowFirst_.size()) - 1;
    int row = RowOfPos(pos);
    int col = pos - rowFirst_[row];
    for (int r = row + dir; r >= 0 && r < rows; r += dir) {
        if (rowHeader_[r] >= 0 || RowEnd(r) == rowFirst_[r]) continue;
        return std::min(rowFirst_[r] + col, RowEnd(r) - 1);
    }
    return pos;
}

int App::RowAtY(float y) const {
    if (rowTop_.size() < 2) return 0;
    auto it = std::upper_bound(rowTop_.begin(), rowTop_.end(), y);
    int row = static_cast<int>(it - rowTop_.begin()) - 1;
    return std::clamp(row, 0, static_cast<int>(rowTop_.size()) - 2);
}

D2D1_RECT_F App::ItemRect(int pos) const {
    int row = RowOfPos(pos);
    if (row < 0 || row + 1 >= static_cast<int>(rowTop_.size())) return {};
    float top = 8 + rowTop_[row], bottom = 8 + rowTop_[row + 1];
    if (settings_.viewMode == 1) return D2D1::RectF(gridRc_.left + 4, top, gridRc_.right - 14, bottom);
    int col = pos - rowFirst_[row];
    float x = gridRc_.left + 8 + col * cellW_;
    return D2D1::RectF(x, top, x + cellW_, bottom);
}

ID2D1Bitmap1* App::AppIconFor(const std::wstring& ext) {
    auto it = appIcons_.find(ext);
    if (it != appIcons_.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap1> bmp;
    OpenWithApp a;
    if (DefaultAppIcon(ext, a)) {
        Pixels px;
        px.w = a.iconW;
        px.h = a.iconH;
        px.data = std::move(a.iconPx);
        bmp = r_.CreateBitmap(px);
    }
    appIcons_[ext] = bmp;
    return bmp.Get();
}

// Bildperiode des Bildschirms, auf dem das Fenster liegt (ms)
double App::RefreshPeriodMs() {
    static double period = 1000.0 / 60.0;
    static DWORD checked = 0;
    static bool have = false;
    DWORD now = GetTickCount();
    if (!have || now - checked > 2000) {
        have = true;
        checked = now;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24 &&
            dm.dmDisplayFrequency <= 1000)
            period = 1000.0 / dm.dmDisplayFrequency;
    }
    return period;
}

// Gemessene Zeit seit dem letzten Bild auf ganze Bildperioden runden. Die Uhr des Prozessors schwankt je nach
// Rechenzeit um einige Millisekunden; gezeigt werden die Bilder aber in festem Abstand. Passt die Messung nicht zum
// Takt (anderer Bildschirm, variable Bildrate), bleibt sie unveraendert.
float App::FrameDt(double rawMs) {
    double p = RefreshPeriodMs();
    double n = std::round(rawMs / p);
    if (n >= 1 && n <= 4 && std::fabs(rawMs - n * p) < 0.35 * p) return static_cast<float>(n * p);
    return static_cast<float>(rawMs);
}

// Messanzeige: mittlerer und groesster Bildabstand sowie Rechenzeit der letzten Bilder (nur fortlaufende Bilder)
void App::PaintPerfHud(ID2D1DeviceContext* dc, double paintStart) {
    double now = NowMs();
    if (perfLast_ > 0 && paintStart - perfLast_ < 100.0) {
        int i = perfN_ % 120;
        perfFrame_[i] = static_cast<float>(paintStart - perfLast_);
        perfCpu_[i] = static_cast<float>(now - paintStart);
        for (int k = 0; k < 4; ++k) perfPart_[k][i] = perfNow_[k];
        ++perfN_;
    }
    perfLast_ = paintStart;
    int n = std::min(perfN_, 120);
    auto stat = [&](const float* v, float& avg, float& mx) {
        avg = mx = 0;
        for (int i = 0; i < n; ++i) { avg += v[i]; mx = std::max(mx, v[i]); }
        if (n) avg /= n;
    };
    float avg, mx, cpu, cmx, pa[4], pm[4];
    stat(perfFrame_, avg, mx);
    stat(perfCpu_, cpu, cmx);
    for (int k = 0; k < 4; ++k) stat(perfPart_[k], pa[k], pm[k]);
    const bool cloud = settings_.viewMode == 2 && !viewer_.open && !settingsOpen_;
    wchar_t buf[200], buf2[240];
    std::swprintf(buf, 200, L"Bild %.1f ms (max %.1f) · CPU %.1f ms (max %.1f) · Abschluss %.1f (max %.1f) · %.0f Hz",   // lang:ok
                  avg, mx, cpu, cmx, pa[3], pm[3], 1000.0 / RefreshPeriodMs());
    std::swprintf(buf2, 240, L"Wolke: %d Karten · nah %d Bild / %d Name / %d Ebene · %d Sprites · Atlas %.1f (max %.1f) · Rechnen %.1f (max %.1f) · Nah %.1f (max %.1f)",   // lang:ok
                  perfCount_[0], perfCount_[1], perfCount_[2], perfCount_[3], perfCount_[4], pa[0], pm[0], pa[1], pm[1], pa[2], pm[2]);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_RECT_F rc = D2D1::RectF(8, H_ - (cloud ? 56.0f : 34.0f), std::min(W_ - 8, 760.0f), H_ - 8);
    FillRound(dc, rc, 4, D2D1::ColorF(0, 0, 0, 0.8f));
    DrawText(dc, buf, D2D1::RectF(rc.left + 10, rc.top, rc.right - 6, rc.top + 26), r_.Text(12), D2D1::ColorF(1, 1, 1, 0.95f));
    if (cloud)
        DrawText(dc, buf2, D2D1::RectF(rc.left + 10, rc.top + 22, rc.right - 6, rc.bottom), r_.Text(12), D2D1::ColorF(1, 1, 1, 0.95f));
}

static float EaseOut(float u) { return 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u); }

bool App::UiAnim(float& v, bool on, float ms) {
    float target = on ? 1.0f : 0.0f;
    if (v == target) return false;
    float step = static_cast<float>(RefreshPeriodMs()) / ms;
    v = on ? std::min(1.0f, v + step) : std::max(0.0f, v - step);
    uiAnimPending_ = true;   // naechstes Bild anfordern
    return true;
}

void App::Paint() {
    if (!r_.BeginDraw()) return;
    if (r_.deviceLost()) ClearBitmaps();
    ID2D1DeviceContext* dc = r_.dc();
    Theme t = MakeTheme(dark_, backdrop_);
    ++frame_;
    hits_.clear();
    double paintStart = NowMs();
    // fertige Vorschaubilder hochladen: waehrend einer Animation nur wenige Millisekunden je Bild
    if (!thumbBacklog_.empty()) DrainThumbs(animating_ ? 4.0 : 12.0);
    if (animating_) {
        // Schritt im Takt des Bildschirms (gleichmaessige Bewegung auch bei schwankender Rechenzeit);
        // der erste Schritt nach dem Start ist genau eine Bildperiode
        float dt = animFresh_ ? static_cast<float>(RefreshPeriodMs())
                              : FrameDt(std::clamp(paintStart - animLast_, 0.0, 50.0));
        animFresh_ = false;
        animLast_ = paintStart;
        if (IsIconic(hwnd_) || !AnimateStep(dt)) animating_ = false;
    }
    Layout();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(t.bg);

    if (compare_.open) {
        PaintCompare(dc);
    } else if (viewer_.open) {
        PaintViewer(dc);
    } else if (viewerStart_) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 1));   // Start mit einer Bilddatei: dunkel, bis der Betrachter offen ist
    } else if (sheet_.open) {
        PaintSheet(dc);
    } else if (atlas_.open) {
        PaintAtlas(dc);
    } else if (settingsOpen_) {
        if (settingsPage_ == 1) PaintShortcuts(dc);
        else {
            // Einstellungen scrollbar: verschoben zeichnen, Trefferflaechen entsprechend verschieben
            Theme tt = MakeTheme(dark_, backdrop_);
            dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(WithAlpha(tt.layer, backdrop_ ? 0.45f : 1.0f)));
            size_t h0 = hits_.size();
            dc->SetTransform(D2D1::Matrix3x2F::Translation(0, -settingsScroll_));
            PaintSettings(dc);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            for (size_t i = h0; i < hits_.size(); ++i) {
                hits_[i].rc.top -= settingsScroll_;
                hits_[i].rc.bottom -= settingsScroll_;
            }
            if (settingsContentH_ > H_) {
                float trackTop = 4, trackH = H_ - 8;
                float thumbH = std::max(32.0f, trackH * H_ / settingsContentH_);
                float maxS = settingsContentH_ - H_;
                float ty = trackTop + (trackH - thumbH) * (maxS > 0 ? settingsScroll_ / maxS : 0);
                FillRound(dc, D2D1::RectF(W_ - 9, ty, W_ - 6, ty + thumbH), 1.5f, WithAlpha(tt.text, 0.3f));
            }
        }
    } else {
        PaintGrid(dc);
        if (SidebarWidth() > 0.5f) PaintSidebar(dc);
        if (InfoWidth() > 0.5f)
            PaintInfoPanel(dc, D2D1::RectF(W_ - InfoWidth(), topH_, W_, H_ - statusH_), InfoSelection(), dark_);
        PaintTopBar(dc);
        PaintChips(dc);
        PaintStatus(dc);
        {   // Liste unter dem Suchfeld: schiebt sich herein und hinaus
            const bool on = showHistory_ && searchFocus_;
            UiAnim(histAnim_, on, 120.0f);
            if (histAnim_ > 0) {
                size_t h0 = hits_.size();
                PaintSearchHistory(dc);
                if (!on) hits_.resize(h0);   // beim Hinausschieben nicht mehr anklickbar
            }
        }
        if (autoScroll_) {
            D2D1_POINT_2F a = autoAnchor_;
            dc->FillEllipse(D2D1::Ellipse(a, 15, 15), r_.Brush(dark_ ? D2D1::ColorF(0.2f, 0.2f, 0.2f, 0.92f)
                                                                     : D2D1::ColorF(1, 1, 1, 0.95f)));
            if (settings_.viewMode == 3) {   // Zeitleiste: links/rechts
                wchar_t lf[2] = {0xE76B, 0}, rt[2] = {0xE76C, 0};
                DrawText(dc, lf, D2D1::RectF(a.x - 14, a.y - 10, a.x - 2, a.y + 10), r_.Icon(8), t.text);
                DrawText(dc, rt, D2D1::RectF(a.x + 2, a.y - 10, a.x + 14, a.y + 10), r_.Icon(8), t.text);
            } else {
                wchar_t up[2] = {0xE70E, 0}, dn[2] = {0xE70D, 0};
                DrawText(dc, up, D2D1::RectF(a.x - 10, a.y - 14, a.x + 10, a.y - 2), r_.Icon(8), t.text);
                DrawText(dc, dn, D2D1::RectF(a.x - 10, a.y + 2, a.x + 10, a.y + 14), r_.Icon(8), t.text);
            }
        }
        if (slidePending_ >= 0) {   // Diashow aus der Ordneransicht gestartet: die Ansicht blendet langsam nach Schwarz ab
            float k = static_cast<float>(1.0 - std::clamp((slidePendingEnd_ - NowMs()) / 800.0, 0.0, 1.0));
            dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(D2D1::ColorF(0, 0, 0, k * k * (3 - 2 * k))));
            hits_.clear();
            // ganz schwarz: Betrachter oeffnen. Als Nachricht, nicht per Zeitgeber: solange jedes Bild das naechste
            // anfordert, stellt Windows WM_TIMER nicht zu (er kaeme nie an)
            if (k >= 1.0f) {
                if (!slidePosted_) {
                    slidePosted_ = true;
                    PostMessageW(hwnd_, WM_TIMER, kTimerSlideStart, 0);
                }
            } else {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
    }
    PaintTip(dc);
    if (perfHud_) PaintPerfHud(dc, paintStart);
    const double endStart = perfHud_ ? NowMs() : 0;
    r_.EndDraw();
    if (perfHud_) perfNow_[3] = static_cast<float>(NowMs() - endStart);
    if (animating_) {
        // naechstes Bild: BeginDraw wartet auf den Takt des Bildschirms. Blieb er aus (Fenster verdeckt), per Timer
        // drosseln. Ohne Takt-Abfrage (Rueckfall): Present wartet selbst; kehrte es sofort zurueck, ebenfalls drosseln.
        bool paced = r_.framePaced() ? r_.frameReady() : NowMs() - paintStart >= 2.0;
        if (paced) InvalidateRect(hwnd_, nullptr, FALSE);
        else SetTimer(hwnd_, kTimerAnim, r_.framePaced() ? 100 : 8, nullptr);
    }
    if (!thumbBacklog_.empty()) InvalidateRect(hwnd_, nullptr, FALSE);   // restliche Vorschaubilder im naechsten Bild
    if (uiAnimPending_) {   // laufende Ein-/Ausblendung: naechstes Bild
        uiAnimPending_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (r_.deviceLost()) {
        ClearBitmaps();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if ((frame_ % 30) == 0) TrimCaches();
}

// ---------------------------------------------------------------------------
// Kopfzeile mit Navigation, Pfadleiste und Suche
// ---------------------------------------------------------------------------

void App::PaintTopBar(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    float y = 8, h = 36;
    float x = 8;
    auto button = [&](HitType type, wchar_t glyph, bool enabled) {
        D2D1_RECT_F rc = D2D1::RectF(x, y, x + 36, y + h);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, enabled ? type : HitType::None, 0});
        if (enabled && hoverHit_ == idx) FillRound(dc, rc, 5, t.hover);
        wchar_t g[2] = {glyph, 0};
        DrawText(dc, g, rc, r_.Icon(14), enabled ? t.text : t.text3);
        x += 40;
    };
    std::wstring parent = path_.empty() ? std::wstring() : ParentPath(path_);
    button(HitType::SidebarToggle, 0xE700, true);
    button(HitType::NavBack, 0xE72B, !back_.empty());
    button(HitType::NavForward, 0xE72A, !fwd_.empty());
    button(HitType::NavUp, 0xE74A, !collectionsView_ && !parent.empty() && parent != path_);
    button(HitType::Refresh, 0xE72C, true);
    x += 4;

    float searchW = std::clamp(W_ * 0.28f, 220.0f, 380.0f);
    D2D1_RECT_F sb = D2D1::RectF(W_ - 12 - searchW, y, W_ - 12, y + h);

    // Pfadleiste (kein Eingabefeld)
    D2D1_RECT_F pb = D2D1::RectF(x, y, sb.left - 10, y + h);
    FillRound(dc, pb, 5, t.chip);
    crumbPaths_.clear();
    IDWriteTextFormat* f = r_.Text(13);
    float cx = pb.left + 6;
    if (bookmarksView_) {
        wchar_t g[2] = {0xE734, 0};
        DrawText(dc, g, D2D1::RectF(cx + 4, pb.top, cx + 24, pb.bottom), r_.Icon(13), t.text2);
        DrawText(dc, T(L"Merkliste"), D2D1::RectF(cx + 30, pb.top, pb.right - 8, pb.bottom), f, t.text);
    } else if (collectionsView_) {
        wchar_t g[2] = {0xE8F1, 0};
        DrawText(dc, g, D2D1::RectF(cx + 4, pb.top, cx + 24, pb.bottom), r_.Icon(13), t.text2);
        DrawText(dc, T(L"Alle Sammlungen"), D2D1::RectF(cx + 30, pb.top, pb.right - 8, pb.bottom), f, t.text);
    } else if (!path_.empty()) {
        std::vector<std::pair<std::wstring, std::wstring>> segs;
        std::wstring acc;
        size_t start = 0;
        while (start < path_.size()) {
            size_t end = path_.find(L'\\', start);
            std::wstring part = path_.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            if (!part.empty()) {
                acc = acc.empty() ? part + L"\\" : JoinPath(acc, part);
                segs.push_back({part, acc});
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        const float chevW = 22;
        std::vector<float> widths;
        float total = 0;
        for (auto& sg : segs) {
            widths.push_back(r_.MeasureText(sg.first, f) + 16);
            total += widths.back() + chevW;
        }
        size_t first = 0;
        float avail = pb.right - pb.left - 30;
        while (first + 1 < segs.size() && total > avail) {
            total -= widths[first] + chevW;
            ++first;
        }
        for (auto& sg : segs) crumbPaths_.push_back(sg.second);
        if (first > 0) {
            DrawText(dc, L"…", D2D1::RectF(cx + 4, pb.top, cx + 20, pb.bottom), f, t.text2);
            cx += 20;
        }
        for (size_t i = first; i < segs.size(); ++i) {
            D2D1_RECT_F sr = D2D1::RectF(cx, pb.top + 4, std::min(cx + widths[i], pb.right - 4), pb.bottom - 4);
            int idx = static_cast<int>(hits_.size());
            hits_.push_back({sr, HitType::Crumb, static_cast<int>(i)});
            if (dropCrumb_ == static_cast<int>(i)) {
                FillRound(dc, sr, 4, WithAlpha(accent_, 0.3f));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(sr, 4, 4), r_.Brush(accent_), 1.5f);
            } else if (hoverHit_ == idx) {
                FillRound(dc, sr, 4, t.hover);
            }
            DrawText(dc, segs[i].first, D2D1::RectF(sr.left + 8, sr.top, sr.right + 40, sr.bottom), f,
                     i + 1 == segs.size() ? t.text : t.text2);
            cx += widths[i];
            if (cx + chevW > pb.right - 2) break;
            D2D1_RECT_F cr = D2D1::RectF(cx, pb.top + 4, cx + chevW, pb.bottom - 4);
            int cIdx = static_cast<int>(hits_.size());
            hits_.push_back({cr, HitType::CrumbChevron, static_cast<int>(i)});
            bool open = openMenu_ == 1000 + static_cast<int>(i);
            if (open) FillRound(dc, cr, 4, t.press);
            else if (hoverHit_ == cIdx) FillRound(dc, cr, 4, t.hover);
            wchar_t g[2] = {static_cast<wchar_t>(open ? 0xE70D : 0xE76C), 0};
            DrawText(dc, g, cr, r_.Icon(9), t.text2);
            cx += chevW;
        }
    }

    // Suchfeld
    searchRc_ = sb;
    int sIdx = static_cast<int>(hits_.size());
    hits_.push_back({sb, HitType::Search, 0});
    FillRound(dc, sb, 5, searchFocus_ ? t.layer : (hoverHit_ == sIdx ? t.chipHover : t.chip));
    if (searchFocus_) {
        D2D1_RECT_F line = D2D1::RectF(sb.left + 1, sb.bottom - 2, sb.right - 1, sb.bottom);
        dc->FillRoundedRectangle(D2D1::RoundedRect(line, 1, 1), r_.Brush(accent_));
    }
    wchar_t g[2] = {0xE721, 0};
    DrawText(dc, g, D2D1::RectF(sb.left + 8, sb.top, sb.left + 30, sb.bottom), r_.Icon(13), t.text2);
    D2D1_RECT_F tr = D2D1::RectF(sb.left + 34, sb.top, sb.right - 34, sb.bottom);
    if (!search_.empty()) {
        if (searchSelAll_ && searchFocus_) {
            float sw = r_.MeasureText(search_, f);
            dc->FillRectangle(D2D1::RectF(tr.left - 1, tr.top + 8, std::min(tr.right, tr.left + sw + 1), tr.bottom - 8),
                              r_.Brush(WithAlpha(accent_, 0.4f)));
        }
        DrawText(dc, search_, tr, f, t.text);
        D2D1_RECT_F cr = D2D1::RectF(sb.right - 32, sb.top + 4, sb.right - 4, sb.bottom - 4);
        int cIdx = static_cast<int>(hits_.size());
        hits_.push_back({cr, HitType::SearchClear, 0});
        if (hoverHit_ == cIdx) FillRound(dc, cr, 4, t.hover);
        wchar_t x2[2] = {0xE711, 0};
        DrawText(dc, x2, cr, r_.Icon(10), t.text2);
    }
    if (searchFocus_ && !searchSelAll_) {
        float caretX = tr.left + r_.MeasureText(search_.substr(0, caret_), f);
        dc->DrawLine(D2D1::Point2F(caretX, tr.top + 9), D2D1::Point2F(caretX, tr.bottom - 9), r_.Brush(t.text), 1.0f);
    }
}

std::wstring App::ChipLabel(ChipId id) const {
    switch (id) {
    case ChipId::Type: {
        static const wchar_t* n[] = {N_(L"Ordner"), N_(L"Bilder"), N_(L"Videos"), N_(L"Sonstige")};
        static const wchar_t* d[] = {L"PDF", L"Photoshop", L"Illustrator", L"SVG"};
        if (filters_.type >= 10) return d[std::clamp(filters_.type - 10, 0, 3)];
        return filters_.type < 0 ? T(L"Typ") : T(n[std::clamp(filters_.type, 0, 3)]);
    }
    case ChipId::Date: {
        static const wchar_t* n[] = {N_(L"Datum"), N_(L"Heute"), N_(L"Letzte 7 Tage"), N_(L"Letzte 30 Tage"),
                                     N_(L"Dieses Jahr"), N_(L"Letztes Jahr"), N_(L"Älter")};
        if (filters_.date == 7) return SpanLabel(filters_.dateFrom, filters_.dateTo);
        return T(n[std::clamp(filters_.date, 0, 6)]);
    }
    case ChipId::Taken: {
        static const wchar_t* n[] = {N_(L"Aufgenommen"), N_(L"Heute aufgenommen"), N_(L"Letzte 7 Tage"),
                                     N_(L"Letzte 30 Tage"), N_(L"Dieses Jahr"), N_(L"Letztes Jahr"), N_(L"Älter")};
        if (filters_.taken == 7) return TF(L"Aufgenommen {0}", {SpanLabel(filters_.takenFrom, filters_.takenTo)});
        return T(n[std::clamp(filters_.taken, 0, 6)]);
    }
    case ChipId::Size: {
        static const wchar_t* n[] = {N_(L"Größe"), L"< 1 MB", L"1–10 MB", L"10–100 MB", L"> 100 MB"};
        return T(n[std::clamp(filters_.size, 0, 4)]);
    }
    case ChipId::Resolution: {
        static const wchar_t* n[] = {N_(L"Auflösung"), N_(L"Unter HD"), N_(L"Ab HD"), N_(L"Ab Full HD"), N_(L"Ab 4K"),
                                     N_(L"Über 20 MP")};
        return T(n[std::clamp(filters_.res, 0, 5)]);
    }
    case ChipId::Orientation: {
        static const wchar_t* n[] = {N_(L"Ausrichtung"), N_(L"Querformat"), N_(L"Hochformat"), N_(L"Quadratisch"),
                                     N_(L"Panorama")};
        return T(n[std::clamp(filters_.orient, 0, 4)]);
    }
    case ChipId::Source:
        return filters_.source.empty() ? std::wstring(T(L"Quelle")) : T(filters_.source);
    case ChipId::Pattern:
        return filters_.templ == UINT32_MAX ? std::wstring(T(L"Muster")) : TPattern(templDict_.Word(filters_.templ));
    case ChipId::Sort: {
        static const wchar_t* n[] = {N_(L"Name"), N_(L"Datum"), N_(L"Größe"), N_(L"Auflösung"), N_(L"Typ"),
                                     N_(L"Aufgenommen")};
        if (settings_.viewMode == 3) return T(L"Aufgenommen");   // Zeitleiste: nur nach Aufnahmedatum
        return T(n[std::clamp(settings_.sortKey, 0, 5)]);
    }
    case ChipId::Recursive:
        return T(L"Unterordner");
    default:
        return {};
    }
}

void App::PaintChips(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    float y = topH_ + 5, h = 30;
    float x = gridRc_.left + 12;
    float right = gridRc_.right - 12;
    IDWriteTextFormat* f = r_.Text(12.5f, DWRITE_FONT_WEIGHT_BOLD);
    IDWriteTextFormat* fc = r_.Text(11.5f);

    // Sortieren: immer ganz rechts, keine Pille (blaues Symbol, weisser Text, blauer Pfeil)
    {
        std::wstring label = ChipLabel(ChipId::Sort);
        float lw = r_.MeasureText(label, f);
        float w = 12 + 18 + 6 + lw + 6 + 14 + 10;
        D2D1_RECT_F rc = D2D1::RectF(right - w, y, right, y + h);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::SortButton, 0});
        if (openMenu_ == static_cast<int>(ChipId::Sort)) FillRound(dc, rc, 5, t.press);
        else if (hoverHit_ == idx) FillRound(dc, rc, 5, t.hover);
        float tx = rc.left + 12;
        wchar_t gs[2] = {0xE8CB, 0};
        DrawText(dc, gs, D2D1::RectF(tx, rc.top, tx + 18, rc.bottom), r_.Icon(13), accent_);
        tx += 24;
        DrawText(dc, label, D2D1::RectF(tx, rc.top, tx + lw + 2, rc.bottom), f, dark_ ? t.onAccent : t.text);
        tx += lw + 6;
        bool desc = settings_.viewMode == 3 ? !settings_.timelineAsc : settings_.sortDesc;
        wchar_t ga[2] = {static_cast<wchar_t>(desc ? 0xE74B : 0xE74A), 0};
        DrawText(dc, ga, D2D1::RectF(tx, rc.top, tx + 14, rc.bottom), r_.Icon(11), accent_);
        right = rc.left - 8;
    }

    // Filterknoepfe (in dieser Reihenfolge); passen nicht alle in die Leiste, kommen die hinteren in "..."
    IDWriteTextFormat* ff = r_.Text(14, DWRITE_FONT_WEIGHT_NORMAL);
    struct ChipDef { ChipId id; bool active, dropdown; wchar_t glyph; };
    std::vector<ChipDef> defs = {{ChipId::Type, filters_.type >= 0, true, 0},
                                 {ChipId::Date, filters_.date != 0, true, 0},
                                 {ChipId::Taken, filters_.taken != 0, true, 0},
                                 {ChipId::Size, filters_.size != 0, true, 0},
                                 {ChipId::Resolution, filters_.res != 0, true, 0},
                                 {ChipId::Orientation, filters_.orient != 0, true, 0}};
    if (!sourceFacets_.empty() || !filters_.source.empty()) defs.push_back({ChipId::Source, !filters_.source.empty(), true, 0});
    if (!templFacets_.empty() || filters_.templ != UINT32_MAX) defs.push_back({ChipId::Pattern, filters_.templ != UINT32_MAX, true, 0});
    if (!collectionsView_) defs.push_back({ChipId::Recursive, settings_.recursive, false, 0xE8B7});
    auto chipW = [&](const ChipDef& d) {
        return r_.MeasureText(ChipLabel(d.id), ff) + 20 + (d.dropdown ? 18 : 0) + (d.glyph ? 22 : 0);
    };
    IDWriteTextFormat* fr = r_.Text(12.5f, DWRITE_FONT_WEIGHT_BOLD);
    bool resetShown = filters_.Any();
    const float resetW = resetShown ? r_.MeasureText(T(L"Zurücksetzen"), fr) + 24 + 8 : 0;
    const float moreW = 36;
    size_t fit = 0;
    {
        float cx = x, lim = right - resetW;
        for (const auto& d : defs) cx += chipW(d) + 2;
        if (cx > lim) {   // nicht alle: Platz fuer "..." lassen, vordere Knoepfe der Reihe nach
            cx = x;
            lim -= moreW + 6;
            for (const auto& d : defs) {
                float w = chipW(d);
                if (cx + w > lim) break;
                cx += w + 2;
                ++fit;
            }
        } else {
            fit = defs.size();
        }
    }
    chipOverflow_.clear();
    for (size_t i = fit; i < defs.size(); ++i) chipOverflow_.push_back(defs[i].id);
    if (!chipOverflow_.empty()) {
        bool anyActive = false;
        for (size_t i = fit; i < defs.size(); ++i) anyActive = anyActive || defs[i].active;
        D2D1_RECT_F rc = D2D1::RectF(right - moreW, y, right, y + h);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::MoreFilters, 0});
        if (openMenu_ == 2000) FillRound(dc, rc, 5, t.press);
        else if (hoverHit_ == idx) FillRound(dc, rc, 5, t.hover);
        else if (anyActive) FillRound(dc, rc, 5, WithAlpha(accent_, 0.12f));   // ein verborgener Filter ist aktiv
        wchar_t gm[2] = {0xE712, 0};
        DrawText(dc, gm, rc, r_.Icon(14), anyActive ? accent_ : t.text);
        right = rc.left - 6;
    }
    if (resetShown) {
        float resetW = r_.MeasureText(T(L"Zurücksetzen"), fr) + 24;
        D2D1_RECT_F rc = D2D1::RectF(right - resetW, y, right, y + h);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::ResetFilters, 0});
        if (hoverHit_ == idx) FillRound(dc, rc, h / 2, t.hover);
        DrawText(dc, T(L"Zurücksetzen"), D2D1::RectF(rc.left + 12, rc.top, rc.right + 20, rc.bottom), fr, accent_);
        right = rc.left - 8;
    }
    float limit = right;

    // Auswahlfilter mit Dropdown: flache Knoepfe wie im Windows Explorer (duenne, etwas groessere Schrift).
    // Erkannte Begriffe (rom, urlaub ...): weiterhin Pillen.
    auto chip = [&](HitType type, int a, const std::wstring& label, const std::wstring& count, bool active,
                    bool dropdown, wchar_t glyph) -> bool {
        bool flat = type == HitType::Chip;
        IDWriteTextFormat* lf = flat ? ff : f;
        float lw = r_.MeasureText(label, lf);
        float pad = flat ? 10.0f : 12.0f;
        float w = lw + 2 * pad + (dropdown ? 18 : 0) + (glyph ? 22 : 0);
        float cw = count.empty() ? 0 : r_.MeasureText(count, fc) + 8;
        w += cw;
        if (x + w > limit) return false;
        D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, type, a});
        bool open = type == HitType::Chip && openMenu_ == a;
        D2D1_COLOR_F fg;
        if (flat) {
            if (open) FillRound(dc, rc, 5, t.press);
            else if (hoverHit_ == idx) FillRound(dc, rc, 5, t.hover);
            else if (active) FillRound(dc, rc, 5, WithAlpha(accent_, 0.12f));
            fg = active ? accent_ : t.text;
        } else {
            D2D1_COLOR_F bg = active ? accent_ : (hoverHit_ == idx ? t.chipHover : t.chip);
            FillRound(dc, rc, h / 2, bg);
            fg = active ? t.onAccent : t.text;
        }
        float tx = rc.left + pad;
        if (glyph) {
            wchar_t g[2] = {glyph, 0};
            DrawText(dc, g, D2D1::RectF(tx, rc.top, tx + 16, rc.bottom), r_.Icon(13), fg);
            tx += 22;
        }
        DrawText(dc, label, D2D1::RectF(tx, rc.top, tx + lw + 4, rc.bottom), lf, fg);
        tx += lw;
        if (!count.empty()) {
            DrawText(dc, count, D2D1::RectF(tx + 6, rc.top, tx + cw + 8, rc.bottom), fc, WithAlpha(fg, fg.a * 0.7f));
            tx += cw;
        }
        if (dropdown) {
            wchar_t g[2] = {static_cast<wchar_t>(open ? 0xE70E : 0xE70D), 0};
            DrawText(dc, g, D2D1::RectF(tx + 6, rc.top, tx + 18, rc.bottom), r_.Icon(9), fg);
        }
        x += w + (flat ? 2 : 6);
        return true;
    };

    for (size_t i = 0; i < fit; ++i)
        chip(HitType::Chip, static_cast<int>(defs[i].id), ChipLabel(defs[i].id), L"", defs[i].active, defs[i].dropdown,
             defs[i].glyph);
    if (!chipOverflow_.empty()) return;   // kein Platz mehr fuer erkannte Begriffe

    x += 8;
    for (uint32_t id : filters_.terms) {
        uint32_t n = 0;
        for (auto& fct : termFacets_) if (fct.id == id) n = fct.count;
        if (!chip(HitType::Term, static_cast<int>(id), dict_.Word(id), n ? Thousands(n) : std::wstring(), true, false, 0))
            break;
    }
    for (auto& fct : termFacets_) {
        if (std::find(filters_.terms.begin(), filters_.terms.end(), fct.id) != filters_.terms.end()) continue;
        if (!chip(HitType::Term, static_cast<int>(fct.id), dict_.Word(fct.id), Thousands(fct.count), false, false, 0))
            break;
    }
}

// Liste der letzten Sucheingaben unter dem Suchfeld (jeder Eintrag mit X, unten "Alle loeschen")
void App::PaintSearchHistory(ID2D1DeviceContext* dc) {
    const auto& h = settings_.searchHistory;
    const float rowH = 34;
    // oben: Suchparameter als blaue Pillen (umbrechend)
    const auto& params = QueryParams();
    IDWriteTextFormat* fp = r_.Text(12.5f, DWRITE_FONT_WEIGHT_BOLD);   // wie die Begriffs-Pillen
    float width = std::max(searchRc_.right - searchRc_.left, 340.0f);
    float left = std::max(8.0f, std::min(searchRc_.left, W_ - 8 - width));
    std::vector<D2D1_RECT_F> pills;
    const float ph = 30;
    float px = left + 10, py = searchRc_.bottom + 4 + 10;
    for (auto& p : params) {
        float w = r_.MeasureText(p.label, fp) + 24;
        if (px + w > left + width - 10 && px > left + 10) { px = left + 10; py += ph + 6; }
        pills.push_back(D2D1::RectF(px, py, px + w, py + ph));
        px += w + 6;
    }
    float pillsBottom = py + ph + 10;
    float listH = h.empty() ? 0 : 8 + rowH * (h.size() + 1) + 9;
    D2D1_RECT_F rc = D2D1::RectF(left, searchRc_.bottom + 4, left + width, pillsBottom + listH);
    histRc_ = rc;
    // Hereinschieben: von oben unter dem Suchfeld hervor (begrenzt auf den Bereich darunter)
    const float ha = EaseOut(histAnim_);
    struct Slide {
        ID2D1DeviceContext* dc;
        D2D1_MATRIX_3X2_F old;
        ~Slide() {
            dc->SetTransform(old);
            dc->PopAxisAlignedClip();
        }
    } slide{dc, {}};
    dc->GetTransform(&slide.old);
    dc->PushAxisAlignedClip(D2D1::RectF(rc.left - 4, searchRc_.bottom + 2, rc.right + 4, rc.bottom + 6), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0, -(1.0f - ha) * (rc.bottom - rc.top + 8)) * slide.old);
    hits_.push_back({rc, HitType::None, 0});
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rc.left - 1, rc.top - 1, rc.right + 1, rc.bottom + 2), 8, 8),
                             r_.Brush(D2D1::ColorF(0, 0, 0, 0.18f)));
    dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 8, 8), r_.Brush(D2D1::ColorF(0.15f, 0.15f, 0.15f, 0.97f)));
    IDWriteTextFormat* f = r_.Text(13);
    D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 0.92f), dim = D2D1::ColorF(1, 1, 1, 0.55f);
    for (size_t i = 0; i < pills.size(); ++i) {
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({pills[i], HitType::SearchParam, static_cast<int>(i)});
        D2D1_COLOR_F bg = accent_;
        if (hoverHit_ == idx) bg = D2D1::ColorF(std::min(1.0f, bg.r * 1.15f + 0.05f), std::min(1.0f, bg.g * 1.15f + 0.05f),
                                                std::min(1.0f, bg.b * 1.15f + 0.05f), 1);
        FillRound(dc, pills[i], ph / 2, bg);
        DrawText(dc, params[i].label, pills[i], r_.Text(12.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER),
                 D2D1::ColorF(1, 1, 1, 1));
    }
    if (h.empty()) return;
    dc->DrawLine(D2D1::Point2F(rc.left + 10, pillsBottom - 2), D2D1::Point2F(rc.right - 10, pillsBottom - 2),
                 r_.Brush(D2D1::ColorF(1, 1, 1, 0.08f)), 1.0f);
    float y = pillsBottom + 2;
    for (size_t i = 0; i < h.size(); ++i) {
        D2D1_RECT_F row = D2D1::RectF(rc.left + 4, y, rc.right - 4, y + rowH);
        D2D1_RECT_F del = D2D1::RectF(row.right - 32, row.top + 3, row.right - 2, row.bottom - 3);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({row, HitType::SearchHistItem, static_cast<int>(i)});
        int dIdx = static_cast<int>(hits_.size());
        hits_.push_back({del, HitType::SearchHistDel, static_cast<int>(i)});
        if (hoverHit_ == idx || hoverHit_ == dIdx)
            dc->FillRoundedRectangle(D2D1::RoundedRect(row, 4, 4), r_.Brush(D2D1::ColorF(1, 1, 1, 0.08f)));
        wchar_t gh[2] = {0xE81C, 0};
        DrawText(dc, gh, D2D1::RectF(row.left + 6, row.top, row.left + 28, row.bottom), r_.Icon(12), dim);
        DrawText(dc, h[i], D2D1::RectF(row.left + 34, row.top, del.left - 4, row.bottom), f, white);
        if (hoverHit_ == dIdx)
            dc->FillRoundedRectangle(D2D1::RoundedRect(del, 4, 4), r_.Brush(D2D1::ColorF(1, 1, 1, 0.12f)));
        wchar_t gx[2] = {0xE711, 0};
        DrawText(dc, gx, del, r_.Icon(10), dim);
        y += rowH;
    }
    y += 4;
    dc->DrawLine(D2D1::Point2F(rc.left + 10, y), D2D1::Point2F(rc.right - 10, y), r_.Brush(D2D1::ColorF(1, 1, 1, 0.08f)), 1.0f);
    y += 4;
    D2D1_RECT_F all = D2D1::RectF(rc.left + 4, y, rc.right - 4, y + rowH);
    int aIdx = static_cast<int>(hits_.size());
    hits_.push_back({all, HitType::SearchHistClear, 0});
    if (hoverHit_ == aIdx) dc->FillRoundedRectangle(D2D1::RoundedRect(all, 4, 4), r_.Brush(D2D1::ColorF(1, 1, 1, 0.08f)));
    wchar_t gd[2] = {0xE74D, 0};
    DrawText(dc, gd, D2D1::RectF(all.left + 6, all.top, all.left + 28, all.bottom), r_.Icon(12), accent_);
    DrawText(dc, T(L"Alle l\u00f6schen"), D2D1::RectF(all.left + 34, all.top, all.right, all.bottom), f, accent_);
}

// ---------------------------------------------------------------------------
// Seitenleiste (animiert, Texte umbrechend, Sammlungen verschiebbar)
// ---------------------------------------------------------------------------

void App::PaintSidebar(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    float sw = SidebarWidth();
    float ox = sw - sidebarW_;
    float bottom = H_ - statusH_;
    dc->PushAxisAlignedClip(D2D1::RectF(0, topH_, sw, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    if (dropSidebar_) {
        D2D1_RECT_F dr = D2D1::RectF(ox + 4, topH_ + 2, ox + sidebarW_ - 4, bottom - 4);
        FillRound(dc, dr, 6, WithAlpha(accent_, 0.12f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(dr, 6, 6), r_.Brush(accent_), 1.5f);
    }
    float y = topH_ + 6;
    float itemsBottom = bottom - 52;
    IDWriteTextFormat* fw = r_.TextWrap(13);
    IDWriteTextFormat* fh = r_.TextWrap(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float textX = ox + 48, textW = sidebarW_ - 48 - 16;

    auto addButton = [&]() {
        float th = r_.MeasureWrapped(T(L"Ordner hinzufügen"), fw, textW);
        float ih = std::max(34.0f, th + 16);
        D2D1_RECT_F ar = D2D1::RectF(ox + 8, y, ox + sidebarW_ - 8, y + ih);
        int aIdx = static_cast<int>(hits_.size());
        hits_.push_back({ar, HitType::SidebarAdd, 0});
        if (hoverHit_ == aIdx) FillRound(dc, ar, 5, t.hover);
        wchar_t ga[2] = {0xE710, 0};
        DrawText(dc, ga, D2D1::RectF(ar.left + 10, ar.top, ar.left + 34, ar.top + 34), r_.Icon(12), t.text2);
        r_.DrawWrapped(T(L"Ordner hinzufügen"), textX, ar.top + (ih - th) / 2, textW, fw, t.text2);
        y += ih + 2;
    };

    for (size_t i = 0; i < sidebar_.size() && y + 34 < itemsBottom; ++i) {
        const SidebarItem& it = sidebar_[i];
        if (it.kind == SidebarItem::Header) {
            y += 6;
            y += r_.DrawWrapped(it.label, ox + 20, y + 4, sidebarW_ - 32, fh, t.text3) + 10;
            if (it.label == T(L"Sammlungen") && settings_.collections.empty()) addButton();
            continue;
        }
        float th = r_.MeasureWrapped(it.label, fw, textW);
        float ih = std::max(34.0f, th + 16);
        D2D1_RECT_F rc = D2D1::RectF(ox + 8, y, ox + sidebarW_ - 8, y + ih);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::SidebarItem, static_cast<int>(i)});
        bool active = (it.kind == SidebarItem::AllCollections && collectionsView_) ||
                      (it.kind == SidebarItem::Bookmarks && bookmarksView_) ||
                      (!collectionsView_ && !bookmarksView_ && !it.path.empty() && _wcsicmp(it.path.c_str(), path_.c_str()) == 0);
        bool dragging = sideDragActive_ && static_cast<int>(i) == sideDragItem_;
        if (static_cast<int>(i) == dropSideItem_) {
            FillRound(dc, rc, 5, WithAlpha(accent_, 0.25f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 5, 5), r_.Brush(accent_), 1.5f);
        }
        if (it.kind == SidebarItem::Bookmarks && dropBookmarks_) {
            FillRound(dc, rc, 5, WithAlpha(accent_, 0.25f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(rc, 5, 5), r_.Brush(accent_), 1.5f);
        }
        if (active || dragging || hoverHit_ == idx) FillRound(dc, rc, 5, (active || dragging) ? t.press : t.hover);
        if (active) FillRound(dc, D2D1::RectF(rc.left, rc.top + 10, rc.left + 3, rc.bottom - 10), 1.5f, accent_);
        wchar_t g[2] = {it.glyph, 0};
        DrawText(dc, g, D2D1::RectF(rc.left + 10, rc.top, rc.left + 34, rc.top + 34), r_.Icon(14), t.text2);
        r_.DrawWrapped(it.label, textX, rc.top + (ih - th) / 2, textW, fw, t.text);
        y += ih + 2;
        bool lastCollection = (it.kind == SidebarItem::Collection || it.kind == SidebarItem::AllCollections) &&
                              (i + 1 >= sidebar_.size() || sidebar_[i + 1].kind == SidebarItem::Header);
        if (lastCollection) addButton();
    }

    // Einfuegemarke beim Verschieben einer Sammlung
    if (sideDragActive_ && sideInsert_ >= 0) {
        std::vector<D2D1_RECT_F> rects;
        for (auto& hit : hits_)
            if (hit.type == HitType::SidebarItem && sidebar_[hit.a].kind == SidebarItem::Collection) rects.push_back(hit.rc);
        if (!rects.empty()) {
            float ly = sideInsert_ < static_cast<int>(rects.size()) ? rects[sideInsert_].top - 1 : rects.back().bottom + 1;
            D2D1_RECT_F line = D2D1::RectF(ox + 12, ly - 1, ox + sidebarW_ - 12, ly + 1);
            FillRound(dc, line, 1, accent_);
        }
    }

    // Einstellungen unten links
    D2D1_RECT_F gr = D2D1::RectF(ox + 8, bottom - 46, ox + sidebarW_ - 8, bottom - 10);
    int gIdx = static_cast<int>(hits_.size());
    hits_.push_back({gr, HitType::SettingsButton, 0});
    if (hoverHit_ == gIdx) FillRound(dc, gr, 5, t.hover);
    wchar_t gg[2] = {0xE713, 0};
    DrawText(dc, gg, D2D1::RectF(gr.left + 10, gr.top, gr.left + 34, gr.bottom), r_.Icon(15), t.text2);
    DrawText(dc, T(L"Einstellungen"), D2D1::RectF(gr.left + 40, gr.top, gr.right, gr.bottom), r_.Text(13), t.text);
    dc->PopAxisAlignedClip();
}

// ---------------------------------------------------------------------------
// Raster und Detailansicht
// ---------------------------------------------------------------------------

void App::PaintGrid(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    bool list = settings_.viewMode == 1;
    // Inhaltsflaeche: eckig, ohne Rahmenlinien
    D2D1_RECT_F lr = D2D1::RectF(gridRc_.left, topH_, gridRc_.right, gridRc_.bottom);
    dc->FillRectangle(lr, r_.Brush(t.layer));
    if (list) PaintListHeader(dc);
    if (bookmarksView_) {
        // Zusaetzliche Leiste der Merkliste
        D2D1_RECT_F br = bookmarkBarRc_;
        wchar_t gs[2] = {0xE735, 0};
        DrawText(dc, gs, D2D1::RectF(br.left + 16, br.top, br.left + 36, br.bottom), r_.Icon(14), accent_);
        std::wstring label = T(L"Merkliste") + std::wstring(L"  \u00b7  ") + Thousands(entries_.size()) +
                             (entries_.size() == 1 ? T(L" Element") : T(L" Elemente"));
        DrawText(dc, label, D2D1::RectF(br.left + 44, br.top, br.right - 200, br.bottom), r_.Text(14), t.text);
        if (!settings_.bookmarks.empty()) {
            IDWriteTextFormat* bf = r_.Text(13.5f);
            std::wstring bl = T(L"Merkliste leeren");
            float bw = r_.MeasureText(bl, bf) + 46;
            D2D1_RECT_F bb = D2D1::RectF(br.right - 16 - bw, br.top + 6, br.right - 16, br.bottom - 6);
            int idx = static_cast<int>(hits_.size());
            hits_.push_back({bb, HitType::ClearBookmarks, 0});
            FillRound(dc, bb, 5, hoverHit_ == idx ? t.chipHover : t.chip);
            wchar_t gd[2] = {0xE74D, 0};
            DrawText(dc, gd, D2D1::RectF(bb.left + 10, bb.top, bb.left + 28, bb.bottom), r_.Icon(12), t.text2);
            DrawText(dc, bl, D2D1::RectF(bb.left + 34, bb.top, bb.right, bb.bottom), bf, t.text);
            // links daneben: Kontaktabzug aus der Merkliste (ausgewaehlte oder alle Elemente)
            std::wstring kl = T(L"Kontaktabzug erstellen \u2026");
            float kw = r_.MeasureText(kl, bf) + 46;
            D2D1_RECT_F kb = D2D1::RectF(bb.left - 8 - kw, bb.top, bb.left - 8, bb.bottom);
            int kIdx = static_cast<int>(hits_.size());
            hits_.push_back({kb, HitType::BookmarkSheet, 0});
            FillRound(dc, kb, 5, hoverHit_ == kIdx ? t.chipHover : t.chip);
            wchar_t gk[2] = {0xECA5, 0};
            DrawText(dc, gk, D2D1::RectF(kb.left + 10, kb.top, kb.left + 28, kb.bottom), r_.Icon(12), t.text2);
            DrawText(dc, kl, D2D1::RectF(kb.left + 34, kb.top, kb.right, kb.bottom), bf, t.text);
        }
    }

    if (settings_.viewMode == 2 && !metaHold_ && !visible_.empty()) {
        PaintCloud(dc);
        return;
    }
    if (settings_.viewMode == 3 && !metaHold_ && !visible_.empty()) {
        PaintTimeline(dc);
        return;
    }
    dc->PushAxisAlignedClip(gridRc_, D2D1_ANTIALIAS_MODE_ALIASED);
    float gh = gridRc_.bottom - gridRc_.top;
    int n = metaHold_ ? 0 : static_cast<int>(visible_.size());
    if (n == 0) {
        std::wstring msg =
            (loading_ || metaHold_) ? T(L"Wird geladen …")
                     : (entries_.empty()
                            ? (bookmarksView_ ? T(L"Die Merkliste ist leer \u2013 Elemente per Kontextmen\u00fc oder durch Ziehen auf \u201eMerkliste\u201c hinzuf\u00fcgen.") : collectionsView_ ? (settings_.collections.empty()
                                                       ? T(L"Noch keine Sammlungen – Ordner in die Seitenleiste ziehen")
                                                       : T(L"Sammlungen werden indexiert …"))
                                                : T(L"Dieser Ordner ist leer."))
                            : T(L"Keine Treffer für diese Filter."));
        IDWriteTextFormat* fw = r_.TextWrap(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        r_.DrawWrapped(msg, gridRc_.left + 20, gridRc_.top + 40, gridRc_.right - gridRc_.left - 40, fw, t.text2);
    } else {
        int rows = static_cast<int>(rowTop_.size()) - 1;
        int firstRow = RowAtY(std::max(0.0f, scroll_ - 8));
        int lastRow = firstRow;
        for (int row = firstRow; row < rows; ++row) {
            if (8 + rowTop_[row] - scroll_ > gh) break;
            lastRow = row;
            if (rowHeader_[row] >= 0) {
                // Gruppenkopf: Beschriftung, Anzahl, feine Linie
                float top = gridRc_.top + 8 + rowTop_[row] - scroll_;
                float lx = gridRc_.left + 16;
                const std::wstring& gl = groupLabels_[rowHeader_[row]];
                IDWriteTextFormat* gf = r_.Text(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                float gw2 = r_.MeasureText(gl, gf);
                DrawText(dc, gl, D2D1::RectF(lx, top + 10, gridRc_.right - 20, top + kGroupH - 4), gf, t.text);
                std::wstring cnt = Thousands(groupCounts_[rowHeader_[row]]);
                IDWriteTextFormat* cf = r_.Text(12);
                float cw2 = r_.MeasureText(cnt, cf);
                DrawText(dc, cnt, D2D1::RectF(lx + gw2 + 10, top + 10, lx + gw2 + 14 + cw2, top + kGroupH - 4), cf, t.text3);
                float ly = std::round((top + kGroupH / 2 + 3) * r_.scale()) / r_.scale() + 0.5f;
                float l0 = lx + gw2 + cw2 + 26;
                if (l0 < gridRc_.right - 24)
                    dc->DrawLine(D2D1::Point2F(l0, ly), D2D1::Point2F(gridRc_.right - 24, ly), r_.Brush(t.stroke), 1.0f);
                continue;
            }
            int p0 = FirstPosOfRow(row), p1 = RowEnd(row);
            for (int pos = p0; pos < p1; ++pos) {
                D2D1_RECT_F rc = ItemRect(pos);
                float dy = gridRc_.top - scroll_;
                rc.top += dy;
                rc.bottom += dy;
                if (list) PaintListRow(dc, pos, rc);
                else PaintTile(dc, pos, rc);
            }
        }
        // Vorausladen: eine Bildschirmhoehe darunter und darueber
        int pre = std::max(2, static_cast<int>(gh / cellH_) + 1);
        auto prefetch = [&](int row) {
            if (row < 0 || row >= rows) return;
            int p0 = FirstPosOfRow(row), p1 = RowEnd(row);
            for (int pos = p0; pos < p1; ++pos) EnsureThumb(entries_[visible_[pos]], 1, frame_);
        };
        for (int row = lastRow + 1; row <= lastRow + pre; ++row) prefetch(row);
        for (int row = firstRow - pre; row < firstRow; ++row) prefetch(row);
    }

    // Auswahlrechteck
    if (marquee_) {
        float dy = gridRc_.top - scroll_;
        D2D1_RECT_F mr = D2D1::RectF(std::min(marqueeStart_.x, marqueeCur_.x), std::min(marqueeStart_.y, marqueeCur_.y) + dy,
                                     std::max(marqueeStart_.x, marqueeCur_.x), std::max(marqueeStart_.y, marqueeCur_.y) + dy);
        dc->FillRectangle(mr, r_.Brush(WithAlpha(accent_, 0.18f)));
        dc->DrawRectangle(mr, r_.Brush(WithAlpha(accent_, 0.85f)), 1.0f);
    }
    dc->PopAxisAlignedClip();

    if (contentH_ > gh) {
        float trackTop = gridRc_.top + 4, trackH = gh - 8;
        float thumbH = std::max(32.0f, trackH * gh / contentH_);
        float maxScroll = contentH_ - gh;
        float ty = trackTop + (trackH - thumbH) * (maxScroll > 0 ? scroll_ / maxScroll : 0);
        float w = (hoverScrollbar_ || scrollDrag_) ? 6.0f : 3.0f;
        D2D1_RECT_F th = D2D1::RectF(gridRc_.right - 6 - w, ty, gridRc_.right - 6, ty + thumbH);
        FillRound(dc, th, w / 2, WithAlpha(t.text, (hoverScrollbar_ || scrollDrag_) ? 0.45f : 0.3f));
    }
}

void App::PaintTile(ID2D1DeviceContext* dc, int pos, const D2D1_RECT_F& cell) {
    Theme t = MakeTheme(dark_, backdrop_);
    uint32_t idx = visible_[pos];
    const FileEntry& e = entries_[idx];
    bool sel = selected_.count(idx) != 0;
    bool hov = pos == hoverPos_;
    bool foc = pos == focus_;
    D2D1_RECT_F inner = D2D1::RectF(cell.left + 3, cell.top + 3, cell.right - 3, cell.bottom - 3);
    if (sel) {
        FillRound(dc, inner, 6, WithAlpha(accent_, dark_ ? 0.30f : 0.22f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, 6, 6), r_.Brush(WithAlpha(accent_, 0.9f)), 1.0f);
    } else if (hov) {
        FillRound(dc, inner, 6, t.hover);
    }
    if (foc && !sel) dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, 6, 6), r_.Brush(t.text3), 1.0f);
    if (pos == dropPos_) {
        FillRound(dc, inner, 6, WithAlpha(accent_, 0.25f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, 6, 6), r_.Brush(accent_), 2.0f);
    }

    float pad = 8;
    D2D1_RECT_F box = D2D1::RectF(inner.left + pad, inner.top + pad, inner.right - pad, inner.top + pad + tileS_);

    auto drawIcon = [&](const FileEntry& fe, const D2D1_RECT_F& b, float frac) {
        EnsureIcon(fe, 0);
        Bmp* ib = &bmps_[IconKey(fe)];
        float bw = b.right - b.left, bh = b.bottom - b.top;
        float side = std::min(bw, bh) * frac;
        D2D1_RECT_F ir = D2D1::RectF(b.left + (bw - side) / 2, b.top + (bh - side) / 2, 0, 0);
        ir.right = ir.left + side;
        ir.bottom = ir.top + side;
        D2D1_RECT_F drawn{};
        if (DrawBitmapFit(dc, ib, ir, 2.0f, 1.0f, &drawn)) return drawn;
        return ir;
    };

    D2D1_RECT_F shown = box;   // tatsaechlich gezeichnete Flaeche (fuer das Merkliste-Symbol)
    if (e.kind == Kind::Folder) {
        D2D1_RECT_F fr = drawIcon(e, box, 0.92f);
        shown = fr;
        if (tileS_ >= 96 && e.coverChecked && !e.coverPath.empty()) {
            Bmp& cb = bmps_[e.coverPath];
            cb.lastUse = frame_;
            if (!cb.bmp && !cb.failed && !(cb.requested && cb.reqPrio == 0)) {
                cb.requested = true;
                cb.reqPrio = 0;
                ThumbRequest r;
                r.key = e.coverPath;
                r.path = e.coverPath;
                r.kind = e.coverKind;
                r.size = e.coverSize;
                r.mtime = e.coverMtime;
                r.prio = 0;
                r.gen = gen_;
                thumbs_.Request(std::move(r));
            }
            if (cb.bmp) {
                float fw = fr.right - fr.left, fh = fr.bottom - fr.top;
                D2D1_RECT_F opening = D2D1::RectF(fr.left + fw * 0.14f, fr.top + fh * 0.30f, fr.right - fw * 0.14f,
                                                  fr.bottom - fh * 0.16f);
                D2D1_RECT_F drawn{};
                if (DrawBitmapFit(dc, &cb, opening, 4.0f, 1.0f, &drawn)) {
                    dc->DrawRectangle(drawn, r_.Brush(D2D1::ColorF(1, 1, 1, 0.9f)), 1.5f);
                    if (e.coverKind == Kind::Video) {
                        wchar_t g[2] = {0xE768, 0};
                        float c = 10;
                        D2D1_RECT_F pr = D2D1::RectF(drawn.right - 2 * c - 4, drawn.bottom - 2 * c - 4, drawn.right - 4,
                                                     drawn.bottom - 4);
                        dc->FillEllipse(
                            D2D1::Ellipse(D2D1::Point2F((pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2), c, c),
                            r_.Brush(D2D1::ColorF(0, 0, 0, 0.55f)));
                        DrawText(dc, g, pr, r_.Icon(9), D2D1::ColorF(1, 1, 1, 1));
                    }
                }
            }
        }
    } else if (e.HasThumb() && !e.offline) {
        EnsureThumb(e, 0, frame_);
        Bmp& b = bmps_[e.path];
        D2D1_RECT_F drawn{};
        bool ok = DrawBitmapFit(dc, &b, box, 2.0f, 1.0f, &drawn);
        if (!ok) {
            // Dokumente: bis das Vorschaubild da ist (oder wenn keins moeglich ist) das grosse Dateisymbol
            if (b.failed || e.Doc()) drawn = drawIcon(e, box, 0.62f);
            else {
                D2D1_RECT_F ph = D2D1::RectF(box.left + 6, box.top + 6, box.right - 6, box.bottom - 6);
                FillRound(dc, ph, 4, t.hover);
                drawn = ph;
            }
        }
        shown = drawn;
        if (ok && e.Doc()) {
            // Dokument: Symbol der Standard-App unten rechts
            if (ID2D1Bitmap1* ai = AppIconFor(e.ext)) {
                float sz = std::clamp(tileS_ * 0.16f, 16.0f, 28.0f);
                D2D1_RECT_F ir = D2D1::RectF(drawn.right - sz - 6, drawn.bottom - sz - 6, drawn.right - 6, drawn.bottom - 6);
                D2D1_RECT_F bg = D2D1::RectF(ir.left - 3, ir.top - 3, ir.right + 3, ir.bottom + 3);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bg.left, bg.top + 1, bg.right, bg.bottom + 1), 5, 5),
                                         r_.Brush(D2D1::ColorF(0, 0, 0, 0.25f)));
                dc->FillRoundedRectangle(D2D1::RoundedRect(bg, 5, 5), r_.Brush(D2D1::ColorF(1, 1, 1, 0.95f)));
                dc->DrawBitmap(ai, ir, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            }
        }
        if (e.kind == Kind::Video && ok) {
            float c = std::clamp(tileS_ * 0.09f, 10.0f, 18.0f);
            D2D1_POINT_2F mid = D2D1::Point2F((drawn.left + drawn.right) / 2, (drawn.top + drawn.bottom) / 2);
            dc->FillEllipse(D2D1::Ellipse(mid, c, c), r_.Brush(D2D1::ColorF(0, 0, 0, 0.5f)));
            wchar_t g[2] = {0xE768, 0};
            DrawText(dc, g, D2D1::RectF(mid.x - c, mid.y - c, mid.x + c, mid.y + c), r_.Icon(c * 0.8f),
                     D2D1::ColorF(1, 1, 1, 0.95f));
            if (e.durationMs) {
                std::wstring d = FormatDuration(e.durationMs);
                IDWriteTextFormat* fd = r_.Text(11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
                float w = r_.MeasureText(d, fd) + 10;
                D2D1_RECT_F br = D2D1::RectF(drawn.right - w - 4, drawn.bottom - 22, drawn.right - 4, drawn.bottom - 4);
                FillRound(dc, br, 3, D2D1::ColorF(0, 0, 0, 0.65f));
                DrawText(dc, d, br, fd, D2D1::ColorF(1, 1, 1, 1));
            }
        }
    } else {
        shown = drawIcon(e, box, 0.62f);
    }

    // Merkliste: kleiner weisser Stern oben links (mit dunklem Schatten, damit er auf hellen Bildern sichtbar bleibt)
    if (!bookmarkSet_.empty() && bookmarkSet_.count(ToLower(e.path))) {
        float sz = std::clamp(tileS_ * 0.1f, 12.0f, 18.0f);
        // mit Auswahlkaestchen liegt oben links das Kaestchen: Stern dann oben rechts
        D2D1_RECT_F sr = settings_.checkBoxes
                             ? D2D1::RectF(shown.right - 8 - sz, shown.top + 4, shown.right - 4, shown.top + 4 + sz + 4)
                             : D2D1::RectF(shown.left + 4, shown.top + 4, shown.left + 4 + sz + 4, shown.top + 4 + sz + 4);
        wchar_t g[2] = {0xE735, 0};
        IDWriteTextFormat* fi = r_.Icon(sz);
        DrawText(dc, g, D2D1::RectF(sr.left + 1, sr.top + 1.5f, sr.right + 1, sr.bottom + 1.5f), fi, D2D1::ColorF(0, 0, 0, 0.55f));
        DrawText(dc, g, sr, fi, D2D1::ColorF(1, 1, 1, 1));
    }

    // Auswahlkaestchen (Einstellung): oben links (beim Ueberfahren, bei ausgewaehlten Elementen immer)
    if (settings_.checkBoxes && (sel || hov)) {
        D2D1_RECT_F cb = CheckRect(pos);
        float dx = cell.left - ItemRect(pos).left, dy = cell.top - ItemRect(pos).top;
        cb.left += dx; cb.right += dx; cb.top += dy; cb.bottom += dy;
        PaintCheck(dc, cb, sel ? 1 : 0, hov && hoverCheck_);
    }

    // Name: vollstaendig, auf mehrere Zeilen umbrochen
    float nameW = cellW_ - 6 - 8;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    r_.BreakLines(e.name, nameW, kNameSize, lines);
    IDWriteTextFormat* fn = r_.Text(kNameSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    float ly = box.bottom + 4;
    for (auto& [s, len] : lines) {
        DrawText(dc, e.name.substr(s, len), D2D1::RectF(inner.left - 2, ly, inner.right + 2, ly + kLineH), fn,
                 e.offline ? t.text3 : t.text);
        ly += kLineH;
    }
}

// Aufnahmedaten im Info-Bereich: beim ersten Anzeigen im Hintergrund aus dem Dateikopf lesen (Sandbox), dann zeigen
void App::PhotoDetailRows(const FileEntry& e, const std::function<void(const std::wstring&, const std::wstring&)>& row) {
    if (e.kind != Kind::Image || e.offline) return;
    auto it = photoDetails_.find(e.path);
    if (it == photoDetails_.end()) {
        if (photoDetailsAsked_.insert(e.path).second) {
            ThumbRequest r;
            r.key = e.path;
            r.path = e.path;
            r.kind = e.kind;
            r.job = ThumbJob::Details;
            r.prio = 0;
            thumbs_.Request(std::move(r));
        }
        return;
    }
    // Objektiv \x1f Blende \x1f Belichtung (s) \x1f ISO
    std::wstring parts[4];
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        size_t sep = it->second.find(L'\x1f', start);
        parts[i] = it->second.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start);
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    auto number = [](const std::wstring& s, double& d) { return !s.empty() && (d = _wtof(s.c_str())) > 0; };
    auto decimal = [](double d) {   // eine Nachkommastelle, ",0" weglassen; Dezimalzeichen je Sprache
        wchar_t b[32];
        swprintf_s(b, L"%.1f", d);
        std::wstring s = b;
        if (s.size() > 2 && s.compare(s.size() - 2, 2, L".0") == 0) s.resize(s.size() - 2);
        if (!LangEn())
            for (auto& c : s) if (c == L'.') c = L',';
        return s;
    };
    double d = 0;
    row(T(L"Objektiv"), parts[0]);
    if (number(parts[1], d)) row(T(L"Blende"), L"f/" + decimal(d));
    if (number(parts[2], d)) {
        std::wstring v;
        if (d < 0.5) v = L"1/" + std::to_wstring(static_cast<long long>(std::llround(1.0 / d))) + L" s";
        else v = decimal(d) + L" s";
        row(T(L"Belichtung"), v);
    }
    if (number(parts[3], d)) row(L"ISO", std::to_wstring(static_cast<long long>(std::llround(d))));
}

// Auswahlkaestchen (Einstellung): schmale feste Spalte mit Auswahlkaestchen links vor dem Namen
static constexpr float kCheckColW = 30;

float App::ListX0() const { return gridRc_.left + 8 + (settings_.checkBoxes ? kCheckColW : 0); }

D2D1_RECT_F App::CheckRect(int pos) const {
    D2D1_RECT_F rc = ItemRect(pos);
    if (settings_.viewMode == 1) {
        float x = gridRc_.left + 8 + (kCheckColW - 16) / 2;
        return D2D1::RectF(x, rc.top + 9, x + 16, rc.top + 25);
    }
    return D2D1::RectF(rc.left + 9, rc.top + 9, rc.left + 29, rc.top + 29);   // Kachel: oben links in der Ecke
}

bool App::OnCheck(int pos, float x, float y) const {
    if (!settings_.checkBoxes || settings_.viewMode >= 2 || pos < 0 || pos >= static_cast<int>(visible_.size())) return false;
    D2D1_RECT_F cb = CheckRect(pos);
    float cy = y - gridRc_.top + scroll_;
    if (settings_.viewMode == 1)   // Detailansicht: die ganze Spalte in Zeilenhoehe
        return x >= gridRc_.left + 4 && x < gridRc_.left + 8 + kCheckColW && cy >= cb.top - 8 && cy <= cb.bottom + 8;
    return x >= cb.left - 5 && x <= cb.right + 5 && cy >= cb.top - 5 && cy <= cb.bottom + 5;
}

// Auswahlkaestchen im Stil von Windows 11: leer mit Rahmen, ausgewaehlt in Akzentfarbe mit Haken
void App::PaintCheck(ID2D1DeviceContext* dc, const D2D1_RECT_F& rc, int state, bool hot) {
    Theme t = MakeTheme(dark_, backdrop_);
    float s = rc.right - rc.left;
    if (state == 0) {
        D2D1_COLOR_F bg = dark_ ? D2D1::ColorF(0.12f, 0.12f, 0.12f, hot ? 0.95f : 0.8f) : D2D1::ColorF(1, 1, 1, hot ? 1.0f : 0.9f);
        FillRound(dc, rc, 4, bg);
        D2D1_RECT_F in = D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f, rc.right - 0.5f, rc.bottom - 0.5f);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(in, 3.5f, 3.5f), r_.Brush(hot ? t.text : t.text2), 1.0f);
        return;
    }
    FillRound(dc, rc, 4, hot ? WithAlpha(accent_, 0.9f) : accent_);
    wchar_t g[2] = {static_cast<wchar_t>(state == 1 ? 0xE73E : 0xE738), 0};
    DrawText(dc, g, rc, r_.Icon(s * 0.6f), t.onAccent);
}

// Namensspalte: der Platz, den die uebrigen Spalten am rechten Rand frei lassen (mindestens 200)
float App::ListNameW() const {
    float rest = 0;
    for (int c = 1; c < 5; ++c) rest += static_cast<float>(settings_.colW[c]);
    float avail = (gridRc_.right - 14) - ListX0();
    return std::max(200.0f, avail - rest);
}

float App::ListColsRight() const {
    float r = ListX0() + ListNameW();
    for (int c = 1; c < 5; ++c) r += static_cast<float>(settings_.colW[c]);
    return r;
}

// Symbol und (umbrochener) Name einer Zeile; daneben beginnt Ziehen ein Auswahlrechteck
bool App::OverListLabel(int pos, float x) {
    if (pos < 0 || pos >= static_cast<int>(visible_.size())) return false;
    float x0 = ListX0();
    float nameW = ListNameW() - 48.0f;
    const FileEntry& e = entries_[visible_[pos]];
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    r_.BreakLines(e.name, nameW, kNameSize, lines);
    float widest = 0;
    IDWriteTextFormat* fn = r_.Text(kNameSize);
    for (auto& [s, len] : lines) widest = std::max(widest, r_.MeasureText(e.name.substr(s, len), fn));
    return x >= x0 && x <= x0 + 40 + widest + 6;
}

void App::PaintListHeader(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    IDWriteTextFormat* f = r_.Text(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float x = ListX0();
    if (settings_.checkBoxes) {
        // Kaestchen vor "Name": alle Elemente der Liste auswaehlen bzw. die Auswahl aufheben
        int n = static_cast<int>(visible_.size()), cnt = 0;
        if (!selected_.empty())
            for (uint32_t i : visible_) cnt += selected_.count(i) ? 1 : 0;
        int state = cnt == 0 ? 0 : cnt == n ? 1 : 2;
        float cx = headerRc_.left + 8 + (kCheckColW - 16) / 2, cy = std::round((headerRc_.top + headerRc_.bottom) / 2 - 8);
        D2D1_RECT_F cb = D2D1::RectF(cx, cy, cx + 16, cy + 16);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({D2D1::RectF(headerRc_.left + 4, headerRc_.top + 2, headerRc_.left + 8 + kCheckColW, headerRc_.bottom - 2),
                         HitType::SelectAll, 0});
        PaintCheck(dc, cb, n == 0 ? 0 : state, hoverHit_ == idx);
    }
    for (int c = 0; c < 5; ++c) {
        float w = c == 0 ? ListNameW() : static_cast<float>(settings_.colW[c]);
        D2D1_RECT_F rc = D2D1::RectF(x, headerRc_.top + 2, x + w, headerRc_.bottom - 2);
        if (rc.left >= headerRc_.right - 14) break;
        rc.right = std::min(rc.right, headerRc_.right - 14);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::ListHeader, c});
        if (hoverHit_ == idx) FillRound(dc, rc, 4, t.hover);
        bool sorted = settings_.sortKey == kColSort[c];
        float tx = rc.left + (c == 0 ? 40 : 8);
        float lw = r_.MeasureText(T(kColName[c]), f);
        DrawText(dc, T(kColName[c]), D2D1::RectF(tx, rc.top, rc.right, rc.bottom), f, sorted ? t.text : t.text2);
        if (sorted && tx + lw + 20 < rc.right) {
            wchar_t ga[2] = {static_cast<wchar_t>(settings_.sortDesc ? 0xE74B : 0xE74A), 0};
            DrawText(dc, ga, D2D1::RectF(tx + lw + 6, rc.top, tx + lw + 20, rc.bottom), r_.Icon(10), accent_);
        }
        // Griff zum Aendern der Spaltenbreite: an der linken Kante jeder Spalte rechts vom Namen
        // (die Spalten haengen am rechten Rand, Ziehen nach links verbreitert sie)
        if (c > 0) {
            D2D1_RECT_F grip = D2D1::RectF(x - 4, rc.top, x + 4, rc.bottom);
            int gIdx = static_cast<int>(hits_.size());
            hits_.push_back({grip, HitType::ListResize, c});
            if (hoverHit_ == gIdx || colDrag_ == c)
                FillRound(dc, D2D1::RectF(x - 1, rc.top + 6, x + 1, rc.bottom - 6), 1, accent_);
        }
        x += w;
    }
}

void App::PaintListRow(ID2D1DeviceContext* dc, int pos, const D2D1_RECT_F& row) {
    Theme t = MakeTheme(dark_, backdrop_);
    uint32_t idx = visible_[pos];
    const FileEntry& e = entries_[idx];
    bool sel = selected_.count(idx) != 0;
    D2D1_RECT_F inner = D2D1::RectF(row.left, row.top + 1, row.right, row.bottom - 1);
    if (sel) FillRound(dc, inner, 4, WithAlpha(accent_, dark_ ? 0.30f : 0.22f));
    else if (pos == hoverPos_) FillRound(dc, inner, 4, t.hover);
    if (pos == focus_ && !sel) dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, 4, 4), r_.Brush(t.text3), 1.0f);
    if (pos == dropPos_) {
        FillRound(dc, inner, 4, WithAlpha(accent_, 0.25f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(inner, 4, 4), r_.Brush(accent_), 1.5f);
    }

    if (settings_.checkBoxes && (sel || pos == hoverPos_)) {
        D2D1_RECT_F cb = CheckRect(pos);
        float dy = row.top - ItemRect(pos).top;
        cb.top += dy;
        cb.bottom += dy;
        PaintCheck(dc, cb, sel ? 1 : 0, pos == hoverPos_ && hoverCheck_);
    }
    float x = ListX0();
    // Symbol bzw. kleines Vorschaubild
    D2D1_RECT_F ib = D2D1::RectF(x + 6, row.top + 5, x + 30, row.top + 29);
    Bmp* b = nullptr;
    if (e.HasThumb() && !e.offline) {
        EnsureThumb(e, 0, frame_);
        auto it = bmps_.find(e.path);
        if (it != bmps_.end() && it->second.bmp) b = &it->second;
    }
    if (!b) {
        EnsureIcon(e, 0);
        b = &bmps_[IconKey(e)];
    }
    DrawBitmapFit(dc, b, ib, 2.0f);

    // Name umbrochen
    const float colName = ListNameW();
    float nameW = colName - 48.0f;
    std::vector<std::pair<uint32_t, uint32_t>> lines;
    r_.BreakLines(e.name, nameW, kNameSize, lines);
    IDWriteTextFormat* fn = r_.Text(kNameSize);
    float ly = row.top + 8;
    for (auto& [s, len] : lines) {
        DrawText(dc, e.name.substr(s, len), D2D1::RectF(x + 40, ly, x + colName, ly + kLineH), fn,
                 e.offline ? t.text3 : t.text);
        ly += kLineH;
    }

    // weitere Spalten: einzeilig, am Fensterrand nur abgeschnitten (kein Umbruch)
    IDWriteTextFormat* fw = r_.Text(12);
    float cx = x + colName;
    for (int c = 1; c < 5; ++c) {
        float w = static_cast<float>(settings_.colW[c]);
        if (cx >= gridRc_.right - 14) break;
        std::wstring v = CellText(e, c);
        if (!v.empty())
            DrawText(dc, v, D2D1::RectF(cx + 8, row.top + 9, std::min(cx + w - 8, gridRc_.right - 14), row.top + 9 + kLineH), fw, t.text2);
        cx += w;
    }
}

std::wstring App::CellText(const FileEntry& e, int c) {
    switch (c) {
    case 1: return FormatDate(e.Time());
    case 2: return TypeName(e);
    case 3: return e.kind != Kind::Folder ? FormatSize(e.size) : std::wstring();
    case 4: return (e.width && e.height) ? std::to_wstring(e.width) + L" \u00d7 " + std::to_wstring(e.height) : std::wstring();
    }
    return {};
}

// Spalten Datum, Typ, Groesse, Aufloesung: nie schmaler als Ueberschrift und laengster Inhalt
int App::ColumnMinWidth(int c) {
    if (c <= 0 || c >= 5) return 120;
    IDWriteTextFormat* fh = r_.Text(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fv = r_.Text(12);
    float m = r_.MeasureText(T(kColName[c]), fh) + 8 + 26;
    std::unordered_map<size_t, bool> seenLen;   // Laengen-Stichprobe: gleiche Textlaenge selten breiter
    for (uint32_t i : visible_) {
        std::wstring v = CellText(entries_[i], c);
        if (v.empty()) continue;
        size_t key = v.size() * 131 + (c == 2 ? std::hash<std::wstring>{}(v) : 0);
        if (c != 2 && seenLen.count(key) && v.size() < 12) continue;
        seenLen[key] = true;
        m = std::max(m, r_.MeasureText(v, fv) + 16 + 2);
    }
    return static_cast<int>(std::ceil(m));
}

// ---------------------------------------------------------------------------
// Statusleiste (ohne Rahmenlinien)
// ---------------------------------------------------------------------------

// Hinweis zum Treffer unter der Maus (nach kurzem Verweilen), ueber dem Knopf, im Fenster gehalten
void App::PaintTip(ID2D1DeviceContext* dc) {
    if (!tipShown_ || hoverHit_ < 0 || hoverHit_ >= static_cast<int>(hits_.size()) || !hits_[hoverHit_].tip) return;
    Theme t = MakeTheme(dark_, false);
    const Hit& h = hits_[hoverHit_];
    std::wstring s = T(h.tip);
    IDWriteTextFormat* f = r_.Text(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = r_.MeasureText(s, f) + 20, hgt = 28;
    float cx = (h.rc.left + h.rc.right) / 2;
    float left = std::clamp(cx - w / 2, 4.0f, std::max(4.0f, W_ - w - 4));
    float top = h.rc.top - hgt - 6;
    if (top < 4) top = h.rc.bottom + 6;
    D2D1_RECT_F rc = D2D1::RectF(left, top, left + w, top + hgt);
    FillRound(dc, D2D1::RectF(rc.left, rc.top + 1, rc.right, rc.bottom + 2), 5, D2D1::ColorF(0, 0, 0, 0.18f));   // Schatten
    FillRound(dc, rc, 5, dark_ ? D2D1::ColorF(0.17f, 0.17f, 0.17f, 0.98f) : D2D1::ColorF(0.99f, 0.99f, 0.99f, 0.98f));
    D2D1_RECT_F in = D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f, rc.right - 0.5f, rc.bottom - 0.5f);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(in, 4.5f, 4.5f), r_.Brush(t.stroke), 1.0f);
    DrawText(dc, s, rc, f, t.text);
}

void App::PaintStatus(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    float y = H_ - statusH_;
    dc->FillRectangle(D2D1::RectF(0, y, W_, H_), r_.Brush(t.status));

    IDWriteTextFormat* f = r_.Text(12);
    IDWriteTextFormat* fb = r_.Text(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float x = 16;
    {   // ganz links in der Ecke: Diashow aller Bilder der Ansicht
        D2D1_RECT_F rc = D2D1::RectF(4, y + 3, 36, H_ - 3);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::SlideshowStart, 0, N_(L"Diashow starten")});
        if (hoverHit_ == idx) FillRound(dc, rc, 4, t.hover);
        wchar_t g[2] = {0xE768, 0};
        DrawText(dc, g, rc, r_.Icon(13), t.text);
        x = 46;
    }
    auto segment = [&](const std::wstring& s, IDWriteTextFormat* fmt, const D2D1_COLOR_F& c) {
        if (s.empty()) return;
        float w = r_.MeasureText(s, fmt);
        DrawText(dc, s, D2D1::RectF(x, y, x + w + 4, H_), fmt, c);
        x += w + 22;
    };

    segment(Thousands(visible_.size()) + (visible_.size() == 1 ? T(L" Element") : T(L" Elemente")) +
                (visible_.size() != entries_.size() ? T(L" von ") + Thousands(entries_.size()) : std::wstring()),
            f, t.text2);
    std::vector<uint32_t> sel = InfoSelection();
    if (sel.size() == 1) {
        const FileEntry& e = entries_[sel.front()];
        if (e.width && e.height) segment(std::to_wstring(e.width) + L" × " + std::to_wstring(e.height), fb, t.text);
        if (e.kind == Kind::Video && e.durationMs) segment(FormatDuration(e.durationMs), f, t.text);
        segment(TypeName(e), f, t.text);
        if (e.kind != Kind::Folder) segment(FormatSize(e.size), f, t.text);
    } else if (sel.size() > 1) {
        uint64_t bytes = 0;
        for (uint32_t i : sel) bytes += entries_[i].size;
        segment(Thousands(sel.size()) + T(L" ausgewählt"), fb, t.text);
        if (bytes) segment(FormatSize(bytes), f, t.text);
    }

    // rechts: i-Symbol, Zoom (Plus/Regler/Minus), Ansicht, Status
    float rx = W_ - 8;
    // eigene Symbole (Linien, 16 Einheiten): 0 Bilderwolke = drei leere Kacheln wie eine Wolke verteilt,
    // 1 Zeitleiste = minimalistische Uhr
    auto drawSymbol = [&](const D2D1_RECT_F& rc, int which, const D2D1_COLOR_F& c) {
        const float u = 17.0f / 16.0f;
        const float bx = std::round((rc.left + rc.right) / 2 - 8.5f), by = std::round((rc.top + rc.bottom) / 2 - 8.5f);
        ID2D1SolidColorBrush* br = r_.Brush(c);
        auto sq = [&](float x, float yy, float w, float r) {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx + x * u, by + yy * u, bx + (x + w) * u, by + (yy + w) * u), r, r),
                                     br, 1.2f);
        };
        if (which == 0) {
            // grosse Kachel vorn; die beiden hinteren ueberschneiden sich mit ihr und sind dort ausgespart
            const float fx = 4.25f, fy = 1.5f, fw = 8.0f, fr = 2.0f, gap = 1.3f;
            ComPtr<ID2D1Factory> fac;
            dc->GetFactory(&fac);
            ComPtr<ID2D1RectangleGeometry> all;
            ComPtr<ID2D1RoundedRectangleGeometry> front;
            ComPtr<ID2D1PathGeometry> mask;
            bool layer = false;
            if (fac && SUCCEEDED(fac->CreateRectangleGeometry(D2D1::RectF(bx - 4, by - 4, bx + 21, by + 21), &all)) &&
                SUCCEEDED(fac->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(D2D1::RectF(bx + (fx - gap) * u, by + (fy - gap) * u, bx + (fx + fw + gap) * u,
                                                  by + (fy + fw + gap) * u), (fr + gap) * u, (fr + gap) * u), &front)) &&
                SUCCEEDED(fac->CreatePathGeometry(&mask))) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(mask->Open(&sink)) &&
                    SUCCEEDED(all->CombineWithGeometry(front.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get())) &&
                    SUCCEEDED(sink->Close())) {
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), mask.Get()), nullptr);
                    layer = true;
                }
            }
            sq(0.25f, 8.5f, 5.75f, 1.5f);
            sq(11.25f, 7.5f, 5.0f, 1.4f);
            if (layer) dc->PopLayer();
            sq(fx, fy, fw, fr);
        } else {
            // minimalistische Uhr: Kreis mit zwei Zeigern
            const D2D1_POINT_2F m = D2D1::Point2F(bx + 8.25f * u, by + 8.25f * u);
            dc->DrawEllipse(D2D1::Ellipse(m, 7.0f * u, 7.0f * u), br, 1.2f);
            dc->DrawLine(m, D2D1::Point2F(m.x, m.y - 4.2f * u), br, 1.2f);
            dc->DrawLine(m, D2D1::Point2F(m.x + 3.0f * u, m.y + 1.2f * u), br, 1.2f);
        }
    };
    auto iconButton = [&](HitType type, wchar_t glyph, float size, bool active, int symbol = -1) {
        D2D1_RECT_F rc = D2D1::RectF(rx - 30, y + 3, rx, H_ - 3);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, type, 0});
        if (active) FillRound(dc, rc, 4, t.press);
        else if (hoverHit_ == idx) FillRound(dc, rc, 4, t.hover);
        if (symbol >= 0) {
            drawSymbol(rc, symbol, active ? accent_ : t.text);
        } else {
            wchar_t g[2] = {glyph, 0};
            DrawText(dc, g, rc, r_.Icon(size + 1), active ? accent_ : t.text);   // deutlich: weiss (dunkel) bzw. fast schwarz (hell)
        }
        rx = rc.left - 2;
    };
    // feste Reihenfolge von rechts: i, Listenansicht, Bilderwolke, Kachelansicht, dann (nur Kacheln) der Zoom
    iconButton(HitType::InfoToggle, 0xE946, 13, settings_.infoPanel);
    rx -= 12;
    iconButton(HitType::ViewList, 0xE8FD, 13, settings_.viewMode == 1);
    iconButton(HitType::ViewCloud, 0, 13, settings_.viewMode == 2, 0);
    iconButton(HitType::ViewTimeline, 0, 13, settings_.viewMode == 3, 1);
    iconButton(HitType::ViewGrid, 0xE8A9, 13, settings_.viewMode == 0);
    rx -= 14;
    if (settings_.viewMode == 0) {
        iconButton(HitType::ZoomIn, 0xE710, 11, false);
        const float sliderW = 110;
        sliderRc_ = D2D1::RectF(rx - sliderW, y + 4, rx, H_ - 4);
        hits_.push_back({sliderRc_, HitType::ZoomSlider, 0});
        float my = (y + H_) / 2;
        float frac = (settings_.tileSize - 64) / 320.0f;
        float kx = sliderRc_.left + 6 + frac * (sliderW - 12);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sliderRc_.left + 6, my - 2, sliderRc_.right - 6, my + 2), 2, 2),
                                 r_.Brush(t.press));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sliderRc_.left + 6, my - 2, kx, my + 2), 2, 2),
                                 r_.Brush(accent_));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, my), 7, 7),
                        r_.Brush(dark_ ? D2D1::ColorF(0.27f, 0.27f, 0.27f, 1) : D2D1::ColorF(1, 1, 1, 1)));
        float knob = sliderDrag_ ? 3.0f : 4.0f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, my), knob, knob), r_.Brush(accent_));
        rx = sliderRc_.left - 2;
        iconButton(HitType::ZoomOut, 0xE738, 11, false);
        rx -= 16;
    } else {
        sliderRc_ = {};
    }

    std::wstring right;
    if (!status_.empty()) right = status_;
    else {
        if (loading_) right = T(L"Wird geladen …");
        size_t pending = meta_.Pending();
        if (pending) {
            if (!right.empty()) right += L"   ·   ";
            right += TF(L"Metadaten: {0} offen", {Thousands(pending)});
        }
        if (indexBusy_) {
            if (!right.empty()) right += L"   ·   ";
            right += L"Index: " + Thousands(indexDone_) + L" / " + Thousands(indexTotal_);   // lang:ok
        }
    }
    if (rx > x + 40)
        DrawText(dc, right, D2D1::RectF(x, y, rx, H_), r_.Text(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING),
                 status_.empty() ? t.text3 : t.text);
}

// ---------------------------------------------------------------------------
// Info-Panel (Uebersicht und Bildbetrachter)
// ---------------------------------------------------------------------------

std::vector<uint32_t> App::InfoSelection() const {
    std::vector<uint32_t> sel;
    if (viewer_.open) {
        if (viewer_.pos >= 0 && viewer_.pos < static_cast<int>(visible_.size())) sel.push_back(visible_[viewer_.pos]);
        return sel;
    }
    for (uint32_t i : visible_)
        if (selected_.count(i)) sel.push_back(i);
    if (sel.empty() && focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) sel.push_back(visible_[focus_]);
    return sel;
}

void App::PaintInfoPanel(ID2D1DeviceContext* dc, const D2D1_RECT_F& rc, const std::vector<uint32_t>& sel, bool dark) {
    Theme t = MakeTheme(dark, backdrop_ && !viewer_.open);
    dc->PushAxisAlignedClip(rc, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->FillRectangle(rc, r_.Brush(viewer_.open ? D2D1::ColorF(0.11f, 0.11f, 0.11f, 0.96f) : t.layer));

    float x0 = rc.right - 300 + 20, x1 = rc.right - 20;   // am rechten Rand verankert (gleitet herein)
    float w = x1 - x0;
    float y = rc.top + 16;
    IDWriteTextFormat* fl = r_.TextWrap(11.5f);
    IDWriteTextFormat* fv = r_.TextWrap(13);
    auto row = [&](const std::wstring& label, const std::wstring& value) {
        if (value.empty() || y > rc.bottom - 30) return;
        y += r_.DrawWrapped(label, x0, y, w, fl, t.text3) + 2;
        y += r_.DrawWrapped(value, x0, y, w, fv, t.text) + 12;
    };

    if (sel.size() == 1) {
        const FileEntry& e = entries_[sel.front()];
        D2D1_RECT_F pv = D2D1::RectF(x0, y, x1, y + 200);
        FillRound(dc, pv, 6, t.hover);
        Bmp* b = nullptr;
        if (e.HasThumb() && !e.offline) {
            EnsureThumb(e, 0, frame_);
            auto it = bmps_.find(e.path);
            if (it != bmps_.end() && it->second.bmp) b = &it->second;
        } else if (e.kind == Kind::Folder && !e.coverPath.empty()) {
            auto it = bmps_.find(e.coverPath);
            if (it != bmps_.end() && it->second.bmp) b = &it->second;
        }
        if (!b) {
            EnsureIcon(e, 0);
            DrawBitmapFit(dc, &bmps_[IconKey(e)], D2D1::RectF(pv.left + 70, pv.top + 40, pv.right - 70, pv.bottom - 40), 2.0f);
        } else {
            DrawBitmapFit(dc, b, D2D1::RectF(pv.left + 8, pv.top + 8, pv.right - 8, pv.bottom - 8), 2.0f);
        }
        y = pv.bottom + 14;
        y += r_.DrawWrapped(e.name, x0, y, w, r_.TextWrap(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), t.text) + 2;
        y += r_.DrawWrapped(TypeName(e), x0, y, w, r_.TextWrap(12.5f), t.text2) + 18;
        if (e.kind != Kind::Folder) row(T(L"Größe"), FormatSize(e.size));
        if (e.width) {
            wchar_t mp[64];
            swprintf_s(mp, L"%u × %u  (%.1f MP)", e.width, e.height, double(e.width) * e.height / 1e6);
            row(T(L"Auflösung"), mp);
        }
        if (e.kind == Kind::Video && e.durationMs) row(T(L"Dauer"), FormatDuration(e.durationMs));
        if (e.Doc() == 1 && e.pages) row(T(L"Seiten"), Thousands(e.pages));
        row(T(L"Aufgenommen"), FormatDate(e.taken));
        row(T(L"Geändert"), FormatDate(e.mtime));
        row(T(L"Erstellt"), FormatDate(e.ctime));
        row(T(L"Kamera"), e.camera);
        PhotoDetailRows(e, row);
        if (e.hasGeo && y <= rc.bottom - 30) {
            // Aufnahmeort: wie ein Link (Klick kopiert die Koordinaten oder oeffnet OpenStreetMap, siehe Einstellungen)
            y += r_.DrawWrapped(T(L"Aufnahmeort"), x0, y, w, fl, t.text3) + 2;
            std::wstring gv = GeoText(e);
            float gw = r_.MeasureText(gv, r_.Text(13));
            D2D1_RECT_F gr = D2D1::RectF(x0 - 4, y - 2, std::min(x1, x0 + gw + 30), y + 20);
            int gi = static_cast<int>(hits_.size());
            hits_.push_back({gr, HitType::InfoGeo, static_cast<int>(sel.front())});
            bool hot = hoverHit_ == gi;
            if (hot) FillRound(dc, gr, 4, t.hover);
            DrawText(dc, gv, D2D1::RectF(x0, y, x1, y + 18), r_.Text(13), accent_);
            wchar_t gg[2] = {static_cast<wchar_t>(settings_.geoClick == 1 ? 0xE707 : 0xE8C8), 0};   // Karte bzw. Kopieren
            DrawText(dc, gg, D2D1::RectF(x0 + gw + 8, y, x0 + gw + 26, y + 18), r_.Icon(11), hot ? accent_ : t.text3);
            y += 18 + 12;
        }
        if (!e.source.empty()) row(T(L"Quelle"), T(e.source));
        if (e.templateId != UINT32_MAX) row(T(L"Namensmuster"), TPattern(templDict_.Word(e.templateId)));
        row(T(L"Ordner"), ParentPath(e.path));
        if (e.offline) row(T(L"Hinweis"), T(L"Nur online verfügbar – keine Vorschau; wird beim Öffnen heruntergeladen"));
        if (!e.offline && (e.ext == L"heic" || e.ext == L"heif" || e.ext == L"hif")) {
            // HEIF zeigt peek ueber den Codec von Windows: fehlt er, sagen, was noetig ist
            auto bi = bmps_.find(e.path);
            if (bi != bmps_.end() && bi->second.failed)
                row(T(L"Hinweis"), HeifCodecInstalled()
                                       ? T(L"Der HEIF-Codec von Windows ließ sich nicht verwenden")
                                       : T(L"Zum Anzeigen werden die „HEIF-Bilderweiterungen“ und „HEVC-Videoerweiterungen“ von Microsoft benötigt"));
        }
    } else if (sel.size() > 1) {
        uint64_t bytes = 0;
        size_t n[4] = {0, 0, 0, 0};
        for (uint32_t i : sel) {
            bytes += entries_[i].size;
            ++n[static_cast<int>(entries_[i].kind)];
        }
        y += r_.DrawWrapped(Thousands(sel.size()) + T(L" Elemente ausgewählt"), x0, y, w,
                            r_.TextWrap(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), t.text) + 18;
        row(T(L"Gesamtgröße"), FormatSize(bytes));
        if (n[1]) row(T(L"Bilder"), Thousands(n[1]));
        if (n[2]) row(T(L"Videos"), Thousands(n[2]));
        if (n[0]) row(T(L"Ordner"), Thousands(n[0]));
        if (n[3]) row(T(L"Sonstige Dateien"), Thousands(n[3]));
    } else {
        y += r_.DrawWrapped(T(L"Kein Element ausgewählt"), x0, y, w, r_.TextWrap(15, DWRITE_FONT_WEIGHT_SEMI_BOLD),
                            t.text) + 18;
        row(T(L"Ansicht"), collectionsView_ ? std::wstring(T(L"Alle Sammlungen")) : path_);
        row(T(L"Elemente"), Thousands(visible_.size()));
    }
    dc->PopAxisAlignedClip();
}

// ---------------------------------------------------------------------------
// Bildbetrachter (Vollbild, ohne Titelleiste)
// ---------------------------------------------------------------------------

// Diashow: das aktuelle Bild blendet ueber dem vorherigen ein; jedes zoomt waehrend seiner ganzen Zeit leicht heran.
// Jedes Bild wird einmal in Bildschirmaufloesung (bei groesstem Zoom) hochwertig gefiltert und danach nur noch
// leicht verkleinert gezeichnet: so bleibt jedes Bild der Animation billig.
void App::PaintSlides(ID2D1DeviceContext* dc, float VW) {
    constexpr double kShow = 4000, kFade = 1500;
    constexpr float kZoom = 0.05f;   // waechst in der ganzen Zeit (Einblenden, Zeigen, Ausblenden) um 5 %
    auto& v = viewer_;
    const double now = NowMs();
    const double t = v.slidePaused ? v.slidePausedAt : now;
    const int n = static_cast<int>(visible_.size());
    const float sc = r_.scale();
    auto bitmapFor = [&](const FileEntry& e) -> Bmp* {
        auto fit = fullBmps_.find(e.path);
        if (fit != fullBmps_.end() && fit->second.bmp) return &fit->second;
        auto tit = bmps_.find(e.path);
        if (tit != bmps_.end() && tit->second.bmp) return &tit->second;
        return nullptr;
    };
    auto drawSlide = [&](int pos, double t0, float alpha) {
        if (pos < 0 || pos >= n || alpha <= 0.002f) return;
        const FileEntry& e = entries_[visible_[pos]];
        Bmp* b = bitmapFor(e);
        if (!b) return;
        float iw, ih;
        float fit = ViewerScale(e, iw, ih);
        float age = static_cast<float>(std::clamp((t - t0) / (kShow + 2 * kFade), 0.0, 1.0));
        float z = 1.0f + kZoom * age;
        // Ausschnitt in Bildschirmaufloesung beim groessten Zoom (einmal je Bild)
        float bw = iw * fit * (1 + kZoom), bh = ih * fit * (1 + kZoom);
        SlideCache* c = nullptr;
        for (auto& sc2 : slideCache_)
            if (sc2.path == e.path) c = &sc2;
        if (!c || c->src != b->bmp.Get() || c->w != bw || c->h != bh || c->dpi != r_.dpi()) {
            if (!c) {   // freien Platz nehmen: nicht den des anderen gerade gezeigten Bildes
                const std::wstring other = (pos == v.pos && v.slidePrev >= 0 && v.slidePrev < n)
                                               ? entries_[visible_[v.slidePrev]].path
                                               : entries_[visible_[v.pos]].path;
                c = slideCache_[0].path == other ? &slideCache_[1] : &slideCache_[0];
            }
            *c = SlideCache{};
            UINT pw = static_cast<UINT>(std::ceil(bw * sc)), ph = static_cast<UINT>(std::ceil(bh * sc));
            if (pw > 0 && ph > 0 && pw <= r_.maxBitmapSize() && ph <= r_.maxBitmapSize()) {
                D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                    r_.dpi(), r_.dpi());
                dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, bp, &c->bmp);
                if (c->bmp) {
                    ComPtr<ID2D1Image> oldTarget;
                    dc->GetTarget(&oldTarget);
                    dc->SetTarget(c->bmp.Get());
                    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                    dc->DrawBitmap(b->bmp.Get(), D2D1::RectF(0, 0, bw, bh), 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->SetTarget(oldTarget.Get());
                    c->path = e.path;
                    c->src = b->bmp.Get();
                    c->w = bw;
                    c->h = bh;
                    c->dpi = r_.dpi();
                }
            }
        }
        float dw = iw * fit * z, dh = ih * fit * z;
        D2D1_RECT_F dst = D2D1::RectF(VW / 2 - dw / 2, H_ / 2 - dh / 2, VW / 2 + dw / 2, H_ / 2 + dh / 2);
        if (c && c->bmp) dc->DrawBitmap(c->bmp.Get(), dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
        else dc->DrawBitmap(b->bmp.Get(), dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
    };

    float u = static_cast<float>(std::clamp((t - v.slideT0) / kFade, 0.0, 1.0));
    float a = u * u * (3 - 2 * u);   // weich ein- und ausklingend
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, VW, H_), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->FillRectangle(D2D1::RectF(0, 0, VW, H_), r_.Brush(D2D1::ColorF(0, 0, 0, 1)));   // ueber Schwarz
    if (v.slidePrev >= 0 && u < 1) drawSlide(v.slidePrev, v.slidePrevT0, 1.0f - a * a);
    drawSlide(v.pos, v.slideT0, a);   // auch das erste Bild blendet langsam aus dem Schwarz ein
    dc->PopAxisAlignedClip();
    if (u >= 1 && v.slidePrev >= 0) v.slidePrev = -1;

    if (!v.slidePaused) {
        // weiter, wenn das Bild lange genug stand und das naechste geladen ist (hoechstens 8 s darauf warten)
        double shown = now - v.slideT0;
        if (shown >= kFade + kShow) {
            int nx = SlideNext(v.pos, 1);
            if (nx != v.pos) {
                auto it = fullBmps_.find(entries_[visible_[nx]].path);
                bool ready = it != fullBmps_.end() && (it->second.bmp || it->second.failed);
                if (ready || shown >= kFade + kShow + 8000) SlideAdvance(1);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);   // naechstes Bild der Animation (Present wartet auf VSync)
    }
    if (GetTickCount() >= v.controlsUntil && GetForegroundWindow() == hwnd_) {
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == hwnd_) SetCursor(nullptr);
    }
}

float App::ViewerScale(const FileEntry& e, float& imgW, float& imgH) const {
    imgW = static_cast<float>(e.width);
    imgH = static_cast<float>(e.height);
    if (imgW <= 0 || imgH <= 0) {
        auto fit = fullBmps_.find(e.path);
        if (fit != fullBmps_.end() && fit->second.bmp) {
            imgW = static_cast<float>(fit->second.srcW);
            imgH = static_cast<float>(fit->second.srcH);
        } else {
            auto tit = bmps_.find(e.path);
            if (tit != bmps_.end() && tit->second.bmp) {
                imgW = static_cast<float>(tit->second.srcW);
                imgH = static_cast<float>(tit->second.srcH);
            }
        }
    }
    if (imgW <= 0 || imgH <= 0) { imgW = 1; imgH = 1; }
    bool rot = (viewer_.rotation % 2) == 1;
    float rw = rot ? imgH : imgW, rh = rot ? imgW : imgH;
    float s100 = 1.0f / r_.scale();
    if (viewer_.zoom > 0) return viewer_.zoom * s100;
    // Zuschnitt ueber das Symbol: etwas kleiner einpassen, damit Rahmen und Griffe frei liegen
    if (viewer_.crop && viewer_.cropInset) {
        float fit = settings_.viewerSmall == 1 ? std::min(ViewerAreaW() / rw, H_ / rh)
                                               : std::min({ViewerAreaW() / rw, H_ / rh, s100});
        return fit * 0.86f;
    }
    // eingepasst: kleine Bilder je nach Einstellung in Originalgroesse (100 %) oder auf die Ansicht vergroessert
    if (settings_.viewerSmall == 1) return std::min(ViewerAreaW() / rw, H_ / rh);
    return std::min({ViewerAreaW() / rw, H_ / rh, s100});
}

void App::PaintViewer(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(t.overlay));
    int n = static_cast<int>(visible_.size());
    if (viewer_.pos < 0 || viewer_.pos >= n) {
        // alle Bilder geloescht: Betrachter bleibt offen (Esc/X schliesst)
        DrawText(dc, T(L"Keine Bilder mehr in dieser Ansicht."), D2D1::RectF(0, H_ / 2 - 20, W_, H_ / 2 + 20),
                 r_.Text(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::ColorF(1, 1, 1, 0.6f));
        D2D1_RECT_F xr = D2D1::RectF(W_ - 48, 0, W_, 44);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({xr, HitType::ViewerClose, 0});
        if (hoverHit_ == idx) dc->FillRectangle(xr, r_.Brush(D2D1::ColorF(0.77f, 0.17f, 0.11f, 1)));
        wchar_t gx[2] = {0xE8BB, 0};
        DrawText(dc, gx, xr, r_.Icon(11), D2D1::ColorF(1, 1, 1, 0.95f));
        if (!viewer_.toast.empty() && GetTickCount() < viewer_.toastUntil) {
            IDWriteTextFormat* ft = r_.Text(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            float tw = std::min(W_ - 40, r_.MeasureText(viewer_.toast, ft) + 56);
            D2D1_RECT_F tr = D2D1::RectF(W_ / 2 - tw / 2, 56, W_ / 2 + tw / 2, 94);
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 8, 8), r_.Brush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.92f)));
            DrawText(dc, viewer_.toast, D2D1::RectF(tr.left + 14, tr.top, tr.right - 14, tr.bottom), ft, D2D1::ColorF(1, 1, 1, 0.95f));
        }
        return;
    }
    const FileEntry& e = entries_[visible_[viewer_.pos]];
    float VW = ViewerAreaW();

    float iw, ih;
    float sTarget = ViewerScale(e, iw, ih);
    // weiche Zoom-Animation: angezeigter Massstab und Versatz laufen dem Ziel exponentiell nach
    double nowMs = NowMs();
    DWORD now = GetTickCount();
    D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 0.95f), dim = D2D1::ColorF(1, 1, 1, 0.6f);
    if (viewer_.slide && nowMs >= viewer_.slideIntroEnd) {
        PaintSlides(dc, VW);   // Diashow: eigene Darstellung (Ueberblenden, leichter Zoom)
    } else {
    // Der Zoom laeuft um einen festen Punkt (Mauszeiger): Mittelpunkt c = A - (A - c0) * s / s0.
    // So bleibt der Punkt unter dem Mauszeiger waehrend der ganzen Animation stehen (keine Bogenbewegung).
    auto& v = viewer_;
    const bool wasSettled = v.animU >= 1;
    if (v.animS <= 0 || v.dragging) {
        v.animS = v.fromS = v.toS = sTarget;
        v.animPan = v.fromPan = v.toPan = v.pan;
        v.animU = 1;
    } else if (sTarget != v.toS || v.pan.x != v.toPan.x || v.pan.y != v.toPan.y) {
        // neues Ziel: vom gerade angezeigten Zustand aus starten
        v.fromS = v.animS;
        v.fromPan = v.animPan;
        v.toS = sTarget;
        v.toPan = v.pan;
        v.animU = 0;
        float k = v.toS / v.fromS;
        v.anchored = std::fabs(k - 1.0f) > 1e-4f;
        if (v.anchored) {
            v.anchor.x = (v.toPan.x - k * v.fromPan.x) / (1.0f - k);
            v.anchor.y = (v.toPan.y - k * v.fromPan.y) / (1.0f - k);
        }
    }
    if (v.animU < 1) {
        // im Takt des Bildschirms; der erste Schritt einer neuen Bewegung ist genau eine Bildperiode
        float dt = wasSettled ? static_cast<float>(RefreshPeriodMs()) : FrameDt(std::clamp(nowMs - v.animTick, 0.0, 50.0));
        v.animU += (1.0f - v.animU) * (1.0f - std::exp(-dt / 55.0f));
        if (v.animU > 0.997f) v.animU = 1;
        if (v.anchored) {
            float ls0 = std::log(v.fromS), ls1 = std::log(v.toS);
            v.animS = std::exp(ls0 + (ls1 - ls0) * v.animU);
            float r = v.animS / v.fromS;
            v.animPan.x = v.anchor.x - (v.anchor.x - v.fromPan.x) * r;
            v.animPan.y = v.anchor.y - (v.anchor.y - v.fromPan.y) * r;
        } else {
            v.animS = v.toS;
            v.animPan.x = v.fromPan.x + (v.toPan.x - v.fromPan.x) * v.animU;
            v.animPan.y = v.fromPan.y + (v.toPan.y - v.fromPan.y) * v.animU;
        }
        if (v.animU >= 1) {
            v.animS = v.toS;
            v.animPan = v.toPan;
        } else {
            InvalidateRect(hwnd_, nullptr, FALSE);   // naechstes Bild (Present wartet auf VSync)
        }
    }
    viewer_.animTick = nowMs;
    float s = viewer_.animS;
    D2D1_POINT_2F c = D2D1::Point2F(VW / 2 + viewer_.animPan.x, H_ / 2 + viewer_.animPan.y);
    float dw = iw * s, dh = ih * s;
    D2D1_RECT_F dst = D2D1::RectF(c.x - dw / 2, c.y - dh / 2, c.x + dw / 2, c.y + dh / 2);

    Bmp* b = nullptr;
    auto fit = fullBmps_.find(e.path);
    if (fit != fullBmps_.end() && fit->second.bmp) b = &fit->second;
    else {
        auto tit = bmps_.find(e.path);
        if (tit != bmps_.end() && tit->second.bmp) b = &tit->second;
    }
    // angezeigte Bildflaeche (gedreht) fuer den Zuschnitt
    viewer_.imgBox = (viewer_.rotation % 2 == 1) ? D2D1::RectF(c.x - dh / 2, c.y - dw / 2, c.x + dh / 2, c.y + dw / 2) : dst;
    D2D1_INTERPOLATION_MODE im = D2D1_INTERPOLATION_MODE_LINEAR;
    bool cached = false;
    if (b) {
        D2D1_SIZE_U px = b->bmp->GetPixelSize();
        float screenScale = dw * r_.scale() / std::max(1u, px.width);
        // Vergroessern: wahlweise geglaettet (kubisch) oder Pixel vergroessert (ab 3-fach scharfe Pixelkanten)
        im = screenScale < 1.0f || settings_.viewerSmooth
                 ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                 : (screenScale >= 3.0f ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_LINEAR);
        // Ruhende Ansicht: das hochwertig gefilterte Bild einmal in Bildschirmaufloesung festhalten und danach nur
        // noch 1:1 kopieren (gleiche Pixel). Sonst wuerde jedes Neuzeichnen (Maus ueber den Pfeilen, Leisten) das
        // ganze Foto erneut filtern.
        auto& vc = viewCache_;
        bool settled = v.animU >= 1 && !v.dragging && !animating_;
        // erst festhalten, wenn die Lage zwei Bilder lang gleich war (nicht waehrend der Infobereich einfaehrt)
        bool stable = settled && vc.seen.left == dst.left && vc.seen.top == dst.top && vc.seen.right == dst.right &&
                      vc.seen.bottom == dst.bottom && vc.seenW == VW;
        vc.seen = dst;
        vc.seenW = VW;
        if (!stable) {
            if (vc.bmp) { vc.bmp.Reset(); vc.path.clear(); }
        } else if (im == D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC) {
            const float sc = r_.scale();
            const D2D1_RECT_F& ib = viewer_.imgBox;
            // vom Ursprung des Fensters an: gleiche Koordinaten wie beim direkten Zeichnen, also dieselben Pixel
            // (ein verschobener Ausschnitt weicht durch Rundung beim Filtern minimal ab, siehe --render-check)
            int x0 = 0, y0 = 0;
            int x1 = static_cast<int>(std::ceil(std::min(ib.right, VW) * sc));
            int y1 = static_cast<int>(std::ceil(std::min(ib.bottom, H_) * sc));
            if (x1 > x0 && y1 > y0 && static_cast<uint32_t>(x1 - x0) <= r_.maxBitmapSize() &&
                static_cast<uint32_t>(y1 - y0) <= r_.maxBitmapSize()) {
                UINT cw = static_cast<UINT>(x1 - x0), ch = static_cast<UINT>(y1 - y0);
                bool same = vc.bmp && vc.path == e.path && vc.src == b->bmp.Get() && vc.rotation == viewer_.rotation &&
                            vc.x0 == x0 && vc.y0 == y0 && vc.w == cw && vc.h == ch && vc.dpi == r_.dpi() &&
                            vc.dst.left == dst.left && vc.dst.top == dst.top && vc.dst.right == dst.right &&
                            vc.dst.bottom == dst.bottom;
                if (!same) {
                    if (!vc.bmp || vc.w != cw || vc.h != ch || vc.dpi != r_.dpi()) {
                        vc.bmp.Reset();
                        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                            D2D1_BITMAP_OPTIONS_TARGET,
                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), r_.dpi(), r_.dpi());
                        dc->CreateBitmap(D2D1::SizeU(cw, ch), nullptr, 0, bp, &vc.bmp);
                    }
                    if (vc.bmp) {
                        ComPtr<ID2D1Image> oldTarget;
                        dc->GetTarget(&oldTarget);
                        dc->SetTarget(vc.bmp.Get());
                        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                        dc->SetTransform(D2D1::Matrix3x2F::Rotation(viewer_.rotation * 90.0f, c));
                        dc->DrawBitmap(b->bmp.Get(), dst, 1.0f, im);
                        dc->SetTransform(D2D1::Matrix3x2F::Identity());
                        dc->SetTarget(oldTarget.Get());
                        vc.path = e.path;
                        vc.src = b->bmp.Get();
                        vc.rotation = viewer_.rotation;
                        vc.x0 = x0;
                        vc.y0 = y0;
                        vc.w = cw;
                        vc.h = ch;
                        vc.dpi = r_.dpi();
                        vc.dst = dst;
                    }
                }
                cached = vc.bmp != nullptr;
            }
        }
    }
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, VW, H_), D2D1_ANTIALIAS_MODE_ALIASED);
    if (b) {
        if (cached) {
            const float sc = r_.scale();
            const auto& vc = viewCache_;
            dc->DrawBitmap(vc.bmp.Get(), D2D1::RectF(vc.x0 / sc, vc.y0 / sc, (vc.x0 + vc.w) / sc, (vc.y0 + vc.h) / sc), 1.0f,
                           D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        } else {
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(viewer_.rotation * 90.0f, c));
            dc->DrawBitmap(b->bmp.Get(), dst, 1.0f, im);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        if (e.ext == L"svg" && viewer_.rotation % 4 == 0) {
            // SVG: Vektorgrafik bleibt in jeder Zoomstufe scharf. Ist das Grundbild groeber als der Bildschirm,
            // rendert resvg den sichtbaren Ausschnitt in Bildschirmaufloesung nach (sobald die Ansicht ruht).
            const float px = r_.scale();
            const float kScreen = s * px;                                   // Pixel je SVG-Einheit
            const float kBase = static_cast<float>(b->bmp->GetPixelSize().width) / std::max(1.0f, iw);
            if (svgDetail_.bmp && svgDetail_.path == e.path && svgDetail_.k > kBase * 1.02f) {
                D2D1_RECT_F dr = D2D1::RectF(dst.left + svgDetail_.u0 * s, dst.top + svgDetail_.v0 * s,
                                             dst.left + svgDetail_.u1 * s, dst.top + svgDetail_.v1 * s);
                dc->DrawBitmap(svgDetail_.bmp.Get(), dr, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
            }
            bool settled = viewer_.animU >= 1 && !viewer_.dragging && !viewer_.crop;
            if (settled && kScreen > kBase * 1.1f && svgPendingKey_.empty()) {
                D2D1_RECT_F vis = D2D1::RectF(std::max(dst.left, 0.0f), std::max(dst.top, 0.0f), std::min(dst.right, VW),
                                              std::min(dst.bottom, H_));
                if (vis.right > vis.left + 1 && vis.bottom > vis.top + 1) {
                    float u0 = (vis.left - dst.left) / s, v0 = (vis.top - dst.top) / s;
                    float u1 = (vis.right - dst.left) / s, v1 = (vis.bottom - dst.top) / s;
                    bool fresh = svgDetail_.path == e.path &&   // auch nach Fehlschlag nicht endlos neu anfordern
                                 std::fabs(svgDetail_.k / kScreen - 1.0f) < 0.01f && svgDetail_.u0 <= u0 + 1e-3f &&
                                 svgDetail_.v0 <= v0 + 1e-3f && svgDetail_.u1 >= u1 - 1e-3f && svgDetail_.v1 >= v1 - 1e-3f;
                    if (!fresh) {
                        uint32_t pw = static_cast<uint32_t>(std::ceil((u1 - u0) * kScreen));
                        uint32_t ph = static_cast<uint32_t>(std::ceil((v1 - v0) * kScreen));
                        if (pw > 0 && ph > 0 && pw <= 16384 && ph <= 16384) {
                            svgPending_.path = e.path;
                            svgPending_.k = kScreen;
                            svgPending_.u0 = u0;
                            svgPending_.v0 = v0;
                            svgPending_.u1 = u0 + pw / kScreen;
                            svgPending_.v1 = v0 + ph / kScreen;
                            svgPendingKey_ = L"svgview|" + std::to_wstring(++svgSeq_) + L"|" + e.path;
                            ThumbRequest r;
                            r.key = svgPendingKey_;
                            r.path = e.path;
                            r.kind = Kind::Image;
                            r.job = ThumbJob::SvgView;
                            r.prio = 0;
                            r.gen = 0;
                            r.vk = kScreen;
                            r.vx = -u0 * kScreen;
                            r.vy = -v0 * kScreen;
                            r.vw = pw;
                            r.vh = ph;
                            thumbs_.Request(std::move(r));
                        }
                    }
                }
            }
        }
    } else {
        bool failed = fit != fullBmps_.end() && fit->second.failed;
        DrawText(dc, failed ? T(L"Bild kann nicht angezeigt werden.") : T(L"Wird geladen …"),
                 D2D1::RectF(0, H_ / 2 - 20, VW, H_ / 2 + 20),
                 r_.Text(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), dim);
    }

    if (viewer_.crop && b) PaintCrop(dc);

    // Blaettern: ganz am Rand, nur kurz nach einer Mausbewegung sichtbar (nicht beim Zuschneiden)
    if (!viewer_.crop && GetTickCount() < viewer_.controlsUntil) {
        bool hasPrev = ViewerNext(viewer_.pos, -1) >= 0, hasNext = ViewerNext(viewer_.pos, 1) >= 0;
        float my = H_ / 2;
        auto arrow = [&](HitType type, wchar_t glyph, float x) {
            D2D1_RECT_F rc = D2D1::RectF(x, my - 64, x + 48, my + 64);
            int idx = static_cast<int>(hits_.size());
            hits_.push_back({rc, type, 0});
            // Hover: dunkelgrau-transparent statt schwarz, damit es auch auf schwarzem Hintergrund sichtbar ist
            dc->FillRectangle(rc, r_.Brush(hoverHit_ == idx ? D2D1::ColorF(0.32f, 0.32f, 0.32f, 0.72f)
                                                            : D2D1::ColorF(0, 0, 0, 0.4f)));
            wchar_t g[2] = {glyph, 0};
            DrawText(dc, g, rc, r_.Icon(18), white);
        };
        if (hasPrev) arrow(HitType::ViewerPrev, 0xE76B, 0);
        if (hasNext) arrow(HitType::ViewerNext, 0xE76C, VW - 48);
    }

    dc->PopAxisAlignedClip();
    }   // Ende: normale Anzeige (keine Diashow)

    // Leiste oben ueber die ganze Breite: das X sitzt immer ganz oben rechts, das Info-Panel darunter
    // Leiste immer einzeilig: reicht der Platz nicht, fallen erst die Angaben weg, dann wird der Name in der Mitte
    // gekuerzt. Im Fenster ohne Titelleiste (kein Vollbild) rechts die drei Knoepfe von Windows, links "Zurueck".
    const bool winCaps = captionless_ && !fullscreen_;
    const float capW = 46;
    const float closeW = winCaps ? 3 * capW : 48;
    // aus dem Explorer geoeffnet bzw. im Fenster: grosser "Zurueck"-Knopf in der Ecke links oben
    const float backW = (viewer_.shell || winCaps) ? 56.0f : 0.0f;
    UiAnim(viewer_.barAnim, viewer_.bar && !viewer_.crop, 80.0f);   // Leiste gleitet herein bzw. hinaus (sehr kurz)
    const float ba = EaseOut(viewer_.barAnim);
    const float barH = (viewer_.bar || ba > 0) ? 48.0f : 0.0f;
    const float by = -(1.0f - ba) * barH;   // Verschiebung der Leiste nach oben
    viewerBarRc_ = {};
    if (InfoWidth() > 0.5f) {
        // auf ganze Pixel: der Streifen oben und das Panel stossen ohne Naht aneinander (sonst duenne helle Linie)
        const float px = r_.scale();
        float panelTop = std::round((48.0f + (barH - 48.0f) * ba) * px) / px;
        float covered = std::round(ba * barH * px) / px;   // Teil oben, den die Leiste (gerade) bedeckt
        if (panelTop > covered) {
            dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            dc->FillRectangle(D2D1::RectF(VW, covered, W_, panelTop), r_.Brush(D2D1::ColorF(0.11f, 0.11f, 0.11f, 0.96f)));
            dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        PaintInfoPanel(dc, D2D1::RectF(VW, panelTop, W_, H_), InfoSelection(), true);
    }

    float btnY = 0;
    auto topButton = [&](HitType type, wchar_t glyph, float left, float right, float height, bool danger, bool active) {
        D2D1_RECT_F rc = D2D1::RectF(left, btnY, right, btnY + height);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, type, 0});
        if (hoverHit_ == idx) dc->FillRectangle(rc, r_.Brush(danger ? D2D1::ColorF(0.77f, 0.17f, 0.11f, 1)
                                                                    : D2D1::ColorF(1, 1, 1, 0.14f)));
        else if (active) dc->FillRectangle(rc, r_.Brush(D2D1::ColorF(1, 1, 1, 0.1f)));
        wchar_t g[2] = {glyph, 0};
        DrawText(dc, g, rc, r_.Icon(13), D2D1::ColorF(1, 1, 1, 1));
    };
    if (viewer_.crop) {
        // Zuschneiden: eigene Leiste mit Abbrechen und Speichern
        const float ch = 52;
        dc->FillRectangle(D2D1::RectF(0, 0, W_, ch), r_.Brush(D2D1::ColorF(0, 0, 0, 0.6f)));
        wchar_t gc[2] = {0xE7A8, 0};
        DrawText(dc, gc, D2D1::RectF(16, 0, 40, ch), r_.Icon(15), white);
        DrawText(dc, T(L"Zuschneiden"), D2D1::RectF(48, 0, 260, ch), r_.Text(14, DWRITE_FONT_WEIGHT_SEMI_BOLD), white);
        IDWriteTextFormat* bf = r_.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        float sw = r_.MeasureText(T(L"Speichern"), bf) + 36, cw = r_.MeasureText(T(L"Abbrechen"), bf) + 36;
        D2D1_RECT_F save = D2D1::RectF(W_ - 16 - sw, 10, W_ - 16, ch - 10);
        D2D1_RECT_F cancel = D2D1::RectF(save.left - 8 - cw, 10, save.left - 8, ch - 10);
        int ci = static_cast<int>(hits_.size());
        hits_.push_back({cancel, HitType::CropCancel, 0});
        int si = static_cast<int>(hits_.size());
        hits_.push_back({save, HitType::CropSave, 0});
        FillRound(dc, cancel, 5, D2D1::ColorF(1, 1, 1, hoverHit_ == ci ? 0.2f : 0.12f));
        DrawText(dc, T(L"Abbrechen"), cancel, bf, white);
        D2D1_COLOR_F sa = accent_;
        if (hoverHit_ == si) sa = D2D1::ColorF(std::min(1.0f, sa.r * 1.15f + 0.05f), std::min(1.0f, sa.g * 1.15f + 0.05f),
                                              std::min(1.0f, sa.b * 1.15f + 0.05f), 1);
        FillRound(dc, save, 5, sa);
        DrawText(dc, T(L"Speichern"), save, bf, D2D1::ColorF(1, 1, 1, 1));
    } else if (ba > 0.001f) {
        btnY = by;
        viewerBarRc_ = D2D1::RectF(0, by, W_, by + barH);   // zieht das Fenster (ohne Titelleiste)
        dc->FillRectangle(D2D1::RectF(0, by, W_, by + barH), r_.Brush(D2D1::ColorF(0, 0, 0, 0.45f)));
        float nameX = 20;
        if (backW > 0) {
            // grosser "Zurueck"-Knopf links fuehrt in die Ordneransicht: nur das Symbol, bis in die Ecke
            D2D1_RECT_F rc = D2D1::RectF(0, by, backW, by + barH);
            int bi = static_cast<int>(hits_.size());
            hits_.push_back({rc, HitType::ViewerBack, 0});
            if (hoverHit_ == bi) dc->FillRectangle(rc, r_.Brush(D2D1::ColorF(1, 1, 1, 0.14f)));
            wchar_t gb[2] = {0xE72B, 0};
            DrawText(dc, gb, rc, r_.Icon(18), white);
            nameX = backW + 16;
        }
        int imgIndex = 0, imgCount = 0;
        ViewerRank(viewer_.pos, imgIndex, imgCount);
        // rechts: Diashow, Zuschneiden, Info, dann X bzw. die Windows-Knoepfe
        const float bRight = W_ - closeW;
        const float textRight = bRight - 3 * 48 - 8;
        IDWriteTextFormat* fn = r_.Text(14, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        IDWriteTextFormat* fi = r_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        std::vector<std::wstring> parts;
        parts.push_back(std::to_wstring(imgIndex) + L" / " + std::to_wstring(imgCount));
        if (e.width) parts.push_back(std::to_wstring(e.width) + L" \u00d7 " + std::to_wstring(e.height));
        parts.push_back(FormatSize(e.size));
        parts.push_back(std::to_wstring(static_cast<int>(std::round(sTarget * r_.scale() * 100))) + L" %");
        auto join = [&]() {
            std::wstring r;
            for (auto& p : parts) r += (r.empty() ? L"" : L"   \u00b7   ") + p;
            return r;
        };
        const float avail = std::max(0.0f, textRight - nameX);
        const float nameFull = r_.MeasureText(e.name, fn);
        std::wstring info = join();
        float infoW = r_.MeasureText(info, fi);
        while (!parts.empty() && nameFull + 28 + infoW > avail) {   // zuerst Angaben weglassen (Zoom, Groesse, ...)
            parts.pop_back();
            info = join();
            infoW = info.empty() ? 0 : r_.MeasureText(info, fi);
        }
        const float nameAvail = std::max(0.0f, avail - (info.empty() ? 0 : infoW + 28));
        std::wstring nameShown = nameFull <= nameAvail
                                 ? e.name
                                 : MiddleEllipsis(e.name, nameAvail, [&](const std::wstring& t2) { return r_.MeasureText(t2, fn); });
        DrawText(dc, nameShown, D2D1::RectF(nameX, by, nameX + nameAvail + 4, by + barH), fn, white);
        if (!info.empty()) DrawText(dc, info, D2D1::RectF(textRight - infoW - 8, by, textRight, by + barH), fi, dim);
        // Diashow: Abspielen (laeuft sie, beendet derselbe Knopf sie)
        topButton(HitType::ViewerSlideshow, viewer_.slide ? 0xE71A : 0xE768, bRight - 144, bRight - 96, barH, false, viewer_.slide);
        if (e.ext != L"svg" && e.ext != L"tga") topButton(HitType::ViewerCrop, 0xE7A8, bRight - 96, bRight - 48, barH, false, false);
        topButton(HitType::ViewerInfo, 0xE946, bRight - 48, bRight, barH, false, settings_.viewerInfo);
        if (winCaps) {
            topButton(HitType::ViewerMin, 0xE921, W_ - 3 * capW, W_ - 2 * capW, barH, false, false);
            topButton(HitType::ViewerMax, IsZoomed(hwnd_) ? 0xE923 : 0xE922, W_ - 2 * capW, W_ - capW, barH, false, false);
            topButton(HitType::ViewerClose, 0xE8BB, W_ - capW, W_, barH, true, false);
        } else {
            topButton(HitType::ViewerClose, 0xE8BB, W_ - 48, W_, barH, true, false);
        }
        btnY = 0;
    } else if (viewer_.corner) {
        if (winCaps) {
            topButton(HitType::ViewerMin, 0xE921, W_ - 3 * capW, W_ - 2 * capW, 44, false, false);
            topButton(HitType::ViewerMax, IsZoomed(hwnd_) ? 0xE923 : 0xE922, W_ - 2 * capW, W_ - capW, 44, false, false);
            topButton(HitType::ViewerClose, 0xE8BB, W_ - capW, W_, 44, true, false);
        } else {
            topButton(HitType::ViewerClose, 0xE8BB, W_ - 48, W_, 44, true, false);
        }
    }

    // kurzer Hinweis oben mittig, verschwindet von selbst
    if (!viewer_.toast.empty() && now < viewer_.toastUntil) {
        IDWriteTextFormat* ft = r_.Text(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        float tw = std::min(VW - 40, r_.MeasureText(viewer_.toast, ft) + 56);
        float ty = 16.0f + 40.0f * ba;
        D2D1_RECT_F tr = D2D1::RectF(VW / 2 - tw / 2, ty, VW / 2 + tw / 2, ty + 38);
        dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 8, 8), r_.Brush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.92f)));
        wchar_t gd[2] = {0xE74D, 0};
        DrawText(dc, gd, D2D1::RectF(tr.left + 14, tr.top, tr.left + 34, tr.bottom), r_.Icon(13), white);
        DrawText(dc, viewer_.toast, D2D1::RectF(tr.left + 36, tr.top, tr.right - 14, tr.bottom), ft, white);
    }
    if (viewer_.slide && nowMs < viewer_.slideIntroEnd) {   // Diashow startet: der Betrachter blendet nach Schwarz ab
        float k = static_cast<float>(1.0 - std::clamp((viewer_.slideIntroEnd - nowMs) / 800.0, 0.0, 1.0));
        dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(D2D1::ColorF(0, 0, 0, k * k * (3 - 2 * k))));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// Einstellungen
// ---------------------------------------------------------------------------

void App::PaintSettings(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);

    D2D1_RECT_F back = D2D1::RectF(12, 12, 48, 48);
    int bIdx = static_cast<int>(hits_.size());
    hits_.push_back({back, HitType::SettingsBack, 0});
    if (hoverHit_ == bIdx) FillRound(dc, back, 5, t.hover);
    wchar_t gb[2] = {0xE72B, 0};
    DrawText(dc, gb, back, r_.Icon(14), t.text);
    DrawText(dc, T(L"Einstellungen"), D2D1::RectF(60, 10, W_ - 20, 50), r_.Text(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), t.text);

    float x0 = 60, x1 = std::min(W_ - 40, 60.0f + 760);
    float y = 72;
    IDWriteTextFormat* fl = r_.TextWrap(13);
    IDWriteTextFormat* fv = r_.TextWrap(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    IDWriteTextFormat* fh = r_.Text(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float labelW = 200, valueX = x0 + 20 + labelW + 20, valueW = x1 - 20 - valueX;

    using Rows = std::vector<std::pair<std::wstring, std::wstring>>;
    auto rowHeight = [&](const std::pair<std::wstring, std::wstring>& r) {
        float h = std::max(r_.MeasureWrapped(r.first, fl, labelW), r_.MeasureWrapped(r.second, fv, valueW));
        return std::max(36.0f, h + 18);
    };
    auto card = [&](const Rows& rows, float extra) {
        float h = 8 + extra;
        for (auto& r : rows) h += rowHeight(r);
        D2D1_RECT_F rc = D2D1::RectF(x0, y, x1, y + h);
        FillRound(dc, rc, 8, t.chip);
        float ry = rc.top + 4;
        for (auto& r : rows) {
            float rh = rowHeight(r);
            float lh = r_.MeasureWrapped(r.first, fl, labelW), vh = r_.MeasureWrapped(r.second, fv, valueW);
            r_.DrawWrapped(r.first, x0 + 20, ry + (rh - lh) / 2, labelW, fl, t.text);
            r_.DrawWrapped(r.second, valueX, ry + (rh - vh) / 2, valueW, fv, t.text2);
            ry += rh;
        }
        return D2D1::RectF(rc.left, rc.top, rc.right, ry);
    };
    auto button = [&](HitType type, const std::wstring& label, float bx, float by, bool danger) {
        IDWriteTextFormat* bf = r_.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        float w = r_.MeasureText(label, bf) + 32;
        D2D1_RECT_F rc = D2D1::RectF(bx, by, bx + w, by + 32);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, type, 0});
        FillRound(dc, rc, 5, hoverHit_ == idx ? t.chipHover : t.layer);
        DrawText(dc, label, rc, bf, danger ? t.danger : t.text);
        return w;
    };

    // --- Darstellung: Farbmodus per Dropdown
    DrawText(dc, T(L"Darstellung"), D2D1::RectF(x0, y, x1, y + 28), fh, t.text);
    y += 34;
    {
        D2D1_RECT_F rc = D2D1::RectF(x0, y, x1, y + 8 + 7 * 56);
        FillRound(dc, rc, 8, t.chip);
        auto dropRow = [&](float ry, const wchar_t* label, HitType type, const std::wstring& cur) {
            DrawText(dc, label, D2D1::RectF(x0 + 20, ry, x1 - 260, ry + 56), r_.Text(13), t.text);
            IDWriteTextFormat* bf = r_.Text(13);
            float w = std::max(200.0f, r_.MeasureText(cur, bf) + 52);
            D2D1_RECT_F dd = D2D1::RectF(x1 - 20 - w, ry + 12, x1 - 20, ry + 44);
            int idx = static_cast<int>(hits_.size());
            hits_.push_back({dd, type, 0});
            FillRound(dc, dd, 5, hoverHit_ == idx ? t.chipHover : t.layer);
            DrawText(dc, cur, D2D1::RectF(dd.left + 12, dd.top, dd.right - 30, dd.bottom), bf, t.text);
            wchar_t g[2] = {0xE70D, 0};
            DrawText(dc, g, D2D1::RectF(dd.right - 30, dd.top, dd.right - 8, dd.bottom), r_.Icon(11), t.text2);
        };
        dropRow(rc.top + 4, T(L"Sprache"), HitType::SettingsLanguage, LanguageName(settings_.language));
        dc->DrawLine(D2D1::Point2F(x0 + 16, rc.top + 60), D2D1::Point2F(x1 - 16, rc.top + 60), r_.Brush(t.stroke), 1.0f);
        dropRow(rc.top + 60, T(L"Farbmodus"), HitType::SettingsTheme, T(kThemeNames[std::clamp(settings_.theme, 0, 2)]));
        dc->DrawLine(D2D1::Point2F(x0 + 16, rc.top + 116), D2D1::Point2F(x1 - 16, rc.top + 116), r_.Brush(t.stroke), 1.0f);
        dropRow(rc.top + 116, T(L"Bildbetrachter im Vollbild"), HitType::SettingsViewerFullscreen,
                T(kViewerFullscreenNames[settings_.viewerFullscreen ? 0 : 1]));
        dc->DrawLine(D2D1::Point2F(x0 + 16, rc.top + 172), D2D1::Point2F(x1 - 16, rc.top + 172), r_.Brush(t.stroke), 1.0f);
        dropRow(rc.top + 172, T(L"Kleine Bilder im Betrachter"), HitType::SettingsViewerSmall,
                T(kViewerSmallNames[std::clamp(settings_.viewerSmall, 0, 1)]));
        dc->DrawLine(D2D1::Point2F(x0 + 16, rc.top + 228), D2D1::Point2F(x1 - 16, rc.top + 228), r_.Brush(t.stroke), 1.0f);
        dropRow(rc.top + 228, T(L"Ordner und Dateien öffnen"), HitType::SettingsSingleClick,
                T(kSingleClickNames[settings_.singleClick ? 1 : 0]));
        dc->DrawLine(D2D1::Point2F(x0 + 16, rc.top + 284), D2D1::Point2F(x1 - 16, rc.top + 284), r_.Brush(t.stroke), 1.0f);
        dropRow(rc.top + 284, T(L"Auswahlkästchen"), HitType::SettingsCheckBoxes,
                T(kCheckBoxNames[settings_.checkBoxes ? 1 : 0]));
        dc->DrawLine(D2D1::Point2F(x0 + 16, rc.top + 340), D2D1::Point2F(x1 - 16, rc.top + 340), r_.Brush(t.stroke), 1.0f);
        dropRow(rc.top + 340, T(L"Klick auf den Aufnahmeort"), HitType::SettingsGeoClick,
                T(kGeoClickNames[std::clamp(settings_.geoClick, 0, 1)]));
        y = rc.bottom + 28;
    }

    // --- Tastenkombinationen (eigene Seite)
    DrawText(dc, T(L"Tastenkombinationen"), D2D1::RectF(x0, y, x1, y + 28), fh, t.text);
    y += 34;
    {
        D2D1_RECT_F rc = D2D1::RectF(x0, y, x1, y + 56);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::SettingsShortcuts, 0});
        FillRound(dc, rc, 8, hoverHit_ == idx ? t.chipHover : t.chip);
        wchar_t gk[2] = {0xE765, 0};
        DrawText(dc, gk, D2D1::RectF(x0 + 16, rc.top, x0 + 44, rc.bottom), r_.Icon(16), t.text);
        DrawText(dc, T(L"Alle Tastenkombinationen anzeigen"), D2D1::RectF(x0 + 56, rc.top, x1 - 50, rc.bottom), r_.Text(13), t.text);
        wchar_t gc[2] = {0xE76C, 0};
        DrawText(dc, gc, D2D1::RectF(x1 - 44, rc.top, x1 - 16, rc.bottom), r_.Icon(13), t.text2);
        y = rc.bottom + 28;
    }

    // --- Index
    DrawText(dc, L"Index", D2D1::RectF(x0, y, x1, y + 28), fh, t.text);
    y += 34;
    std::wstring dataDir = DataDir();
    std::wstring exeDir = ExeDir();
    bool portable = _wcsnicmp(dataDir.c_str(), exeDir.c_str(), exeDir.size()) == 0;
    Rows rows = {
        {T(L"Datenordner"), dataDir},
        {T(L"Speicherort"), portable ? T(L"Portabel (neben peek.exe)") : T(L"Benutzerprofil (lokale Anwendungsdaten)")},
        {T(L"Dateien im Index"), TF(L"{0}  (davon {1} Bilder/Videos, {2} mit Metadaten)",
                                    {Thousands(dbStats_.files), Thousands(dbStats_.media), Thousands(dbStats_.withMeta)})},
        {T(L"Vorschaubilder"), Thousands(dbStats_.thumbs)},
        {T(L"Speicherbedarf"), TF(L"Index {0}  ·  Vorschaubilder {1}", {FormatSize(dbBytes_), FormatSize(thumbBytes_)})},
        {L"Status", indexBusy_ ? TF(L"Indexierung läuft: {0} / {1}", {Thousands(indexDone_), Thousands(indexTotal_)})
                               : std::wstring(T(L"Bereit"))}};
    if (collectionCounts_.empty()) rows.push_back({T(L"Sammlungen"), T(L"Keine – Ordner in die Seitenleiste ziehen")});
    for (size_t i = 0; i < collectionCounts_.size(); ++i)
        rows.push_back({i == 0 ? T(L"Sammlungen") : L"",
                        collectionCounts_[i].first + L"  ·  " + Thousands(collectionCounts_[i].second) + T(L" Dateien")});
    D2D1_RECT_F ic = card(rows, 60);
    float bx = ic.left + 20, by = ic.bottom + 12;
    bx += button(HitType::SettingsOpenData, T(L"Datenordner öffnen"), bx, by, false) + 10;
    bx += button(HitType::SettingsRescan, T(L"Neu indexieren"), bx, by, false) + 10;
    button(HitType::SettingsDeleteIndex, T(L"Index löschen"), bx, by, true);
    y = by + 32 + 16 + 28;

    // --- Ueber peek (mit App-Symbol)
    DrawText(dc, T(L"Über peek"), D2D1::RectF(x0, y, x1, y + 28), fh, t.text);
    y += 34;
    D2D1_RECT_F about = D2D1::RectF(x0, y, x1, y + 104);
    FillRound(dc, about, 8, t.chip);
    // Logo mit Schriftzug (Seitenverhaeltnis 1072 : 512), Variante passend zum Farbschema
    const float logoH = 80, logoW = logoH * 1072.0f / 512.0f;
    r_.DrawSvgResource(dark_ ? 201 : 202, D2D1::RectF(about.left + 8, about.top + 12, about.left + 8 + logoW, about.top + 12 + logoH));
    float tx = about.left + 8 + logoW + 12;
    std::wstring about1 = std::wstring(L"Version ") + PEEK_VERSION + L"  \u00b7  Build " + Utf8ToWide(__DATE__);
    std::wstring about2 = T(L"Nativ \u00b7 portabel \u00b7 ohne Netzwerk- und Internetfunktionen") + std::wstring(L"\n") + DecoderSandboxText();
    IDWriteTextFormat* fa = r_.TextWrap(12.5f);
    float aw = about.right - 20 - tx;
    float ah = r_.MeasureWrapped(about1 + L"\n" + about2, fa, aw);
    r_.DrawWrapped(about1 + L"\n" + about2, tx, about.top + (104 - ah) / 2, aw, fa, t.text2);
    settingsContentH_ = about.bottom + 32;
}

// Eigene Seite der Einstellungen: alle Tastenkombinationen, nach Bereichen gruppiert (mit Mausrad scrollbar)
void App::PaintShortcuts(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(WithAlpha(t.layer, backdrop_ ? 0.45f : 1.0f)));

    struct Group { const wchar_t* title; std::vector<std::pair<std::wstring, std::wstring>> rows; };
    static const std::vector<Group> groups = {
        {N_(L"Allgemein"),
         {{N_(L"Strg+F / Tippen"), N_(L"Suchen")},
          {N_(L"Strg+,"), N_(L"Einstellungen")},
          {L"F11", N_(L"Vollbild ein/aus")},
          {L"F9", N_(L"Seitenleiste ein/aus")},
          {N_(L"Strg+I"), N_(L"Info-Bereich ein/aus")},
          {L"F5", N_(L"Aktualisieren")},
          {N_(L"Maustaste Zurück / Vor"), N_(L"Zurück / Vor (in den Einstellungen: schließen)")}}},
        {N_(L"Navigation und Auswahl"),
         {{N_(L"Alt+← / Rücktaste"), N_(L"Zurück")},
          {L"Alt+→", N_(L"Vor")},
          {L"Alt+↑", N_(L"Übergeordneter Ordner")},
          {N_(L"Pfeiltasten"), N_(L"Fokus bewegen")},
          {N_(L"Bild↑ / Bild↓"), N_(L"Seitenweise bewegen")},
          {N_(L"Pos1 / Ende"), N_(L"Erstes / letztes Element")},
          {N_(L"Umschalt+Pfeiltasten"), N_(L"Auswahl erweitern")},
          {N_(L"Strg+Leertaste"), N_(L"Element zur Auswahl hinzufügen oder entfernen")},
          {N_(L"Strg+A"), N_(L"Alles auswählen")},
          {L"Esc", N_(L"Auswahl aufheben")},
          {N_(L"Enter / Doppelklick"), N_(L"Öffnen (Ordner, Bildbetrachter, Standard-App)")},
          {N_(L"Leertaste"), N_(L"Bild im Betrachter öffnen")},
          {N_(L"Kontextmenü-Taste"), N_(L"Kontextmenü")}}},
        {N_(L"Dateien"),
         {{N_(L"Strg+C"), N_(L"Kopieren")},
          {N_(L"Strg+Umschalt+C"), N_(L"Pfad kopieren")},
          {N_(L"Strg+V"), N_(L"Einfügen")},
          {L"F2", N_(L"Umbenennen")},
          {N_(L"Strg+Umschalt+N"), N_(L"Neuer Ordner")},
          {N_(L"Entf"), N_(L"In den Papierkorb")},
          {N_(L"Strg+E"), N_(L"Im Explorer zeigen")},
          {L"Alt+Enter", N_(L"Eigenschaften")}}},
        {N_(L"Ansicht"),
         {{N_(L"Strg+1 / Strg+2 / Strg+3 / Strg+4"), N_(L"Vorschaubilder / Liste / Bilderwolke / Zeitleiste")},
          {N_(L"Strg+Plus / Strg+Minus"), N_(L"Vorschaubilder größer / kleiner")},
          {N_(L"Strg+Mausrad"), N_(L"Vorschaubildgröße stufenlos")},
          {N_(L"Strg+0"), N_(L"Standardgröße")},
          {N_(L"Strg+R"), N_(L"Unterordner ein/aus")}}},
        {N_(L"Suchfeld"),
         {{L"Enter / ↓", N_(L"Zu den Ergebnissen")},
          {L"Esc", N_(L"Suche leeren")},
          {N_(L"Strg+A / Doppelklick"), N_(L"Gesamten Text markieren")}}},
        {N_(L"Bilder vergleichen"),
         {{L"Tab / M", N_(L"Nebeneinander / übereinander")},
          {N_(L"Mausrad / Ziehen"), N_(L"Zoomen / verschieben (beide Bilder)")},
          {N_(L"0 / Doppelklick"), N_(L"Einpassen")},
          {L"Esc", N_(L"Schließen")}}},
        {N_(L"Bilderwolke"),
         {{N_(L"Pfeiltasten"), N_(L"Bewegen")},
          {N_(L"Mausrad / Plus / Minus"), N_(L"Hinein- / herausfahren")},
          {N_(L"Strg+Mausrad"), N_(L"Abstand der Karten")},
          {N_(L"Strg+0"), N_(L"Standardabstand")},
          {N_(L"Pos1"), N_(L"Ganze Wolke zeigen")},
          {N_(L"← / → (Bild ausgewählt)"), N_(L"Vorheriges / nächstes Bild")},
          {L"Esc", N_(L"Auswahl aufheben")}}},
        {N_(L"Bildbetrachter"),
         {{L"← / →", N_(L"Vorheriges / nächstes Bild")},
          {N_(L"Leertaste / Bild↓"), N_(L"Nächstes Bild")},
          {N_(L"Pos1 / Ende"), N_(L"Erstes / letztes Bild")},
          {N_(L"Plus / Minus / Mausrad"), N_(L"Zoomen")},
          {N_(L"0 / 1 / Doppelklick"), N_(L"Einpassen / 100 % / umschalten")},
          {N_(L"R / Umschalt+R"), N_(L"Nach rechts / links drehen")},
          {L"I", N_(L"Info-Bereich ein/aus")},
          {N_(L"Entf"), N_(L"In den Papierkorb")},
          {N_(L"Strg+C / Strg+Umschalt+C"), N_(L"Kopieren / Pfad kopieren")},
          {N_(L"Strg+E"), N_(L"Im Explorer zeigen")},
          {N_(L"Esc / Enter / Rücktaste"), N_(L"Betrachter schließen")}}},
        {N_(L"Zeitleiste"),
         {{N_(L"← / → / Mausrad"), N_(L"Vor und zurück")},
          {N_(L"Bild↑ / Bild↓"), N_(L"Eine Fensterbreite")},
          {N_(L"Pos1 / Ende"), N_(L"Neueste / älteste Bilder")},
          {N_(L"Ziehen mit der Maus"), N_(L"Ansicht verschieben")},
          {N_(L"Übersicht unten"), N_(L"Klicken oder ziehen: dorthin springen")}}},
        {N_(L"Diashow"),
         {{L"← / →", N_(L"Vorheriges / nächstes Bild")},
          {N_(L"Leertaste"), N_(L"Anhalten / fortsetzen")},
          {L"Esc", N_(L"Diashow beenden")}}},
    };

    float sy = settingsScroll_;
    float x0 = 60, x1 = std::min(W_ - 40, 60.0f + 760);
    float y = 72 - sy;
    IDWriteTextFormat* fh = r_.Text(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    IDWriteTextFormat* fk = r_.Text(12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat* fd = r_.Text(13);
    IDWriteTextFormat* fs = r_.Text(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    const float rowH = 40, keyW = 300;
    for (auto& g : groups) {
        DrawText(dc, T(g.title), D2D1::RectF(x0, y, x1, y + 28), fh, t.text);
        y += 34;
        D2D1_RECT_F card = D2D1::RectF(x0, y, x1, y + 8 + rowH * g.rows.size());
        FillRound(dc, card, 8, t.chip);
        float ry = card.top + 4;
        for (size_t i = 0; i < g.rows.size(); ++i) {
            if (i > 0)
                dc->DrawLine(D2D1::Point2F(x0 + 16, ry), D2D1::Point2F(x1 - 16, ry), r_.Brush(t.stroke), 1.0f);
            // Tasten als kleine Tastenkappen; Alternativen mit " / " getrennt
            const std::wstring keys = T(g.rows[i].first);
            float kx = x0 + 16;
            size_t s = 0;
            while (s <= keys.size()) {
                size_t e = keys.find(L" / ", s);
                std::wstring k = keys.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
                float w = r_.MeasureText(k, fk) + 18;
                D2D1_RECT_F kr = D2D1::RectF(kx, ry + 8, kx + w, ry + rowH - 8);
                FillRound(dc, kr, 4, t.layer);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(kr, 4, 4), r_.Brush(t.stroke), 1.0f);
                DrawText(dc, k, kr, fk, t.text);
                kx = kr.right;
                if (e == std::wstring::npos) break;
                DrawText(dc, L"/", D2D1::RectF(kx, ry, kx + 16, ry + rowH), fs, t.text3);
                kx += 16;
                s = e + 3;
            }
            DrawText(dc, T(g.rows[i].second), D2D1::RectF(std::max(x0 + 16 + keyW, kx + 16), ry, x1 - 16, ry + rowH), fd, t.text2);
            ry += rowH;
        }
        y = card.bottom + 28;
    }
    settingsContentH_ = y + sy + 20;

    // Kopfzeile fest oben (ueber dem gescrollten Inhalt)
    dc->FillRectangle(D2D1::RectF(0, 0, W_, 62), r_.Brush(dark_ ? D2D1::ColorF(0.16f, 0.16f, 0.16f, 0.97f)
                                                             : D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.97f)));
    D2D1_RECT_F back = D2D1::RectF(12, 12, 48, 48);
    int bIdx = static_cast<int>(hits_.size());
    hits_.push_back({back, HitType::SettingsBack, 0});
    if (hoverHit_ == bIdx) FillRound(dc, back, 5, t.hover);
    wchar_t gb[2] = {0xE72B, 0};
    DrawText(dc, gb, back, r_.Icon(14), t.text);
    DrawText(dc, T(L"Tastenkombinationen"), D2D1::RectF(60, 10, W_ - 20, 50), r_.Text(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), t.text);

    // Bildlaufleiste
    if (settingsContentH_ > H_) {
        float trackTop = 66, trackH = H_ - 70;
        float thumbH = std::max(32.0f, trackH * H_ / settingsContentH_);
        float maxS = settingsContentH_ - H_;
        float ty = trackTop + (trackH - thumbH) * (maxS > 0 ? sy / maxS : 0);
        FillRound(dc, D2D1::RectF(W_ - 9, ty, W_ - 6, ty + thumbH), 1.5f, WithAlpha(t.text, 0.3f));
    }
}

// ---------------------------------------------------------------------------
// Kontaktabzug: Werkzeugleiste, Optionen, eine Seite (WYSIWYG), Bildstreifen
// ---------------------------------------------------------------------------

void App::PaintSheet(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    auto& s = sheet_;
    dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(WithAlpha(t.layer, backdrop_ ? 0.55f : 1.0f)));
    if (!sheetFontR_ || !sheetFontB_) return;
    const SheetFont& fr = *sheetFontR_;
    const SheetFont& fb = *sheetFontB_;
    auto glyphText = [&](const SheetFont& f, const std::wstring& text, float size, float x, float baseline,
                         const D2D1_COLOR_F& c) {
        std::vector<UINT16> g;
        std::vector<float> a;
        f.Shape(text, size, g, a);
        if (g.empty()) return;
        DWRITE_GLYPH_RUN run{};
        run.fontFace = f.Face();
        run.fontEmSize = size;
        run.glyphCount = static_cast<UINT32>(g.size());
        run.glyphIndices = g.data();
        run.glyphAdvances = a.data();
        dc->DrawGlyphRun(D2D1::Point2F(x, baseline), &run, r_.Brush(c));
    };

    std::vector<int> on = SheetOnItems();
    const int per = SheetPerPage();
    const int pages = SheetPageCount(static_cast<int>(on.size()), per);
    s.page = std::clamp(s.page, 0, pages - 1);

    // --- Werkzeugleiste
    const float toolH = 52, optH = 48, stripH = 136;
    dc->FillRectangle(D2D1::RectF(0, 0, W_, toolH), r_.Brush(t.status));
    wchar_t gi[2] = {0xECA5, 0};
    DrawText(dc, gi, D2D1::RectF(16, 0, 40, toolH), r_.Icon(15), t.text);
    DrawText(dc, T(L"Kontaktabzug"), D2D1::RectF(48, 0, 220, toolH), r_.Text(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), t.text);
    std::wstring cnt = TF(L"{0} von {1} Bildern", {Thousands(on.size()), Thousands(s.items.size())}) + L"  ·  " +
                       Thousands(pages) + (pages == 1 ? T(L" Seite") : T(L" Seiten"));
    DrawText(dc, cnt, D2D1::RectF(170, 0, W_ / 2 + 120, toolH), r_.Text(12.5f), t.text2);
    IDWriteTextFormat* bf = r_.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    auto toolButton = [&](HitType type, const std::wstring& label, float right, bool primary) {
        float w = r_.MeasureText(label, bf) + 36;
        D2D1_RECT_F rc = D2D1::RectF(right - w, 10, right, toolH - 10);
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, type, 0});
        D2D1_COLOR_F bg = primary ? accent_ : (hoverHit_ == idx ? t.chipHover : t.chip);
        if (primary && hoverHit_ == idx)
            bg = D2D1::ColorF(std::min(1.0f, bg.r * 1.15f + 0.05f), std::min(1.0f, bg.g * 1.15f + 0.05f),
                              std::min(1.0f, bg.b * 1.15f + 0.05f), 1);
        FillRound(dc, rc, 5, bg);
        DrawText(dc, label, rc, bf, primary ? D2D1::ColorF(1, 1, 1, 1) : t.text);
        return rc.left;
    };
    float rx = W_ - 16;
    if (s.busy) {
        rx = toolButton(HitType::SheetCancel, T(L"Abbrechen"), rx, false) - 16;
        float pw = 200;
        D2D1_RECT_F bar = D2D1::RectF(rx - pw, toolH / 2 - 3, rx, toolH / 2 + 3);
        FillRound(dc, bar, 3, t.chip);
        FillRound(dc, D2D1::RectF(bar.left, bar.top, bar.left + pw * s.progress / 1000.0f, bar.bottom), 3, accent_);
        DrawText(dc, T(L"PDF wird erstellt …"), D2D1::RectF(rx - pw - 170, 0, rx - pw - 12, toolH),
                 r_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), t.text2);
    } else {
        rx = toolButton(HitType::SheetCreate, T(L"PDF erstellen …"), rx, true) - 8;
        toolButton(HitType::SheetCancel, T(L"Abbrechen"), rx, false);
    }

    // --- Optionen (wie die Filterleiste)
    float ox = 16, oy = toolH + 8, oh = 32;
    IDWriteTextFormat* of = r_.Text(13);
    auto chip = [&](HitType type, const std::wstring& label, bool active, bool enabled, wchar_t glyph, bool drop) {
        float w = r_.MeasureText(label, of) + 24 + (glyph ? 24 : 0) + (drop ? 18 : 0);
        D2D1_RECT_F rc = D2D1::RectF(ox, oy, ox + w, oy + oh);
        int idx = static_cast<int>(hits_.size());
        if (enabled) hits_.push_back({rc, type, 0});
        if (active) FillRound(dc, rc, 5, WithAlpha(accent_, 0.14f));
        else if (enabled && hoverHit_ == idx) FillRound(dc, rc, 5, t.hover);
        D2D1_COLOR_F fg = !enabled ? t.text3 : (active ? accent_ : t.text);
        float tx = rc.left + 12;
        if (glyph) {
            wchar_t g[2] = {glyph, 0};
            DrawText(dc, g, D2D1::RectF(tx, rc.top, tx + 18, rc.bottom), r_.Icon(13), fg);
            tx += 24;
        }
        DrawText(dc, label, D2D1::RectF(tx, rc.top, rc.right, rc.bottom), of, fg);
        if (drop) {
            wchar_t g[2] = {0xE70D, 0};
            DrawText(dc, g, D2D1::RectF(rc.right - 24, rc.top, rc.right - 8, rc.bottom), r_.Icon(9), fg);
        }
        ox = rc.right + 6;
    };
    auto stepper = [&](const wchar_t* label, int value, HitType minus, HitType plus) {
        DrawText(dc, label, D2D1::RectF(ox + 8, oy, ox + 80, oy + oh), of, t.text2);
        ox += r_.MeasureText(label, of) + 16;
        auto btn = [&](HitType type, wchar_t glyph) {
            D2D1_RECT_F rc = D2D1::RectF(ox, oy + 2, ox + 28, oy + oh - 2);
            int idx = static_cast<int>(hits_.size());
            hits_.push_back({rc, type, 0});
            FillRound(dc, rc, 5, hoverHit_ == idx ? t.chipHover : t.chip);
            wchar_t g[2] = {glyph, 0};
            DrawText(dc, g, rc, r_.Icon(11), t.text);
            ox = rc.right;
        };
        btn(minus, 0xE738);
        DrawText(dc, std::to_wstring(value), D2D1::RectF(ox, oy, ox + 28, oy + oh),
                 r_.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), t.text);
        ox += 28;
        btn(plus, 0xE710);
        ox += 12;
    };
    chip(HitType::SheetOrient, settings_.sheetLandscape ? T(L"A4 Querformat") : T(L"A4 Hochformat"), false, true,
         settings_.sheetLandscape ? 0xE7AD : 0xE7C3, false);
    ox += 6;
    stepper(T(L"Spalten"), settings_.sheetCols, HitType::SheetColsMinus, HitType::SheetColsPlus);
    stepper(T(L"Zeilen"), settings_.sheetRows, HitType::SheetRowsMinus, HitType::SheetRowsPlus);
    chip(HitType::SheetNames, T(L"Dateinamen"), settings_.sheetNames, true, settings_.sheetNames ? 0xE73E : 0, false);
    chip(HitType::SheetExts, T(L"Dateiendungen"), settings_.sheetNames && settings_.sheetExts, settings_.sheetNames,
         settings_.sheetNames && settings_.sheetExts ? 0xE73E : 0, false);
    static const wchar_t* qn[] = {N_(L"Qualität: Klein"), N_(L"Qualität: Mittel"), N_(L"Qualität: Hoch")};
    chip(HitType::SheetQuality, T(qn[std::clamp(settings_.sheetQuality, 0, 2)]), false, true, 0, true);

    // --- Seite
    SheetOptions so;
    so.landscape = settings_.sheetLandscape;
    so.cols = settings_.sheetCols;
    so.rows = settings_.sheetRows;
    so.names = settings_.sheetNames;
    so.lineFactor = fr.LineFactor();
    SheetGeometry g = ComputeSheet(so);
    const float areaTop = toolH + optH + 8, areaBottom = H_ - stripH - 8, arrowW = 56;
    float availW = std::max(50.0f, W_ - 2 * arrowW - 32), availH = std::max(50.0f, areaBottom - areaTop - 16);
    float k = std::min(availW / g.pageW, availH / g.pageH);
    float pw = g.pageW * k, ph = g.pageH * k;
    D2D1_RECT_F page = D2D1::RectF((W_ - pw) / 2, areaTop + 8 + (availH - ph) / 2, (W_ + pw) / 2, areaTop + 8 + (availH - ph) / 2 + ph);
    s.pageRc = page;
    s.scale = k;
    for (int i = 4; i >= 1; --i)
        dc->FillRectangle(D2D1::RectF(page.left - i + 2, page.top - i + 4, page.right + i - 2, page.bottom + i + 2),
                          r_.Brush(D2D1::ColorF(0, 0, 0, 0.05f)));
    dc->FillRectangle(page, r_.Brush(D2D1::ColorF(1, 1, 1, 1)));
    auto P = [&](const SheetRect& r) {
        return D2D1::RectF(page.left + r.x * k, page.top + r.y * k, page.left + r.Right() * k, page.top + r.Bottom() * k);
    };
    const D2D1_COLOR_F black = D2D1::ColorF(0, 0, 0, 1);

    // Ueberschrift (direkt auf der Seite bearbeitbar)
    {
        D2D1_RECT_F hr = P(g.header);
        hr.top -= 4;
        hr.bottom += 4;
        s.headRc = hr;
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({hr, HitType::SheetHeadline, 0});
        float size = kSheetHeadSize * k;
        std::wstring text = s.headline;
        if (!s.editing) text = EndEllipsis(text, g.header.w, [&](const std::wstring& x) { return fb.Width(x, kSheetHeadSize); });
        float w = fb.Width(text, size);
        float x = page.left + (g.header.x + g.header.w / 2) * k - w / 2;
        float base = page.top + (g.header.y + fb.Ascent() * kSheetHeadSize) * k;
        s.headX = x;
        if (!s.editing && hoverHit_ == idx) FillRound(dc, hr, 4, D2D1::ColorF(0, 0, 0, 0.05f));
        if (s.editing) dc->DrawRoundedRectangle(D2D1::RoundedRect(hr, 4, 4), r_.Brush(WithAlpha(accent_, 0.7f)), 1.0f);
        if (text.empty() && !s.editing) {
            std::wstring ph2 = T(L"Überschrift eingeben");
            float w2 = fr.Width(ph2, size);
            glyphText(fr, ph2, size, page.left + (g.header.x + g.header.w / 2) * k - w2 / 2, base, D2D1::ColorF(0, 0, 0, 0.35f));
        } else {
            if (s.editing && s.selAll)
                dc->FillRectangle(D2D1::RectF(x, base - fb.Ascent() * size, x + w, base + (fb.LineFactor() - fb.Ascent()) * size),
                                  r_.Brush(WithAlpha(accent_, 0.3f)));
            glyphText(fb, text, size, x, base, black);
        }
        if (s.editing && !s.selAll) {
            float cx = x + fb.Width(s.headline.substr(0, std::min(s.caret, s.headline.size())), size);
            dc->DrawLine(D2D1::Point2F(cx, base - fb.Ascent() * size), D2D1::Point2F(cx, base + 0.2f * size), r_.Brush(black), 1.0f);
        }
    }
    // Seitenzahl unten
    {
        std::wstring label = SheetPageLabel(s.page + 1, pages);
        float size = kSheetFootSize * k;
        float w = fr.Width(label, size);
        glyphText(fr, label, size, page.left + (g.footer.x + g.footer.w / 2) * k - w / 2,
                  page.top + (g.footer.y + fr.Ascent() * kSheetFootSize) * k, black);
    }
    // Bilder der Seite
    s.cellRc.clear();
    std::vector<std::pair<D2D1_RECT_F, int>> checks;
    for (int c = 0; c < per; ++c) {
        D2D1_RECT_F cellR = P(g.cells[c]);
        s.cellRc.push_back(cellR);
        size_t ci = static_cast<size_t>(s.page) * per + c;
        if (ci >= on.size()) continue;
        int item = on[ci];
        const FileEntry& e = s.items[item].e;
        EnsureThumb(e, 0, frame_);
        Bmp& b = bmps_[e.path];
        b.lastUse = frame_;
        float aw = b.srcW ? static_cast<float>(b.srcW) : 1, ah = b.srcH ? static_cast<float>(b.srcH) : 1;
        D2D1_RECT_F ir = P(FitInto(g.cells[c], aw, ah, so.names ? 1.0f : 0.5f));   // mit Dateinamen: unten buendig
        bool dragged = s.dragging && s.press == item;
        if (b.bmp) dc->DrawBitmap(b.bmp.Get(), ir, dragged ? 0.3f : 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        else dc->FillRectangle(ir, r_.Brush(D2D1::ColorF(0, 0, 0, 0.06f)));
        if (s.focus == item && !s.dragging)
            dc->DrawRectangle(D2D1::RectF(ir.left - 2, ir.top - 2, ir.right + 2, ir.bottom + 2), r_.Brush(accent_), 2.0f);
        if (so.names && c < static_cast<int>(g.captions.size())) {
            std::wstring cap = MiddleEllipsis(SheetCaption(e), g.captions[c].w,
                                              [&](const std::wstring& x) { return fr.Width(x, kSheetCaptionSize); });
            float size = kSheetCaptionSize * k;
            float w = fr.Width(cap, size);
            D2D1_RECT_F cr = P(g.captions[c]);
            glyphText(fr, cap, size, (cr.left + cr.right) / 2 - w / 2, cr.top + fr.Ascent() * size, black);
        }
        hits_.push_back({D2D1::RectF(cellR.left, cellR.top, cellR.right,
                                     so.names ? P(g.captions[c]).bottom : cellR.bottom), HitType::SheetCell, item});
        if (!s.dragging && (s.hover == item || s.focus == item)) checks.push_back({ir, item});
    }
    // Kaestchen zum Aushaken (ueber dem Bild, oben links)
    for (auto& [ir, item] : checks) {
        D2D1_RECT_F cb = D2D1::RectF(ir.left + 6, ir.top + 6, ir.left + 26, ir.top + 26);
        hits_.push_back({D2D1::RectF(cb.left - 4, cb.top - 4, cb.right + 4, cb.bottom + 4), HitType::SheetCheck, item});
        FillRound(dc, cb, 4, accent_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(cb, 4, 4), r_.Brush(D2D1::ColorF(1, 1, 1, 0.9f)), 1.5f);
        wchar_t g2[2] = {0xE73E, 0};
        DrawText(dc, g2, cb, r_.Icon(11), D2D1::ColorF(1, 1, 1, 1));
    }
    // Einfuegemarke auf der Seite
    if (s.dragging && s.dropOnPage && s.dropCell >= 0 && !s.cellRc.empty()) {
        int c = std::min(s.dropCell, per);
        D2D1_RECT_F ref = s.cellRc[std::min(c, per - 1)];
        float gap = kSheetGap * k / 2;
        float bx = c < per ? ref.left - gap : ref.right + gap;
        if (c > 0 && c % std::max(1, settings_.sheetCols) == 0 && c < per) {
            // Zeilenanfang: Marke ans Ende der vorigen Zeile ist gleichwertig; links der Zelle zeichnen
            bx = ref.left - gap;
        }
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx - 1.5f, ref.top, bx + 1.5f, ref.bottom), 1.5f, 1.5f), r_.Brush(accent_));
    }
    // Blaetterpfeile
    auto arrow = [&](HitType type, wchar_t glyph, float cx, bool enabled, D2D1_RECT_F& store) {
        D2D1_RECT_F rc = D2D1::RectF(cx - 22, (page.top + page.bottom) / 2 - 32, cx + 22, (page.top + page.bottom) / 2 + 32);
        store = rc;
        int idx = static_cast<int>(hits_.size());
        if (enabled) hits_.push_back({rc, type, 0});
        bool hot = enabled && (hoverHit_ == idx || (s.dragging && s.flipDir == (type == HitType::SheetPrev ? -1 : 1)));
        if (hot) FillRound(dc, rc, 6, t.chipHover);
        wchar_t gch[2] = {glyph, 0};
        DrawText(dc, gch, rc, r_.Icon(18), enabled ? t.text : t.text3);
    };
    arrow(HitType::SheetPrev, 0xE76B, page.left - 34, s.page > 0, s.prevRc);
    arrow(HitType::SheetNext, 0xE76C, page.right + 34, s.page < pages - 1, s.nextRc);

    // --- Bildstreifen
    D2D1_RECT_F strip = D2D1::RectF(0, H_ - stripH, W_, H_);
    s.stripRc = strip;
    dc->FillRectangle(strip, r_.Brush(t.status));
    const float th = 88, gapS = 10;
    float total = s.items.size() * (th + gapS) - gapS + 32;
    s.stripScroll = std::clamp(s.stripScroll, 0.0f, std::max(0.0f, total - W_));
    dc->PushAxisAlignedClip(strip, D2D1_ANTIALIAS_MODE_ALIASED);
    s.stripItemRc.clear();
    std::unordered_set<int> onPage;
    for (int c = 0; c < per; ++c) {
        size_t ci = static_cast<size_t>(s.page) * per + c;
        if (ci < on.size()) onPage.insert(on[ci]);
    }
    float sx = 16 - s.stripScroll, sy = strip.top + 16;
    for (size_t i = 0; i < s.items.size(); ++i) {
        D2D1_RECT_F rc = D2D1::RectF(sx, sy, sx + th, sy + th);
        s.stripItemRc.push_back(rc);
        sx += th + gapS;
        if (rc.right < 0 || rc.left > W_) continue;
        const auto& it = s.items[i];
        EnsureThumb(it.e, 1, frame_);
        Bmp& b = bmps_[it.e.path];
        b.lastUse = frame_;
        int idx = static_cast<int>(hits_.size());
        hits_.push_back({rc, HitType::SheetStrip, static_cast<int>(i)});
        if (hoverHit_ == idx) FillRound(dc, D2D1::RectF(rc.left - 4, rc.top - 4, rc.right + 4, rc.bottom + 4), 6, t.hover);
        float opacity = it.on ? 1.0f : 0.3f;
        if (s.dragging && s.press == static_cast<int>(i)) opacity = 0.2f;
        if (b.bmp) DrawBitmapFit(dc, &b, rc, 2.0f, opacity);
        else FillRound(dc, rc, 4, t.chip);
        if (onPage.count(static_cast<int>(i)))
            FillRound(dc, D2D1::RectF(rc.left + 8, rc.bottom + 5, rc.right - 8, rc.bottom + 8), 1.5f, accent_);
        // Kaestchen
        D2D1_RECT_F cb = D2D1::RectF(rc.left + 4, rc.top + 4, rc.left + 22, rc.top + 22);
        hits_.push_back({D2D1::RectF(cb.left - 3, cb.top - 3, cb.right + 3, cb.bottom + 3), HitType::SheetStripCheck, static_cast<int>(i)});
        if (it.on) {
            FillRound(dc, cb, 4, accent_);
            wchar_t g2[2] = {0xE73E, 0};
            DrawText(dc, g2, cb, r_.Icon(10), D2D1::ColorF(1, 1, 1, 1));
        } else {
            FillRound(dc, cb, 4, D2D1::ColorF(1, 1, 1, 0.85f));
        }
        dc->DrawRoundedRectangle(D2D1::RoundedRect(cb, 4, 4), r_.Brush(D2D1::ColorF(0, 0, 0, 0.35f)), 1.0f);
        if (s.focus == static_cast<int>(i) && !s.dragging)
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rc.left - 3, rc.top - 3, rc.right + 3, rc.bottom + 3), 5, 5),
                                     r_.Brush(accent_), 2.0f);
    }
    if (s.dragging && !s.dropOnPage && s.dropStrip >= 0 && !s.stripItemRc.empty()) {
        float bx = s.dropStrip < static_cast<int>(s.stripItemRc.size()) ? s.stripItemRc[s.dropStrip].left - gapS / 2
                                                                         : s.stripItemRc.back().right + gapS / 2;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx - 1.5f, sy - 4, bx + 1.5f, sy + th + 4), 1.5f, 1.5f), r_.Brush(accent_));
    }
    dc->PopAxisAlignedClip();

    // gezogenes Bild am Mauszeiger
    if (s.dragging && s.press >= 0 && s.press < static_cast<int>(s.items.size())) {
        Bmp& b = bmps_[s.items[s.press].e.path];
        D2D1_RECT_F gr = D2D1::RectF(s.mouse.x - 36, s.mouse.y - 36, s.mouse.x + 36, s.mouse.y + 36);
        if (b.bmp) DrawBitmapFit(dc, &b, gr, 2.0f, 0.85f);
    }
}

void App::PaintAtlas(ID2D1DeviceContext* dc) {
    Theme t = MakeTheme(dark_, backdrop_);
    auto& a = atlas_;
    dc->FillRectangle(D2D1::RectF(0, 0, W_, H_), r_.Brush(WithAlpha(t.layer, backdrop_ ? 0.55f : 1.0f)));
    const int n = static_cast<int>(a.items.size());
    a.cols = std::clamp(a.cols, 1, std::max(1, n));
    const int cols = a.cols, rows = AtlasRowCount();
    const bool valid = a.problem.empty() && a.info.w > 0 && a.info.h > 0;
    const uint64_t texW = static_cast<uint64_t>(cols) * a.info.w, texH = static_cast<uint64_t>(rows) * a.info.h;

    // --- Werkzeugleiste
    const float toolH = 52, optH = 48;
    dc->FillRectangle(D2D1::RectF(0, 0, W_, toolH), r_.Brush(t.status));
    wchar_t gi[2] = {0xE8A9, 0};
    DrawText(dc, gi, D2D1::RectF(16, 0, 40, toolH), r_.Icon(15), t.text);
    IDWriteTextFormat* tf = r_.Text(15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    DrawText(dc, T(L"Kacheltextur"), D2D1::RectF(48, 0, 220, toolH), tf, t.text);
    float tx = 48 + r_.MeasureText(T(L"Kacheltextur"), tf) + 16;
    std::wstring folderName = FileNameOf(a.folder);
    std::wstring cnt = (folderName.empty() ? a.folder : folderName) + L"  ·  " + Thousands(n) + (n == 1 ? T(L" Bild") : T(L" Bilder"));
    IDWriteTextFormat* bf = r_.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    auto toolButton = [&](HitType type, const std::wstring& label, float right, bool primary, bool enabled) {
        float w = r_.MeasureText(label, bf) + 36;
        D2D1_RECT_F rc = D2D1::RectF(right - w, 10, right, toolH - 10);
        int idx = static_cast<int>(hits_.size());
        if (enabled) hits_.push_back({rc, type, 0});
        D2D1_COLOR_F bg = primary ? accent_ : (hoverHit_ == idx ? t.chipHover : t.chip);
        if (primary && enabled && hoverHit_ == idx)
            bg = D2D1::ColorF(std::min(1.0f, bg.r * 1.15f + 0.05f), std::min(1.0f, bg.g * 1.15f + 0.05f),
                              std::min(1.0f, bg.b * 1.15f + 0.05f), 1);
        if (primary && !enabled) bg = t.chip;
        FillRound(dc, rc, 5, bg);
        DrawText(dc, label, rc, bf, !enabled ? t.text3 : (primary ? D2D1::ColorF(1, 1, 1, 1) : t.text));
        return rc.left;
    };
    float rx = W_ - 16;
    if (a.busy) {
        rx = toolButton(HitType::AtlasCancel, T(L"Abbrechen"), rx, false, true) - 16;
        float pw = 200;
        D2D1_RECT_F bar = D2D1::RectF(rx - pw, toolH / 2 - 3, rx, toolH / 2 + 3);
        FillRound(dc, bar, 3, t.chip);
        FillRound(dc, D2D1::RectF(bar.left, bar.top, bar.left + pw * a.progress / 1000.0f, bar.bottom), 3, accent_);
        DrawText(dc, T(L"Textur wird erstellt …"), D2D1::RectF(rx - pw - 190, 0, rx - pw - 12, toolH),
                 r_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), t.text2);
        rx -= pw + 190;
    } else {
        rx = toolButton(HitType::AtlasCreate, T(L"PNG erstellen …"), rx, true, valid) - 8;
        rx = toolButton(HitType::AtlasCancel, T(L"Abbrechen"), rx, false, true);
    }
    DrawText(dc, cnt, D2D1::RectF(tx, 0, std::max(tx, rx - 16), toolH), r_.Text(12.5f), t.text2);

    // --- Optionen: Spalten, Zeilen, Texturgroesse
    float ox = 16, oy = toolH + 8, oh = 32;
    IDWriteTextFormat* of = r_.Text(13);
    auto stepper = [&](const wchar_t* label, int value, HitType minus, bool minusOn, HitType plus, bool plusOn) {
        DrawText(dc, label, D2D1::RectF(ox + 8, oy, ox + 80, oy + oh), of, t.text2);
        ox += r_.MeasureText(label, of) + 16;
        auto btn = [&](HitType type, wchar_t glyph, bool on) {
            D2D1_RECT_F rc = D2D1::RectF(ox, oy + 2, ox + 28, oy + oh - 2);
            int idx = static_cast<int>(hits_.size());
            if (on) hits_.push_back({rc, type, 0});
            FillRound(dc, rc, 5, on && hoverHit_ == idx ? t.chipHover : t.chip);
            wchar_t g[2] = {glyph, 0};
            DrawText(dc, g, rc, r_.Icon(11), on ? t.text : t.text3);
            ox = rc.right;
        };
        btn(minus, 0xE738, minusOn && !a.busy);
        DrawText(dc, std::to_wstring(value), D2D1::RectF(ox, oy, ox + 36, oy + oh),
                 r_.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), t.text);
        ox += 36;
        btn(plus, 0xE710, plusOn && !a.busy);
        ox += 16;
    };
    stepper(T(L"Spalten"), cols, HitType::AtlasColsMinus, cols > 1, HitType::AtlasColsPlus, cols < n);
    stepper(T(L"Zeilen"), rows, HitType::AtlasRowsMinus, rows > 1, HitType::AtlasRowsPlus, rows < n);
    ox += 8;
    if (!valid) {
        wchar_t wg[2] = {0xE7BA, 0};
        DrawText(dc, wg, D2D1::RectF(ox, oy, ox + 20, oy + oh), r_.Icon(13), t.danger);
        DrawText(dc, a.problem.empty() ? std::wstring(T(L"Keine Textur möglich.")) : a.problem,
                 D2D1::RectF(ox + 26, oy, W_ - 16, oy + oh), of, t.danger);
    } else {
        IDWriteTextFormat* sf = r_.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring size = TF(L"Texturgröße: {0} × {1} px", {Thousands(texW), Thousands(texH)});
        DrawText(dc, size, D2D1::RectF(ox, oy, W_ - 16, oy + oh), sf, t.text);
        ox += r_.MeasureText(size, sf) + 4;
        int empty = cols * rows - n;
        std::initializer_list<std::wstring> dims = {Thousands(n), Thousands(a.info.w), Thousands(a.info.h)};
        std::wstring det = L"  ·  " + (n == 1 ? TF(L"{0} Bild à {1} × {2}", dims) : TF(L"{0} Bilder à {1} × {2}", dims)) +
                           L"  ·  " + a.info.label;
        if (empty > 0) det += L"  ·  " + Thousands(empty) + (empty == 1 ? T(L" leeres Feld") : T(L" leere Felder"));
        DrawText(dc, det, D2D1::RectF(ox, oy, W_ - 16, oy + oh), of, t.text2);
        ox += r_.MeasureText(det, of) + 16;
        if (texW > 16384 || texH > 16384) {
            wchar_t wg[2] = {0xE7BA, 0};
            DrawText(dc, wg, D2D1::RectF(ox, oy, ox + 20, oy + oh), r_.Icon(13), t.text2);
            DrawText(dc, T(L"Größer als 16.384 px – nicht jedes Programm lädt so große Texturen."),
                     D2D1::RectF(ox + 24, oy, W_ - 16, oy + oh), of, t.text2);
        }
    }

    // --- Vorschau der Textur (aus den Vorschaubildern)
    const float top = toolH + optH + 16, bottom = H_ - 44;
    const float tileW = static_cast<float>(std::max<uint32_t>(1, a.info.w)), tileH = static_cast<float>(std::max<uint32_t>(1, a.info.h));
    float availW = std::max(40.0f, W_ - 64), availH = std::max(40.0f, bottom - top);
    float k = std::min({availW / (cols * tileW), availH / (rows * tileH), 8.0f});
    float pw = cols * tileW * k, ph = rows * tileH * k;
    D2D1_RECT_F tex = D2D1::RectF(std::floor((W_ - pw) / 2), std::floor(top + (availH - ph) / 2), 0, 0);
    tex.right = tex.left + pw;
    tex.bottom = tex.top + ph;
    a.texRc = tex;
    // Hintergrund: Schachbrett bei Transparenz, sonst schwarz (leere Felder werden so gespeichert)
    if (a.info.alpha || !valid) {
        ComPtr<ID2D1Bitmap> cb;
        const uint32_t px[4] = {dark_ ? 0xFF3A3A3Au : 0xFFFFFFFFu, dark_ ? 0xFF2A2A2Au : 0xFFE6E6E6u,
                                dark_ ? 0xFF2A2A2Au : 0xFFE6E6E6u, dark_ ? 0xFF3A3A3Au : 0xFFFFFFFFu};
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1BitmapBrush> brush;
        ID2D1RenderTarget* rt = dc;
        if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(2, 2), px, 8, bp, &cb)) &&
            SUCCEEDED(rt->CreateBitmapBrush(cb.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                                  D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                            &brush))) {
            brush->SetTransform(D2D1::Matrix3x2F::Scale(8, 8) * D2D1::Matrix3x2F::Translation(tex.left, tex.top));
            dc->FillRectangle(tex, brush.Get());
        }
    } else {
        dc->FillRectangle(tex, r_.Brush(D2D1::ColorF(0, 0, 0, 1)));
    }
    D2D1_RECT_F hoverRc{};
    for (int i = 0; i < n; ++i) {
        int c = i % cols, r = i / cols;
        D2D1_RECT_F rc = D2D1::RectF(tex.left + c * tileW * k, tex.top + r * tileH * k, tex.left + (c + 1) * tileW * k,
                                     tex.top + (r + 1) * tileH * k);
        hits_.push_back({rc, HitType::AtlasTile, i});
        const FileEntry& e = a.items[i];
        EnsureThumb(e, 1, frame_);
        Bmp& b = bmps_[e.path];
        b.lastUse = frame_;
        if (b.bmp) {
            // Pixelgrafik (Vorschaubild in Originalgroesse) beim Vergroessern scharf zeigen
            D2D1_SIZE_U bs = b.bmp->GetPixelSize();
            float dpx = (rc.right - rc.left) * r_.scale();
            bool sharp = bs.width >= a.info.w && dpx > bs.width;
            dc->DrawBitmap(b.bmp.Get(), rc, 1.0f,
                           sharp ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        if (a.hover == i) hoverRc = rc;
    }
    dc->DrawRectangle(D2D1::RectF(tex.left - 0.5f, tex.top - 0.5f, tex.right + 0.5f, tex.bottom + 0.5f), r_.Brush(t.stroke), 1.0f);
    if (a.hover >= 0 && a.hover < n) {
        dc->DrawRectangle(hoverRc, r_.Brush(accent_), 2.0f);
        std::wstring label = TF(L"Nr. {0}", {Thousands(a.hover + 1)}) + L"  ·  " + a.items[a.hover].name + L"  ·  " +
                             TF(L"Spalte {0}, Zeile {1}", {std::to_wstring(a.hover % cols + 1), std::to_wstring(a.hover / cols + 1)}) +
                             L"  ·  x " +
                             Thousands(static_cast<uint64_t>(a.hover % cols) * a.info.w) + L", y " +
                             Thousands(static_cast<uint64_t>(a.hover / cols) * a.info.h);
        DrawText(dc, label, D2D1::RectF(16, H_ - 40, W_ - 16, H_ - 8),
                 r_.Text(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), t.text2);
    }
}

} // namespace peek
