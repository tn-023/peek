// Datums-Picker fuer die Filter "Datum" und "Aufgenommen" (Eintrag "Individuell"):
// Jahre > Monate > Tage. Ein Klick waehlt das Feld (und oeffnet die naechste Ebene),
// Ziehen ueber mehrere Felder waehlt einen Zeitraum.
#include "app.h"
#include "core/textutil.h"
#include "popup.h"
#include "renderer.h"

#include <algorithm>
#include <cmath>

namespace peek {

namespace {
const wchar_t* MonthLong(unsigned m) { return MonthName(m, false); }
const wchar_t* MonthShort(unsigned m) { return MonthName(m, true); }

unsigned DaysInMonth(int y, unsigned m) {
    int64_t a = DaysFromCivil(y, m, 1);
    int64_t b = m == 12 ? DaysFromCivil(y + 1, 1, 1) : DaysFromCivil(y, m + 1, 1);
    return static_cast<unsigned>(b - a);
}

std::wstring Thousands(uint64_t n) { return FormatThousands(n); }


constexpr float kPad = 8, kHeadH = 44, kRowH = 32;

struct DateCellInfo { int64_t from = 0, to = 0; std::wstring label; uint32_t count = 0; int y = 0; unsigned m = 0, d = 0; };
}  // namespace

int64_t App::EntryTimeFor(ChipId chip, const FileEntry& e) const {
    if (e.kind == Kind::Folder) return 0;
    return chip == ChipId::Taken ? e.taken : e.Time();
}

// Zeitraum als Text in Ortszeit: "2024", "August 2024", "17.08.2024" oder "von – bis"
std::wstring App::SpanLabel(int64_t from, int64_t to) const {
    if (to <= from) return T(L"Individuell");
    int64_t d0 = (from + tzOffset_) / 86400, d1 = (to + tzOffset_) / 86400;   // [d0, d1)
    int y0, y1; unsigned m0, m1, a0, a1;
    CivilFromDays(d0, y0, m0, a0);
    CivilFromDays(d1 - 1, y1, m1, a1);
    bool fullYears = m0 == 1 && a0 == 1 && d1 == DaysFromCivil(y1 + 1, 1, 1);
    bool fullMonths = a0 == 1 && a1 == DaysInMonth(y1, m1);
    if (fullYears) return y0 == y1 ? std::to_wstring(y0) : std::to_wstring(y0) + L"–" + std::to_wstring(y1);
    if (fullMonths) {
        if (y0 == y1 && m0 == m1) return std::wstring(MonthLong(m0)) + L" " + std::to_wstring(y0);
        if (y0 == y1) return std::wstring(MonthShort(m0)) + L"–" + MonthShort(m1) + L" " + std::to_wstring(y0);
        return std::wstring(MonthShort(m0)) + L" " + std::to_wstring(y0) + L" – " + MonthShort(m1) + L" " +
               std::to_wstring(y1);
    }
    std::wstring s0 = FormatDay(y0, m0, a0);
    if (d1 - d0 == 1) return s0;
    return s0 + L" – " + FormatDay(y1, m1, a1);
}

void App::ApplyDateSpan(ChipId chip, int64_t from, int64_t to) {
    if (chip == ChipId::Taken) {
        filters_.taken = 7;
        filters_.takenFrom = from;
        filters_.takenTo = to;
    } else {
        filters_.date = 7;
        filters_.dateFrom = from;
        filters_.dateTo = to;
    }
    Refilter();
    UpdateWindow(hwnd_);   // Ansicht dahinter sofort aktualisieren
}

class DatePickerPanel : public PopupPanel {
public:
    DatePickerPanel(App& app, ChipId chip, const D2D1_COLOR_F& accent) : app_(app), chip_(chip), accent_(accent) {
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        year_ = lt.wYear;
        month_ = lt.wMonth;
        int cur = Current();
        if (cur == 7) {
            int64_t from = chip_ == ChipId::Taken ? app_.filters_.takenFrom : app_.filters_.dateFrom;
            int64_t to = chip_ == ChipId::Taken ? app_.filters_.takenTo : app_.filters_.dateTo;
            int y; unsigned m, d;
            CivilFromDays((from + app_.tzOffset_) / 86400, y, m, d);
            year_ = y;
            month_ = m;
            int64_t days = (to - from) / 86400;
            level_ = days > 31 ? (days > 366 ? 0 : 1) : 2;
        }
        Build();
    }

    D2D1_SIZE_F Measure(Renderer&) override {
        Layout();
        return D2D1::SizeF(width_, height_);
    }

    void Paint(Renderer& r, float, float) override {
        ID2D1DeviceContext* dc = r.dc();
        const D2D1_COLOR_F text = PopupText(), dim = PopupDim(), off = D2D1::ColorF(1, 1, 1, 0.28f);
        auto draw = [&](const std::wstring& s, const D2D1_RECT_F& rc, IDWriteTextFormat* f, const D2D1_COLOR_F& c) {
            if (f && !s.empty()) dc->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, rc, r.Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        auto hoverFill = [&](const D2D1_RECT_F& rc) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4), r.Brush(PopupHover()));
        };
        hits_.clear();

        // Kopf: zurueck, Titel, frueher/spaeter
        D2D1_RECT_F back = D2D1::RectF(kPad, 6, kPad + 32, kHeadH - 6);
        if (level_ > 0) {
            Add(back, 1, 0);
            if (hover_ == 1) hoverFill(back);
        }
        wchar_t gb[2] = {0xE72B, 0};
        draw(gb, back, r.Icon(12), level_ > 0 ? text : off);
        std::wstring title = level_ == 0 ? std::wstring(chip_ == ChipId::Taken ? T(L"Aufgenommen") : T(L"Datum"))
                             : level_ == 1 ? std::to_wstring(year_)
                                           : std::wstring(MonthLong(month_)) + L" " + std::to_wstring(year_);
        draw(title, D2D1::RectF(kPad + 40, 0, width_ - 84, kHeadH), r.Text(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), text);
        if (level_ > 0) {
            D2D1_RECT_F prev = D2D1::RectF(width_ - kPad - 68, 6, width_ - kPad - 36, kHeadH - 6);
            D2D1_RECT_F next = D2D1::RectF(width_ - kPad - 32, 6, width_ - kPad, kHeadH - 6);
            Add(prev, 2, 0);
            Add(next, 3, 0);
            if (hover_ == 2) hoverFill(prev);
            if (hover_ == 3) hoverFill(next);
            wchar_t gp[2] = {0xE70E, 0}, gn[2] = {0xE70D, 0};
            draw(gp, prev, r.Icon(11), text);
            draw(gn, next, r.Icon(11), text);
        }
        Line(dc, r, kHeadH);

        // Auswahl
        int cur = Current();
        int64_t selFrom = 0, selTo = 0;
        if (cur == 7) {
            selFrom = chip_ == ChipId::Taken ? app_.filters_.takenFrom : app_.filters_.dateFrom;
            selTo = chip_ == ChipId::Taken ? app_.filters_.takenTo : app_.filters_.dateTo;
        }
        int lo = -1, hi = -1;
        if (dragging_) { lo = std::min(dragStart_, dragCur_); hi = std::max(dragStart_, dragCur_); }

        float gy = gridTop_;
        if (level_ == 2) {
            for (int i = 0; i < 7; ++i)
                draw(WeekdayShort(static_cast<unsigned>(i)), D2D1::RectF(kPad + i * cw_, gy - 24, kPad + (i + 1) * cw_, gy),
                     r.Text(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), dim);
        }
        IDWriteTextFormat* fl = r.Text(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        IDWriteTextFormat* fc = r.Text(10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        for (size_t i = 0; i < cells_.size(); ++i) {
            const auto& c = cells_[i];
            D2D1_RECT_F rc = CellRect(static_cast<int>(i));
            Add(rc, 10, static_cast<int>(i));
            bool inDrag = lo >= 0 && static_cast<int>(i) >= lo && static_cast<int>(i) <= hi;
            bool inSel = !dragging_ && selTo > selFrom && c.from < selTo && c.to > selFrom;
            if (inDrag || inSel)
                dc->FillRoundedRectangle(D2D1::RoundedRect(rc, 4, 4),
                                         r.Brush(D2D1::ColorF(accent_.r, accent_.g, accent_.b, inDrag ? 0.7f : 0.55f)));
            else if (hover_ == 10 && hoverIdx_ == static_cast<int>(i))
                hoverFill(rc);
            bool empty = c.count == 0;
            if (level_ == 2) {
                draw(c.label, D2D1::RectF(rc.left, rc.top, rc.right, rc.bottom - 6), fl, empty ? off : text);
                if (!empty)
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((rc.left + rc.right) / 2, rc.bottom - 6), 1.8f, 1.8f),
                                    r.Brush(inDrag || inSel ? text : accent_));
            } else {
                draw(c.label, D2D1::RectF(rc.left, rc.top + 3, rc.right, rc.top + 23), fl, empty ? off : text);
                draw(empty ? std::wstring(L"\u2013") : Thousands(c.count), D2D1::RectF(rc.left, rc.top + 22, rc.right, rc.bottom - 2), fc,
                     empty ? off : dim);
            }
        }

        // Fuss: Hinweis bzw. Zuruecksetzen wie ein Menueeintrag
        float fy = height_ - kPad - kRowH * (cur != 0 ? 2 : 1);
        Line(dc, r, fy - 4);
        draw(T(L"Klicken oder ziehen f\u00fcr einen Zeitraum"), D2D1::RectF(kPad + 12, fy, width_ - kPad - 12, fy + kRowH),
             r.Text(12), dim);
        if (cur != 0) {
            D2D1_RECT_F rr = D2D1::RectF(kPad, fy + kRowH, width_ - kPad, fy + 2 * kRowH);
            Add(rr, 4, 0);
            if (hover_ == 4) hoverFill(rr);
            wchar_t gx[2] = {0xE894, 0};
            draw(gx, D2D1::RectF(rr.left + 8, rr.top, rr.left + 30, rr.bottom), r.Icon(13), text);
            draw(T(L"Zur\u00fccksetzen"), D2D1::RectF(rr.left + 32, rr.top, rr.right - 12, rr.bottom), r.Text(13), text);
        }
    }

    void OnMouse(UINT msg, float x, float y) override {
        int type = 0, idx = -1;
        HitAt(x, y, type, idx);
        if (msg == WM_MOUSEMOVE) {
            if (dragging_) {
                int cell = NearestCell(x, y);
                if (cell >= 0 && cell != dragCur_) { dragCur_ = cell; dirty = true; }
                return;
            }
            if (type != hover_ || idx != hoverIdx_) { hover_ = type; hoverIdx_ = idx; dirty = true; }
            return;
        }
        if (msg == WM_LBUTTONDOWN) {
            switch (type) {
            case 1: if (level_ > 0) --level_; Build(); break;
            case 2: case 3: Step(type == 2 ? -1 : 1); break;
            case 4:
                if (chip_ == ChipId::Taken) app_.filters_.taken = 0;
                else app_.filters_.date = 0;
                app_.Refilter();
                UpdateWindow(app_.hwnd_);
                break;
            case 10:
                dragging_ = true;
                dragStart_ = dragCur_ = idx;
                break;
            default: break;
            }
            dirty = true;
            return;
        }
        if (msg == WM_LBUTTONUP && dragging_) {
            dragging_ = false;
            int n = static_cast<int>(cells_.size());
            if (dragStart_ < 0 || dragStart_ >= n || dragCur_ < 0 || dragCur_ >= n) return;
            int lo = std::min(dragStart_, dragCur_), hi = std::max(dragStart_, dragCur_);
            DateCellInfo a = cells_[lo], b = cells_[hi];
            app_.ApplyDateSpan(chip_, a.from, b.to);
            if (lo == hi && level_ < 2) {
                // Klick auf Jahr/Monat: gewaehlt und eine Ebene tiefer
                if (level_ == 0) year_ = a.y;
                else month_ = a.m;
                ++level_;
            }
            Build();
            dirty = true;
        }
    }

private:
    struct HitRc { D2D1_RECT_F rc; int type; int a; };

    int Current() const { return chip_ == ChipId::Taken ? app_.filters_.taken : app_.filters_.date; }
    void Add(const D2D1_RECT_F& rc, int type, int a) { hits_.push_back({rc, type, a}); }
    void HitAt(float x, float y, int& type, int& a) const {
        type = 0; a = -1;
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (x >= it->rc.left && x < it->rc.right && y >= it->rc.top && y < it->rc.bottom) { type = it->type; a = it->a; return; }
    }
    int NearestCell(float x, float y) const {
        int best = -1;
        float bd = 1e9f;
        for (int i = 0; i < static_cast<int>(cells_.size()); ++i) {
            D2D1_RECT_F rc = CellRect(i);
            float dx = std::max({rc.left - x, 0.0f, x - rc.right}), dy = std::max({rc.top - y, 0.0f, y - rc.bottom});
            float d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
    void Line(ID2D1DeviceContext* dc, Renderer& r, float y) {
        float yy = std::round(y * r.scale()) / r.scale() + 0.5f;
        dc->DrawLine(D2D1::Point2F(8, yy), D2D1::Point2F(width_ - 8, yy), r.Brush(PopupLine()), 1.0f);
    }
    void Step(int dir) {
        if (level_ == 1) year_ += dir;
        else if (level_ == 2) {
            int m = static_cast<int>(month_) + dir;
            if (m < 1) { m = 12; --year_; }
            if (m > 12) { m = 1; ++year_; }
            month_ = static_cast<unsigned>(m);
        }
        Build();
    }
    void Layout() {
        cols_ = level_ == 2 ? 7 : 4;
        cw_ = level_ == 2 ? 40.0f : 70.0f;
        ch_ = level_ == 2 ? 36.0f : 46.0f;
        int slots = static_cast<int>(cells_.size()) + lead_;
        int rows = std::max(1, (slots + cols_ - 1) / cols_);
        gridTop_ = kHeadH + 6 + (level_ == 2 ? 24.0f : 0.0f);
        width_ = kPad * 2 + cols_ * cw_;
        height_ = gridTop_ + rows * ch_ + 6 + kRowH * (Current() != 0 ? 2 : 1) + kPad;
    }
    D2D1_RECT_F CellRect(int i) const {
        int slot = i + lead_;
        int col = slot % cols_, row = slot / cols_;
        return D2D1::RectF(kPad + col * cw_ + 1.5f, gridTop_ + row * ch_ + 1.5f, kPad + (col + 1) * cw_ - 1.5f,
                           gridTop_ + (row + 1) * ch_ - 1.5f);
    }
    void Build() {
        cells_.clear();
        lead_ = 0;
        const int64_t off = app_.tzOffset_;
        auto dayStart = [&](int y, unsigned m, unsigned d) { return DaysFromCivil(y, m, d) * 86400 - off; };
        if (level_ == 0) {
            SYSTEMTIME lt;
            GetLocalTime(&lt);
            int minY = lt.wYear, maxY = lt.wYear;
            for (auto& e : app_.entries_) {
                int64_t t = app_.EntryTimeFor(chip_, e);
                if (t <= 0) continue;
                int y; unsigned m, d;
                CivilFromDays((t + off) / 86400, y, m, d);
                if (y < 1900 || y > 2200) continue;
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
            minY = std::max(std::min(minY, maxY - 11), maxY - 59);
            for (int y = minY; y <= maxY; ++y) {
                DateCellInfo c;
                c.from = dayStart(y, 1, 1);
                c.to = dayStart(y + 1, 1, 1);
                c.label = std::to_wstring(y);
                c.y = y;
                cells_.push_back(c);
            }
        } else if (level_ == 1) {
            for (unsigned m = 1; m <= 12; ++m) {
                DateCellInfo c;
                c.from = dayStart(year_, m, 1);
                c.to = m == 12 ? dayStart(year_ + 1, 1, 1) : dayStart(year_, m + 1, 1);
                c.label = MonthShort(m);
                c.y = year_;
                c.m = m;
                cells_.push_back(c);
            }
        } else {
            int64_t first = DaysFromCivil(year_, month_, 1);
            lead_ = static_cast<int>(((first % 7) + 7 + 3) % 7);   // 1.1.1970 war ein Donnerstag; Montag = 0
            unsigned n = DaysInMonth(year_, month_);
            for (unsigned d = 1; d <= n; ++d) {
                DateCellInfo c;
                c.from = dayStart(year_, month_, d);
                c.to = c.from + 86400;
                c.label = std::to_wstring(d);
                c.y = year_;
                c.m = month_;
                c.d = d;
                cells_.push_back(c);
            }
        }
        // Anzahl der Elemente je Feld (alle anderen Filter beruecksichtigt)
        if (cells_.empty()) return;
        int64_t lo = cells_.front().from, hi = cells_.back().to;
        for (auto& e : app_.entries_) {
            int64_t t = app_.EntryTimeFor(chip_, e);
            if (t < lo || t >= hi || !app_.PassFilters(e, static_cast<int>(chip_))) continue;
            auto it = std::upper_bound(cells_.begin(), cells_.end(), t,
                                       [](int64_t v, const DateCellInfo& c) { return v < c.from; });
            if (it != cells_.begin()) {
                --it;
                if (t < it->to) ++it->count;
            }
        }
        hover_ = 0;
        hoverIdx_ = -1;
        dirty = true;
    }

    App& app_;
    ChipId chip_;
    D2D1_COLOR_F accent_;
    int level_ = 0, year_ = 2026;
    unsigned month_ = 1;
    std::vector<DateCellInfo> cells_;
    int lead_ = 0, cols_ = 4;
    float cw_ = 70, ch_ = 46, gridTop_ = 50, width_ = 300, height_ = 300;
    std::vector<HitRc> hits_;
    int hover_ = 0, hoverIdx_ = -1;
    bool dragging_ = false;
    int dragStart_ = -1, dragCur_ = -1;
};

void App::OpenDatePicker(ChipId chip, const D2D1_RECT_F& anchor) {
    ComputeDateBounds();
    float s = r_.scale();
    POINT pt = {static_cast<LONG>(anchor.left * s), static_cast<LONG>((anchor.bottom + 4) * s)};
    ClientToScreen(hwnd_, &pt);
    openMenu_ = static_cast<int>(chip);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    DatePickerPanel panel(*this, chip, accent_);
    ShowPopupPanel(hwnd_, panel, pt, accent_);
    openMenu_ = -1;
    lastMenu_ = static_cast<int>(chip);
    lastMenuClose_ = GetTickCount();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace peek
