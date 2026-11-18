#include "core/sheetlayout.h"

#include <algorithm>

namespace peek {

SheetGeometry ComputeSheet(const SheetOptions& o) {
    SheetGeometry g;
    const float a4w = 595.2756f, a4h = 841.8898f;
    g.pageW = o.landscape ? a4h : a4w;
    g.pageH = o.landscape ? a4w : a4h;
    const float m = kSheetMargin;
    const float headH = kSheetHeadSize * o.lineFactor, footH = kSheetFootSize * o.lineFactor;
    g.header = {m, m, g.pageW - 2 * m, headH};
    g.footer = {m, g.pageH - m - footH, g.pageW - 2 * m, footH};
    int cols = std::clamp(o.cols, 1, 8), rows = std::clamp(o.rows, 1, 8);
    float top = m + headH + kSheetHeadGap, bottom = g.pageH - m - footH - kSheetHeadGap;
    float cw = (g.pageW - 2 * m - (cols - 1) * kSheetGap) / cols;
    float ch = (bottom - top - (rows - 1) * kSheetGap) / rows;
    float capH = o.names ? kSheetCaptionSize * o.lineFactor + 1.5f * kSheetMm : 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            float x = m + c * (cw + kSheetGap), y = top + r * (ch + kSheetGap);
            g.cells.push_back({x, y, cw, std::max(1.0f, ch - capH)});
            if (o.names) g.captions.push_back({x, y + ch - kSheetCaptionSize * o.lineFactor, cw, kSheetCaptionSize * o.lineFactor});
        }
    return g;
}

SheetRect FitInto(const SheetRect& box, float aw, float ah, float valign) {
    if (aw <= 0 || ah <= 0) return box;
    float s = std::min(box.w / aw, box.h / ah);
    float w = aw * s, h = ah * s;
    return {box.x + (box.w - w) / 2, box.y + (box.h - h) * valign, w, h};
}

int SheetPageCount(int images, int perPage) {
    if (perPage <= 0) return 1;
    return std::max(1, (images + perPage - 1) / perPage);
}

std::wstring MiddleEllipsis(const std::wstring& s, float maxW, const std::function<float(const std::wstring&)>& width) {
    if (width(s) <= maxW || s.size() < 3) return s;
    // gleich viele Zeichen vorne und hinten, schrittweise weniger
    size_t keep = s.size() - 1;
    while (keep > 1) {
        size_t front = (keep + 1) / 2, back = keep - front;
        std::wstring t = s.substr(0, front) + L"…" + s.substr(s.size() - back);
        if (width(t) <= maxW) return t;
        --keep;
    }
    return L"…";
}

std::wstring EndEllipsis(const std::wstring& s, float maxW, const std::function<float(const std::wstring&)>& width) {
    if (width(s) <= maxW) return s;
    for (size_t n = s.size(); n > 0; --n) {
        std::wstring t = s.substr(0, n) + L"…";
        if (width(t) <= maxW) return t;
    }
    return L"…";
}

} // namespace peek
