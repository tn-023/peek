#pragma once
// TGA (Truevision Targa): eigener, kleiner Dekoder ohne Windows-Abhaengigkeiten (getestet in tests/test_core.cpp).
// Unterstuetzt: unkomprimiert und RLE; Palette (8 Bit Index, Eintraege 15/16/24/32 Bit), Graustufen (8 Bit),
// Echtfarben (15/16/24/32 Bit); alle vier Ausrichtungen. Jede Laengenangabe wird gegen die Dateigroesse geprueft.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace peek {

struct TgaInfo {
    uint32_t width = 0, height = 0;
    bool alpha = false;
};

// Kopf lesen und auf Plausibilitaet pruefen (TGA hat keine Signatur)
bool TgaReadInfo(const uint8_t* data, size_t size, TgaInfo& info);

// Dekodiert nach BGRA mit vormultipliertem Alpha, Zeilen von oben nach unten.
// maxPixels begrenzt die Bildflaeche (Schutz vor riesigen Angaben im Kopf).
bool TgaDecode(const uint8_t* data, size_t size, uint64_t maxPixels, std::vector<uint8_t>& bgra, uint32_t& width,
               uint32_t& height);

} // namespace peek
