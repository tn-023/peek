// Kacheltextur: alle PNG-Dateien eines Ordners (Reihenfolge nach Dateinamen) pixelgenau zu einer PNG-Datei
// zusammensetzen. Fenster mit Vorschau, Spalten/Zeilen und finaler Texturgroesse. Zeichnen: App::PaintAtlas.
#include "app.h"
#include "core/atlaslayout.h"
#include "core/textutil.h"

#include <shobjidl.h>

namespace peek {

namespace {
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
std::wstring SafeName(std::wstring s) {
    for (auto& c : s)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'-';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s.empty() ? std::wstring(T(L"Kacheltextur")) : s;
}
}  // namespace

int App::AtlasRowCount() const { return AtlasRows(static_cast<int>(atlas_.items.size()), atlas_.cols); }

void App::OpenAtlas(const std::wstring& folder) {
    if (!DirectoryExists(folder)) return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    // nur PNG-Dateien direkt im Ordner (ohne Unterordner), natuerlich sortiert nach Dateinamen
    std::vector<FileEntry> found;
    EnumerateFolder(folder, false, settings_.showHidden, [] { return false; }, [&](std::vector<FileEntry>&& part) {
        for (auto& e : part)
            if (e.kind != Kind::Folder && e.ext == L"png" && !e.offline) found.push_back(std::move(e));
    });
    std::sort(found.begin(), found.end(),
              [](const FileEntry& a, const FileEntry& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    std::vector<std::wstring> files;
    for (auto& e : found) files.push_back(e.path);
    AtlasInfo info;
    std::wstring problem;
    InspectAtlas(files, info, problem);
    SetCursor(old);
    if (found.empty()) {
        SetStatus(T(L"Der Ordner enthält keine PNG-Dateien."));
        return;
    }

    if (viewer_.open) CloseViewer();
    atlas_.folder = folder;
    atlas_.items = std::move(found);
    atlas_.info = info;
    atlas_.problem = problem;
    atlas_.cols = AtlasAutoCols(static_cast<int>(atlas_.items.size()), info.w, info.h);
    atlas_.hover = -1;
    atlas_.open = true;
    searchFocus_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseAtlas() {
    if (atlas_.busy) {   // laufende Erstellung abbrechen
        atlas_.cancel = true;
        return;
    }
    atlas_.open = false;
    atlas_.items.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::AtlasMouseDown(float x, float y) {
    auto& a = atlas_;
    const Hit* h = HitAt(x, y);
    const int n = static_cast<int>(a.items.size());
    if (h && !a.busy) {
        switch (h->type) {
        case HitType::AtlasCancel: CloseAtlas(); break;
        case HitType::AtlasCreate: AtlasCreate(); break;
        case HitType::AtlasColsMinus: a.cols = std::max(1, a.cols - 1); break;
        case HitType::AtlasColsPlus: a.cols = std::min(std::max(1, n), a.cols + 1); break;
        case HitType::AtlasRowsMinus: a.cols = AtlasColsForFewerRows(n, a.cols); break;
        case HitType::AtlasRowsPlus: a.cols = AtlasColsForMoreRows(n, a.cols); break;
        default: break;
        }
    } else if (h && h->type == HitType::AtlasCancel) {
        CloseAtlas();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::AtlasMouseMove(float x, float y) {
    int hh = -1;
    for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i)
        if (hits_[i].type != HitType::None && Inside(hits_[i].rc, x, y)) { hh = i; break; }
    int tile = hh >= 0 && hits_[hh].type == HitType::AtlasTile ? hits_[hh].a : -1;
    if (hh != hoverHit_ || tile != atlas_.hover) {
        hoverHit_ = hh;
        atlas_.hover = tile;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool App::AtlasKey(WPARAM vk) {
    auto& a = atlas_;
    const int n = static_cast<int>(a.items.size());
    switch (vk) {
    case VK_ESCAPE: case VK_BACK: CloseAtlas(); return true;
    case VK_RETURN:
        if (GetKeyState(VK_CONTROL) < 0 && !a.busy) AtlasCreate();
        return true;
    case VK_LEFT: if (!a.busy) a.cols = std::max(1, a.cols - 1); break;
    case VK_RIGHT: if (!a.busy) a.cols = std::min(std::max(1, n), a.cols + 1); break;
    case VK_UP: if (!a.busy) a.cols = AtlasColsForFewerRows(n, a.cols); break;
    case VK_DOWN: if (!a.busy) a.cols = AtlasColsForMoreRows(n, a.cols); break;
    default: return false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void App::AtlasCreate() {
    auto& a = atlas_;
    if (a.busy || !a.problem.empty() || a.items.empty()) return;
    // Vorschlag: <Ordnername>.png im uebergeordneten Ordner (nicht in der Sequenz selbst)
    std::wstring folder = a.folder;
    while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
    std::wstring name = FileNameOf(folder);
    if (name.empty() || name.back() == L':') name = T(L"Kacheltextur");
    std::wstring dir = ParentPath(folder);
    if (dir.empty()) dir = folder;
    std::wstring out;
    ComPtr<IFileSaveDialog> dlg;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        COMDLG_FILTERSPEC spec[] = {{T(L"PNG-Bild"), L"*.png"}};
        dlg->SetFileTypes(1, spec);
        dlg->SetDefaultExtension(L"png");
        dlg->SetFileName((SafeName(name) + L".png").c_str());
        dlg->SetTitle(T(L"Kacheltextur speichern"));
        DWORD opt = 0;
        dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
        ComPtr<IShellItem> fi;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&fi)))) dlg->SetFolder(fi.Get());
        if (SUCCEEDED(dlg->Show(hwnd_))) {
            ComPtr<IShellItem> res;
            PWSTR p = nullptr;
            if (SUCCEEDED(dlg->GetResult(&res)) && SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                out = p;
                CoTaskMemFree(p);
            }
        }
    }
    if (out.empty()) return;
    if (!IsLocalDrivePath(out)) {
        SetStatus(T(L"Nur lokale Laufwerke werden unterstützt – Netzlaufwerke sind ausgeschlossen."));
        return;
    }
    for (auto& e : a.items)
        if (_wcsicmp(e.path.c_str(), out.c_str()) == 0) {
            SetStatus(T(L"Die Textur darf kein Bild der Sequenz überschreiben."));
            return;
        }

    std::vector<std::wstring> files;
    for (auto& e : a.items) files.push_back(e.path);
    if (atlasThread_.joinable()) atlasThread_.join();
    a.busy = true;
    a.ok = false;
    a.cancel = false;
    a.progress = 0;
    a.error.clear();
    a.savedPath = out;
    HWND hwnd = hwnd_;
    AtlasInfo info = a.info;
    int cols = a.cols, rows = AtlasRowCount();
    atlasThread_ = std::thread([this, files, info, cols, rows, out, hwnd] {
        ComScope com(COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::wstring err;
        DWORD last = 0;
        bool ok = WriteAtlas(files, info, cols, rows, out, [&](float f) {
            atlas_.progress = static_cast<int>(f * 1000);
            DWORD now = GetTickCount();
            if (now - last > 100) {
                last = now;
                PostMessageW(hwnd, WM_APP_ATLAS, 0, 0);
            }
            return !atlas_.cancel.load();
        }, err);
        atlas_.ok = ok;
        atlas_.error = err;
        PostMessageW(hwnd, WM_APP_ATLAS, 1, 0);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::AtlasDone() {
    auto& a = atlas_;
    if (atlasThread_.joinable()) atlasThread_.join();
    a.busy = false;
    if (a.ok) {
        a.open = false;
        a.items.clear();
        SetStatus(T(L"Kacheltextur gespeichert: ") + FileNameOf(a.savedPath));
    } else if (!a.cancel) {
        SetStatus(a.error.empty() ? std::wstring(T(L"Kacheltextur konnte nicht erstellt werden.")) : a.error);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

} // namespace peek
