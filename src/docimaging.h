#pragma once
// Vorschaubilder fuer Dokumente: PDF (erste Seite, Windows.Data.Pdf), Illustrator (wie PDF, sonst eingebettetes
// XMP-Vorschaubild) und Photoshop (Gesamtbild per stb_image, sonst eingebettetes Vorschaubild).
// Fuer Hintergrund-Threads (COM initialisiert). Keine Netzwerkfunktionen.
#include "db.h"

namespace peek {

// Vorschaubild mit langer Kante edge. meta (optional): Seitenzahl in durationMs (PDF/AI), Abmessungen (PSD).
bool DecodeDocThumb(const std::wstring& path, uint32_t edge, Pixels& out, MetaRow* meta = nullptr);
// Nur Metadaten (Seitenzahl bzw. Abmessungen), ohne Vorschaubild
bool ReadDocMeta(const std::wstring& path, MetaRow& m);

} // namespace peek
