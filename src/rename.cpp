// Umbenennen (F2, Kontextmenue) direkt an der Stelle des Namens und Eigenschaften-Dialog des Explorers.
// Das Eingabefeld ist ein kleines eigenes Fenster ueber dem Namen (die App zeichnet per DirectComposition,
// untergeordnete Steuerelemente waeren dahinter unsichtbar). Enter oder Klick daneben uebernimmt, Esc bricht ab.
#include "app.h"
#include "core/textutil.h"

#include <dwmapi.h>
#include <shlobj.h>

namespace peek {

namespace {

constexpr wchar_t kRenameClass[] = L"peekRename";
WNDPROC g_editProc = nullptr;
HWND g_app = nullptr;
bool g_done = false;

LRESULT CALLBACK RenameEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_ESCAPE) {
            if (!g_done) {
                g_done = true;
                PostMessageW(g_app, WM_APP_RENAME, wp == VK_RETURN ? 1 : 0, 0);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == 27) return 0;   // kein Signalton
        break;
    case WM_KILLFOCUS:
        if (!g_done) {   // Klick daneben: wie im Explorer uebernehmen
            g_done = true;
            PostMessageW(g_app, WM_APP_RENAME, 1, 0);
        }
        break;
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

LRESULT CALLBACK RenameHostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLOREDIT: {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        bool dark = app && app->IsDarkTheme();
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, dark ? RGB(255, 255, 255) : RGB(0, 0, 0));
        SetBkColor(dc, dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
        static HBRUSH darkBr = CreateSolidBrush(RGB(32, 32, 32)), lightBr = CreateSolidBrush(RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(dark ? darkBr : lightBr);
    }
    case WM_ERASEBKGND: {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        bool dark = app && app->IsDarkTheme();
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
        FillRect(reinterpret_cast<HDC>(wp), &rc, br);
        DeleteObject(br);
        return 1;
    }
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool ValidName(const std::wstring& n) {
    if (n.empty() || n == L"." || n == L"..") return false;
    for (wchar_t c : n)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) return false;
    // reservierte Geraetenamen (CON, NUL, COM1 ...)
    std::wstring stem = ToLower(n.substr(0, n.find(L'.')));
    static const wchar_t* reserved[] = {L"con", L"prn", L"aux", L"nul", L"com1", L"com2", L"com3", L"com4", L"com5",
                                        L"com6", L"com7", L"com8", L"com9", L"lpt1", L"lpt2", L"lpt3", L"lpt4",
                                        L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9"};
    for (auto* r : reserved)
        if (stem == r) return false;
    return true;
}

}  // namespace

bool App::IsDarkTheme() const { return dark_; }

void App::BeginRename(int pos) {
    if (renameHost_) FinishRename(true);
    if (pos < 0 || pos >= static_cast<int>(visible_.size()) || viewer_.open) return;
    const FileEntry& e = entries_[visible_[pos]];
    if (e.offline || !IsLocalDrivePath(e.path)) return;

    // Element sichtbar machen und die Lage des Namens bestimmen (DIP, Fensterkoordinaten)
    D2D1_RECT_F nr{};
    const float fieldH = 26;
    if (settings_.viewMode >= 2) {   // Wolke (bei der Karte) bzw. Zeitleiste (unten in der Mitte)
        bool found = false;
        auto ci = cloud_.index.find(e.path);
        if (ci != cloud_.index.end())
            for (auto& d : cloud_.drawn)
                if (d.card == ci->second) {
                    float cx = (d.rc.left + d.rc.right) / 2, w = std::max(240.0f, d.rc.right - d.rc.left);
                    float top = std::min(d.rc.bottom + 8, gridRc_.bottom - fieldH - 8);
                    nr = D2D1::RectF(cx - w / 2, top, cx + w / 2, top + fieldH);
                    found = true;
                    break;
                }
        if (!found) {
            float cx = (gridRc_.left + gridRc_.right) / 2;
            float b = gridRc_.bottom - (settings_.viewMode == 3 ? 76.0f : 16.0f);
            nr = D2D1::RectF(cx - 160, b - fieldH, cx + 160, b);
        }
    } else {
        ScrollToPos(pos);
        scroll_ = scrollTarget_;
        Layout();
        D2D1_RECT_F rc = ItemRect(pos);
        float dy = gridRc_.top - scroll_;
        rc.top += dy;
        rc.bottom += dy;
        if (settings_.viewMode == 1) {
            float x = ListX0() + 34;
            nr = D2D1::RectF(x, rc.top + 4, x + std::max(160.0f, ListNameW() - 40.0f), rc.top + 4 + fieldH);
        } else {
            float top = rc.top + 3 + 8 + tileS_ + 2;
            nr = D2D1::RectF(rc.left + 6, top, rc.right - 6, top + fieldH);
        }
    }
    nr.left = std::max(nr.left, gridRc_.left + 2);
    nr.right = std::min(nr.right, gridRc_.right - 2);

    // Fenster anlegen
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = RenameHostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kRenameClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        RegisterClassExW(&wc);
        registered = true;
    }
    float s = r_.scale();
    POINT p0 = {static_cast<LONG>(nr.left * s), static_cast<LONG>(nr.top * s)};
    ClientToScreen(hwnd_, &p0);
    int w = static_cast<int>((nr.right - nr.left) * s), h = static_cast<int>(fieldH * s);
    renameHost_ = CreateWindowExW(WS_EX_TOOLWINDOW, kRenameClass, L"", WS_POPUP | WS_CLIPCHILDREN, p0.x, p0.y, w, h, hwnd_,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!renameHost_) return;
    SetWindowLongPtrW(renameHost_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    DWORD corner = 3;   // DWMWCP_ROUNDSMALL
    DwmSetWindowAttribute(renameHost_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    COLORREF border = RGB(static_cast<int>(accent_.r * 255), static_cast<int>(accent_.g * 255), static_cast<int>(accent_.b * 255));
    DwmSetWindowAttribute(renameHost_, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));

    int fontPx = static_cast<int>(kNameSize * s + 0.5f);
    if (renameFont_) DeleteObject(renameFont_);
    renameFont_ = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    int padX = static_cast<int>(8 * s);
    int editH = static_cast<int>(fontPx * 1.45f);
    renameEdit_ = CreateWindowExW(0, L"EDIT", e.name.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, padX, (h - editH) / 2,
                                  w - 2 * padX, editH, renameHost_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(renameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(renameFont_), TRUE);
    SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    g_app = hwnd_;
    g_done = false;
    g_editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(renameEdit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RenameEditProc)));
    // wie im Explorer: nur der Name ohne Endung ist markiert
    size_t selEnd = e.name.size();
    if (e.kind != Kind::Folder) {
        size_t dot = e.name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) selEnd = dot;
    }
    SendMessageW(renameEdit_, EM_SETSEL, 0, static_cast<LPARAM>(selEnd));
    renamePath_ = e.path;
    ShowWindow(renameHost_, SW_SHOW);
    SetFocus(renameEdit_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::FinishRename(bool commit) {
    if (!renameHost_) return;
    std::wstring text;
    if (commit && renameEdit_) {
        int len = GetWindowTextLengthW(renameEdit_);
        text.resize(static_cast<size_t>(len) + 1);
        GetWindowTextW(renameEdit_, text.data(), len + 1);
        text.resize(static_cast<size_t>(len));
    }
    g_done = true;
    HWND host = renameHost_;
    renameHost_ = nullptr;
    renameEdit_ = nullptr;
    DestroyWindow(host);
    SetFocus(hwnd_);
    std::wstring oldPath = renamePath_;
    renamePath_.clear();
    if (!commit) return;

    // Leerzeichen am Anfang sowie Leerzeichen und Punkte am Ende entfernt Windows ohnehin
    while (!text.empty() && text.front() == L' ') text.erase(0, 1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'.')) text.pop_back();
    std::wstring oldName = FileNameOf(oldPath);
    if (text == oldName) return;
    if (!ValidName(text)) {
        SetStatus(T(L"Ungültiger Name. Nicht erlaubt sind \\ / : * ? \" < > | und reservierte Namen wie CON oder NUL."));
        return;
    }
    std::wstring newPath = JoinPath(ParentPath(oldPath), text);
    bool caseOnly = _wcsicmp(text.c_str(), oldName.c_str()) == 0;
    if (!caseOnly && GetFileAttributesW(LongPath(newPath).c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetStatus(TF(L"„{0}“ gibt es in diesem Ordner bereits.", {text}));
        return;
    }
    if (!MoveFileExW(LongPath(oldPath).c_str(), LongPath(newPath).c_str(), 0)) {
        DWORD err = GetLastError();
        wchar_t* msg = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err,
                       0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
        std::wstring m = msg ? msg : L"";
        if (msg) LocalFree(msg);
        while (!m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ')) m.pop_back();
        SetStatus(T(L"Umbenennen nicht möglich: ") + m);
        return;
    }
    db_.Forget({oldPath});   // alter Name: Index-Eintraege und Vorschaubilder (auch eines Ordners) entfernen

    // Merkliste mitziehen (auch Elemente in einem umbenannten Ordner)
    bool marks = false;
    for (auto& b : settings_.bookmarks) {
        if (_wcsicmp(b.c_str(), oldPath.c_str()) == 0) { b = newPath; marks = true; }
        else if (b.size() > oldPath.size() && _wcsnicmp(b.c_str(), oldPath.c_str(), oldPath.size()) == 0 &&
                 b[oldPath.size()] == L'\\') {
            b = newPath + b.substr(oldPath.size());
            marks = true;
        }
    }
    if (marks) {
        settings_.Save();
        BuildSidebar();
    }

    // Eintrag sofort umstellen (Auswahl bleibt); das Neueinlesen holt Typ, Vorschaubild und Metadaten neu
    auto it = byPath_.find(oldPath);
    if (it != byPath_.end()) {
        uint32_t idx = it->second;
        FileEntry& fe = entries_[idx];
        fe.path = newPath;
        fe.name = text;
        fe.mtime = -1;   // gilt beim Neueinlesen als geaendert
        byPath_.erase(it);
        byPath_[newPath] = idx;
        auto ci = cloud_.index.find(oldPath);
        if (ci != cloud_.index.end()) {
            int card = ci->second;
            cloud_.index.erase(ci);
            cloud_.cards[card].path = newPath;
            cloud_.index[newPath] = card;
        }
    }
    SetStatus(TF(L"Umbenannt in „{0}“", {text}) + L".");
    QuietReload();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowProperties(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (paths.size() == 1) {
        SHObjectProperties(hwnd_, SHOP_FILEPATH, paths[0].c_str(), nullptr);
        return;
    }
    // Mehrfachauswahl: Sammeldialog wie im Explorer
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
    }
    if (!pidls.empty()) {
        ComPtr<IShellItemArray> arr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                        const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), &arr))) {
            ComPtr<IDataObject> data;
            if (SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))))
                SHMultiFileProperties(data.Get(), 0);
        }
    }
    for (auto p : pidls) CoTaskMemFree(p);
}

} // namespace peek
