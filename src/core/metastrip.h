#pragma once
// Metadaten entfernen, ohne die Bilddaten neu zu kodieren (verlustfrei): JPEG, PNG, WebP.
// Entfernt werden EXIF, XMP, IPTC, Kommentare und Textbloecke (auch Angaben zur Bilderzeugung), Zeitstempel und
// Aufloesungsangaben. Erhalten bleibt, was die Darstellung bestimmt: Farbprofil, Gamma, Transparenz, Animation.
// JPEG: Eine Drehung aus dem EXIF bleibt als einziger Eintrag erhalten, sonst laege das Bild danach quer.
#include <cstdint>
#include <string>
#include <vector>

namespace peek {

enum class StripFormat { None, Jpeg, Png, WebP };

// Format anhand des Dateikopfs
StripFormat DetectStripFormat(const uint8_t* data, size_t size);
// Endung (klein, ohne Punkt) eines Formats, das sich bearbeiten laesst
bool StripSupportedExt(const std::wstring& ext);

// false bei unbekanntem oder beschaedigtem Aufbau (out bleibt dann leer)
bool StripMetadata(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

} // namespace peek
