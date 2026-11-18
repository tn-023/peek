#pragma once
// Kacheltextur: PNG-Sequenz pixelgenau zu einer einzigen PNG-Datei zusammensetzen.
// Die Pixel werden nur kopiert (keine Skalierung, keine Farbumrechnung, kein vormultipliziertes Alpha).
#include "common.h"

namespace peek {

struct AtlasInfo {
    uint32_t w = 0, h = 0;      // Groesse eines Einzelbildes
    GUID format{};              // Pixelformat der Textur (WIC)
    uint32_t bpp = 0;           // Bits pro Pixel
    bool alpha = false;
    std::wstring label;         // z. B. "RGBA 8 Bit"
};

// Prueft die Sequenz (nur Dateikoepfe): alle Bilder gleich gross und im selben Pixelformat.
// Palettenbilder werden als RGBA 8 Bit uebernommen (Farben unveraendert), Graustufen unter 8 Bit als 8 Bit.
bool InspectAtlas(const std::vector<std::wstring>& files, AtlasInfo& info, std::wstring& err);

// Schreibt die Textur (Reihenfolge = files, zeilenweise). Leere Felder bleiben 0 (transparent bzw. schwarz).
// progress(0..1) liefert false zum Abbrechen. COM muss im aufrufenden Thread initialisiert sein.
bool WriteAtlas(const std::vector<std::wstring>& files, const AtlasInfo& info, int cols, int rows,
                const std::wstring& output, const std::function<bool(float)>& progress, std::wstring& err);

} // namespace peek
