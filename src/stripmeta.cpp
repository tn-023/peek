// Metadaten entfernen (Info-Bereich): Rueckfrage, dann die Datei ersetzen oder unter neuem Namen speichern.
// Die Bilddaten bleiben unveraendert (core/metastrip.h).
#include "app.h"
#include "core/metastrip.h"
#include "core/textutil.h"

#include <commctrl.h>
#include <cwctype>
#include <shobjidl.h>

namespace peek {

namespace {

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30);
    if (ok) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD got = 0;
            DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1 << 24));
            ok = ReadFile(f, out.data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(f);
    return ok;
}

bool WriteWhole(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE f = CreateFileW(LongPath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size()) {
        DWORD put = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1 << 24));
        ok = WriteFile(f, data.data() + done, want, &put, nullptr) && put == want;
        done += put;
    }
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (!ok) DeleteFileW(LongPath(path).c_str());
    return ok;
}

}  // namespace

bool App::CanStripMetadata(const FileEntry& e) const {
    return e.kind == Kind::Image && !e.offline && IsLocalDrivePath(e.path) && StripSupportedExt(e.ext);
}

void App::RemoveMetadata(uint32_t index) {
    if (index >= entries_.size()) return;
    const FileEntry e = entries_[index];
    // Meldung: in der Statusleiste bzw. im Bildbetrachter als kurzer Hinweis
    auto notify = [&](const std::wstring& msg) {
        SetStatus(msg);
        if (viewer_.open) {
            viewer_.toast = msg;
            viewer_.toastUntil = GetTickCount() + 2600;
            SetTimer(hwnd_, kTimerToast, 2700, nullptr);
        }
    };
    if (!CanStripMetadata(e)) return;

    // Rueckfrage: Abbrechen, Speichern (ersetzt die Datei), Speichern unter …
    const std::wstring save = T(L"Speichern"), saveAs = T(L"Speichern unter …"), cancel = T(L"Abbrechen");
    const std::wstring title = T(L"Alle Metadaten entfernen?");
    const std::wstring text = TF(L"„{0}“ – Aufnahmedaten, Kamera, Aufnahmeort, Beschreibungen und weitere Angaben werden "
                                 L"entfernt. Das Bild selbst bleibt unverändert.", {e.name});
    TASKDIALOG_BUTTON buttons[] = {{1001, save.c_str()}, {1002, saveAs.c_str()}, {IDCANCEL, cancel.c_str()}};
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = L"peek";
    cfg.pszMainInstruction = title.c_str();
    cfg.pszContent = text.c_str();
    cfg.cButtons = ARRAYSIZE(buttons);
    cfg.pButtons = buttons;
    cfg.nDefaultButton = IDCANCEL;
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&cfg, &choice, nullptr, nullptr)) || (choice != 1001 && choice != 1002)) return;

    std::wstring target = e.path;
    if (choice == 1002) {
        target.clear();
        std::wstring stem = e.name, ext;
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) {
            ext = stem.substr(dot + 1);
            stem.resize(dot);
        }
        ComPtr<IFileSaveDialog> dlg;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            std::wstring up = ext;
            for (auto& ch : up) ch = static_cast<wchar_t>(towupper(ch));
            std::wstring pattern = L"*." + ext, label = TF(L"{0}-Bild", {up});
            COMDLG_FILTERSPEC spec[] = {{label.c_str(), pattern.c_str()}};
            dlg->SetFileTypes(1, spec);
            dlg->SetDefaultExtension(ext.c_str());
            dlg->SetFileName(TF(L"{0} (ohne Metadaten)", {stem}).append(L"." + ext).c_str());
            dlg->SetTitle(T(L"Ohne Metadaten speichern"));
            DWORD opt = 0;
            dlg->GetOptions(&opt);
            dlg->SetOptions(opt | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
            ComPtr<IShellItem> fi;
            if (SUCCEEDED(SHCreateItemFromParsingName(ParentPath(e.path).c_str(), nullptr, IID_PPV_ARGS(&fi))))
                dlg->SetFolder(fi.Get());
            if (SUCCEEDED(dlg->Show(hwnd_))) {
                ComPtr<IShellItem> res;
                PWSTR p = nullptr;
                if (SUCCEEDED(dlg->GetResult(&res)) && SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    target = p;
                    CoTaskMemFree(p);
                }
            }
        }
        if (target.empty()) return;
    }

    std::vector<uint8_t> data, clean;
    if (!ReadWhole(e.path, data)) {
        notify(T(L"Die Datei ließ sich nicht lesen."));
        return;
    }
    if (!StripMetadata(data.data(), data.size(), clean)) {
        notify(T(L"Metadaten ließen sich nicht entfernen: Aufbau der Datei nicht erkannt."));
        return;
    }
    const bool replace = _wcsicmp(target.c_str(), e.path.c_str()) == 0;
    if (replace) {
        // erst vollstaendig daneben schreiben, dann ersetzen (Erstelldatum und Rechte der Datei bleiben)
        std::wstring tmp = e.path + L".peek-neu";   // lang:ok
        if (!WriteWhole(tmp, clean)) {
            notify(T(L"Speichern nicht möglich."));
            return;
        }
        if (!ReplaceFileW(LongPath(e.path).c_str(), LongPath(tmp).c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) &&
            !MoveFileExW(LongPath(tmp).c_str(), LongPath(e.path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(LongPath(tmp).c_str());
            notify(T(L"Speichern nicht möglich."));
            return;
        }
        db_.Forget({e.path});
        notify(T(L"Metadaten entfernt."));
    } else {
        if (!WriteWhole(target, clean)) {
            notify(T(L"Speichern nicht möglich."));
            return;
        }
        notify(TF(L"Ohne Metadaten gespeichert: „{0}“", {FileNameOf(target)}));
    }
    // neu einlesen (in Sammlungen/Merkliste wuerde das den Bildbetrachter schliessen; dort folgt es spaeter)
    if (!viewer_.open || (!collectionsView_ && !bookmarksView_)) QuietReload();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace peek
