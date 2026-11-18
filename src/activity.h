#pragma once
// Zeitpunkt der letzten Vordergrund-Aktivitaet (Eingaben, Vorschaubilder, Metadaten der Ansicht).
// Der Indexer pausiert, solange die Oberflaeche gerade arbeitet, damit sie fluessig bleibt.
#include <windows.h>
#include <atomic>

namespace peek {
inline std::atomic<DWORD> g_uiActivity{0};
inline void MarkUiActivity() { g_uiActivity.store(GetTickCount(), std::memory_order_relaxed); }
inline bool UiRecentlyActive(DWORD ms) { return GetTickCount() - g_uiActivity.load(std::memory_order_relaxed) < ms; }
} // namespace peek
