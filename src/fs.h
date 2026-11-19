#pragma once
// Dateisystem: Eintraege, Klassifizierung, schnelle Aufzaehlung, nur lokale Datentraeger.
#include "common.h"

namespace peek {

// Dokumente mit Vorschaubild (oeffnen in der Standard-App): 1 PDF, 2 Photoshop, 3 Illustrator, sonst 0
inline int DocType(std::wstring_view e) {
    if (e == L"pdf") return 1;
    if (e == L"psd" || e == L"psb") return 2;
    if (e == L"ai") return 3;
    return 0;
}

struct FileEntry {
    std::wstring path;         // vollstaendiger Pfad
    std::wstring name;         // Dateiname inkl. Endung
    std::wstring nameLower;    // klein + gefaltet (Suche)
    std::wstring folderLower;  // Ordnerpfad klein + gefaltet (Suche)
    std::wstring ext;          // Endung klein, ohne Punkt
    Kind kind = Kind::Other;
    uint64_t size = 0;
    int64_t mtime = 0, ctime = 0;  // Unix-Sekunden
    uint32_t attrs = 0;
    bool offline = false;          // Cloud-Platzhalter: nie lesen
    bool gone = false;             // gerade in den Papierkorb gelegt (bis zum Neueinlesen ausgeblendet)

    // Metadaten
    bool metaDone = false;
    uint32_t width = 0, height = 0, durationMs = 0;
    int64_t taken = 0;
    std::wstring camera, cameraLower;
    uint32_t pages = 0;             // PDF/AI: Seitenzahl (in der Datenbank im Feld "dur" gespeichert)

    // Mustererkennung
    std::vector<uint32_t> tokens;
    uint32_t templateId = UINT32_MAX;
    std::wstring source;
    int64_t nameDate = 0;          // Datum aus dem Namen (Unix-Sekunden) oder 0

    // Ordner-Vorschaubild
    bool coverChecked = false;
    std::wstring coverPath;
    uint64_t coverSize = 0;
    int64_t coverMtime = 0;
    Kind coverKind = Kind::Other;

    int64_t Time() const { return taken ? taken : (nameDate ? nameDate : mtime); }
    bool IsMedia() const { return kind == Kind::Image || kind == Kind::Video; }
    int Doc() const { return kind == Kind::Other ? DocType(ext) : 0; }
    bool HasThumb() const { return IsMedia() || Doc() != 0; }   // Vorschaubild statt Dateisymbol
};

Kind ClassifyExt(std::wstring_view extLower);
bool IsViewerFormat(std::wstring_view extLower);   // JPEG/JPG, PNG, GIF, BMP, WebP, AVIF, TIFF, ICO, SVG

// Laufwerke mit Buchstaben: lokal (DRIVE_FIXED / DRIVE_REMOVABLE) und eingebundene Netzlaufwerke (DRIVE_REMOTE).
// Netzwerkpfade ohne Buchstaben (\\server\...) nicht. peek hat keinen eigenen Netzwerkcode: Netzlaufwerke liest Windows.
bool IsLocalDrivePath(const std::wstring& path);
bool IsNetworkPath(const std::wstring& path);        // Laufwerk ist ein eingebundenes Netzlaufwerk
std::vector<std::wstring> LocalDrives();             // alle unterstuetzten Laufwerke (auch Netzlaufwerke)
std::wstring NetworkDriveLabel(const std::wstring& root);   // "freigabe (\\server)" ohne Netzwerkzugriff, sonst leer
std::wstring KnownFolderPath(REFKNOWNFOLDERID id);

std::wstring ParentPath(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring LongPath(const std::wstring& path);   // \\?\-Praefix fuer lange Pfade
bool DirectoryExists(const std::wstring& path);
std::wstring NormalizeDir(std::wstring path);

FileEntry MakeEntry(const std::wstring& dir, const WIN32_FIND_DATAW& fd);

// Zaehlt einen Ordner auf (optional rekursiv). sink erhaelt Teilmengen; Abbruch, wenn cancel() true liefert.
// Ergebnis: true, wenn alle Ordner gelesen werden konnten (fehlende Rechte zaehlen nicht als Fehler). false z. B.,
// wenn ein Netzlaufwerk nicht erreichbar war: dann darf nichts aus dem Index entfernt werden.
bool EnumerateFolder(const std::wstring& dir, bool recursive, bool showHidden,
                     const std::function<bool()>& cancel,
                     const std::function<void(std::vector<FileEntry>&&)>& sink,
                     size_t batch = 2000);

std::wstring ExeDir();
std::wstring DataDir();     // peek-data neben der EXE (portabel) oder %LOCALAPPDATA%\peek
void OverrideDataDirForTest(const std::wstring& dir);   // nur Pruefmodus

} // namespace peek
