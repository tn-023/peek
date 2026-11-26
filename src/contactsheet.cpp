// Kontaktabzug: Bilder als PDF-Uebersicht. WYSIWYG-Seite im Fenster: Ueberschrift direkt auf der Seite eintippen,
// Bilder auf der Seite aushaken und umsortieren, Bildstreifen unten mit allen Bildern. Zeichnen: App::PaintSheet.
#include "app.h"
#include "core/textutil.h"

#include <shobjidl.h>
#include <cmath>

namespace peek {

namespace {
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
bool CtrlDown() { return GetKeyState(VK_CONTROL) < 0; }
std::wstring Stem(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}
std::wstring Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return FormatDay(st.wYear, st.wMonth, st.wDay);
}
std::wstring SafeFileName(std::wstring s) {
    for (auto& c : s)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'-';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s.empty() ? std::wstring(T(L"Kontaktabzug")) : s;
}
}  // namespace

std::vector<int> App::SheetOnItems() const {
    std::vector<int> on;
    for (size_t i = 0; i < sheet_.items.size(); ++i)
        if (sheet_.items[i].on) on.push_back(static_cast<int>(i));
    return on;
}

int App::SheetPerPage() const { return std::clamp(settings_.sheetCols, 1, 8) * std::clamp(settings_.sheetRows, 1, 8); }

std::wstring App::SheetCaption(const FileEntry& e) const {
    if (!settings_.sheetNames) return {};
    return settings_.sheetExts ? e.name : Stem(e.name);
}

void App::OpenContactSheet(const std::vector<std::wstring>& paths) {
    std::vector<SheetItem> items;
    std::unordered_set<std::wstring> seen;
    auto add = [&](const FileEntry& e) {
        if (e.kind != Kind::Image || e.offline) return;
        if (!seen.insert(ToLower(e.path)).second) return;
        items.push_back({e, true});
    };
    for (auto& p : paths) {
        if (DirectoryExists(p)) {
            // Ordner: enthaltene Bilder (Unterordner nur, wenn "Unterordner" aktiv ist), nach Namen
            std::vector<FileEntry> found;
            EnumerateFolder(p, settings_.recursive, settings_.showHidden, [] { return false; },
                            [&](std::vector<FileEntry>&& part) {
                                for (auto& e : part) found.push_back(std::move(e));
                            });
            std::sort(found.begin(), found.end(), [](const FileEntry& a, const FileEntry& b) {
                return StrCmpLogicalW(a.path.c_str(), b.path.c_str()) < 0;
            });
            for (auto& e : found) add(e);
        } else {
            auto it = byPath_.find(p);
            if (it != byPath_.end()) add(entries_[it->second]);
        }
    }
    if (items.empty()) {
        SetStatus(T(L"Keine Bilder für einen Kontaktabzug ausgewählt."));
        return;
    }
    if (!sheetFontR_) {
        sheetFontR_ = std::make_unique<SheetFont>();
        sheetFontB_ = std::make_unique<SheetFont>();
        if (!sheetFontR_->Load(false) || !sheetFontB_->Load(true)) {
            sheetFontR_.reset();
            sheetFontB_.reset();
            SetStatus(T(L"Schrift Segoe UI nicht gefunden – Kontaktabzug nicht möglich."));
            return;
        }
    }
    // Ueberschrift: Ordnername (alle aus einem Ordner) bzw. "Kontaktabzug", dazu das heutige Datum
    std::wstring folder = ParentPath(items.front().e.path);
    bool same = true;
    for (auto& it : items)
        if (_wcsicmp(ParentPath(it.e.path).c_str(), folder.c_str()) != 0) { same = false; break; }
    if (paths.size() == 1 && DirectoryExists(paths.front())) { folder = paths.front(); same = true; }
    std::wstring name = same ? folder.substr(folder.find_last_of(L'\\') + 1) : std::wstring();
    if (name.empty() || name.back() == L':') name = T(L"Kontaktabzug");

    if (viewer_.open) CloseViewer();
    sheet_.items = std::move(items);
    sheet_.headline = name + L" " + Today();
    sheet_.caret = sheet_.headline.size();
    sheet_.editing = sheet_.selAll = false;
    sheet_.page = 0;
    sheet_.focus = sheet_.hover = sheet_.press = -1;
    sheet_.dragging = false;
    sheet_.stripScroll = 0;
    sheet_.savedUntil = 0;
    sheet_.open = true;
    searchFocus_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseContactSheet() {
    if (sheet_.busy) {   // laufende Erstellung abbrechen
        sheet_.cancel = true;
        return;
    }
    sheet_.open = false;
    sheet_.items.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SheetMove(int from, int before) {
    auto& v = sheet_.items;
    int n = static_cast<int>(v.size());
    if (from < 0 || from >= n) return;
    before = std::clamp(before, 0, n);
    if (before == from || before == from + 1) return;
    SheetItem it = std::move(v[from]);
    v.erase(v.begin() + from);
    if (before > from) --before;
    v.insert(v.begin() + before, std::move(it));
    sheet_.focus = before;
}

void App::SheetEditHeadline(const std::wstring& text, size_t caret) {
    sheet_.headline = text;
    sheet_.caret = std::min(caret, text.size());
    sheet_.selAll = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Maus

void App::SheetMouseDown(float x, float y) {
    auto& s = sheet_;
    const Hit* h = HitAt(x, y);
    HitType type = h ? h->type : HitType::None;
    int a = h ? h->a : 0;
    if (type != HitType::SheetHeadline) s.editing = false;
    s.press = -1;
    switch (type) {
    case HitType::SheetCancel: CloseContactSheet(); break;
    case HitType::SheetCreate: if (!s.busy) SheetCreatePdf(); break;
    case HitType::SheetOrient:
        settings_.sheetLandscape = !settings_.sheetLandscape;
        std::swap(settings_.sheetCols, settings_.sheetRows);   // 4 x 5 hoch <-> 5 x 4 quer
        settings_.Save();
        break;
    case HitType::SheetColsMinus: settings_.sheetCols = std::max(1, settings_.sheetCols - 1); settings_.Save(); break;
    case HitType::SheetColsPlus: settings_.sheetCols = std::min(8, settings_.sheetCols + 1); settings_.Save(); break;
    case HitType::SheetRowsMinus: settings_.sheetRows = std::max(1, settings_.sheetRows - 1); settings_.Save(); break;
    case HitType::SheetRowsPlus: settings_.sheetRows = std::min(8, settings_.sheetRows + 1); settings_.Save(); break;
    case HitType::SheetNames: settings_.sheetNames = !settings_.sheetNames; settings_.Save(); break;
    case HitType::SheetExts: if (settings_.sheetNames) { settings_.sheetExts = !settings_.sheetExts; settings_.Save(); } break;
    case HitType::SheetQuality: {
        static const wchar_t* n[] = {N_(L"Klein (150 dpi)"), N_(L"Mittel (220 dpi)"), N_(L"Hoch (300 dpi)")};
        std::vector<MenuItem> items;
        for (int i = 0; i < 3; ++i) {
            MenuItem m;
            m.id = 100 + i;
            m.text = T(n[i]);
            m.checked = settings_.sheetQuality == i;
            items.push_back(m);
        }
        float sc = r_.scale();
        POINT pt = {static_cast<LONG>(h->rc.left * sc), static_cast<LONG>((h->rc.bottom + 4) * sc)};
        ContentToScreen(pt);
        UINT cmd = ShowPopupMenu(hwnd_, items, pt, accent_);
        if (cmd >= 100 && cmd <= 102) {
            settings_.sheetQuality = static_cast<int>(cmd) - 100;
            settings_.Save();
        }
        break;
    }
    case HitType::SheetPrev: s.page = std::max(0, s.page - 1); break;
    case HitType::SheetNext: ++s.page; break;   // wird beim Zeichnen begrenzt
    case HitType::SheetHeadline: {
        // Cursor an die angeklickte Stelle
        s.editing = true;
        s.selAll = false;
        float k = s.scale;
        size_t best = s.headline.size();
        float bestD = 1e9f;
        for (size_t i = 0; i <= s.headline.size(); ++i) {
            float px = s.headX + sheetFontB_->Width(s.headline.substr(0, i), kSheetHeadSize * k);
            float d = std::fabs(px - x);
            if (d < bestD) { bestD = d; best = i; }
        }
        s.caret = best;
        break;
    }
    case HitType::SheetCheck:
    case HitType::SheetStripCheck:
        if (a >= 0 && a < static_cast<int>(s.items.size())) {
            s.items[a].on = !s.items[a].on;
            s.focus = a;
        }
        break;
    case HitType::SheetCell:
    case HitType::SheetStrip:
        s.press = a;
        s.pressStrip = type == HitType::SheetStrip;
        s.downPt = D2D1::Point2F(x, y);
        s.focus = a;
        break;
    default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SheetDoubleClick(float x, float y) {
    const Hit* h = HitAt(x, y);
    if (h && h->type == HitType::SheetHeadline) {
        sheet_.editing = true;
        sheet_.selAll = !sheet_.headline.empty();
        sheet_.caret = sheet_.headline.size();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    SheetMouseDown(x, y);   // Buttons: wie ein zweiter Klick
}

void App::SheetMouseMove(float x, float y) {
    auto& s = sheet_;
    s.mouse = D2D1::Point2F(x, y);
    // Hover fuer Buttons und Bilder
    int hh = -1;
    for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i)
        if (hits_[i].type != HitType::None && Inside(hits_[i].rc, x, y)) { hh = i; break; }
    int hoverItem = -1;
    if (hh >= 0 && (hits_[hh].type == HitType::SheetCell || hits_[hh].type == HitType::SheetCheck)) hoverItem = hits_[hh].a;
    bool redraw = hh != hoverHit_ || hoverItem != s.hover;
    hoverHit_ = hh;
    s.hover = hoverItem;

    if (s.press >= 0 && !s.dragging && (std::fabs(x - s.downPt.x) > 5 || std::fabs(y - s.downPt.y) > 5)) {
        s.dragging = true;
        s.editing = false;
    }
    if (s.dragging) {
        s.dropCell = s.dropStrip = -1;
        s.dropOnPage = false;
        int dir = Inside(s.prevRc, x, y) ? -1 : (Inside(s.nextRc, x, y) ? 1 : 0);
        if (dir != s.flipDir) {
            s.flipDir = dir;
            s.flipSince = GetTickCount();
        } else if (dir != 0 && GetTickCount() - s.flipSince > 650) {
            // beim Verweilen ueber einem Blaetterpfeil weiterblaettern
            s.page = std::max(0, s.page + dir);
            s.flipSince = GetTickCount();
        }
        if (Inside(s.pageRc, x, y) && !s.cellRc.empty()) {
            // Einfuegestelle: naechste Zelle, links oder rechts davon
            s.dropOnPage = true;
            float best = 1e9f;
            for (size_t k = 0; k < s.cellRc.size(); ++k) {
                const auto& r = s.cellRc[k];
                float cx = std::clamp(x, r.left, r.right), cy = std::clamp(y, r.top, r.bottom);
                float d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
                if (d < best) {
                    best = d;
                    s.dropCell = static_cast<int>(k) + (x > (r.left + r.right) / 2 ? 1 : 0);
                }
            }
        } else if (Inside(s.stripRc, x, y)) {
            int n = static_cast<int>(s.stripItemRc.size());
            s.dropStrip = n;
            for (int i = 0; i < n; ++i)
                if (x < (s.stripItemRc[i].left + s.stripItemRc[i].right) / 2) { s.dropStrip = i; break; }
            // am Rand des Streifens mitscrollen
            if (x < s.stripRc.left + 40) s.stripScroll = std::max(0.0f, s.stripScroll - 12);
            else if (x > s.stripRc.right - 40) s.stripScroll += 12;
        }
        redraw = true;
    }
    if (redraw) InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SheetMouseUp() {
    auto& s = sheet_;
    if (s.dragging && s.press >= 0 && s.press < static_cast<int>(s.items.size())) {
        int item = s.press;
        if (s.dropOnPage && s.dropCell >= 0) {
            // Ziel: angehaktes Bild an der Einfuegestelle der Seite
            std::vector<int> on = SheetOnItems();
            on.erase(std::remove(on.begin(), on.end(), item), on.end());
            size_t t = static_cast<size_t>(s.page) * SheetPerPage() + s.dropCell;
            // die gezogene Position selbst nicht mitzaehlen, wenn sie davor auf derselben Seite lag
            std::vector<int> before = SheetOnItems();
            auto pos = std::find(before.begin(), before.end(), item);
            if (pos != before.end() && static_cast<size_t>(pos - before.begin()) < t) --t;
            int target;
            if (t < on.size()) target = on[t];
            else target = on.empty() ? static_cast<int>(s.items.size()) : on.back() + 1;
            s.items[item].on = true;
            SheetMove(item, target);
        } else if (s.dropStrip >= 0) {
            SheetMove(item, s.dropStrip);
        }
    } else if (s.press >= 0 && s.pressStrip && s.press < static_cast<int>(s.items.size())) {
        // Klick im Streifen: zur Seite des Bildes blaettern
        std::vector<int> on = SheetOnItems();
        auto it = std::find(on.begin(), on.end(), s.press);
        if (it != on.end()) s.page = static_cast<int>(it - on.begin()) / SheetPerPage();
    }
    s.press = -1;
    s.dragging = false;
    s.dropCell = s.dropStrip = -1;
    s.flipDir = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SheetWheel(float x, float y, int delta) {
    auto& s = sheet_;
    if (Inside(s.stripRc, x, y)) s.stripScroll = std::max(0.0f, s.stripScroll - delta / 120.0f * 96);
    else s.page = std::max(0, s.page + (delta > 0 ? -1 : 1));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Tastatur

bool App::SheetKey(WPARAM vk) {
    auto& s = sheet_;
    bool ctrl = CtrlDown();
    if (s.editing) {
        if (s.selAll && (vk == VK_BACK || vk == VK_DELETE)) { SheetEditHeadline(L"", 0); return true; }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME || vk == VK_END) s.selAll = false;
        switch (vk) {
        case VK_RETURN: case VK_ESCAPE: case VK_TAB: s.editing = false; break;
        case VK_LEFT: if (s.caret > 0) --s.caret; break;
        case VK_RIGHT: if (s.caret < s.headline.size()) ++s.caret; break;
        case VK_HOME: s.caret = 0; break;
        case VK_END: s.caret = s.headline.size(); break;
        case VK_DELETE:
            if (s.caret < s.headline.size()) {
                std::wstring t = s.headline;
                t.erase(s.caret, 1);
                SheetEditHeadline(t, s.caret);
            }
            break;
        case 'A': if (ctrl) { s.selAll = !s.headline.empty(); break; } return true;
        case 'V':
            if (ctrl) {
                std::wstring t = PasteTextFromClipboard(hwnd_);
                for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                if (s.selAll) SheetEditHeadline(t, t.size());
                else {
                    std::wstring u = s.headline;
                    u.insert(s.caret, t);
                    SheetEditHeadline(u, s.caret + t.size());
                }
            }
            return true;
        default: return true;   // Zeichen kommen ueber WM_CHAR
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    std::vector<int> on = SheetOnItems();
    int per = SheetPerPage();
    auto posOf = [&](int item) {
        auto it = std::find(on.begin(), on.end(), item);
        return it == on.end() ? -1 : static_cast<int>(it - on.begin());
    };
    switch (vk) {
    case VK_ESCAPE: CloseContactSheet(); return true;
    case VK_PRIOR: s.page = std::max(0, s.page - 1); break;
    case VK_NEXT: ++s.page; break;
    case VK_HOME: s.page = 0; break;
    case VK_END: s.page = SheetPageCount(static_cast<int>(on.size()), per) - 1; break;
    case VK_SPACE:
        if (s.focus >= 0 && s.focus < static_cast<int>(s.items.size())) s.items[s.focus].on = !s.items[s.focus].on;
        break;
    case VK_RETURN:
        if (ctrl && !s.busy) SheetCreatePdf();
        break;
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
        if (on.empty()) break;
        int step = (vk == VK_LEFT || vk == VK_RIGHT) ? 1 : std::clamp(settings_.sheetCols, 1, 8);
        int dir = (vk == VK_LEFT || vk == VK_UP) ? -1 : 1;
        int p = posOf(s.focus);
        if (p < 0) { s.focus = on[std::min<size_t>(static_cast<size_t>(s.page) * per, on.size() - 1)]; break; }
        int q = std::clamp(p + dir * step, 0, static_cast<int>(on.size()) - 1);
        if (ctrl) {
            // Bild verschieben
            int target = dir > 0 ? (q + 1 < static_cast<int>(on.size()) ? on[q + 1] : static_cast<int>(s.items.size())) : on[q];
            SheetMove(s.focus, target);
            on = SheetOnItems();
            q = posOf(s.focus);
        } else {
            s.focus = on[q];
        }
        if (q >= 0) s.page = q / per;
        break;
    }
    case VK_BACK: CloseContactSheet(); return true;
    default: return false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void App::SheetChar(wchar_t c) {
    auto& s = sheet_;
    if (!s.editing) return;
    if (c == 8) {
        if (s.selAll) SheetEditHeadline(L"", 0);
        else if (s.caret > 0) {
            std::wstring t = s.headline;
            t.erase(s.caret - 1, 1);
            SheetEditHeadline(t, s.caret - 1);
        }
        return;
    }
    if (c < 32 || c == 127) return;
    if (s.selAll) { SheetEditHeadline(std::wstring(1, c), 1); return; }
    std::wstring t = s.headline;
    t.insert(s.caret, 1, c);
    SheetEditHeadline(t, s.caret + 1);
}

// ---------------------------------------------------------------------------
// PDF erstellen (Speichern-Dialog, dann im Hintergrund)

void App::SheetCreatePdf() {
    auto& s = sheet_;
    std::vector<int> on = SheetOnItems();
    if (on.empty()) {
        SetStatus(T(L"Keine Bilder angehakt."));
        return;
    }
    s.editing = false;
    std::wstring name = SafeFileName(s.headline) + L".pdf";
    std::wstring dir = ParentPath(s.items[on.front()].e.path);
    std::wstring out;
    ComPtr<IFileSaveDialog> dlg;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        COMDLG_FILTERSPEC spec[] = {{T(L"PDF-Dokument"), L"*.pdf"}};
        dlg->SetFileTypes(1, spec);
        dlg->SetDefaultExtension(L"pdf");
        dlg->SetFileName(name.c_str());
        dlg->SetTitle(T(L"Kontaktabzug speichern"));
        DWORD opt = 0;
        dlg->GetOptions(&opt);
        dlg->SetOptions(opt | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder)))) dlg->SetFolder(folder.Get());
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
        SetStatus(T(L"Nur Ordner auf Laufwerken mit Buchstaben werden unterstützt."));
        return;
    }

    SheetJob job;
    job.output = out;
    job.options.landscape = settings_.sheetLandscape;
    job.options.cols = settings_.sheetCols;
    job.options.rows = settings_.sheetRows;
    job.options.names = settings_.sheetNames;
    job.headline = s.headline;
    job.quality = settings_.sheetQuality;
    for (int i : on) job.images.push_back({s.items[i].e.path, SheetCaption(s.items[i].e)});

    if (sheetThread_.joinable()) sheetThread_.join();
    s.busy = true;
    s.ok = false;
    s.cancel = false;
    s.progress = 0;
    s.error.clear();
    s.savedPath = out;
    HWND hwnd = hwnd_;
    sheetThread_ = std::thread([this, job, hwnd] {
        ComScope com(COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::wstring err;
        DWORD last = 0;
        bool ok = WriteContactSheet(job, [&](float f) {
            sheet_.progress = static_cast<int>(f * 1000);
            DWORD now = GetTickCount();
            if (now - last > 100) {
                last = now;
                PostMessageW(hwnd, WM_APP_SHEET, 0, 0);
            }
            return !sheet_.cancel.load();
        }, err);
        sheet_.ok = ok;
        sheet_.error = err;
        PostMessageW(hwnd, WM_APP_SHEET, 1, 0);
    });
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SheetDone() {
    auto& s = sheet_;
    if (sheetThread_.joinable()) sheetThread_.join();
    s.busy = false;
    if (s.ok) {
        // gespeichert: Kontaktabzug schliessen, Hinweis in der Statusleiste
        std::wstring nm = s.savedPath.substr(s.savedPath.find_last_of(L'\\') + 1);
        s.open = false;
        s.items.clear();
        SetStatus(T(L"Kontaktabzug gespeichert: ") + nm);
        OpenWithDefaultApp(hwnd_, s.savedPath);   // fertiges PDF in der Standard-App zeigen
    } else {
        s.savedPath.clear();
        if (!s.cancel) SetStatus(s.error.empty() ? std::wstring(T(L"PDF konnte nicht erstellt werden.")) : s.error);
        if (s.cancel && !s.open) {}
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace peek
