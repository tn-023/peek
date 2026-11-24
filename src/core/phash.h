#pragma once
// Bild-Fingerabdruck (dHash) und Gruppieren aehnlicher Bilder fuer die Duplikatsuche.
// dHash: Bild auf 9 x 8 Grauwerte verkleinern, je Zeile benachbarte Werte vergleichen -> 64 Bit. Aehnliche Bilder
// (verkleinert, neu gespeichert, leicht veraendert) haben Fingerabdruecke mit wenigen abweichenden Bits.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace peek {

// bgra: Pixel mit 4 Byte (B, G, R, A), Zeilenabstand stride Byte
uint64_t DHash(const uint8_t* bgra, uint32_t w, uint32_t h, size_t stride);

int HammingDistance(uint64_t a, uint64_t b);

// Fast einfarbige Bilder (Fingerabdruck fast nur 0 oder 1) taugen nicht fuer den Vergleich: alle schwarzen Bilder
// saehen gleich aus
bool DHashUsable(uint64_t h);

// Paare von Fingerabdruecken mit hoechstens maxDist abweichenden Bits (Kandidaten fuer die genaue Pruefung).
// Nicht brauchbare Fingerabdruecke (DHashUsable) bleiben aussen vor. Schnell auch fuer 100.000 Eintraege: nur
// Eintraege, die in einem Abschnitt der Bits uebereinstimmen, werden verglichen (bei maxDist Abweichungen und
// maxDist + 1 Abschnitten stimmt mindestens einer ueberein). Jedes Paar (i < j) genau einmal.
std::vector<std::pair<uint32_t, uint32_t>> CandidatePairs(const std::vector<uint64_t>& hashes, int maxDist);

// Kleine Farbfassung fuer die genaue Pruefung: 16 x 16 Zellen, je Zelle der Mittelwert von B, G, R
constexpr size_t kSmallRgbSize = 16 * 16 * 3;
void SmallRgb(const uint8_t* bgra, uint32_t w, uint32_t h, size_t stride, uint8_t out[kSmallRgbSize]);
// Visuell identisch: im Mittel kaum Abweichung und keine Zelle deutlich anders (trennt Serienbilder und
// Bearbeitungen wie Helligkeit, Farbe oder Retusche ab; Verkleinern und neu Speichern bleiben gleich)
bool SmallRgbSame(const uint8_t a[kSmallRgbSize], const uint8_t b[kSmallRgbSize]);

}  // namespace peek
