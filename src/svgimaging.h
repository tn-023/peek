#pragma once
// SVG mit resvg (statisch eingebunden): Vorschaubilder, Grundbild fuer den Betrachter und scharfe Ausschnitte
// in jeder Zoomstufe. Externe Verweise (Dateien, Netzwerk) werden vor dem Parsen entfernt.
#include "common.h"

namespace peek {

bool IsSvgPath(const std::wstring& path);

// Natuerliche Groesse (width/height bzw. viewBox) in px
bool SvgSize(const std::wstring& path, float& w, float& h);

// Ganzes Bild rendern: lange Kante = maxEdge (Vorschaubild). Ab 4096 (Betrachter): natuerliche Groesse,
// mindestens 2048 und hoechstens 4096 px. Pixel: BGRA, vormultipliziert. srcW/srcH = natuerliche Groesse.
bool DecodeSvg(const std::wstring& path, uint32_t maxEdge, Pixels& out);

// Ausschnitt rendern: Pixel (x, y) der Ausgabe = SVG-Punkt * k + (tx, ty)
bool RenderSvgRegion(const std::wstring& path, float k, float tx, float ty, uint32_t w, uint32_t h, Pixels& out);

} // namespace peek
