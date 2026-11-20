// HEIF/HEIC: Dateikopf lesen, ohne die Bilddaten zu dekodieren (das uebernimmt der HEIF-Codec von Windows).
// Liefert Groesse, Drehung/Spiegelung des Hauptbildes (irot/imir) und die Lage des EXIF-Blocks.
// Dazu ein kleiner EXIF-Leser fuer die Angaben im Info-Bereich und das Aufnahmedatum.
// Plattformunabhaengig, ohne Speicheranforderungen aus Dateiangaben: robust gegen beschaedigte Dateien.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace peek {

struct HeifInfo {
    uint32_t width = 0, height = 0;   // Groesse des Hauptbildes vor Drehung (ispe), 0 = unbekannt
    int orientation = 1;              // Drehung/Spiegelung als EXIF-Ausrichtung 1..8 (aus irot/imir)
    uint64_t exifOffset = 0;          // Lage des EXIF-Eintrags in der Datei (beginnt mit 4 Byte Abstand zum
    uint64_t exifLength = 0;          // TIFF-Kopf, siehe HeifExifParse); Laenge 0 = keiner
    uint64_t metaEnd = 0;             // Ende des meta-Kastens (so viel Kopf wird zum Lesen gebraucht)
};

// data: Dateianfang (mindestens bis zum Ende des meta-Kastens). Gibt false zurueck, wenn kein HEIF-Kopf
// gefunden wurde; ist meta laenger als data, steht in info.metaEnd die benoetigte Laenge.
bool HeifParse(const uint8_t* data, size_t size, HeifInfo& info);

// Zusammensetzung von Drehung (irot, angle * 90 Grad gegen den Uhrzeigersinn) und Spiegelung (imir,
// axis 0 = an der senkrechten Achse, 1 = an der waagerechten; mirrorAxis < 0 = keine) in dieser
// Reihenfolge, als EXIF-Ausrichtung 1..8
int HeifOrientation(int angle, int mirrorAxis, bool mirrorFirst);

struct ExifInfo {
    std::string make, model, lens;
    std::string dateTaken;            // "JJJJ:MM:TT hh:mm:ss" (DateTimeOriginal, sonst DateTime)
    double fnumber = 0, exposure = 0;
    uint32_t iso = 0;
    int orientation = 0;              // 0 = nicht angegeben
};

// data: EXIF-Block ab dem TIFF-Kopf ("II*\0" oder "MM\0*")
bool ExifParse(const uint8_t* data, size_t size, ExifInfo& out);
// EXIF-Eintrag einer HEIF-Datei (mit vorangestelltem Abstand zum TIFF-Kopf)
bool HeifExifParse(const uint8_t* item, size_t size, ExifInfo& out);

}  // namespace peek
