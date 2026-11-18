#pragma once
// Drag & Drop (rein und raus), Zwischenablage und Shell-Aktionen.
#include "common.h"

namespace peek {

class DropTarget : public IDropTarget {
public:
    // Liefert den Effekt fuer die aktuelle Position (leave = true: Ziehen hat das Fenster verlassen)
    using OverFn = std::function<DWORD(const std::vector<std::wstring>& paths, POINT screen, DWORD keys,
                                       DWORD allowed, bool leave)>;
    // Fuehrt das Ablegen aus und liefert den tatsaechlichen Effekt
    using DropFn = std::function<DWORD(const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD allowed)>;

    DropTarget(HWND hwnd, OverFn onOver, DropFn onDrop);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

private:
    virtual ~DropTarget() = default;
    LONG refs_ = 1;
    HWND hwnd_;
    OverFn onOver_;
    DropFn onDrop_;
    std::vector<std::wstring> paths_;
    DWORD allowed_ = 0;
    ComPtr<IDropTargetHelper> helper_;
};

std::vector<std::wstring> PathsFromDataObject(IDataObject* data);
// Kopiert oder verschiebt Dateien in einen Ordner (Shell-Dialoge, Rueckgaengig moeglich)
bool CopyOrMoveFiles(HWND hwnd, const std::vector<std::wstring>& paths, const std::wstring& target, bool move);
HRESULT CreateShellDataObject(const std::vector<std::wstring>& paths, IDataObject** out);
void StartFileDrag(HWND hwnd, const std::vector<std::wstring>& paths);
bool CopyFilesToClipboard(const std::vector<std::wstring>& paths);
bool CopyTextToClipboard(HWND hwnd, const std::wstring& text);
std::wstring PasteTextFromClipboard(HWND hwnd);
// Dateien aus der Zwischenablage (CF_HDROP); move = mit "Ausschneiden" kopiert
bool FilesFromClipboard(HWND hwnd, std::vector<std::wstring>& paths, bool& move);
bool RecycleFiles(HWND hwnd, const std::vector<std::wstring>& paths);
// Wird nach erfolgreichem Loeschen bzw. Verschieben mit den Quellpfaden aufgerufen (Index aufraeumen)
void SetFilesGoneHook(std::function<void(const std::vector<std::wstring>&)> fn);

// "Oeffnen mit": fuer die Dateiendung registrierte Apps (wie im Explorer), mit Symbol
struct OpenWithApp {
    std::wstring name;
    std::vector<uint8_t> iconPx;   // 32 x 32 BGRA vormultipliert (leer, wenn kein Symbol)
    UINT iconW = 0, iconH = 0;
    Microsoft::WRL::ComPtr<IAssocHandler> handler;
};
std::vector<OpenWithApp> GetOpenWithApps(const std::wstring& path);
bool OpenWithApp_Invoke(const OpenWithApp& app, const std::vector<std::wstring>& paths);
void ShowOpenWithDialog(HWND hwnd, const std::wstring& path);   // "Andere App auswaehlen"
// Symbol der Standard-App fuer eine Dateiendung (wie im Menue "Oeffnen mit"), 32 x 32
bool DefaultAppIcon(const std::wstring& ext, OpenWithApp& out);

void ShowInExplorer(const std::wstring& path);
bool OpenWithDefaultApp(HWND hwnd, const std::wstring& path);

} // namespace peek
