#pragma once
// Seitenberechnung fuer den Kontaktabzug (A4, Masse in Punkt, Ursprung oben links).
// Wird von der Vorschau und vom PDF-Schreiber gleichermassen benutzt (WYSIWYG).
#include <functional>
#include <string>
#include <vector>

namespace peek {

struct SheetRect {
    float x = 0, y = 0, w = 0, h = 0;
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
};

struct SheetOptions {
    bool landscape = false;
    int cols = 4, rows = 5;
    bool names = true;          // Dateinamen unter den Bildern
    float lineFactor = 1.33f;   // Zeilenhoehe je Schriftgroesse (Ascent + Descent der Schrift)
};

struct SheetGeometry {
    float pageW = 0, pageH = 0;
    SheetRect header, footer;           // Ueberschrift oben, "Seite x von y" unten (je zentriert)
    std::vector<SheetRect> cells;       // Zeilenweise: Bildflaeche (ohne Bildunterschrift)
    std::vector<SheetRect> captions;    // Bildunterschriften (leer, wenn names aus)
    int PerPage() const { return static_cast<int>(cells.size()); }
};

constexpr float kSheetMm = 72.0f / 25.4f;
constexpr float kSheetMargin = 12 * kSheetMm, kSheetGap = 4 * kSheetMm, kSheetHeadGap = 5 * kSheetMm;
constexpr float kSheetHeadSize = 14, kSheetFootSize = 9, kSheetCaptionSize = 8;

SheetGeometry ComputeSheet(const SheetOptions& o);
// Bild mit Seitenverhaeltnis aw:ah in box einpassen, waagerecht zentriert; valign 0 oben, 0.5 Mitte, 1 unten
SheetRect FitInto(const SheetRect& box, float aw, float ah, float valign = 0.5f);
int SheetPageCount(int images, int perPage);
// Text so kuerzen, dass er in maxW passt: Mitte wird durch "…" ersetzt (Anfang und Ende bleiben sichtbar)
std::wstring MiddleEllipsis(const std::wstring& s, float maxW, const std::function<float(const std::wstring&)>& width);
// Text am Ende kuerzen ("…")
std::wstring EndEllipsis(const std::wstring& s, float maxW, const std::function<float(const std::wstring&)>& width);

} // namespace peek
