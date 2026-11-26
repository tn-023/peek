#pragma once
// Modernes, dunkles Popup-Menue mit leichter Transparenz (Acrylic) fuer alle Dropdowns und Kontextmenues.
#include "common.h"

namespace peek {

struct MenuItem {
    std::wstring text;
    std::wstring right;      // rechts ausgerichtet (Anzahl, Tastenkuerzel)
    UINT id = 0;
    bool checked = false;
    bool disabled = false;
    bool separator = false;
    wchar_t glyph = 0;       // Symbol links (z. B. Stern)
    std::vector<uint8_t> iconPx;   // App-Symbol links (BGRA, vormultipliert), statt glyph
    UINT iconW = 0, iconH = 0;
    // Untermenue: liefert die Eintraege erst beim Aufklappen (ids muessen im ganzen Menue eindeutig sein)
    std::function<std::vector<MenuItem>()> submenu;

    static MenuItem Sep() {
        MenuItem m;
        m.separator = true;
        return m;
    }
};

class Renderer;

// Eigener Inhalt im selben Popup-Stil wie die Menues (dunkel, Acrylic, abgerundet), z. B. der Datums-Picker.
// Koordinaten in DIP relativ zur linken oberen Ecke des Popups.
class PopupPanel {
public:
    virtual ~PopupPanel() = default;
    virtual D2D1_SIZE_F Measure(Renderer& r) = 0;
    virtual void Paint(Renderer& r, float width, float height) = 0;   // Hintergrund ist bereits gezeichnet
    virtual void OnMouse(UINT msg, float x, float y) = 0;             // WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP
    virtual void OnKey(WPARAM) {}
    // Texteingabe (z. B. Farbwerte): Tab und Zeichen gehen an das Panel statt es zu schliessen
    virtual bool WantsText() const { return false; }
    virtual void OnChar(wchar_t) {}
    bool close = false;   // Popup schliessen
    bool dirty = true;    // neu messen und zeichnen
};

// Zeigt das Panel modal (bis close gesetzt wird, Esc oder Klick ausserhalb).
void ShowPopupPanel(HWND owner, PopupPanel& panel, POINT screen, const D2D1_COLOR_F& accent);

// Gemeinsame Farben der Popups
inline D2D1_COLOR_F PopupText() { return D2D1::ColorF(1, 1, 1, 0.92f); }
inline D2D1_COLOR_F PopupDim() { return D2D1::ColorF(1, 1, 1, 0.5f); }
inline D2D1_COLOR_F PopupDisabled() { return D2D1::ColorF(1, 1, 1, 0.36f); }
inline D2D1_COLOR_F PopupHover() { return D2D1::ColorF(1, 1, 1, 0.08f); }
inline D2D1_COLOR_F PopupLine() { return D2D1::ColorF(1, 1, 1, 0.08f); }

// Zeigt das Menue an einer Bildschirmposition (linke obere Ecke) und liefert die gewaehlte id oder 0.
UINT ShowPopupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screen, const D2D1_COLOR_F& accent);

} // namespace peek
