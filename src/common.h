#pragma once
// Gemeinsame Windows-Includes und Hilfen fuer peek.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/query.h"

namespace peek {
// Hochaufloesende Zeit in Millisekunden (fuer gleichmaessige Animationen; GetTickCount hat nur ~16 ms Aufloesung)
inline double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}
}  // namespace peek

using Microsoft::WRL::ComPtr;

namespace peek {

// Fenster-Nachrichten von Hintergrund-Threads an den UI-Thread
constexpr UINT WM_APP_THUMB = WM_APP + 1;     // Vorschaubilder fertig
constexpr UINT WM_APP_META = WM_APP + 2;      // Metadaten fertig
constexpr UINT WM_APP_LOAD = WM_APP + 3;      // Ordnerinhalt geladen (Teil)
constexpr UINT WM_APP_FSCHANGE = WM_APP + 4;  // Aenderung im aktuellen Ordner
constexpr UINT WM_APP_INDEX = WM_APP + 5;     // Fortschritt des Indexers

// Pixelpuffer im Format 32bpp PBGRA (vormultipliertes Alpha), Zeilenabstand = w * 4.
struct Pixels {
    uint32_t w = 0, h = 0;
    uint32_t srcW = 0, srcH = 0;  // Originalgroesse des Bildes
    std::vector<uint8_t> data;
    bool Valid() const { return w > 0 && h > 0 && data.size() >= size_t(w) * h * 4; }
};

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);
int64_t FileTimeToUnix(const FILETIME& ft);
int64_t NowUnix();

// Hilfsklasse: COM fuer die Lebensdauer eines Threads initialisieren
struct ComScope {
    HRESULT hr;
    explicit ComScope(DWORD mode = COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE) {
        hr = CoInitializeEx(nullptr, mode);
    }
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

} // namespace peek
