#pragma once
// Dekodierung von Bildern (WIC, libwebp), Video-Standbildern (Media Foundation) und Dateisymbolen.
// Alle Funktionen sind fuer Hintergrund-Threads gedacht (COM muss initialisiert sein).
#include "db.h"

namespace peek {

IWICImagingFactory* Wic();  // pro Thread

// Bild dekodieren, auf maxEdge (lange Kante) verkleinert, EXIF-Ausrichtung angewendet. maxEdge 0 = Originalgroesse.
bool DecodeImageFile(const std::wstring& path, uint32_t maxEdge, Pixels& out);

// Zuschnitt speichern: Quelle in Anzeigeausrichtung (EXIF + viewRot x 90 Grad im Uhrzeigersinn), Ausschnitt in
// relativen Koordinaten (0..1). Format nach container (JPEG mit Metadaten, Qualitaet 95 %, Ausrichtung danach normal).
bool SaveCroppedImage(const std::wstring& src, int viewRot, double x0, double y0, double x1, double y1,
                      const std::wstring& dst, const GUID& container, uint32_t* outW = nullptr, uint32_t* outH = nullptr);
bool DecodeImageMemory(const uint8_t* data, size_t size, Pixels& out);
// Eingebettetes Vorschaubild (EXIF bei JPEG/TIFF): liest nur den Dateikopf. Fuer Netzlaufwerke als erste, schnelle Stufe.
bool DecodeEmbeddedThumb(const std::wstring& path, uint32_t maxEdge, Pixels& out);
// Aufnahmedaten fuer den Info-Bereich (EXIF): Objektiv, Blende, Belichtungszeit (s), ISO als Text mit
// Trennzeichen U+001F, Zahlen unformatiert ("2.8", "0.004", "400"); fehlende Angaben leer
bool ReadPhotoDetails(const std::wstring& path, std::wstring& out);
// Ist ein HEIF-Codec in Windows eingetragen? (nur die Liste der Codecs, keine Datei; einmal ermittelt)
bool HeifCodecInstalled();

// JPEG fuer PDF: Transparenz auf Weiss, Qualitaet 0..1
bool EncodeJpegOnWhite(const Pixels& px, float quality, std::vector<uint8_t>& out);

// Vorschaubild speichern: JPEG (ohne Transparenz) oder PNG (mit Transparenz)
bool EncodeThumb(const Pixels& px, std::vector<uint8_t>& out);

bool ReadImageMeta(const std::wstring& path, MetaRow& m);

// Ein Standbild bei ca. 10 % der Laufzeit; liefert optional Dauer/Aufloesung
bool VideoThumbnail(const std::wstring& path, uint32_t maxEdge, Pixels& out, MetaRow* meta);
bool ReadVideoMeta(const std::wstring& path, MetaRow& m);

// Dateitypsymbol (ohne den Dateiinhalt zu lesen, ausser bei perFile = true)
bool ShellIcon(const std::wstring& pathOrName, bool isDir, bool perFile, Pixels& out);

// Groesste Bildflaeche, die peek in voller Aufloesung dekodiert (16.384 x 16.384 Pixel = 1 GB).
// Schuetzt vor Dateien, die im Kopf riesige Abmessungen angeben.
constexpr uint64_t kMaxDecodePixels = 16384ull * 16384;

// Fuehrt fn aus und faengt sichere Fehler in Codecs ab (C++-Ausnahmen, Nullzeiger, Lesefehler).
// Andere Zugriffsverletzungen werden nicht abgefangen (siehe imaging.cpp). Liefert false bei Fehler.
bool GuardedCall(const std::function<bool()>& fn);

// Verkleinert Pixel auf maxEdge (Fant-Filter)
bool ScalePixels(const Pixels& in, uint32_t maxEdge, Pixels& out);

} // namespace peek
