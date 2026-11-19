#pragma once
// Abgeschottete Dekodierung fremder Dateien.
//
// Bilder, Videos und Dokumente werden nicht im Hauptprozess dekodiert, sondern in Hilfsprozessen
// ("peek.exe --decoder-worker"). Diese laufen in einer AppContainer-Sandbox ohne Faehigkeiten: kein Netzwerk,
// kein Zugriff auf Dateien des Benutzers, keine Kindprozesse, in einem Job-Objekt mit Speichergrenze. Der
// Hauptprozess oeffnet die Datei selbst und reicht dem Worker nur das Lese-Handle weiter; die Pixel kommen ueber
// gemeinsamen Speicher zurueck. Stuerzt ein Worker ab oder haengt er, wird er beendet und neu gestartet.
//
// Klappt AppContainer auf einem PC nicht, laeuft der Worker mit niedriger Integritaetsstufe (Low Integrity);
// klappt auch das nicht, dekodiert peek wie frueher im eigenen Prozess.
#include "db.h"

namespace peek {

enum class SandboxMode { Pending, AppContainer, LowIntegrity, InProcess };

// Hoechstzahl gleichzeitiger Worker (vor der ersten Anfrage aufrufen)
void InitDecoderSandbox(int maxWorkers);
// Startet einen ersten Worker im Hintergrund (bestimmt den Modus frueh)
void PrewarmDecoderSandbox();
// Beendet alle Worker und entfernt das AppContainer-Profil (nach dem Anhalten aller Dekoder-Threads aufrufen)
void ShutdownDecoderSandbox();
SandboxMode DecoderSandboxMode();
const wchar_t* DecoderSandboxText();   // fuer "Ueber peek"

// Wie die gleichnamigen Funktionen in imaging.h, docimaging.h und svgimaging.h - nur abgeschottet
bool SbDecodeImage(const std::wstring& path, uint32_t maxEdge, Pixels& out);
bool SbEmbeddedThumb(const std::wstring& path, uint32_t maxEdge, Pixels& out);   // nur eingebettetes Vorschaubild (Dateikopf)
bool SbSvgRegion(const std::wstring& path, float k, float tx, float ty, uint32_t w, uint32_t h, Pixels& out);
bool SbVideoThumbnail(const std::wstring& path, uint32_t maxEdge, Pixels& out, MetaRow* meta);
bool SbDocThumb(const std::wstring& path, uint32_t edge, Pixels& out, MetaRow* meta);
bool SbImageMeta(const std::wstring& path, MetaRow& m);
bool SbVideoMeta(const std::wstring& path, MetaRow& m);
bool SbDocMeta(const std::wstring& path, MetaRow& m);

// Einstieg des Worker-Prozesses (main.cpp): liest Auftraege aus der geerbten Pipe
int RunDecoderWorker(HANDLE pipe);

// Pruefmodus fuer die CI: dekodiert alle Dateien eines Ordners abgeschottet und im Prozess, vergleicht und prueft,
// dass der Worker keine Datei ueber ihren Pfad oeffnen kann. Bericht als UTF-8-Text. 0 = alles in Ordnung.
int RunSandboxTest(const std::wstring& folder, const std::wstring& report);

} // namespace peek
