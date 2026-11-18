#pragma once
// Lesezugriff der Dekoder auf Eingabedateien.
//
// Im Hauptprozess oeffnet OpenInput die Datei ueber ihren Pfad. Im abgeschotteten Dekoder-Prozess (AppContainer)
// ist das nicht erlaubt: Dort uebergibt der Hauptprozess ein bereits geoeffnetes Handle, das fuer die Dauer eines
// Auftrags per ScopedInput hinterlegt wird; OpenInput liefert dann eine Kopie davon und sonst nichts.
//
// Weil sich Kopien eines Handles die Dateiposition teilen, lesen alle Funktionen mit festem Versatz (ReadAt) und
// der Datenstrom (InputStream) fuehrt seine eigene Position.
#include "common.h"

#include <objidl.h>

namespace peek {

// Datei zum Lesen oeffnen. INVALID_HANDLE_VALUE bei Fehler; der Aufrufer schliesst das Handle.
HANDLE OpenInput(const std::wstring& path);

// Ab Versatz offset lesen (unabhaengig von der Dateiposition). Liefert die gelesenen Bytes.
DWORD ReadAt(HANDLE h, uint64_t offset, void* buf, DWORD len);

// Anfang einer Datei (hoechstens maxBytes) bzw. die ganze Datei (bis maxBytes, sonst Fehler) lesen
bool ReadInputHead(const std::wstring& path, size_t maxBytes, std::vector<uint8_t>& out);
bool ReadInputAll(const std::wstring& path, size_t maxBytes, std::vector<uint8_t>& out, int64_t* mtime = nullptr);

// Nur lesbarer COM-Datenstrom ueber der Eingabedatei (fuer WIC, Media Foundation und Windows.Data.Pdf)
ComPtr<IStream> OpenInputStream(const std::wstring& path);

// Im Dekoder-Prozess: Pfadzugriffe sperren (OpenInput liefert nur noch hinterlegte Handles)
void SetInputSandboxed(bool on);
bool InputSandboxed();

// Hinterlegt fuer den aktuellen Thread ein Handle fuer path (nicht besitzend)
class ScopedInput {
public:
    ScopedInput(const std::wstring& path, HANDLE h);
    ~ScopedInput();
    ScopedInput(const ScopedInput&) = delete;
    ScopedInput& operator=(const ScopedInput&) = delete;
};

} // namespace peek
