#pragma once
// Kacheltextur: Aufteilung einer Bildsequenz in Spalten und Zeilen (plattformunabhaengig, mit Tests).
#include <cstdint>

namespace peek {

// Zeilen fuer n Bilder bei cols Spalten
int AtlasRows(int n, int cols);
// Spaltenzahl, bei der die Textur (cols*w x rows*h) moeglichst quadratisch wird; bei Gleichstand weniger leere Felder
int AtlasAutoCols(int n, uint32_t w, uint32_t h);
// Zeilen um eins mehr/weniger: passende Spaltenzahl (die Zeilen ergeben sich daraus)
int AtlasColsForMoreRows(int n, int cols);
int AtlasColsForFewerRows(int n, int cols);

} // namespace peek
