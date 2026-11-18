#include "dragdrop.h"
#include "fs.h"
#include "imaging.h"

namespace peek {

DropTarget::DropTarget(HWND hwnd, OverFn onOver, DropFn onDrop)
    : hwnd_(hwnd), onOver_(std::move(onOver)), onDrop_(std::move(onDrop)) {
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
}

HRESULT DropTarget::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *ppv = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG DropTarget::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }

ULONG DropTarget::Release() {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
}

std::vector<std::wstring> PathsFromDataObject(IDataObject* data) {
    std::vector<std::wstring> paths;
    if (!data) return paths;
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM med = {};
    if (FAILED(data->GetData(&fmt, &med))) return paths;
    if (auto drop = static_cast<HDROP>(GlobalLock(med.hGlobal))) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len, L'\0');
            DragQueryFileW(drop, i, s.data(), len + 1);
            paths.push_back(s);
        }
        GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return paths;
}

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    paths_ = PathsFromDataObject(data);
    allowed_ = *effect;
    POINT p{pt.x, pt.y};
    *effect = paths_.empty() ? DROPEFFECT_NONE : (onOver_(paths_, p, keys, allowed_, false) & allowed_);
    if (helper_) helper_->DragEnter(hwnd_, data, &p, *effect);
    return S_OK;
}

HRESULT DropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect) {
    POINT p{pt.x, pt.y};
    *effect = paths_.empty() ? DROPEFFECT_NONE : (onOver_(paths_, p, keys, allowed_, false) & allowed_);
    if (helper_) helper_->DragOver(&p, *effect);
    return S_OK;
}

HRESULT DropTarget::DragLeave() {
    if (helper_) helper_->DragLeave();
    onOver_(paths_, POINT{0, 0}, 0, 0, true);
    paths_.clear();
    return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    POINT p{pt.x, pt.y};
    if (paths_.empty()) paths_ = PathsFromDataObject(data);
    DWORD e = paths_.empty() ? DROPEFFECT_NONE : (onOver_(paths_, p, keys, allowed_, false) & allowed_);
    if (helper_) helper_->Drop(data, &p, e);
    onOver_(paths_, POINT{0, 0}, 0, 0, true);
    *effect = e ? onDrop_(paths_, p, keys, allowed_) : DROPEFFECT_NONE;
    paths_.clear();
    return S_OK;
}

namespace {
std::function<void(const std::vector<std::wstring>&)> g_filesGone;
}

void SetFilesGoneHook(std::function<void(const std::vector<std::wstring>&)> fn) { g_filesGone = std::move(fn); }

bool CopyOrMoveFiles(HWND hwnd, const std::vector<std::wstring>& paths, const std::wstring& target, bool move) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
    }
    bool ok = false;
    ComPtr<IShellItem> dest;
    if (!pidls.empty() && SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&dest)))) {
        ComPtr<IShellItemArray> arr;
        ComPtr<IFileOperation> op;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &arr)) &&
            SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
            op->SetOwnerWindow(hwnd);
            op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD);
            HRESULT hr = move ? op->MoveItems(arr.Get(), dest.Get()) : op->CopyItems(arr.Get(), dest.Get());
            if (SUCCEEDED(hr) && SUCCEEDED(op->PerformOperations())) {
                BOOL aborted = FALSE;
                op->GetAnyOperationsAborted(&aborted);
                ok = !aborted;
            }
        }
    }
    for (auto p : pidls) CoTaskMemFree(p);
    // Verschoben: die alten Pfade gibt es nicht mehr (bei Abbruch raeumt das naechste Einlesen auf)
    if (ok && move && g_filesGone) g_filesGone(paths);
    return ok;
}

HRESULT CreateShellDataObject(const std::vector<std::wstring>& paths, IDataObject** out) {
    *out = nullptr;
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
    }
    HRESULT hr = E_FAIL;
    if (!pidls.empty()) {
        ComPtr<IShellItemArray> arr;
        hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                               const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &arr);
        if (SUCCEEDED(hr)) hr = arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(out));
    }
    for (auto p : pidls) CoTaskMemFree(p);
    return hr;
}

void StartFileDrag(HWND hwnd, const std::vector<std::wstring>& paths) {
    ComPtr<IDataObject> data;
    if (FAILED(CreateShellDataObject(paths, &data))) return;
    DWORD effect = 0;
    SHDoDragDrop(hwnd, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
}

bool CopyFilesToClipboard(const std::vector<std::wstring>& paths) {
    ComPtr<IDataObject> data;
    if (FAILED(CreateShellDataObject(paths, &data))) return false;
    return SUCCEEDED(OleSetClipboard(data.Get()));
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

bool FilesFromClipboard(HWND hwnd, std::vector<std::wstring>& paths, bool& move) {
    paths.clear();
    move = false;
    if (!OpenClipboard(hwnd)) return false;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        if (HDROP drop = static_cast<HDROP>(GlobalLock(h))) {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring p(len, L'\0');
                DragQueryFileW(drop, i, p.data(), len + 1);
                if (!p.empty()) paths.push_back(p);
            }
            GlobalUnlock(h);
        }
    }
    static UINT fmt = RegisterClipboardFormatW(L"Preferred DropEffect");
    if (HANDLE h = GetClipboardData(fmt)) {
        if (auto e = static_cast<const DWORD*>(GlobalLock(h))) {
            move = (*e & DROPEFFECT_MOVE) != 0;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return !paths.empty();
}

std::wstring PasteTextFromClipboard(HWND hwnd) {
    std::wstring out;
    if (!OpenClipboard(hwnd)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

bool RecycleFiles(HWND hwnd, const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
    }
    bool ok = false;
    if (!pidls.empty()) {
        ComPtr<IShellItemArray> arr;
        ComPtr<IFileOperation> op;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &arr)) &&
            SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) {
            op->SetOwnerWindow(hwnd);
            op->SetOperationFlags(FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMMKDIR);
            if (SUCCEEDED(op->DeleteItems(arr.Get())) && SUCCEEDED(op->PerformOperations())) {
                BOOL aborted = FALSE;
                op->GetAnyOperationsAborted(&aborted);
                ok = !aborted;
            }
        }
    }
    for (auto p : pidls) CoTaskMemFree(p);
    if (ok && g_filesGone) g_filesGone(paths);   // Vorschaubilder und Index-Eintraege sofort entfernen
    return ok;
}

void ShowInExplorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
    }
}

bool OpenWithDefaultApp(HWND hwnd, const std::wstring& path) {
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = hwnd;
    sei.lpVerb = nullptr;   // Standardaktion (z. B. Wiedergabe im Standard-Mediaplayer)
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    // Keine Zuordnung: "Oeffnen mit"-Dialog
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"openas";
    return ShellExecuteExW(&sei) != FALSE;
}

// ---------------------------------------------------------------------------
// Oeffnen mit
// ---------------------------------------------------------------------------
namespace {
bool PixelsFromSource(IWICBitmapSource* src, OpenWithApp& out) {
    if (!src) return false;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmapSource> conv;
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    IWICBitmapSource* cur = src;
    if (w != 32 || h != 32) {
        if (FAILED(Wic()->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(src, 32, 32, WICBitmapInterpolationModeFant)))
            return false;
        cur = scaler.Get();
    }
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, cur, &conv))) return false;
    out.iconPx.assign(32 * 32 * 4, 0);
    if (FAILED(conv->CopyPixels(nullptr, 32 * 4, static_cast<UINT>(out.iconPx.size()), out.iconPx.data()))) {
        out.iconPx.clear();
        return false;
    }
    out.iconW = out.iconH = 32;
    return true;
}

void LoadHandlerIcon(IAssocHandler* h, OpenWithApp& out) {
    LPWSTR loc = nullptr;
    int index = 0;
    if (FAILED(h->GetIconLocation(&loc, &index)) || !loc) return;
    std::wstring path = loc;
    CoTaskMemFree(loc);
    if (!path.empty() && path[0] == L'@') {
        // Store-Apps: Ressourcenverweis -> Pfad zu einem PNG
        wchar_t buf[MAX_PATH * 2] = {};
        if (SUCCEEDED(SHLoadIndirectString(path.c_str(), buf, static_cast<UINT>(std::size(buf)), nullptr))) {
            ComPtr<IWICBitmapDecoder> dec;
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(Wic()->CreateDecoderFromFilename(buf, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
                SUCCEEDED(dec->GetFrame(0, &frame)))
                PixelsFromSource(frame.Get(), out);
        }
        return;
    }
    wchar_t exp[MAX_PATH * 2] = {};
    ExpandEnvironmentStringsW(path.c_str(), exp, static_cast<DWORD>(std::size(exp)));
    HICON large = nullptr;
    if (ExtractIconExW(exp, index, &large, nullptr, 1) > 0 && large) {
        ComPtr<IWICBitmap> bmp;
        if (SUCCEEDED(Wic()->CreateBitmapFromHICON(large, &bmp))) PixelsFromSource(bmp.Get(), out);
        DestroyIcon(large);
    }
}
}  // namespace

std::vector<OpenWithApp> GetOpenWithApps(const std::wstring& path) {
    std::vector<OpenWithApp> out;
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (!ext || !*ext) return out;
    ComPtr<IEnumAssocHandlers> en;
    if (FAILED(SHAssocEnumHandlers(ext, ASSOC_FILTER_RECOMMENDED, &en)) || !en) return out;
    ComPtr<IAssocHandler> h;
    ULONG got = 0;
    while (out.size() < 16 && en->Next(1, &h, &got) == S_OK && got == 1) {
        OpenWithApp a;
        LPWSTR name = nullptr;
        if (SUCCEEDED(h->GetUIName(&name)) && name) {
            a.name = name;
            CoTaskMemFree(name);
        }
        if (!a.name.empty()) {
            LoadHandlerIcon(h.Get(), a);
            a.handler = h;
            out.push_back(std::move(a));
        }
        h.Reset();
    }
    return out;
}

bool DefaultAppIcon(const std::wstring& extNoDot, OpenWithApp& out) {
    std::wstring ext = L"." + extNoDot;
    // Name der Standard-App, dann den passenden Eintrag der Liste "Oeffnen mit" suchen
    wchar_t name[512] = {};
    DWORD n = static_cast<DWORD>(std::size(name));
    std::wstring def;
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME, ext.c_str(), nullptr, name, &n)))
        def = name;
    ComPtr<IEnumAssocHandlers> en;
    if (FAILED(SHAssocEnumHandlers(ext.c_str(), ASSOC_FILTER_RECOMMENDED, &en)) || !en) return false;
    ComPtr<IAssocHandler> h, first;
    ULONG got = 0;
    while (en->Next(1, &h, &got) == S_OK && got == 1) {
        if (!first) first = h;
        LPWSTR ui = nullptr;
        bool match = false;
        if (!def.empty() && SUCCEEDED(h->GetUIName(&ui)) && ui) {
            match = _wcsicmp(ui, def.c_str()) == 0;
            CoTaskMemFree(ui);
        }
        if (match) break;
        h.Reset();
    }
    IAssocHandler* pick = h ? h.Get() : first.Get();
    if (!pick) return false;
    LoadHandlerIcon(pick, out);
    return !out.iconPx.empty();
}

bool OpenWithApp_Invoke(const OpenWithApp& app, const std::vector<std::wstring>& paths) {
    if (!app.handler || paths.empty()) return false;
    ComPtr<IDataObject> data;
    if (FAILED(CreateShellDataObject(paths, &data))) return false;
    return SUCCEEDED(app.handler->Invoke(data.Get()));
}

void ShowOpenWithDialog(HWND hwnd, const std::wstring& path) {
    OPENASINFO info = {};
    info.pcszFile = path.c_str();
    info.oaifInFlags = OAIF_EXEC | OAIF_ALLOW_REGISTRATION;
    SHOpenWithDialog(hwnd, &info);
}

} // namespace peek
