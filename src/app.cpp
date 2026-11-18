// App: Fenster, Navigation, Datenfluss, Filter und Sortierung
#include "app.h"
#include "activity.h"
#include "sandbox.h"
#include "core/textutil.h"

#include <cmath>
#include <cwctype>

namespace peek {

// ---------------------------------------------------------------------------
// Start und Fenster
// ---------------------------------------------------------------------------

// Auswahllisten der Einstellungen
const wchar_t* const kThemeNames[3] = {N_(L"System (Standard)"), N_(L"Hell"), N_(L"Dunkel")};
const wchar_t* const kViewerSmallNames[2] = {N_(L"Originalgr\u00f6\u00dfe (100 %)"), N_(L"An die Ansicht anpassen")};

const wchar_t* LanguageName(int setting) {
    switch (setting) {
    case 1: return L"Deutsch";   // lang:ok (Sprachnamen stehen immer in der eigenen Sprache)
    case 2: return L"English";
    default: return T(L"Wie Windows (Standard)");
    }
}

namespace {
// 0 = wie Windows: Deutsch, wenn die Anzeigesprache von Windows Deutsch ist, sonst Englisch
Lang ResolveLanguage(int setting) {
    if (setting == 1) return Lang::De;
    if (setting == 2) return Lang::En;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN ? Lang::De : Lang::En;
}
}  // namespace

void App::ApplyLanguage() {
    SetCurrentLang(ResolveLanguage(langOverride_ > 0 ? langOverride_ : settings_.language));
    if (!hwnd_) return;
    typeNames_.clear();
    BuildSidebar();
    UpdateTitle();
    rowsGroupKey_ = -1000;   // Gruppenkoepfe neu beschriften
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::Init(HINSTANCE inst, int show) {
    inst_ = inst;
    settings_.Load();
    // Sprache vor allem anderen (Fehlermeldungen beim Start); --lang=de|en gilt nur fuer diesen Start
    {
        std::wstring cmd = GetCommandLineW();
        if (cmd.find(L"--lang=de") != std::wstring::npos) langOverride_ = 1;
        else if (cmd.find(L"--lang=en") != std::wstring::npos) langOverride_ = 2;
        SetCurrentLang(ResolveLanguage(langOverride_ > 0 ? langOverride_ : settings_.language));
    }
    // Neue Version (oder neues App-Symbol) an gleicher Stelle: Windows haelt das alte Symbol der EXE im
    // Symbol-Cache (Task-Manager, Explorer, Taskleiste). Beim ersten Start einer Version den Eintrag auffrischen.
    const std::wstring build = std::wstring(PEEK_VERSION) + L" " + Utf8ToWide(__DATE__ " " __TIME__);
    if (settings_.lastVersion != build) {
        wchar_t exe[MAX_PATH * 2] = {};
        DWORD len = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (len > 0 && len < std::size(exe)) {
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, exe, nullptr);
            std::wstring dir = ParentPath(exe);
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, dir.c_str(), nullptr);
        }
        settings_.lastVersion = build;
        settings_.Save();
    }
    db_.Open(DataDir());
    // Geloeschte oder verschobene Dateien: Index-Eintraege und Vorschaubilder sofort entfernen
    SetFilesGoneHook([this](const std::vector<std::wstring>& paths) { db_.Forget(paths); });

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.lpszClassName = L"peek.main";
    if (!RegisterClassExW(&wc)) return false;

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1400, h = 900;
    if (settings_.hasWindow) {
        RECT rc = settings_.window;
        if (MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) && rc.right - rc.left >= 640 && rc.bottom - rc.top >= 420) {
            x = rc.left;
            y = rc.top;
            w = rc.right - rc.left;
            h = rc.bottom - rc.top;
        }
    }
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"peek", WS_OVERLAPPEDWINDOW, x, y, w, h,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    ApplyTheme();
    if (!r_.Init(hwnd_)) {
        MessageBoxW(hwnd_, T(L"Die Grafikausgabe (Direct3D 11 / Direct2D) konnte nicht initialisiert werden."), L"peek",
                    MB_ICONERROR);
        return false;
    }

    drop_.Attach(new DropTarget(
        hwnd_,
        [this](const std::vector<std::wstring>& p, POINT pt, DWORD keys, DWORD allowed, bool leave) {
            return OnDragOver(p, pt, keys, allowed, leave);
        },
        [this](const std::vector<std::wstring>& p, POINT pt, DWORD keys, DWORD allowed) {
            return OnDrop(p, pt, keys, allowed);
        }));
    sidebarAnim_ = settings_.sidebar ? 1.0f : 0.0f;
    infoAnim_ = settings_.infoPanel ? 1.0f : 0.0f;
    RegisterDragDrop(hwnd_, drop_.Get());

    unsigned hc = std::thread::hardware_concurrency();
    int workers = static_cast<int>(std::clamp<unsigned>(hc > 1 ? hc - 1 : 1, 2, 8));
    InitDecoderSandbox(workers + 1);   // Dekoder-Prozesse in der Sandbox (sandbox.h)
    PrewarmDecoderSandbox();
    thumbs_.Start(hwnd_, &db_, workers);
    meta_.Start(hwnd_, &db_, 2);
    loader_.Start(hwnd_, &db_);
    indexer_.Start(hwnd_, &db_);
    indexer_.SetRoots(settings_.collections, settings_.showHidden);

    BuildSidebar();

    // Startordner: Kommandozeile, zuletzt genutzt, Bilder, erstes Laufwerk
    std::wstring start, select;
    bool openSettings = false, openSheet = false, openAtlas = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        // Optionen: --view=grid|list  --info  --tile=N  --size=BxH  --viewer=<Datei> [--crop]  --settings
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            auto value = [&](const wchar_t* prefix) -> std::wstring {
                size_t n = wcslen(prefix);
                return a.compare(0, n, prefix) == 0 ? a.substr(n) : std::wstring();
            };
            if (a == L"--info") { settings_.infoPanel = settings_.viewerInfo = true; infoAnim_ = 1.0f; continue; }
            if (a == L"--settings") { openSettings = true; continue; }
            if (a == L"--theme=dark" || a == L"--theme=light") { settings_.theme = a == L"--theme=dark" ? 2 : 1; ApplyTheme(); continue; }
            if (a == L"--group") { settings_.group = true; continue; }
            if (a == L"--sheet") { openSheet = true; continue; }
            if (a == L"--crop") { pendingCrop_ = true; continue; }
            if (a == L"--atlas") { openAtlas = true; continue; }
            if (!value(L"--term=").empty()) { pendingTerm_ = value(L"--term="); continue; }
            if (!value(L"--search=").empty()) { pendingSearch_ = value(L"--search="); continue; }
            if (a == L"--search-open") { pendingSearchOpen_ = true; continue; }
            if (a == L"--rename") { pendingRename_ = true; continue; }
            if (!value(L"--date-range=").empty()) { pendingDateRange_ = value(L"--date-range="); continue; }
            if (a == L"--open-datepicker") { pendingDatePicker_ = true; continue; }
            if (!value(L"--cloud-zoom=").empty()) { pendingCloudZoom_ = static_cast<float>(_wtof(value(L"--cloud-zoom=").c_str())); continue; }
            if (!value(L"--compare=").empty()) {
                pendingCompare_ = value(L"--compare=");
                start = ParentPath(pendingCompare_.substr(0, pendingCompare_.find(L';')));
                continue;
            }
            if (!value(L"--compare-mode=").empty()) { pendingCompareMode_ = _wtoi(value(L"--compare-mode=").c_str()); continue; }
            if (!value(L"--viewer-zoom=").empty()) { pendingZoom_ = static_cast<float>(_wtof(value(L"--viewer-zoom=").c_str())); continue; }
            if (!value(L"--open-chip=").empty()) { pendingChip_ = value(L"--open-chip=") == L"source" ? static_cast<int>(ChipId::Source) : static_cast<int>(ChipId::Pattern); continue; }
            if (!value(L"--cloud-select=").empty()) { pendingCloudSel_ = value(L"--cloud-select="); continue; }
            if (a == L"--cloud-tour" || !value(L"--cloud-tour=").empty()) { cloudTour_ = true; cloudTourMark_ = value(L"--cloud-tour="); continue; }
            if (a == L"--no-sidebar") { settings_.sidebar = false; sidebarAnim_ = 0.0f; continue; }
            if (!value(L"--view=").empty()) {
                std::wstring v = value(L"--view=");
                settings_.viewMode = v == L"list" ? 1 : (v == L"cloud" ? 2 : 0);
                settings_.sidebar = settings_.sidebarView[settings_.viewMode];
                settings_.infoPanel = settings_.infoView[settings_.viewMode];
                sidebarAnim_ = settings_.sidebar ? 1.0f : 0.0f;
                infoAnim_ = settings_.infoPanel ? 1.0f : 0.0f;
                continue;
            }
            if (!value(L"--tile=").empty()) { settings_.tileSize = std::clamp(_wtoi(value(L"--tile=").c_str()), 64, 384); continue; }
            if (!value(L"--size=").empty()) {
                std::wstring v = value(L"--size=");
                size_t xpos = v.find(L'x');
                if (xpos != std::wstring::npos) {
                    int w2 = _wtoi(v.substr(0, xpos).c_str()), h2 = _wtoi(v.substr(xpos + 1).c_str());
                    if (w2 >= 640 && h2 >= 420) {
                        settings_.maximized = false;
                        SetWindowPos(hwnd_, nullptr, 0, 0, w2, h2, SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                }
                continue;
            }
            if (!value(L"--viewer=").empty()) {
                pendingViewer_ = value(L"--viewer=");
                start = ParentPath(pendingViewer_);
                continue;
            }
            if (a.rfind(L"--", 0) == 0) continue;
            DWORD attr = GetFileAttributesW(a.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                if (attr & FILE_ATTRIBUTE_DIRECTORY) start = a;
                else { start = ParentPath(a); select = a; }
            }
        }
        LocalFree(argv);
    }
    if (start.empty() && !settings_.lastFolder.empty() && DirectoryExists(settings_.lastFolder)) start = settings_.lastFolder;
    if (start.empty()) start = KnownFolderPath(FOLDERID_Pictures);
    if (start.empty() || !IsLocalDrivePath(start)) {
        auto drives = LocalDrives();
        if (!drives.empty()) start = drives.front();
    }
    if (!start.empty()) Navigate(start, false, select);
    if (openSettings) OpenSettings();
    if (openSheet && !start.empty()) OpenContactSheet({start});   // nur fuer Dokumentation/Tests
    if (openAtlas && !start.empty()) OpenAtlas(start);

    ShowWindow(hwnd_, settings_.maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd_);
    return true;
}

int App::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK App::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<App*>(cs->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) return app->WndProc(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
        msg == WM_SIZE || msg == WM_APP_LOAD)
        MarkUiActivity();
    const float s = r_.scale() > 0 ? r_.scale() : 1.0f;
    auto X = [&](LPARAM l) { return static_cast<float>(GET_X_LPARAM(l)) / s; };
    auto Y = [&](LPARAM l) { return static_cast<float>(GET_Y_LPARAM(l)) / s; };

    switch (msg) {
    case WM_MOVE:
        if (renameHost_) FinishRename(true);
        break;
    case WM_SIZE:
        if (renameHost_) FinishRename(true);
        settings_.maximized = (wp == SIZE_MAXIMIZED);
        r_.Resize();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        Paint();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        r_.SetDpi(static_cast<float>(HIWORD(wp)));
        auto rc = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = static_cast<LONG>(640 * s);
        mmi->ptMinTrackSize.y = static_cast<LONG>(420 * s);
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (autoScroll_) { StopAutoScroll(); return 0; }
        if (viewer_.open && !settingsOpen_ && X(lp) < ViewerAreaW() && viewer_.pos >= 0) {
            // mittlere Maustaste im Betrachter: Ausschnitt direkt aufziehen
            if (!viewer_.crop) EnterCrop(false);
            SetCapture(hwnd_);
            CropMouseDown(X(lp), Y(lp), true);
            return 0;
        }
        if (!viewer_.open && !settingsOpen_) {
            float x = X(lp), y = Y(lp);
            if (x >= gridRc_.left && x < gridRc_.right && y >= gridRc_.top && y < gridRc_.bottom) {
                autoScroll_ = true;
                autoSticky_ = false;
                autoAnchor_ = D2D1::Point2F(x, y);
                autoMouseY_ = y;
                autoStart_ = GetTickCount();
                SetCapture(hwnd_);
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                StartAnimation();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MBUTTONUP:
        if (viewer_.open && viewer_.cropDrag) {
            if (GetCapture() == hwnd_) ReleaseCapture();
            CropMouseUp();
            return 0;
        }
        if (autoScroll_ && !autoSticky_) {
            // kurzer Klick ohne Bewegung: Modus bleibt aktiv bis zum naechsten Klick (wie im Browser)
            if (GetTickCount() - autoStart_ < 350 && std::fabs(Y(lp) - autoAnchor_.y) < 6) autoSticky_ = true;
            else StopAutoScroll();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (autoScroll_ && reinterpret_cast<HWND>(lp) != hwnd_) StopAutoScroll();
        break;
    case WM_RBUTTONDOWN:
        if (autoScroll_) StopAutoScroll();
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        OnMouseMove(X(lp), Y(lp), wp);
        return 0;
    }
    case WM_MOUSELEAVE:
        hoverPos_ = -1;
        hoverHit_ = -1;
        hoverScrollbar_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (autoScroll_) { StopAutoScroll(); return 0; }
        SetCapture(hwnd_);
        OnLButtonDown(X(lp), Y(lp), wp);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd_) ReleaseCapture();
        OnLButtonUp(X(lp), Y(lp));
        return 0;
    case WM_LBUTTONDBLCLK: {
        if (sheet_.open) {
            SetCapture(hwnd_);
            SheetDoubleClick(X(lp), Y(lp));
            return 0;
        }
        if (atlas_.open) {   // Buttons: wie ein zweiter Klick
            AtlasMouseDown(X(lp), Y(lp));
            return 0;
        }
        if (compare_.open) {   // Doppelklick: wieder einpassen (Buttons wie ein zweiter Klick)
            const Hit* ch = HitAt(X(lp), Y(lp));
            if (ch) CompareMouseDown(X(lp), Y(lp));
            else CompareReset();
            return 0;
        }
        // Doppelklick auf einen Button (Zoom, Blaettern, Umschalter …) wirkt wie zwei Klicks; nur Elemente,
        // Suchfeld und Spaltengriffe haben eine eigene Doppelklick-Funktion
        const Hit* h = HitAt(X(lp), Y(lp));
        if (h && h->type != HitType::Search && h->type != HitType::ListResize) {
            if (autoScroll_) { StopAutoScroll(); return 0; }
            SetCapture(hwnd_);
            OnLButtonDown(X(lp), Y(lp), wp);
        } else {
            OnDoubleClick(X(lp), Y(lp));
        }
        return 0;
    }
    case WM_RBUTTONUP:
        OnRButtonUp(X(lp), Y(lp));
        return 0;
    case WM_XBUTTONUP:
        if (sheet_.open) {
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1 && !sheet_.editing) CloseContactSheet();
        } else if (atlas_.open) {
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) CloseAtlas();
        } else if (compare_.open) {
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) CloseCompare();
        } else if (settingsOpen_) {
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) CloseSettings();   // Maus-Zurueck schliesst die Einstellungen
        } else if (viewer_.open) ViewerStep(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? -1 : 1);   // Bilder blaettern
        else if (!settingsOpen_) {
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) GoBack();
            else GoForward();
        }
        return TRUE;
    case WM_MOUSEWHEEL: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        OnWheel(pt.x / s, pt.y / s, GET_WHEEL_DELTA_WPARAM(wp), GET_KEYSTATE_WPARAM(wp));
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (autoScroll_) { StopAutoScroll(); if (wp == VK_ESCAPE) return 0; }
        if (OnKeyDown(wp)) return 0;
        break;
    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wp));
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            if (autoScroll_) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
            if (atlas_.open || compare_.open) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            if (sheet_.open) {
                const Hit* sh = HitAt(pt.x / s, pt.y / s);
                LPCWSTR c = IDC_ARROW;
                if (sheet_.dragging) c = IDC_SIZEALL;
                else if (sh && sh->type == HitType::SheetHeadline) c = IDC_IBEAM;
                else if (sh && (sh->type == HitType::SheetCell || sh->type == HitType::SheetStrip)) c = IDC_HAND;
                SetCursor(LoadCursorW(nullptr, c));
                return TRUE;
            }
            if (viewer_.open && viewer_.crop && pt.x / s < ViewerAreaW()) {
                const Hit* ch = HitAt(pt.x / s, pt.y / s);
                SetCursor(LoadCursorW(nullptr, ch ? IDC_ARROW : CropCursor(pt.x / s, pt.y / s)));
                return TRUE;
            }
            const Hit* h = viewer_.open ? nullptr : HitAt(pt.x / s, pt.y / s);
            LPCWSTR cur = IDC_ARROW;
            if (h && h->type == HitType::Search) cur = IDC_IBEAM;
            else if (h && h->type == HitType::ZoomSlider) cur = IDC_HAND;
            else if (colDrag_ >= 0 || (h && h->type == HitType::ListResize)) cur = IDC_SIZEWE;
            SetCursor(LoadCursorW(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_TIMER:
        switch (wp) {
        case kTimerAnim:   // Rueckfall, falls Present nicht auf VSync wartet (z. B. verdecktes Fenster)
            KillTimer(hwnd_, kTimerAnim);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case kTimerDocMenu: {
            KillTimer(hwnd_, kTimerDocMenu);
            if (pendingDatePicker_) {   // Dokumentation: Kalender des Datumsfilters zeigen
                pendingDatePicker_ = false;
                for (auto& h : hits_)
                    if (h.type == HitType::Chip && h.a == static_cast<int>(ChipId::Date)) {
                        D2D1_RECT_F rc = h.rc;
                        OpenDatePicker(ChipId::Date, rc);
                        break;
                    }
                break;
            }
            if (pendingRename_) {   // Dokumentation: Umbenennen des ausgewaehlten Elements zeigen
                pendingRename_ = false;
                BeginRename(focus_);
                break;
            }
            int chip = pendingChip_;
            pendingChip_ = -1;
            for (auto& h : hits_)
                if (h.type == HitType::Chip && h.a == chip) {
                    D2D1_RECT_F rc = h.rc;
                    ShowChipMenu(static_cast<ChipId>(chip), rc);
                    break;
                }
            break;
        }
        case kTimerToast:
            KillTimer(hwnd_, kTimerToast);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case kTimerViewerUi:
            KillTimer(hwnd_, kTimerViewerUi);
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case kTimerViewerClick:
            KillTimer(hwnd_, kTimerViewerClick);
            if (viewer_.open) {
                viewer_.bar = !viewer_.bar;
                settings_.viewerBar = viewer_.bar;   // fuer das naechste Oeffnen merken
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            break;
        case kTimerRefilter:
            KillTimer(hwnd_, kTimerRefilter);
            Refilter();
            break;
        case kTimerReload:
            KillTimer(hwnd_, kTimerReload);
            if (savingCrop_) { SetTimer(hwnd_, kTimerReload, 300, nullptr); break; }
            if (!loading_) QuietReload();
            break;
        case kTimerMetaHold:
            KillTimer(hwnd_, kTimerMetaHold);
            ReleaseMetaHold();
            break;
        case kTimerStatus:
            KillTimer(hwnd_, kTimerStatus);
            status_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        return 0;
    case WM_APP_LOAD:
        OnLoadResults();
        return 0;
    case WM_APP_FILEOP:
        DoPendingFileOp();
        return 0;
    case WM_APP_THUMB:
        OnThumbResults();
        return 0;
    case WM_APP_META:
        OnMetaResults();
        return 0;
    case WM_APP_FSCHANGE:
        if (static_cast<uint32_t>(wp) == gen_ && !collectionsView_) SetTimer(hwnd_, kTimerReload, 700, nullptr);
        return 0;
    case WM_APP_SHEET:
        if (wp == 1) SheetDone();
        else InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_APP_RENAME:
        FinishRename(wp == 1);
        return 0;
    case WM_APP_ATLAS:
        if (wp == 1) AtlasDone();
        else InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_APP_INDEX:
        indexBusy_ = indexer_.Busy();
        indexDone_ = indexer_.Done();
        indexTotal_ = indexer_.Total();
        if (settingsOpen_ && !indexBusy_) RefreshSettingsInfo();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        if (r_.UpdateTextScale()) {
            // Textgroesse geaendert: alles neu anordnen (Groessen in DIP bleiben, nur der Massstab waechst)
            Layout();
            if (focus_ >= 0) ScrollToPos(focus_);
        }
        [[fallthrough]];
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    case WM_DEVICECHANGE:
        BuildSidebar();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return TRUE;
    case WM_DESTROY: {
        if (sheetThread_.joinable()) {   // laufende PDF-Erstellung beenden
            sheet_.cancel = true;
            sheetThread_.join();
        }
        if (atlasThread_.joinable()) {   // laufende Kacheltextur beenden
            atlas_.cancel = true;
            atlasThread_.join();
        }
        if (!fullscreen_) {
            WINDOWPLACEMENT wpl = {sizeof(wpl)};
            if (GetWindowPlacement(hwnd_, &wpl)) {
                settings_.window = wpl.rcNormalPosition;
                settings_.hasWindow = true;
                settings_.maximized = wpl.showCmd == SW_SHOWMAXIMIZED;
            }
        }
        settings_.Save();
        RevokeDragDrop(hwnd_);
        indexer_.Stop();
        thumbs_.Stop();
        meta_.Stop();
        loader_.Stop();
        ShutdownDecoderSandbox();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void App::ApplyTheme() {
    DWORD v = 1, sz = sizeof(v);
    dark_ = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS
                ? v == 0
                : false;
    if (settings_.theme == 1) dark_ = false;        // Einstellung: immer hell
    else if (settings_.theme == 2) dark_ = true;    // immer dunkel
    BOOL d = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    int backdrop = DWMSBT_MAINWINDOW;
    backdrop_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)));
    if (backdrop_) {
        MARGINS m = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
    }
    DWORD accent = 0;
    sz = sizeof(accent);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                     &accent, &sz) == ERROR_SUCCESS) {
        // 0xAABBGGRR
        accent_ = D2D1::ColorF((accent & 0xFF) / 255.0f, ((accent >> 8) & 0xFF) / 255.0f,
                               ((accent >> 16) & 0xFF) / 255.0f, 1.0f);
        if (dark_) {  // im dunklen Modus etwas aufhellen, wie Windows
            accent_.r = accent_.r + (1 - accent_.r) * 0.25f;
            accent_.g = accent_.g + (1 - accent_.g) * 0.25f;
            accent_.b = accent_.b + (1 - accent_.b) * 0.25f;
        }
    }
}

void App::SetStatus(const std::wstring& msg) {
    status_ = msg;
    SetTimer(hwnd_, kTimerStatus, 4000, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::UpdateTitle() {
    std::wstring t = L"peek";
    if (collectionsView_) t += T(L" – Alle Sammlungen");
    else if (!path_.empty()) {
        std::wstring n = FileNameOf(path_);
        t += L" – " + (n.empty() ? path_ : n);
    }
    SetWindowTextW(hwnd_, t.c_str());
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void App::StartView() {
    ++gen_;
    thumbs_.CancelBelow(gen_);
    meta_.SetGeneration(gen_);
    // abgebrochene Vorschau-Auftraege duerfen neu angefordert werden
    for (auto& [k, b] : bmps_)
        if (!b.bmp) { b.requested = false; b.reqPrio = 9; }
    entries_.clear();
    byPath_.clear();
    visible_.clear();
    ++visibleVersion_;
    selected_.clear();
    marquee_ = false;
    focus_ = anchor_ = -1;
    hoverPos_ = -1;
    if (keepScroll_ < 0) scroll_ = scrollTarget_ = 0;
    loading_ = true;
    quietLoad_ = false;
    stage_.clear();
    stageIdx_.clear();
    metaPending_ = 0;
    metaHold_ = settings_.sortKey == 1 || settings_.sortKey == 3 || settings_.sortKey == 5;
    if (metaHold_) SetTimer(hwnd_, kTimerMetaHold, 2500, nullptr);
    if (viewer_.open) CloseViewer();
    fullBmps_.clear();
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SaveViewState() {
    if (!collectionsView_ && !bookmarksView_ && path_.empty()) return;
    ViewState v;
    v.scroll = scroll_;
    if (focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) v.focus = entries_[visible_[focus_]].path;
    v.selection = SelectedPaths();
    viewStates_[CurrentLocation()] = std::move(v);
}

// Exakte Rueckkehr: beim Zurueck/Vor und beim Verlassen der Merkliste
bool App::TakeRestore() {
    bool r = restoreNext_ || bookmarksView_;
    restoreNext_ = false;
    return r;
}

void App::ApplyViewState(const std::wstring& loc) {
    auto it = viewStates_.find(loc);
    if (it == viewStates_.end()) return;
    keepScroll_ = it->second.scroll;
    keepSelection_ = it->second.selection;
    pendingSelect_ = it->second.focus;
}

void App::Navigate(const std::wstring& raw, bool history, const std::wstring& select) {
    std::wstring p = NormalizeDir(raw);
    if (!IsLocalDrivePath(p)) {
        SetStatus(T(L"Nur lokale Laufwerke werden unterstützt – Netzlaufwerke sind ausgeschlossen."));
        return;
    }
    if (!DirectoryExists(p)) {
        SetStatus(T(L"Ordner nicht gefunden: ") + p);
        return;
    }
    bool restore = TakeRestore();
    SaveViewState();
    if (history && (collectionsView_ || bookmarksView_ || !path_.empty()) &&
        (collectionsView_ || bookmarksView_ || p != path_)) {
        back_.push_back(CurrentLocation());
        fwd_.clear();
    }
    if (_wcsicmp(p.c_str(), path_.c_str()) != 0) settings_.recursive = false;   // neuer Ordner: ohne Unterordner
    path_ = p;
    collectionsView_ = false;
    bookmarksView_ = false;
    pendingSelect_ = select;
    keepSelection_.clear();
    keepScroll_ = -1;
    if (restore) ApplyViewState(p);
    settings_.lastFolder = p;
    filters_.terms.clear();
    filters_.source.clear();
    filters_.templ = UINT32_MAX;
    StartView();
    loader_.LoadFolder(path_, settings_.recursive, settings_.showHidden, gen_);
}

void App::ShowAllCollections(bool history) {
    bool restore = TakeRestore();
    SaveViewState();
    if (history && !collectionsView_ && (bookmarksView_ || !path_.empty())) {
        back_.push_back(CurrentLocation());
        fwd_.clear();
    }
    collectionsView_ = true;
    bookmarksView_ = false;
    path_.clear();
    pendingSelect_.clear();
    keepSelection_.clear();
    keepScroll_ = -1;
    if (restore) ApplyViewState(std::wstring());
    filters_.terms.clear();
    StartView();
    loader_.LoadCollections(&db_, settings_.collections, gen_);
}

void App::Reload(bool keepState) {
    if (keepState) {
        keepSelection_ = SelectedPaths();
        keepScroll_ = scroll_;
        if (focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) pendingSelect_ = entries_[visible_[focus_]].path;
    } else {
        keepSelection_.clear();
        keepScroll_ = -1;
    }
    bool wasViewer = viewer_.open;
    StartView();
    (void)wasViewer;
    if (bookmarksView_) {
        bookmarksView_ = false;
        ShowBookmarks(false);
        return;
    }
    if (collectionsView_) loader_.LoadCollections(&db_, settings_.collections, gen_);
    else loader_.LoadFolder(path_, settings_.recursive, settings_.showHidden, gen_);
}

std::wstring App::CurrentLocation() const {
    if (bookmarksView_) return L"::merkliste";   // lang:ok
    if (collectionsView_) return std::wstring();
    return path_;
}

void App::GoTo(const std::wstring& target, const std::wstring& select) {
    if (target.empty()) ShowAllCollections(false);
    else if (target == L"::merkliste") ShowBookmarks(false);   // lang:ok
    else Navigate(target, false, select);
}

void App::ShowBookmarks(bool history) {
    if (!bookmarksView_) {
        bool restore = restoreNext_;
        restoreNext_ = false;
        SaveViewState();
        if (restore) ApplyViewState(L"::merkliste");   // lang:ok
    }
    if (history && !bookmarksView_ && (collectionsView_ || !path_.empty())) {
        back_.push_back(CurrentLocation());
        fwd_.clear();
    }
    bool keep = keepScroll_ >= 0 || !keepSelection_.empty();
    if (!keep) {
        pendingSelect_.clear();
        filters_.terms.clear();
    }
    bookmarksView_ = true;
    collectionsView_ = false;
    path_.clear();
    StartView();
    // Merkliste: Eintraege direkt aus den gespeicherten Pfaden (nur lokale, vorhandene Dateien)
    for (auto& p : settings_.bookmarks) {
        if (!IsLocalDrivePath(p)) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(LongPath(p).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        FileEntry e = MakeEntry(ParentPath(p), fd);
        if (byPath_.count(e.path)) continue;
        PrepareEntry(e);
        byPath_[e.path] = static_cast<uint32_t>(entries_.size());
        entries_.push_back(std::move(e));
    }
    RequestMeta(0);
    loading_ = false;
    Refilter();
    for (auto& p : keepSelection_) {
        auto it = byPath_.find(p);
        if (it != byPath_.end()) selected_.insert(it->second);
    }
    keepSelection_.clear();
    if (keepScroll_ >= 0) {
        scroll_ = scrollTarget_ = keepScroll_;
        keepScroll_ = -1;
        Layout();
    }
    if (!pendingSelect_.empty()) {
        auto it = byPath_.find(pendingSelect_);
        if (it != byPath_.end())
            for (size_t i = 0; i < visible_.size(); ++i)
                if (visible_[i] == it->second) {
                    focus_ = anchor_ = static_cast<int>(i);
                    if (selected_.empty()) selected_.insert(it->second);
                    ScrollToPos(focus_);
                    break;
                }
        pendingSelect_.clear();
    }
    UpdateTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::IsBookmarked(const std::wstring& path) const {
    for (auto& b : settings_.bookmarks)
        if (_wcsicmp(b.c_str(), path.c_str()) == 0) return true;
    return false;
}

void App::SetBookmarked(const std::vector<std::wstring>& paths, bool add) {
    auto& b = settings_.bookmarks;
    size_t changed = 0;
    for (auto& p : paths) {
        auto it = std::find_if(b.begin(), b.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), p.c_str()) == 0; });
        if (add && it == b.end() && IsLocalDrivePath(p)) { b.push_back(p); ++changed; }
        if (!add && it != b.end()) { b.erase(it); ++changed; }
    }
    if (!changed) return;
    settings_.Save();
    BuildSidebar();
    SetStatus(std::to_wstring(changed) + (add ? T(L" Element(e) zur Merkliste hinzugef\u00fcgt") : T(L" Element(e) von der Merkliste entfernt")));
    if (bookmarksView_) {
        if (viewer_.open) reloadAfterViewer_ = true;   // Betrachter nicht schliessen, erst danach neu laden
        else Reload(true);
    }
}

void App::AddSearchHistory(const std::wstring& raw) {
    std::wstring t(Trim(raw));
    if (t.empty()) return;
    auto& h = settings_.searchHistory;
    h.erase(std::remove_if(h.begin(), h.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), t.c_str()) == 0; }),
            h.end());
    h.insert(h.begin(), t);
    if (h.size() > 10) h.resize(10);
    settings_.Save();
}

void App::GoBack() {
    if (back_.empty()) return;
    fwd_.push_back(CurrentLocation());
    std::wstring target = back_.back();
    back_.pop_back();
    restoreNext_ = true;
    GoTo(target, path_);
}

void App::GoForward() {
    if (fwd_.empty()) return;
    back_.push_back(CurrentLocation());
    std::wstring target = fwd_.back();
    fwd_.pop_back();
    restoreNext_ = true;
    GoTo(target);
}

void App::GoUp() {
    if (collectionsView_ || bookmarksView_ || path_.empty()) return;
    std::wstring parent = ParentPath(path_);
    if (parent.empty() || parent == path_) return;
    Navigate(parent, true, path_);
}

// ---------------------------------------------------------------------------
// Ergebnisse der Hintergrunddienste
// ---------------------------------------------------------------------------

void App::PrepareEntry(FileEntry& e) {
    if (e.Doc() && e.durationMs) {   // aus dem Index: Seitenzahl steht im Feld der Dauer
        e.pages = e.durationMs;
        e.durationMs = 0;
    }
    std::wstring stem = e.name;
    if (e.kind != Kind::Folder) {
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) stem.resize(dot);
    }
    std::vector<uint32_t> ids;
    for (auto& t : TokenizeName(stem)) ids.push_back(dict_.Id(t));
    std::wstring parent = FileNameOf(ParentPath(e.path));
    for (auto& t : TokenizeName(parent)) {
        uint32_t id = dict_.Id(t);
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    e.tokens = std::move(ids);
    if (e.kind != Kind::Folder) {
        e.source = DetectSource(stem);
        std::wstring t = NameTemplate(stem);
        if (t != stem && t.size() >= 3) e.templateId = templDict_.Id(t);
        int64_t d = DateFromName(stem);
        if (d >= 0) e.nameDate = d * 86400;
    }
}

void App::RequestMeta(size_t from) {
    std::vector<MetaRequest> reqs;
    bool covers = !collectionsView_ && !settings_.recursive;
    for (size_t i = from; i < entries_.size(); ++i) {
        const FileEntry& e = entries_[i];
        if (e.kind == Kind::Folder) {
            if (covers) {
                MetaRequest r;
                r.entry.path = e.path;
                r.cover = true;
                r.showHidden = settings_.showHidden;
                r.gen = gen_;
                reqs.push_back(std::move(r));
            }
        } else if (e.HasThumb() && !e.metaDone && !e.offline) {
            MetaRequest r;
            r.entry.path = e.path;
            r.entry.name = e.name;
            r.entry.kind = e.kind;
            r.entry.size = e.size;
            r.entry.mtime = e.mtime;
            r.entry.ctime = e.ctime;
            r.entry.attrs = e.attrs;
            r.gen = gen_;
            reqs.push_back(std::move(r));
            ++metaPending_;
        }
    }
    meta_.Enqueue(std::move(reqs));
}

void App::OnLoadResults() {
    bool done = false;
    if (quietLoad_) {
        for (auto& b : loader_.Take()) {
            if (b.gen != gen_) continue;
            for (auto& e : b.entries) {
                if (stageIdx_.count(e.path)) continue;
                stageIdx_[e.path] = static_cast<uint32_t>(stage_.size());
                stage_.push_back(std::move(e));
            }
            if (b.done) done = true;
        }
        if (done) FinishQuietReload();
        return;
    }
    for (auto& b : loader_.Take()) {
        if (b.gen != gen_) continue;
        if (!b.entries.empty()) {
            size_t from = entries_.size();
            entries_.reserve(from + b.entries.size());
            for (auto& e : b.entries) {
                if (byPath_.count(e.path)) continue;
                PrepareEntry(e);
                byPath_[e.path] = static_cast<uint32_t>(entries_.size());
                entries_.push_back(std::move(e));
            }
            RequestMeta(from);
        }
        if (b.done) done = true;
    }
    if (!done) {
        ScheduleRefilter(entries_.size() < 5000 ? 40 : 400);
        return;
    }
    loading_ = false;
    if (metaHold_ && metaPending_ <= 0) { metaHold_ = false; KillTimer(hwnd_, kTimerMetaHold); }
    Refilter();

    // Zustand wiederherstellen (Neuladen) bzw. Auswahl setzen
    for (auto& p : keepSelection_) {
        auto it = byPath_.find(p);
        if (it != byPath_.end()) selected_.insert(it->second);
    }
    keepSelection_.clear();
    if (keepScroll_ >= 0) {
        scroll_ = scrollTarget_ = keepScroll_;
        keepScroll_ = -1;
        Layout();
    }
    if (!pendingSelect_.empty()) {
        auto it = byPath_.find(pendingSelect_);
        if (it != byPath_.end()) {
            for (size_t i = 0; i < visible_.size(); ++i)
                if (visible_[i] == it->second) {
                    focus_ = anchor_ = static_cast<int>(i);
                    if (selected_.empty()) selected_.insert(it->second);
                    ScrollToPos(focus_);
                    break;
                }
        }
        pendingSelect_.clear();
    }
    if (!pendingCompare_.empty()) {   // Dokumentation: Vergleich oeffnen
        size_t semi = pendingCompare_.find(L';');
        if (semi != std::wstring::npos) {
            OpenCompare({pendingCompare_.substr(0, semi), pendingCompare_.substr(semi + 1)});
            compare_.mode = pendingCompareMode_;
            compare_.splitX = W_ * 0.42f;
        }
        pendingCompare_.clear();
    }
    if (pendingDateRange_.size() >= 22) {   // Dokumentation: Zeitraum "Individuell"
        auto day = [&](const std::wstring& t) {
            return DaysFromCivil(_wtoi(t.substr(0, 4).c_str()), static_cast<unsigned>(_wtoi(t.substr(5, 2).c_str())),
                                 static_cast<unsigned>(_wtoi(t.substr(8, 2).c_str())));
        };
        int64_t d0 = day(pendingDateRange_.substr(0, 10)), d1 = day(pendingDateRange_.substr(12, 10));
        ApplyDateSpan(ChipId::Date, d0 * 86400 - tzOffset_, (d1 + 1) * 86400 - tzOffset_);
        pendingDateRange_.clear();
    }
    if (!pendingTerm_.empty()) {   // Dokumentation: Begriff als aktive Pille
        std::wstring t = ToLower(pendingTerm_);
        pendingTerm_.clear();
        for (uint32_t id = 0; id < dict_.Size(); ++id)
            if (dict_.Word(id) == t) { filters_.terms.push_back(id); Refilter(); break; }
    }
    if (!pendingSearch_.empty()) {
        std::wstring q = pendingSearch_;
        pendingSearch_.clear();
        EditSearch(q, q.size());
    }
    if (pendingSearchOpen_) {
        pendingSearchOpen_ = false;
        searchFocus_ = true;
        caret_ = search_.size();
        showHistory_ = true;
    }
    if (pendingChip_ >= 0 || pendingRename_ || pendingDatePicker_) SetTimer(hwnd_, kTimerDocMenu, 1500, nullptr);
    if (!pendingViewer_.empty()) {
        auto it = byPath_.find(pendingViewer_);
        pendingViewer_.clear();
        if (it != byPath_.end())
            for (size_t i = 0; i < visible_.size(); ++i)
                if (visible_[i] == it->second) { OpenViewer(static_cast<int>(i)); break; }
        if (pendingCrop_ && viewer_.open) {   // nur fuer Dokumentation: Zuschnitt mit sichtbarem Rahmen
            EnterCrop(true);
            viewer_.cropN = D2D1::RectF(0.14f, 0.12f, 0.80f, 0.90f);
        }
        pendingCrop_ = false;
        if (pendingZoom_ > 0 && viewer_.open) viewer_.zoom = pendingZoom_;   // Dokumentation/Test: Zoomstufe
        pendingZoom_ = 0;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnThumbResults() {
    for (auto& res : thumbs_.Take()) {
        if (res.job == ThumbJob::SvgView) {   // scharfer SVG-Ausschnitt fuer den Betrachter
            if (res.key == svgPendingKey_) {
                svgPendingKey_.clear();
                svgDetail_ = svgPending_;
                if (res.ok) svgDetail_.bmp = r_.CreateBitmap(res.px);
            }
            continue;
        }
        if (res.job == ThumbJob::Full) {
            auto it = fullBmps_.find(res.key);
            if (it == fullBmps_.end()) continue;   // nicht mehr benoetigt
            Bmp& b = it->second;
            if (res.ok) {
                b.bmp = r_.CreateBitmap(res.px);
                b.srcW = res.px.srcW;
                b.srcH = res.px.srcH;
            }
            b.failed = !b.bmp;
            continue;
        }
        Bmp& b = bmps_[res.key];
        if (b.bmp) { bmpBytes_ -= b.bytes; b.bmp.Reset(); }
        if (res.ok) {
            b.bmp = r_.CreateBitmap(res.px);
            b.srcW = res.px.srcW;
            b.srcH = res.px.srcH;
            b.bytes = b.bmp ? size_t(res.px.w) * res.px.h * 4 : 0;
            bmpBytes_ += b.bytes;
        }
        b.failed = !b.bmp;
        b.lastUse = frame_;
        if (res.hasMeta) {
            auto it = byPath_.find(res.path);
            if (it != byPath_.end()) {
                FileEntry& e = entries_[it->second];
                if (!e.durationMs) e.durationMs = res.meta.durationMs;
                if (!e.width) { e.width = res.meta.width; e.height = res.meta.height; }
            }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnMetaResults() {
    bool changed = false;
    for (auto& r : meta_.Take()) {
        if (r.gen != gen_) continue;
        auto it = byPath_.find(r.path);
        if (it == byPath_.end()) continue;
        FileEntry& e = entries_[it->second];
        if (r.cover) {
            e.coverChecked = true;
            if (r.ok) {
                e.coverPath = r.coverPath;
                e.coverSize = r.coverSize;
                e.coverMtime = r.coverMtime;
                e.coverKind = r.coverKind;
            }
        } else {
            e.metaDone = true;
            --metaPending_;
            if (r.ok) {
                e.width = r.row.width;
                e.height = r.row.height;
                if (e.Doc()) e.pages = r.row.durationMs;   // Dokumente: Seitenzahl
                else if (r.row.durationMs) e.durationMs = r.row.durationMs;
                e.taken = r.row.taken;
                e.camera = r.row.camera;
                e.cameraLower = FoldForMatch(ToLower(e.camera));
            }
            changed = true;
        }
    }
    if (metaHold_ && !loading_ && metaPending_ <= 0) { ReleaseMetaHold(); return; }
    if (changed && NeedsMeta() && !metaHold_) ScheduleRefilter(400);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ReleaseMetaHold() {
    if (!metaHold_) return;
    metaHold_ = false;
    KillTimer(hwnd_, kTimerMetaHold);
    if (!loading_) {
        Refilter();
        if (focus_ >= 0) ScrollToPos(focus_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Ordner hat sich auf der Platte geaendert: neu einlesen, ohne die Ansicht zu leeren oder zu springen
void App::QuietReload() {
    if (collectionsView_ || bookmarksView_ || path_.empty()) { Reload(true); return; }
    ++gen_;
    meta_.SetGeneration(gen_);
    quietLoad_ = true;
    stage_.clear();
    stageIdx_.clear();
    loader_.LoadFolder(path_, settings_.recursive, settings_.showHidden, gen_);
}

void App::FinishQuietReload() {
    quietLoad_ = false;
    // Zustand merken: Auswahl, Fokus, Ansichtsviertel (oberstes sichtbares Element und Abstand)
    std::vector<std::wstring> sel = SelectedPaths();
    std::wstring focusPath = (focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) ? entries_[visible_[focus_]].path : L"";
    std::wstring anchorPath, viewerPath;
    float anchorOff = 0;
    if (!visible_.empty() && rowTop_.size() > 1) {
        int row = RowAtY(scroll_);
        int p = FirstPosOfRow(row);
        if (p >= 0 && p < static_cast<int>(visible_.size())) {
            anchorPath = entries_[visible_[p]].path;
            anchorOff = scroll_ - rowTop_[row];
        }
    }
    if (viewer_.open && viewer_.pos >= 0 && viewer_.pos < static_cast<int>(visible_.size()))
        viewerPath = entries_[visible_[viewer_.pos]].path;

    // Unveraenderte Eintraege samt Metadaten uebernehmen
    std::vector<FileEntry> next;
    next.reserve(stage_.size());
    for (auto& e : stage_) {
        auto it = byPath_.find(e.path);
        if (it != byPath_.end()) {
            const FileEntry& o = entries_[it->second];
            if (o.mtime == e.mtime && o.size == e.size) { next.push_back(o); continue; }
            bmps_.erase(e.path);   // Datei geaendert (z. B. zugeschnitten): Vorschaubild neu laden
            fullBmps_.erase(e.path);
        }
        PrepareEntry(e);
        next.push_back(std::move(e));
    }
    stage_.clear();
    stageIdx_.clear();
    entries_ = std::move(next);
    byPath_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) byPath_[entries_[i].path] = static_cast<uint32_t>(i);
    selected_.clear();
    focus_ = anchor_ = -1;
    hoverPos_ = -1;
    // Betrachter: Position zeigt noch in die alte Liste -> waehrend Refilter ausblenden (sonst schliesst er sich),
    // danach ueber den Pfad wiederherstellen
    if (viewer_.open) viewer_.pos = -1;
    metaPending_ = 0;
    RequestMeta(0);
    Refilter();
    for (auto& p : sel) {
        auto it = byPath_.find(p);
        if (it != byPath_.end()) selected_.insert(it->second);
    }
    auto posOf = [&](const std::wstring& path) {
        auto it = byPath_.find(path);
        if (it == byPath_.end()) return -1;
        for (size_t i = 0; i < visible_.size(); ++i)
            if (visible_[i] == it->second) return static_cast<int>(i);
        return -1;
    };
    if (!focusPath.empty()) focus_ = anchor_ = posOf(focusPath);
    Layout();
    int ap = anchorPath.empty() ? -1 : posOf(anchorPath);
    if (ap >= 0) {
        int row = RowOfPos(ap);
        if (row + 1 < static_cast<int>(rowTop_.size())) {
            float gh = gridRc_.bottom - gridRc_.top;
            float maxScroll = std::max(0.0f, contentH_ - gh);
            scroll_ = scrollTarget_ = std::clamp(rowTop_[row] + anchorOff, 0.0f, maxScroll);
        }
    }
    if (viewer_.open && !viewerSwitchTo_.empty()) {
        // nach dem Zuschneiden: das gespeicherte Bild zeigen (Original bzw. Kopie)
        int sp = posOf(viewerSwitchTo_);
        if (sp >= 0) {
            viewerPath = viewerSwitchTo_;
            viewerSwitchTo_.clear();
            viewer_.animS = 0;
        }
    }
    if (viewer_.open) {
        int vp = viewerPath.empty() ? -1 : posOf(viewerPath);
        if (vp >= 0) viewer_.pos = vp;
        else {
            // aktuelles Bild verschwunden: letztes vorhandenes Bild zeigen, Betrachter nie selbst schliessen
            viewer_.pos = -1;
            for (int p = static_cast<int>(visible_.size()) - 1; p >= 0; --p)
                if (entries_[visible_[p]].kind == Kind::Image && !entries_[visible_[p]].offline) { viewer_.pos = p; break; }
            viewer_.animS = 0;
        }
    }
    if (viewer_.open) ViewerPrefetch();   // volles Bild der (evtl. neuen) Position laden
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Filter, Sortierung, Facetten
// ---------------------------------------------------------------------------

void App::ScheduleRefilter(DWORD delayMs) {
    refilterPending_ = true;
    SetTimer(hwnd_, kTimerRefilter, delayMs, nullptr);
}

bool App::NeedsMeta() const {
    return filters_.date || filters_.taken || filters_.res || filters_.orient || settings_.sortKey == 1 ||
           settings_.sortKey == 3 || settings_.sortKey == 5 || query_.taken.active() ||
           query_.width.active() || query_.height.active() || query_.megapixel.active() || query_.date.active() ||
           query_.durationSec.active() || !query_.cams.empty() || !query_.terms.empty();
}

void App::ComputeDateBounds() {
    now_ = NowUnix();
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    // Versatz Ortszeit -> UTC aus aktueller Zeit bestimmen
    int64_t localDays = DaysFromCivil(lt.wYear, lt.wMonth, lt.wDay);
    int64_t localNow = localDays * 86400 + lt.wHour * 3600 + lt.wMinute * 60 + lt.wSecond;
    int64_t offset = localNow - now_;   // Sekunden, um die Ortszeit vor UTC liegt
    tzOffset_ = offset;
    todayStart_ = localDays * 86400 - offset;
    yearStart_ = DaysFromCivil(lt.wYear, 1, 1) * 86400 - offset;
    lastYearStart_ = DaysFromCivil(lt.wYear - 1, 1, 1) * 86400 - offset;
}

bool App::MatchOption(ChipId id, int v, const FileEntry& e) const {
    switch (id) {
    case ChipId::Type:
        if (v < 0) return true;
        if (v == 13) return e.ext == L"svg";                         // 13 SVG (Teil der Bilder)
        if (v >= 10) return e.Doc() == v - 9;                       // 10 PDF, 11 Photoshop, 12 Illustrator
        if (v == static_cast<int>(Kind::Other)) return e.kind == Kind::Other && !e.Doc();
        return static_cast<int>(e.kind) == v;
    case ChipId::Date:
    case ChipId::Taken: {
        if (!v) return true;
        if (e.kind == Kind::Folder) return false;
        int64_t t = id == ChipId::Taken ? e.taken : e.Time();
        if (id == ChipId::Taken && !t) return false;
        switch (v) {
        case 7:
            return id == ChipId::Taken ? (t >= filters_.takenFrom && t < filters_.takenTo)
                                       : (t >= filters_.dateFrom && t < filters_.dateTo);
        case 1: return t >= todayStart_;
        case 2: return t >= now_ - 7 * 86400;
        case 3: return t >= now_ - 30 * 86400;
        case 4: return t >= yearStart_;
        case 5: return t >= lastYearStart_ && t < yearStart_;
        case 6: return t < lastYearStart_;
        }
        return true;
    }
    case ChipId::Size: {
        if (!v) return true;
        if (e.kind == Kind::Folder) return false;
        const uint64_t MB = 1024ull * 1024;
        switch (v) {
        case 1: return e.size < MB;
        case 2: return e.size >= MB && e.size < 10 * MB;
        case 3: return e.size >= 10 * MB && e.size < 100 * MB;
        case 4: return e.size >= 100 * MB;
        }
        return true;
    }
    case ChipId::Resolution: {
        if (!v) return true;
        if (!e.width || !e.height) return false;
        uint32_t m = std::max(e.width, e.height);
        switch (v) {
        case 1: return m < 1280;
        case 2: return m >= 1280;
        case 3: return m >= 1920;
        case 4: return m >= 3840;
        case 5: return double(e.width) * e.height > 20e6;
        }
        return true;
    }
    case ChipId::Orientation: {
        if (!v) return true;
        if (!e.width || !e.height) return false;
        double w = e.width, h = e.height;
        switch (v) {
        case 1: return w > h * 1.05;
        case 2: return h > w * 1.05;
        case 3: return w <= h * 1.05 && h <= w * 1.05;
        case 4: return std::max(w, h) >= 2.0 * std::min(w, h);
        }
        return true;
    }
    default:
        return true;
    }
}

// skip: Nummer eines ChipId, der ignoriert wird; -2 = Begriffe ignorieren; -1 = nichts ignorieren
bool App::PassFilters(const FileEntry& e, int skip) const {
    if (e.gone) return false;
    if (skip != static_cast<int>(ChipId::Type) && !MatchOption(ChipId::Type, filters_.type, e)) return false;
    if (skip != static_cast<int>(ChipId::Date) && !MatchOption(ChipId::Date, filters_.date, e)) return false;
    if (skip != static_cast<int>(ChipId::Taken) && !MatchOption(ChipId::Taken, filters_.taken, e)) return false;
    if (skip != static_cast<int>(ChipId::Size) && !MatchOption(ChipId::Size, filters_.size, e)) return false;
    if (skip != static_cast<int>(ChipId::Resolution) && !MatchOption(ChipId::Resolution, filters_.res, e)) return false;
    if (skip != static_cast<int>(ChipId::Orientation) && !MatchOption(ChipId::Orientation, filters_.orient, e))
        return false;
    if (skip != static_cast<int>(ChipId::Source) && !filters_.source.empty() && e.source != filters_.source) return false;
    if (skip != static_cast<int>(ChipId::Pattern) && filters_.templ != UINT32_MAX && e.templateId != filters_.templ)
        return false;
    if (skip != -2) {
        for (uint32_t t : filters_.terms)
            if (std::find(e.tokens.begin(), e.tokens.end(), t) == e.tokens.end()) return false;
    }
    if (!query_.Empty()) {
        MatchInput mi;
        mi.name = e.nameLower;
        mi.folder = e.folderLower;
        mi.ext = e.ext;
        mi.camera = e.cameraLower;
        mi.kind = e.kind;
        mi.width = e.width;
        mi.height = e.height;
        mi.size = e.size;
        mi.time = e.Time();
        mi.taken = e.taken;
        mi.durationSec = e.durationMs / 1000.0;
        if (!Matches(query_, mi)) return false;
    }
    return true;
}

void App::SortVisible() {
    const int key = settings_.sortKey;
    const bool desc = settings_.sortDesc;
    auto& E = entries_;
    std::sort(visible_.begin(), visible_.end(), [&](uint32_t a, uint32_t b) {
        const FileEntry& x = E[a];
        const FileEntry& y = E[b];
        bool xf = x.kind == Kind::Folder, yf = y.kind == Kind::Folder;
        if (xf != yf) return xf;   // Ordner immer zuerst
        int c = 0;
        switch (key) {
        case 1: c = x.Time() < y.Time() ? -1 : (x.Time() > y.Time() ? 1 : 0); break;
        case 2: c = x.size < y.size ? -1 : (x.size > y.size ? 1 : 0); break;
        case 3: {
            uint64_t px = uint64_t(x.width) * x.height, py = uint64_t(y.width) * y.height;
            c = px < py ? -1 : (px > py ? 1 : 0);
            break;
        }
        case 4: c = x.ext.compare(y.ext); break;
        case 5:
            // Aufgenommen: Dateien ohne Aufnahmedatum immer am Ende
            if ((x.taken != 0) != (y.taken != 0)) return x.taken != 0;
            c = x.taken < y.taken ? -1 : (x.taken > y.taken ? 1 : 0);
            break;
        default: break;
        }
        if (c == 0) c = StrCmpLogicalW(x.name.c_str(), y.name.c_str());
        if (c == 0) return a < b;
        return desc ? c > 0 : c < 0;
    });
}

void App::UpdateFacets() {
    std::vector<const std::vector<uint32_t>*> docs;
    std::unordered_map<std::wstring, uint32_t> src;
    std::unordered_map<uint32_t, uint32_t> tpl;
    for (auto& e : entries_) {
        if (e.kind == Kind::Folder) continue;
        if (PassFilters(e, -2)) docs.push_back(&e.tokens);
        if (!e.source.empty() && PassFilters(e, static_cast<int>(ChipId::Source))) ++src[e.source];
        if (e.templateId != UINT32_MAX && PassFilters(e, static_cast<int>(ChipId::Pattern))) ++tpl[e.templateId];
    }
    termFacets_ = ComputeFacets(docs, 40, 3, 0.8);
    sourceFacets_.assign(src.begin(), src.end());
    std::sort(sourceFacets_.begin(), sourceFacets_.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    templFacets_.clear();
    for (auto& [id, n] : tpl) {
        const std::wstring& t = templDict_.Word(id);
        if (n >= 5 && (t.find(L'#') != std::wstring::npos || t.find(L'{') != std::wstring::npos))
            templFacets_.push_back({id, n});
    }
    std::sort(templFacets_.begin(), templFacets_.end(), [](const Facet& a, const Facet& b) { return a.count > b.count; });
    if (templFacets_.size() > 20) templFacets_.resize(20);
}

void App::Refilter() {
    refilterPending_ = false;
    KillTimer(hwnd_, kTimerRefilter);
    ComputeDateBounds();
    int focusIdx = (focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) ? static_cast<int>(visible_[focus_]) : -1;
    int viewerIdx = (viewer_.open && viewer_.pos >= 0 && viewer_.pos < static_cast<int>(visible_.size()))
                        ? static_cast<int>(visible_[viewer_.pos])
                        : -1;
    visible_.clear();
    ++visibleVersion_;
    visible_.reserve(entries_.size());
    for (uint32_t i = 0; i < entries_.size(); ++i)
        if (PassFilters(entries_[i], -1)) visible_.push_back(i);
    SortVisible();

    // Auswahl auf sichtbare Eintraege beschraenken, Fokus erhalten
    std::vector<char> vis(entries_.size(), 0);
    for (uint32_t i : visible_) vis[i] = 1;
    for (auto it = selected_.begin(); it != selected_.end();) {
        if (*it >= vis.size() || !vis[*it]) it = selected_.erase(it);
        else ++it;
    }
    focus_ = anchor_ = -1;
    for (size_t p = 0; p < visible_.size(); ++p) {
        if (static_cast<int>(visible_[p]) == focusIdx) focus_ = anchor_ = static_cast<int>(p);
        if (static_cast<int>(visible_[p]) == viewerIdx) viewer_.pos = static_cast<int>(p);
    }
    if (viewer_.open && viewerIdx >= 0 && (viewer_.pos < 0 || viewer_.pos >= static_cast<int>(visible_.size()) ||
                                           static_cast<int>(visible_[viewer_.pos]) != viewerIdx))
        CloseViewer();
    UpdateFacets();
    Layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Bilder und Caches
// ---------------------------------------------------------------------------

Bmp& App::BmpFor(const std::wstring& key) { return bmps_[key]; }

std::wstring App::IconKey(const FileEntry& e) const {
    if (e.kind == Kind::Folder) return L"icon:\\folder";
    if (!e.offline && (e.ext == L"exe" || e.ext == L"lnk" || e.ext == L"ico")) return L"iconf:" + e.path;
    return L"icon:." + e.ext;
}

void App::EnsureIcon(const FileEntry& e, int prio) {
    std::wstring key = IconKey(e);
    Bmp& b = bmps_[key];
    b.lastUse = frame_;
    if (b.bmp || b.failed || (b.requested && b.reqPrio <= prio)) return;
    b.requested = true;
    b.reqPrio = prio;
    ThumbRequest r;
    r.key = key;
    r.job = ThumbJob::Icon;
    r.prio = prio;
    r.gen = 0;
    if (e.kind == Kind::Folder) { r.path = L"folder"; r.isDir = true; }
    else if (key.rfind(L"iconf:", 0) == 0) { r.path = e.path; r.perFile = true; }
    else r.path = L"datei." + e.ext;   // lang:ok
    thumbs_.Request(std::move(r));
}

void App::EnsureThumb(const FileEntry& e, int prio, uint64_t frame) {
    if (!e.HasThumb() || e.offline) { EnsureIcon(e, prio); return; }
    Bmp& b = bmps_[e.path];
    b.lastUse = frame;
    if (b.failed) { EnsureIcon(e, prio); return; }
    if (b.bmp || (b.requested && b.reqPrio <= prio)) return;
    b.requested = true;
    b.reqPrio = prio;
    ThumbRequest r;
    r.key = e.path;
    r.path = e.path;
    r.kind = e.kind;
    r.job = ThumbJob::Thumb;
    r.size = e.size;
    r.mtime = e.mtime;
    r.prio = prio;
    r.gen = gen_;
    thumbs_.Request(std::move(r));
}

void App::TrimCaches() {
    const size_t budget = 512ull * 1024 * 1024, target = 384ull * 1024 * 1024;
    if (bmpBytes_ <= budget) return;
    std::vector<std::pair<uint64_t, const std::wstring*>> order;
    for (auto& [k, b] : bmps_)
        if (b.bmp && b.lastUse + 2 < frame_) order.push_back({b.lastUse, &k});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [use, key] : order) {
        if (bmpBytes_ <= target) break;
        Bmp& b = bmps_[*key];
        bmpBytes_ -= b.bytes;
        b.bmp.Reset();
        b.bytes = 0;
        b.requested = false;
        b.reqPrio = 9;
    }
}

void App::ClearBitmaps() {
    CloudReleaseGpu();
    appIcons_.clear();
    bmps_.clear();
    fullBmps_.clear();
    bmpBytes_ = 0;
}

// ---------------------------------------------------------------------------
// Oeffnen und Betrachter
// ---------------------------------------------------------------------------

void App::OpenPos(int pos) {
    if (pos < 0 || pos >= static_cast<int>(visible_.size())) return;
    const FileEntry& e = entries_[visible_[pos]];
    if (e.kind == Kind::Folder) {
        Navigate(e.path);
    } else if (e.kind == Kind::Image && !e.offline) {
        OpenViewer(pos);
    } else {
        // Videos und alle anderen Dateien: Standard-App (z. B. Standard-Mediaplayer)
        if (!OpenWithDefaultApp(hwnd_, e.path)) SetStatus(T(L"Datei konnte nicht geöffnet werden."));
    }
}

void App::OpenViewer(int pos) {
    viewer_ = ViewerState{};
    viewer_.open = true;
    viewer_.pos = pos;
    viewer_.bar = settings_.viewerBar;                  // Leiste oben und Infobereich wie beim letzten Mal
    infoAnim_ = settings_.viewerInfo ? 1.0f : 0.0f;     // ohne Einfahren
    searchFocus_ = false;
    if (settingsOpen_) settingsOpen_ = false;
    // Betrachter ohne Titelleiste: randloses Vollbild auf dem aktuellen Bildschirm
    if (!fullscreen_) {
        ToggleFullscreen();
        viewer_.ownFullscreen = fullscreen_;
    }
    viewer_.controlsUntil = GetTickCount() + 2000;
    SetTimer(hwnd_, kTimerViewerUi, 2100, nullptr);
    indexer_.Pause(true);
    ViewerPrefetch();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseViewer() {
    if (!viewer_.open) return;
    if (reloadAfterViewer_) {
        reloadAfterViewer_ = false;
        PostMessageW(hwnd_, WM_TIMER, kTimerReload, 0);
    }
    int pos = viewer_.pos;
    bool ownFs = viewer_.ownFullscreen;
    viewer_.open = false;
    infoAnim_ = settings_.infoPanel ? 1.0f : 0.0f;      // Infobereich der Ansicht wiederherstellen (ohne Animation)
    settings_.Save();                                   // Betrachter-Leisten merken
    KillTimer(hwnd_, kTimerViewerClick);
    if (ownFs && fullscreen_) ToggleFullscreen();
    fullBmps_.clear();
    indexer_.Pause(false);
    if (pos >= 0 && pos < static_cast<int>(visible_.size())) {
        focus_ = anchor_ = pos;
        selected_.clear();
        selected_.insert(visible_[pos]);
        ScrollToPos(pos);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ViewerStep(int delta) {
    if (!viewer_.open) return;
    if (viewer_.crop) CancelCrop();
    int n = static_cast<int>(visible_.size());
    int p = viewer_.pos;
    for (;;) {
        p += delta > 0 ? 1 : -1;
        if (p < 0 || p >= n) return;
        const FileEntry& e = entries_[visible_[p]];
        if (e.kind == Kind::Image && !e.offline) break;
    }
    viewer_.pos = p;
    viewer_.zoom = 0;
    viewer_.pan = {0, 0};
    viewer_.rotation = 0;
    viewer_.animS = 0;   // neues Bild: ohne Zoom-Animation
    ViewerPrefetch();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Entf im Betrachter: aktuelle Datei in den Papierkorb, weiter zum naechsten Bild, kurzer Hinweis oben
void App::ViewerDeleteCurrent() {
    int n = static_cast<int>(visible_.size());
    if (!viewer_.open || viewer_.pos < 0 || viewer_.pos >= n) return;
    if (viewer_.crop) CancelCrop();
    uint32_t idx = visible_[viewer_.pos];
    std::wstring path = entries_[idx].path, name = entries_[idx].name;
    if (!RecycleFiles(hwnd_, {path})) return;
    entries_[idx].gone = true;
    selected_.erase(idx);
    int pos = viewer_.pos;
    visible_.erase(visible_.begin() + pos);
    ++visibleVersion_;
    focus_ = anchor_ = -1;
    fullBmps_.erase(path);
    // naechstes Bild (sonst vorheriges); keins mehr -> Betrachter schliessen
    int next = -1;
    for (int p = pos; p < static_cast<int>(visible_.size()); ++p)
        if (entries_[visible_[p]].kind == Kind::Image && !entries_[visible_[p]].offline) { next = p; break; }
    if (next < 0)
        for (int p = pos - 1; p >= 0; --p)
            if (entries_[visible_[p]].kind == Kind::Image && !entries_[visible_[p]].offline) { next = p; break; }
    // kein Bild mehr: Betrachter bleibt offen und zeigt einen Hinweis
    viewer_.pos = next;
    viewer_.zoom = 0;
    viewer_.pan = {0, 0};
    viewer_.rotation = 0;
    viewer_.animS = 0;
    viewer_.toast = TF(L"\u201e{0}\u201c in den Papierkorb verschoben", {name});
    viewer_.toastUntil = GetTickCount() + 2200;
    SetTimer(hwnd_, kTimerToast, 2300, nullptr);
    ViewerPrefetch();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ViewerPrefetch() {
    int n = static_cast<int>(visible_.size());
    if (viewer_.pos < 0 || viewer_.pos >= n) return;
    std::vector<std::pair<int, int>> wanted;   // (pos, prio)
    wanted.push_back({viewer_.pos, 0});
    for (int dir : {1, -1}) {
        int found = 0;
        for (int p = viewer_.pos + dir; p >= 0 && p < n && found < 2; p += dir) {
            const FileEntry& e = entries_[visible_[p]];
            if (e.kind == Kind::Image && !e.offline) {
                wanted.push_back({p, 1});
                ++found;
            }
        }
    }
    std::unordered_set<std::wstring> keep;
    for (auto& [p, prio] : wanted) {
        const FileEntry& e = entries_[visible_[p]];
        keep.insert(e.path);
        EnsureThumb(e, prio, frame_);
        auto it = fullBmps_.find(e.path);
        if (it != fullBmps_.end()) continue;
        fullBmps_[e.path].requested = true;
        ThumbRequest r;
        r.key = e.path;
        r.path = e.path;
        r.kind = e.kind;
        r.job = ThumbJob::Full;
        r.prio = prio;
        r.gen = gen_;
        thumbs_.Request(std::move(r));
    }
    for (auto it = fullBmps_.begin(); it != fullBmps_.end();) {
        if (!keep.count(it->first)) it = fullBmps_.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------------------
// Sammlungen, Seitenleiste
// ---------------------------------------------------------------------------

void App::AddCollection(const std::wstring& raw) {
    std::wstring p = NormalizeDir(raw);
    if (!IsLocalDrivePath(p) || !DirectoryExists(p)) {
        SetStatus(T(L"Nur lokale Ordner können Sammlungen sein."));
        return;
    }
    for (auto& c : settings_.collections)
        if (_wcsicmp(c.c_str(), p.c_str()) == 0) { SetStatus(T(L"Ist bereits eine Sammlung.")); return; }
    settings_.collections.push_back(p);
    settings_.Save();
    indexer_.SetRoots(settings_.collections, settings_.showHidden);
    BuildSidebar();
    SetStatus(TF(L"Sammlung hinzugefügt: {0} – wird im Hintergrund indexiert.", {p}));
}

void App::RemoveCollection(const std::wstring& p) {
    auto& c = settings_.collections;
    auto it = std::find_if(c.begin(), c.end(), [&](const std::wstring& x) { return _wcsicmp(x.c_str(), p.c_str()) == 0; });
    if (it == c.end()) return;
    c.erase(it);
    settings_.Save();
    indexer_.SetRoots(settings_.collections, settings_.showHidden);
    db_.DeleteUnder(p);
    BuildSidebar();
    if (collectionsView_) Reload(false);
    SetStatus(T(L"Sammlung entfernt: ") + p);
}

void App::PickFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(T(L"Ordner als Sammlung hinzufügen"));
    if (!collectionsView_ && !path_.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(path_.c_str(), nullptr, IID_PPV_ARGS(&folder))))
            dlg->SetFolder(folder.Get());
    }
    if (FAILED(dlg->Show(hwnd_))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;
    PWSTR p = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        AddCollection(p);
        CoTaskMemFree(p);
    }
}

void App::BuildSidebar() {
    bookmarkSet_.clear();
    for (auto& b : settings_.bookmarks) bookmarkSet_.insert(ToLower(b));
    sidebar_.clear();
    sidebar_.push_back({SidebarItem::Header, T(L"Schnellzugriff"), L"", 0});
    sidebar_.push_back({SidebarItem::Bookmarks,
                        T(L"Merkliste") + (settings_.bookmarks.empty() ? std::wstring()
                                                                    : L"  (" + std::to_wstring(settings_.bookmarks.size()) + L")"),
                        L"", 0xE734});
    struct KF { const KNOWNFOLDERID* id; const wchar_t* label; wchar_t glyph; };
    const KF kfs[] = {{&FOLDERID_Desktop, L"Desktop", 0xE8B7},
                      {&FOLDERID_Downloads, L"Downloads", 0xE896},
                      {&FOLDERID_Pictures, TC(L"sidebar:Bilder", L"Bilder"), 0xE8B9},
                      {&FOLDERID_Videos, L"Videos", 0xE714},
                      {&FOLDERID_Documents, T(L"Dokumente"), 0xE8A5}};
    for (auto& k : kfs) {
        std::wstring p = KnownFolderPath(*k.id);
        if (!p.empty() && IsLocalDrivePath(p)) sidebar_.push_back({SidebarItem::Folder, k.label, p, k.glyph});
    }
    sidebar_.push_back({SidebarItem::Header, T(L"Sammlungen"), L"", 0});
    if (!settings_.collections.empty())
        sidebar_.push_back({SidebarItem::AllCollections, T(L"Alle Sammlungen"), L"", 0xE8F1});
    for (auto& c : settings_.collections) {
        std::wstring n = FileNameOf(c);
        sidebar_.push_back({SidebarItem::Collection, n.empty() ? c : n, c, 0xE8B7});
    }
    sidebar_.push_back({SidebarItem::Header, T(L"Laufwerke"), L"", 0});
    for (auto& d : LocalDrives()) {
        wchar_t label[MAX_PATH + 1] = {};
        GetVolumeInformationW(d.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        std::wstring name = label[0] ? std::wstring(label) : std::wstring(T(L"Lokaler Datenträger"));
        name += L" (" + d.substr(0, 2) + L")";
        bool removable = GetDriveTypeW(d.c_str()) == DRIVE_REMOVABLE;
        sidebar_.push_back({SidebarItem::Drive, name, d, static_cast<wchar_t>(removable ? 0xE88E : 0xEDA2)});
    }
}

// ---------------------------------------------------------------------------
// Auswahl, Dateiaktionen, Vollbild, Drag & Drop
// ---------------------------------------------------------------------------

std::vector<std::wstring> App::SelectedPaths() const {
    std::vector<std::wstring> out;
    for (uint32_t idx : visible_)
        if (selected_.count(idx)) out.push_back(entries_[idx].path);
    if (out.empty() && focus_ >= 0 && focus_ < static_cast<int>(visible_.size()))
        out.push_back(entries_[visible_[focus_]].path);
    return out;
}

void App::DeleteSelection() {
    auto paths = SelectedPaths();
    if (paths.empty()) return;
    if (RecycleFiles(hwnd_, paths)) Reload(false);
}

void App::ToggleFullscreen() {
    if (viewer_.open) viewer_.ownFullscreen = false;   // Benutzer hat selbst umgeschaltet
    DWORD style = static_cast<DWORD>(GetWindowLongW(hwnd_, GWL_STYLE));
    if (!fullscreen_) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetWindowPlacement(hwnd_, &prevPlacement_) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            settings_.window = prevPlacement_.rcNormalPosition;
            settings_.hasWindow = true;
            GetWindowRect(hwnd_, &prevRect_);
            SetWindowLongW(hwnd_, GWL_STYLE, static_cast<LONG>(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen_ = true;
        }
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, static_cast<LONG>(style | WS_OVERLAPPEDWINDOW));
        if (prevPlacement_.showCmd == SW_SHOWMAXIMIZED) {
            SetWindowPlacement(hwnd_, &prevPlacement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        } else {
            // Angedockte (Snap-)Fenster: genau die vorherige Lage wiederherstellen, nicht die "normale" Position
            SetWindowPos(hwnd_, nullptr, prevRect_.left, prevRect_.top, prevRect_.right - prevRect_.left,
                         prevRect_.bottom - prevRect_.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        fullscreen_ = false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

DWORD App::CrumbDropEffect(const std::vector<std::wstring>& paths, const std::wstring& target, DWORD keys,
                            DWORD allowed) const {
    if (!IsLocalDrivePath(target)) return DROPEFFECT_NONE;
    std::wstring tl = ToLower(target);
    while (tl.size() > 3 && tl.back() == L'\\') tl.pop_back();
    for (auto& p : paths) {
        if (!IsLocalDrivePath(p)) return DROPEFFECT_NONE;
        std::wstring pl = ToLower(p);
        if (ToLower(ParentPath(p)) == tl) return DROPEFFECT_NONE;                 // liegt schon dort
        if (tl == pl || (tl.size() > pl.size() && tl.compare(0, pl.size(), pl) == 0 && tl[pl.size()] == L'\\'))
            return DROPEFFECT_NONE;                                               // Ordner in sich selbst
    }
    DWORD e;
    if (keys & MK_CONTROL) e = DROPEFFECT_COPY;
    else if (keys & MK_SHIFT) e = DROPEFFECT_MOVE;
    else e = (towupper(paths.front()[0]) == towupper(target[0])) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    if (!(e & allowed)) e = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_MOVE);
    return e;
}

DWORD App::OnDragOver(const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD allowed, bool leave) {
    if (leave) {
        if (dropCrumb_ != -1 || dropSidebar_ || dropBookmarks_ || dropPos_ != -1 || dropSideItem_ != -1) {
            dropCrumb_ = -1;
            dropSidebar_ = false;
            dropBookmarks_ = false;
            dropPos_ = -1;
            dropSideItem_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return DROPEFFECT_NONE;
    }
    if (viewer_.open || settingsOpen_ || sheet_.open || atlas_.open || compare_.open || paths.empty()) return DROPEFFECT_NONE;
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);
    float x = pt.x / r_.scale(), y = pt.y / r_.scale();
    int crumb = -1;
    bool side = false;
    DWORD eff = DROPEFFECT_NONE;
    const Hit* h = HitAt(x, y);
    bool onBookmarks = h && h->type == HitType::SidebarItem && sidebar_[h->a].kind == SidebarItem::Bookmarks;
    if (onBookmarks != dropBookmarks_) {
        dropBookmarks_ = onBookmarks;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    std::wstring folderTarget;
    int fPos = -1, fSide = -1;
    bool onFolder = !onBookmarks && FolderDropTarget(x, y, folderTarget, fPos, fSide);
    if (onFolder) {
        eff = CrumbDropEffect(paths, folderTarget, keys, allowed);
        if (eff == DROPEFFECT_NONE) fPos = fSide = -1;
    }
    if (fPos != dropPos_ || fSide != dropSideItem_) {
        dropPos_ = fPos;
        dropSideItem_ = fSide;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (onFolder) {
        // Ablegen auf einen Ordner (Ansicht oder Seitenleiste): dorthin verschieben bzw. kopieren
    } else if (onBookmarks) {
        eff = (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : (allowed & DROPEFFECT_COPY);
    } else if (h && h->type == HitType::Crumb && h->a >= 0 && h->a < static_cast<int>(crumbPaths_.size())) {
        // Ablegen auf ein Pfad-Element: dorthin kopieren oder verschieben
        crumb = h->a;
        eff = CrumbDropEffect(paths, crumbPaths_[h->a], keys, allowed);
    } else if (!ownDrag_) {
        if (SidebarWidth() > 1 && x < SidebarWidth() && y > topH_) {
            for (auto& p : paths)
                if (DirectoryExists(p)) { side = true; break; }
            eff = side ? DROPEFFECT_LINK : DROPEFFECT_NONE;
        } else {
            eff = DROPEFFECT_LINK;   // oeffnen
        }
        if (eff == DROPEFFECT_LINK && !(allowed & DROPEFFECT_LINK))
            eff = (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : (allowed & DROPEFFECT_MOVE);
    }
    if (crumb != dropCrumb_ || side != dropSidebar_) {
        dropCrumb_ = crumb;
        dropSidebar_ = side;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return eff;
}

DWORD App::OnDrop(const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD allowed) {
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);
    float x = pt.x / r_.scale(), y = pt.y / r_.scale();
    const Hit* h = HitAt(x, y);
    bool hadTarget = dropPos_ >= 0 || dropSideItem_ >= 0;
    dropPos_ = dropSideItem_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
    std::wstring folderTarget;
    int fPos = -1, fSide = -1;
    if (hadTarget && FolderDropTarget(x, y, folderTarget, fPos, fSide)) {
        DWORD e = CrumbDropEffect(paths, folderTarget, keys, allowed);
        if (e == DROPEFFECT_NONE) return e;
        fileOps_.push_back({paths, folderTarget, e == DROPEFFECT_MOVE});
        PostMessageW(hwnd_, WM_APP_FILEOP, 0, 0);
        return e;
    }
    if (h && h->type == HitType::SidebarItem && sidebar_[h->a].kind == SidebarItem::Bookmarks) {
        SetBookmarked(paths, true);
        return (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : DROPEFFECT_COPY;
    }
    if (h && h->type == HitType::Crumb && h->a >= 0 && h->a < static_cast<int>(crumbPaths_.size())) {
        DWORD e = CrumbDropEffect(paths, crumbPaths_[h->a], keys, allowed);
        if (e == DROPEFFECT_NONE) return e;
        // nach Rueckkehr aus Drop ausfuehren, damit die Quelle nicht blockiert
        fileOps_.push_back({paths, crumbPaths_[h->a], e == DROPEFFECT_MOVE});
        PostMessageW(hwnd_, WM_APP_FILEOP, 0, 0);
        return e;
    }
    if (ownDrag_) return DROPEFFECT_NONE;
    if (SidebarWidth() > 1 && x < SidebarWidth() && y > topH_) {
        for (auto& p : paths)
            if (DirectoryExists(p)) AddCollection(p);
        return DROPEFFECT_LINK & allowed ? DROPEFFECT_LINK : DROPEFFECT_COPY;
    }
    const std::wstring& first = paths.front();
    if (!IsLocalDrivePath(first)) {
        SetStatus(T(L"Nur lokale Dateien werden unterst\u00fctzt \u2013 Netzlaufwerke sind ausgeschlossen."));
        return DROPEFFECT_NONE;
    }
    if (DirectoryExists(first)) Navigate(first);
    else Navigate(ParentPath(first), true, first);
    SetForegroundWindow(hwnd_);
    return (allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : DROPEFFECT_COPY;
}

// Ordner unter dem Mauszeiger als Ablageziel: Ordner-Kachel/-Zeile der Ansicht oder Ordner der Seitenleiste
bool App::FolderDropTarget(float x, float y, std::wstring& target, int& pos, int& side) const {
    pos = side = -1;
    if (const Hit* h = HitAt(x, y)) {
        if (h->type == HitType::SidebarItem && h->a >= 0 && h->a < static_cast<int>(sidebar_.size())) {
            const SidebarItem& it = sidebar_[h->a];
            if ((it.kind == SidebarItem::Folder || it.kind == SidebarItem::Drive || it.kind == SidebarItem::Collection) &&
                !it.path.empty() && IsLocalDrivePath(it.path)) {
                target = it.path;
                side = h->a;
                return true;
            }
        }
        return false;
    }
    int p = GridPosAt(x, y);
    if (p < 0) return false;
    const FileEntry& e = entries_[visible_[p]];
    if (e.kind != Kind::Folder || e.offline) return false;
    target = e.path;
    pos = p;
    return true;
}

void App::PasteFiles(const std::wstring& target) {
    if (target.empty() || !IsLocalDrivePath(target)) {
        SetStatus(T(L"Einf\u00fcgen ist nur in einem Ordner m\u00f6glich."));
        return;
    }
    std::vector<std::wstring> paths;
    bool move = false;
    if (!FilesFromClipboard(hwnd_, paths, move)) {
        SetStatus(T(L"Keine Dateien in der Zwischenablage."));
        return;
    }
    std::vector<std::wstring> ok;
    for (auto& p : paths)
        if (IsLocalDrivePath(p)) ok.push_back(p);
    if (ok.empty()) {
        SetStatus(T(L"Nur lokale Dateien werden unterst\u00fctzt \u2013 Netzlaufwerke sind ausgeschlossen."));
        return;
    }
    // Ordner nicht in sich selbst einfuegen
    std::wstring tl = ToLower(target);
    for (auto& p : ok) {
        std::wstring pl = ToLower(p);
        if (tl == pl || (tl.size() > pl.size() && tl.compare(0, pl.size(), pl) == 0 && tl[pl.size()] == L'\\')) {
            SetStatus(T(L"Ein Ordner kann nicht in sich selbst eingef\u00fcgt werden."));
            return;
        }
    }
    // gleicher Ordner wie die Quelle: Windows legt "Kopie" an (nur beim Kopieren sinnvoll)
    fileOps_.push_back({ok, target, move});
    PostMessageW(hwnd_, WM_APP_FILEOP, 0, 0);
}

void App::DoPendingFileOp() {
    auto ops = std::move(fileOps_);
    fileOps_.clear();
    bool any = false;
    for (auto& op : ops) {
        if (CopyOrMoveFiles(hwnd_, op.paths, op.target, op.move)) {
            any = true;
            SetStatus(op.move ? TF(L"{0} Element(e) verschoben nach {1}", {std::to_wstring(op.paths.size()), op.target})
                              : TF(L"{0} Element(e) kopiert nach {1}", {std::to_wstring(op.paths.size()), op.target}));
        }
    }
    if (any) QuietReload();
}

std::wstring App::TypeName(const FileEntry& e) {
    if (e.kind == Kind::Folder) return T(L"Dateiordner");
    auto it = typeNames_.find(e.ext);
    if (it != typeNames_.end()) return it->second;
    std::wstring name;
    SHFILEINFOW sfi{};
    std::wstring probe = L"datei." + e.ext;   // lang:ok
    if (SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
        name = sfi.szTypeName;
    if (name.empty()) {
        std::wstring up = e.ext;
        for (auto& c : up) c = static_cast<wchar_t>(towupper(c));
        name = up.empty() ? std::wstring(T(L"Datei")) : TF(L"{0}-Datei", {up});
    }
    typeNames_[e.ext] = name;
    return name;
}

// ---------------------------------------------------------------------------
// Animationen
// ---------------------------------------------------------------------------

bool App::AnimateStep(float dt) {
    bool more = false;
    // bildratenunabhaengige Annaeherung: gleicher Verlauf bei 60, 120 oder 144 Hz
    const float kPanel = 1.0f - std::exp(-dt / 47.0f), kScroll = 1.0f - std::exp(-dt / 44.0f);
    const float step = dt / 15.6f;   // Bezug der frueheren Geschwindigkeiten (ein Timer-Tick)
    auto approach = [&](float& v, float target) {
        float d = target - v;
        if (std::fabs(d) < 0.002f) { v = target; return; }
        v += d * kPanel;
        more = true;
    };
    approach(sidebarAnim_, settings_.sidebar ? 1.0f : 0.0f);
    approach(infoAnim_, (viewer_.open ? settings_.viewerInfo : settings_.infoPanel) ? 1.0f : 0.0f);
    if (settings_.viewMode == 2 && CloudStep(dt)) more = true;

    float gh = gridRc_.bottom - gridRc_.top;
    float maxScroll = std::max(0.0f, contentH_ - gh);
    if (marquee_) {
        // Auswahlrechteck am Rand: automatisch weiterscrollen
        float my = marqueeMouse_.y / r_.scale();
        float d = 0;
        if (my < gridRc_.top) d = my - gridRc_.top;
        else if (my > gridRc_.bottom) d = my - gridRc_.bottom;
        if (d != 0) {
            scroll_ = scrollTarget_ = std::clamp(scroll_ + d * 0.3f * step, 0.0f, maxScroll);
            UpdateMarquee();
            more = true;
        }
    }
    if (autoScroll_) {
        float dy = autoMouseY_ - autoAnchor_.y;
        const float dead = 12;
        if (std::fabs(dy) > dead) {
            float v = std::fabs(dy) - dead;
            float speed = v * 0.06f + v * v * 0.0008f;
            scroll_ = std::clamp(scroll_ + (dy > 0 ? speed : -speed) * step, 0.0f, maxScroll);
        }
        scrollTarget_ = scroll_;
        more = true;
    } else {
        float d = scrollTarget_ - scroll_;
        if (std::fabs(d) < 0.25f) scroll_ = scrollTarget_;
        else { scroll_ += d * kScroll; more = true; }
    }
    return more;
}

void App::StopAutoScroll() {
    if (!autoScroll_) return;
    autoScroll_ = false;
    autoSticky_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Einstellungen
// ---------------------------------------------------------------------------

static uint64_t FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

void App::RefreshSettingsInfo() {
    dbStats_ = db_.GetStats();
    std::wstring d = DataDir();
    dbBytes_ = FileBytes(JoinPath(d, L"index.db")) + FileBytes(JoinPath(d, L"index.db-wal"));
    thumbBytes_ = FileBytes(JoinPath(d, L"thumbs.db")) + FileBytes(JoinPath(d, L"thumbs.db-wal"));
    collectionCounts_.clear();
    for (auto& c : settings_.collections) collectionCounts_.push_back({c, db_.CountUnder(c)});
}

void App::OpenSettings() {
    if (viewer_.open) CloseViewer();
    settingsOpen_ = true;
    settingsPage_ = 0;
    settingsScroll_ = 0;
    searchFocus_ = false;
    RefreshSettingsInfo();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseSettings() {
    if (settingsPage_ != 0) {   // Unterseite: zurueck zu den Einstellungen
        settingsPage_ = 0;
        settingsScroll_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    settingsOpen_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::DeleteIndex() {
    int r = MessageBoxW(hwnd_,
                        T(L"Index und Vorschaubild-Cache werden gel\u00f6scht.\n\n"
                        L"Ihre Dateien bleiben unver\u00e4ndert. Vorhandene Sammlungen werden anschlie\u00dfend im "
                        L"Hintergrund neu indexiert.\n\nFortfahren?"),
                        T(L"Index l\u00f6schen"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (r != IDYES) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    indexer_.SetRoots({}, settings_.showHidden);
    for (int i = 0; i < 100 && indexer_.Busy(); ++i) Sleep(50);
    db_.ClearAll();
    ClearBitmaps();
    indexer_.SetRoots(settings_.collections, settings_.showHidden);
    RefreshSettingsInfo();
    SetStatus(T(L"Index gel\u00f6scht."));
}

} // namespace peek
