// App: Maus, Tastatur, Menues
#include "app.h"
#include "core/textutil.h"

#include <cmath>
#include <cstdlib>

namespace peek {

namespace {
bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
bool Intersects(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}
bool CtrlDown() { return GetKeyState(VK_CONTROL) < 0; }
bool ShiftDown() { return GetKeyState(VK_SHIFT) < 0; }
bool AltDown() { return GetKeyState(VK_MENU) < 0; }
const int kColSortKey[5] = {0, 1, 4, 2, 3};   // Spalte -> Sortierschluessel
}  // namespace

const Hit* App::HitAt(float x, float y) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (it->type != HitType::None && Inside(it->rc, x, y)) return &*it;
    return nullptr;
}

// Nur Treffer auf einem Element; Zwischenraeume gelten als leere Flaeche (Auswahlrechteck)
// Auswahlkaestchen (Einstellung): wie Strg+Klick, mit Umschalt alle Elemente vom Anker bis hier dazu
void App::ToggleCheck(int pos, bool shift) {
    if (pos < 0 || pos >= static_cast<int>(visible_.size())) return;
    uint32_t idx = visible_[pos];
    if (shift && anchor_ >= 0 && anchor_ < static_cast<int>(visible_.size())) {
        int a = std::min(anchor_, pos), b = std::max(anchor_, pos);
        for (int p = a; p <= b; ++p) selected_.insert(visible_[p]);
        focus_ = pos;
    } else {
        if (selected_.count(idx)) selected_.erase(idx);
        else selected_.insert(idx);
        focus_ = anchor_ = pos;
    }
    lDown_ = false;
    selectOnUp_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int App::GridPosAt(float x, float y) const {
    if (settings_.viewMode == 2) return -1;   // Bilderwolke: eigene Trefferbestimmung (CloudPick)
    if (settings_.viewMode == 3) return settingsOpen_ ? -1 : TimelinePick(x, y);
    if (!Inside(gridRc_, x, y) || x > gridRc_.right - 14 || visible_.empty()) return -1;
    float cy = y - gridRc_.top + scroll_;
    if (cy < 8 || cy >= 8 + rowTop_.back()) return -1;
    int row = RowAtY(cy - 8);
    int pos;
    if (settings_.viewMode == 1) {
        if (RowEnd(row) == rowFirst_[row]) return -1;   // Gruppenkopf
        pos = rowFirst_[row];
    } else {
        float lx = x - gridRc_.left - 8;
        if (lx < 0) return -1;
        int col = static_cast<int>(lx / cellW_);
        if (col >= cols_) return -1;
        pos = rowFirst_[row] + col;
        if (pos >= RowEnd(row)) return -1;   // Gruppenkopf oder kurze letzte Zeile einer Gruppe
    }
    if (pos < 0 || pos >= static_cast<int>(visible_.size())) return -1;
    D2D1_RECT_F rc = ItemRect(pos);
    if (settings_.viewMode == 0) {
        rc.left += 3; rc.right -= 3; rc.top += 3; rc.bottom -= 3;
    } else {
        rc.right = std::min(rc.right, ListColsRight());
    }
    return Inside(rc, x, cy) ? pos : -1;
}

void App::StartAnimation() {
    // Animationen laufen im Takt der Bildwiederholung: jedes gezeichnete Bild fordert das naechste an,
    // Present wartet auf die vertikale Synchronisation (keine ungleichmaessigen Timer-Ticks)
    if (!animating_) {
        animating_ = true;
        animFresh_ = true;
        animLast_ = NowMs();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ScrollToPos(int pos) {
    if (settings_.viewMode == 3) {   // Zeitleiste: Element ins Bild holen
        float vw = gridRc_.right - gridRc_.left;
        for (auto& it : tl_.items)
            if (it.pos == pos && !it.stack) {
                if (it.x < tl_.target || it.x + it.s > tl_.target + vw) {
                    tl_.target = std::clamp(it.x - vw / 3, TimelineScrollMin(), TimelineScrollMax());
                    StartAnimation();
                }
                break;
            }
        return;
    }
    if (pos < 0 || cols_ <= 0) return;
    Layout();
    float gh = gridRc_.bottom - gridRc_.top;
    int row = RowOfPos(pos);
    if (row + 1 >= static_cast<int>(rowTop_.size())) return;
    float top = rowTop_[row], bottom = rowTop_[row + 1];
    if (top < scrollTarget_) scrollTarget_ = top;
    else if (bottom + 16 > scrollTarget_ + gh) scrollTarget_ = bottom + 16 - gh;
    float maxScroll = std::max(0.0f, contentH_ - gh);
    scrollTarget_ = std::clamp(scrollTarget_, 0.0f, maxScroll);
    scroll_ = scrollTarget_;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SetFocusPos(int pos, bool extend, bool toggle) {
    int n = static_cast<int>(visible_.size());
    if (n == 0) return;
    pos = std::clamp(pos, 0, n - 1);
    focus_ = pos;
    if (extend && anchor_ >= 0) {
        selected_.clear();
        int a = std::min(anchor_, pos), b = std::max(anchor_, pos);
        for (int p = a; p <= b; ++p) selected_.insert(visible_[p]);
    } else if (!toggle) {
        selected_.clear();
        selected_.insert(visible_[pos]);
        anchor_ = pos;
    }
    ScrollToPos(pos);
}

void App::SetTileSize(int size) {
    size = std::clamp(size, 64, 384);
    if (size == settings_.tileSize) return;
    settings_.tileSize = size;
    Layout();
    if (focus_ >= 0) ScrollToPos(focus_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::SetTileFromSlider(float x) {
    float w = sliderRc_.right - sliderRc_.left - 12;
    if (w <= 0) return;
    float frac = std::clamp((x - sliderRc_.left - 6) / w, 0.0f, 1.0f);
    SetTileSize(64 + static_cast<int>(std::round(frac * 320 / 8)) * 8);
}

void App::EditSearch(const std::wstring& text, size_t caret) {
    searchSelAll_ = false;
    showHistory_ = false;
    search_ = text;
    caret_ = std::min(caret, search_.size());
    query_ = ParseQuery(search_);
    ScheduleRefilter(80);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::MenuJustClosed(int code) {
    // Der Klick, der ein Menue schliesst, oeffnet es nicht gleich wieder
    if (code == lastMenu_ && GetTickCount() - lastMenuClose_ < 300) {
        lastMenu_ = -1;
        return true;
    }
    return false;
}

void App::SortByColumn(int col) {
    int key = kColSortKey[std::clamp(col, 0, 4)];
    if (settings_.sortKey == key) settings_.sortDesc = !settings_.sortDesc;
    else {
        settings_.sortKey = key;
        settings_.sortDesc = key == 1 || key == 2 || key == 3;   // Datum/Groesse/Aufloesung: groesste zuerst
    }
    RememberSort();
    Refilter();
}

std::wstring App::SortStoreKey() const {
    if (collectionsView_) return L"::collections";
    if (bookmarksView_) return L"::bookmarks";
    return ToLower(path_);
}

void App::ApplyFolderSort() {
    if (sortFromCli_) {   // Befehlszeile (z. B. --group fuer Bildschirmfotos) gilt fuer den ersten Ordner
        sortFromCli_ = false;
        return;
    }
    int v = settings_.FolderSort(SortStoreKey());
    if (v < 0) v = 0;   // Standard: Name aufsteigend, nicht gruppiert
    settings_.sortKey = std::clamp(v & 15, 0, 5);
    settings_.sortDesc = (v & 16) != 0;
    settings_.group = (v & 32) != 0;
}

void App::RememberSort() {
    if (!collectionsView_ && !bookmarksView_ && path_.empty()) return;
    int v = settings_.sortKey | (settings_.sortDesc ? 16 : 0) | (settings_.group ? 32 : 0);
    settings_.SetFolderSort(SortStoreKey(), v);
    settings_.Save();
}

void App::UpdateMarquee() {
    POINT pt = marqueeMouse_;
    float x = pt.x / r_.scale(), y = pt.y / r_.scale();
    x = std::clamp(x, gridRc_.left, gridRc_.right - 14);
    marqueeCur_ = D2D1::Point2F(x, std::clamp(y, gridRc_.top, gridRc_.bottom) - gridRc_.top + scroll_);
    D2D1_RECT_F mr = D2D1::RectF(std::min(marqueeStart_.x, marqueeCur_.x), std::min(marqueeStart_.y, marqueeCur_.y),
                                 std::max(marqueeStart_.x, marqueeCur_.x), std::max(marqueeStart_.y, marqueeCur_.y));
    selected_ = marqueeBase_;
    int n = static_cast<int>(visible_.size());
    if (n && rowTop_.size() > 1) {
        int r0 = RowAtY(std::max(0.0f, mr.top - 8)), r1 = RowAtY(std::max(0.0f, mr.bottom - 8));
        for (int row = r0; row <= r1; ++row) {
            int p0 = FirstPosOfRow(row), p1 = RowEnd(row);
            for (int pos = p0; pos < p1 && pos < n; ++pos) {
                D2D1_RECT_F rc = ItemRect(pos);
                rc.left += 3; rc.right -= 3; rc.top += 3; rc.bottom -= 3;
                if (Intersects(rc, mr)) {
                    uint32_t idx = visible_[pos];
                    if (marqueeBase_.count(idx)) selected_.erase(idx);   // mit Strg: umschalten
                    else selected_.insert(idx);
                    focus_ = pos;
                }
            }
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Maus
// ---------------------------------------------------------------------------

void App::OnMouseMove(float x, float y, WPARAM keys) {
    if (sheet_.open) { SheetMouseMove(x, y); return; }
    if (atlas_.open) { AtlasMouseMove(x, y); return; }
    if (compare_.open) { CompareMouseMove(x, y); return; }
    if (dup_.open && !autoScroll_) { DupMouseMove(x, y); return; }
    if ((tl_.scrub || tl_.pan) && TimelineMouseMove(x, y)) return;
    if (autoScroll_) {
        autoMouseY_ = y;
        autoMouseX_ = x;
        return;
    }
    if (settingsScrollDrag_) {
        const float trackTop = settingsPage_ == 1 ? 66.0f : 4.0f;
        float trackH = settingsPage_ == 1 ? H_ - 70 : H_ - 8;
        float thumbH = std::max(32.0f, trackH * H_ / std::max(1.0f, settingsContentH_));
        float maxS = std::max(0.0f, settingsContentH_ - H_);
        float rel = (y - settingsDragOffset_ - trackTop) / std::max(1.0f, trackH - thumbH);
        settingsScroll_ = std::clamp(rel, 0.0f, 1.0f) * maxS;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (viewer_.open && viewer_.cropDrag) {
        CropMouseMove(x, y);
        return;
    }
    if (viewer_.open) {
        if (viewer_.dragging) {
            POINT pt;
            GetCursorPos(&pt);
            if (std::abs(pt.x - viewer_.dragStart.x) > GetSystemMetrics(SM_CXDRAG) ||
                std::abs(pt.y - viewer_.dragStart.y) > GetSystemMetrics(SM_CYDRAG))
                viewer_.moved = true;
            viewer_.pan.x = viewer_.panStart.x + (pt.x - viewer_.dragStart.x) / r_.scale();
            viewer_.pan.y = viewer_.panStart.y + (pt.y - viewer_.dragStart.y) / r_.scale();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        // nur neu zeichnen, wenn sich die Anzeige aendert (Pfeile erscheinen, Hervorhebung wechselt):
        // das Bild muss nicht bei jeder Mausbewegung neu gefiltert werden
        bool shown = GetTickCount() < viewer_.controlsUntil;
        viewer_.controlsUntil = GetTickCount() + 2000;
        SetTimer(hwnd_, kTimerViewerUi, 2100, nullptr);
        bool corner = x > W_ - (captionless_ && !fullscreen_ ? 170 : 80) && y < 60;   // Ecke: X bzw. Windows-Knoepfe
        int hh = -1;
        for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i)
            if (hits_[i].type != HitType::None && Inside(hits_[i].rc, x, y)) { hh = i; break; }
        if (!shown || corner != viewer_.corner || hh != hoverHit_) {
            viewer_.corner = corner;
            hoverHit_ = hh;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (marquee_) {
        GetCursorPos(&marqueeMouse_);
        ScreenToClient(hwnd_, &marqueeMouse_);
        UpdateMarquee();
        if (y < gridRc_.top || y > gridRc_.bottom) StartAnimation();
        return;
    }
    if (colDrag_ >= 0) {
        // Griff an der linken Kante: nach links ziehen verbreitert die Spalte (die Namensspalte gibt Platz ab)
        settings_.colW[colDrag_] = std::clamp(colDragW_ - static_cast<int>(x - colDragX_), colDragMin_, std::max(colDragMin_, 1200));
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (sideDragItem_ >= 0 && (keys & MK_LBUTTON)) {
        if (!sideDragActive_ && std::fabs(y - sideDragY_) * r_.scale() > GetSystemMetrics(SM_CYDRAG)) sideDragActive_ = true;
        if (sideDragActive_) {
            // Einfuegeposition zwischen den Sammlungen bestimmen
            int k = 0, insert = 0;
            for (auto& h : hits_) {
                if (h.type != HitType::SidebarItem || sidebar_[h.a].kind != SidebarItem::Collection) continue;
                if (y > (h.rc.top + h.rc.bottom) / 2) insert = k + 1;
                ++k;
            }
            sideInsert_ = insert;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (sliderDrag_) {
        SetTileFromSlider(x);
        return;
    }
    if (scrollDrag_) {
        float gh = gridRc_.bottom - gridRc_.top;
        float trackH = gh - 8;
        float thumbH = std::max(32.0f, trackH * gh / std::max(1.0f, contentH_));
        float maxScroll = std::max(0.0f, contentH_ - gh);
        float rel = (y - scrollDragOffset_ - gridRc_.top - 4) / std::max(1.0f, trackH - thumbH);
        scroll_ = scrollTarget_ = std::clamp(rel, 0.0f, 1.0f) * maxScroll;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (lDown_ && downPos_ >= 0 && (keys & MK_LBUTTON)) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        if (std::abs(pt.x - downPt_.x) > GetSystemMetrics(SM_CXDRAG) ||
            std::abs(pt.y - downPt_.y) > GetSystemMetrics(SM_CYDRAG)) {
            if (rowMarquee_) {
                // Detailansicht neben dem Namen: Auswahlrechteck statt Dateien ziehen
                lDown_ = false;
                selectOnUp_ = false;
                rowMarquee_ = false;
                marquee_ = true;
                marqueeBase_ = std::move(rowMarqueeBase_);
                rowMarqueeBase_.clear();
                marqueeStart_ = rowMarqueeStart_;
                marqueeCur_ = marqueeStart_;
                marqueeMouse_ = pt;
                UpdateMarquee();
                return;
            }
            if (!selected_.count(visible_[downPos_])) {
                selected_.clear();
                selected_.insert(visible_[downPos_]);
            }
            auto paths = SelectedPaths();
            lDown_ = false;
            selectOnUp_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            ownDrag_ = true;
            StartFileDrag(hwnd_, paths);
            ownDrag_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    if (settings_.viewMode == 2 && !settingsOpen_) {
        if (cloud_.down || Inside(gridRc_, x, y)) CloudMouseMove(x, y);
        else if (cloud_.hover >= 0) { cloud_.hover = -1; InvalidateRect(hwnd_, nullptr, FALSE); }
    }
    int hp = settingsOpen_ ? -1 : GridPosAt(x, y);
    // ueber der Suchliste keine Hervorhebung der Dateien dahinter
    if (showHistory_ && searchFocus_ && Inside(histRc_, x, y)) hp = -1;
    int hh = -1;
    for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i)
        if (hits_[i].type != HitType::None && Inside(hits_[i].rc, x, y)) { hh = i; break; }
    bool sb = !settingsOpen_ && Inside(gridRc_, x, y) && x > gridRc_.right - 14;
    bool hc = hp >= 0 && OnCheck(hp, x, y);
    if (hh != hoverHit_) {   // Hinweis: neu abwarten, sobald die Maus einen anderen Treffer erreicht
        tipShown_ = false;
        KillTimer(hwnd_, kTimerTip);
        if (hh >= 0 && hits_[hh].tip) SetTimer(hwnd_, kTimerTip, 600, nullptr);
    }
    if (hp != hoverPos_ || hh != hoverHit_ || sb != hoverScrollbar_ || hc != hoverCheck_) {
        hoverPos_ = hp;
        hoverCheck_ = hc;
        hoverHit_ = hh;
        hoverScrollbar_ = sb;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::OnLButtonDown(float x, float y, WPARAM keys) {
    SetFocus(hwnd_);
    tipShown_ = false;
    KillTimer(hwnd_, kTimerTip);
    if (sheet_.open) { SheetMouseDown(x, y); return; }
    if (atlas_.open) { AtlasMouseDown(x, y); return; }
    if (compare_.open) { CompareMouseDown(x, y); return; }
    if (dup_.open) { DupMouseDown(x, y, false); return; }
    if (viewer_.open) {
        if (const Hit* h = HitAt(x, y)) {
            switch (h->type) {
            case HitType::ViewerClose:
            case HitType::ViewerBack:
                // erst beim Loslassen: schliesst sich das Fenster schon beim Druecken, landet das Loslassen im
                // Fenster dahinter (dort z. B. auf dessen X)
                viewerPress_ = h->type;
                SetCapture(hwnd_);
                return;
            case HitType::ViewerPrev: ViewerStep(-1); return;
            case HitType::ViewerNext: ViewerStep(1); return;
            case HitType::ViewerMin: ShowWindow(hwnd_, SW_MINIMIZE); return;
            case HitType::ViewerMax: ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); return;
            case HitType::ViewerInfo:
                settings_.viewerInfo = !settings_.viewerInfo;
                StartAnimation();
                return;
            case HitType::ViewerCrop: StopSlideshow(true); EnterCrop(); return;
            case HitType::InfoGeo: GeoClick(static_cast<uint32_t>(h->a)); return;
            case HitType::InfoStripMeta: StopSlideshow(true); RemoveMetadata(static_cast<uint32_t>(h->a)); return;
            case HitType::ViewerSlideshow:
                if (viewer_.slide) StopSlideshow(true);
                else StartSlideshow(false);
                return;
            case HitType::CropSave: SaveCrop(); return;
            case HitType::CropCancel: CancelCrop(); return;
            default: break;
            }
        }
        if (x >= ViewerAreaW()) return;   // Klick ins Info-Panel
        if (viewer_.crop) {
            CropMouseDown(x, y, false);
            return;
        }
        viewer_.dragging = true;
        viewer_.moved = false;
        GetCursorPos(&viewer_.dragStart);
        viewer_.panStart = viewer_.pan;
        return;
    }

    const Hit* h = HitAt(x, y);
    if (settingsOpen_) {
        // Bildlaufleiste am rechten Rand: Griff ziehen oder in die Leiste klicken
        // (Seite "Tastenkombinationen": Leiste beginnt unter der Ueberschrift)
        const float trackTop = settingsPage_ == 1 ? 66.0f : 4.0f;
        if (settingsContentH_ > H_ && x > W_ - 16 && y >= trackTop) {
            float trackH = settingsPage_ == 1 ? H_ - 70 : H_ - 8;
            float thumbH = std::max(32.0f, trackH * H_ / settingsContentH_);
            float maxS = settingsContentH_ - H_;
            float ty = trackTop + (trackH - thumbH) * (maxS > 0 ? settingsScroll_ / maxS : 0);
            settingsScrollDrag_ = true;
            settingsDragOffset_ = (y >= ty && y <= ty + thumbH) ? y - ty : thumbH / 2;
            OnMouseMove(x, y, keys);
            return;
        }
        if (!h) return;
        switch (h->type) {
        case HitType::SettingsBack: CloseSettings(); break;
        case HitType::SettingsOpenData: OpenWithDefaultApp(hwnd_, DataDir()); break;
        case HitType::SettingsRescan:
            indexer_.Rescan();
            SetStatus(T(L"Indexierung gestartet."));
            break;
        case HitType::SettingsDeleteIndex: DeleteIndex(); break;
        case HitType::SettingsTheme: ShowThemeMenu(h->rc); break;
        case HitType::SettingsLanguage: ShowLanguageMenu(h->rc); break;
        case HitType::SettingsViewerFullscreen: ShowViewerFullscreenMenu(h->rc); break;
        case HitType::SettingsSingleClick: ShowSingleClickMenu(h->rc); break;
        case HitType::SettingsCheckBoxes: ShowCheckBoxMenu(h->rc); break;
        case HitType::SettingsAnimations: ShowAnimationsMenu(h->rc); break;
        case HitType::SettingsGeoClick: ShowGeoClickMenu(h->rc); break;
        case HitType::SettingsViewerSmall: ShowViewerSmallMenu(h->rc); break;
        case HitType::SettingsShortcuts:
            settingsPage_ = 1;
            settingsScroll_ = 0;
            break;
        default: break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (h) {
        bool histHit = h->type == HitType::SearchHistItem || h->type == HitType::SearchHistDel ||
                       h->type == HitType::SearchHistClear || h->type == HitType::SearchParam || (h->type == HitType::None && showHistory_);
        if (h->type != HitType::Search && h->type != HitType::SearchClear && !histHit) {
            if (searchFocus_) AddSearchHistory(search_);
            searchFocus_ = false;
            showHistory_ = false;
        }
        searchSelAll_ = false;
        D2D1_RECT_F rc = h->rc;
        int a = h->a;
        switch (h->type) {
        case HitType::SidebarToggle:
            settings_.sidebar = !settings_.sidebar;
            StartAnimation();
            break;
        case HitType::InfoToggle:
            settings_.infoPanel = !settings_.infoPanel;
            StartAnimation();
            break;
        case HitType::ZoomSlider:
            sliderDrag_ = true;
            SetTileFromSlider(x);
            break;
        case HitType::ZoomIn: SetTileSize(settings_.tileSize + 32); break;
        case HitType::ZoomOut: SetTileSize(settings_.tileSize - 32); break;
        case HitType::ViewGrid: SetViewMode(0); break;
        case HitType::ViewList: SetViewMode(1); break;
        case HitType::ViewCloud: SetViewMode(2); break;
        case HitType::SlideshowStart: StartSlideshow(true); break;
        case HitType::InfoGeo: GeoClick(static_cast<uint32_t>(a)); break;
        case HitType::InfoStripMeta: RemoveMetadata(static_cast<uint32_t>(a)); break;
        case HitType::ViewTimeline: SetViewMode(3); break;
        case HitType::TimelineGroup: TimelineToggle(a); break;
        case HitType::TimelineScrub:
            tl_.scrub = true;
            TimelineScrubAt(x, false);
            break;
        case HitType::SortButton:
            if (!MenuJustClosed(static_cast<int>(ChipId::Sort))) ShowSortMenu(rc);
            break;
        case HitType::MoreFilters:
            if (!MenuJustClosed(2000)) ShowMoreFiltersMenu(rc);
            break;
        case HitType::ListHeader: SortByColumn(a); break;
        case HitType::ListResize:
            colDrag_ = a;
            colDragX_ = x;
            colDragW_ = settings_.colW[a];
            colDragMin_ = ColumnMinWidth(a);
            break;
        case HitType::SettingsButton: OpenSettings(); break;
        case HitType::NavBack: GoBack(); break;
        case HitType::NavForward: GoForward(); break;
        case HitType::NavUp: GoUp(); break;
        case HitType::Refresh:
            if (collectionsView_) indexer_.Rescan();
            Reload(true);
            break;
        case HitType::Crumb:   // uebergeordneter Ordner: den Ordner auswaehlen, aus dem man kommt (wie "Nach oben")
            if (a >= 0 && a < static_cast<int>(crumbPaths_.size())) Navigate(crumbPaths_[a], true, ChildOnPath(crumbPaths_[a]));
            break;
        case HitType::CrumbChevron:
            if (!MenuJustClosed(1000 + a)) ShowCrumbMenu(a, rc);
            break;
        case HitType::Search:
            searchFocus_ = true;
            caret_ = search_.size();
            showHistory_ = !showHistory_ || !searchFocus_;
            break;
        case HitType::SearchHistItem:
            if (a >= 0 && a < static_cast<int>(settings_.searchHistory.size())) {
                std::wstring q = settings_.searchHistory[a];
                EditSearch(q, q.size());
                AddSearchHistory(q);
                searchFocus_ = true;
            }
            break;
        case HitType::SearchHistDel:
            if (a >= 0 && a < static_cast<int>(settings_.searchHistory.size())) {
                settings_.searchHistory.erase(settings_.searchHistory.begin() + a);
                settings_.Save();
            }
            searchFocus_ = true;
            break;
        case HitType::SearchParam: {
            const auto& params = QueryParams();
            if (a < 0 || a >= static_cast<int>(params.size())) break;
            std::wstring ins = params[a].insert;
            std::wstring s = searchSelAll_ ? std::wstring() : search_;
            size_t c = searchSelAll_ ? 0 : std::min(caret_, s.size());
            if (c > 0 && s[c - 1] != L' ') ins = L" " + ins;
            s.insert(c, ins);
            EditSearch(s, c + ins.size());
            searchFocus_ = true;
            break;
        }
        case HitType::DupSearch:
            OpenDuplicates();
            return;
        case HitType::SaveSearch:
            SaveCurrentSearch();
            break;
        case HitType::DeleteSearch:
            DeleteSavedSearch(MatchingSavedSearch());
            break;
        case HitType::SearchHistClear:
            settings_.searchHistory.clear();
            settings_.Save();
            showHistory_ = false;
            searchFocus_ = true;
            break;
        case HitType::BookmarkSheet: {
            // ausgewaehlte Elemente der Merkliste, sonst alle (in der angezeigten Reihenfolge)
            std::vector<std::wstring> paths;
            for (uint32_t idx : visible_)
                if (selected_.count(idx)) paths.push_back(entries_[idx].path);
            if (paths.empty())
                for (uint32_t idx : visible_) paths.push_back(entries_[idx].path);
            OpenContactSheet(paths);
            break;
        }
        case HitType::ClearBookmarks:
            settings_.bookmarks.clear();
            settings_.Save();
            BuildSidebar();
            Reload(false);
            SetStatus(T(L"Merkliste geleert."));
            break;
        case HitType::SearchClear:
            EditSearch(L"", 0);
            searchFocus_ = true;
            break;
        case HitType::Chip: {
            ChipId id = static_cast<ChipId>(a);
            if (id == ChipId::Recursive) {
                settings_.recursive = !settings_.recursive;
                Reload(false);
            } else if (!MenuJustClosed(a)) {
                ShowChipMenu(id, rc);
            }
            break;
        }
        case HitType::Term: {
            uint32_t id = static_cast<uint32_t>(a);
            auto& t = filters_.terms;
            auto it = std::find(t.begin(), t.end(), id);
            if (it != t.end()) t.erase(it);
            else t.push_back(id);
            Refilter();
            break;
        }
        case HitType::ResetFilters:
            filters_ = Filters{};
            Refilter();
            break;
        case HitType::SidebarItem: {
            if (a < 0 || a >= static_cast<int>(sidebar_.size())) break;
            const SidebarItem it = sidebar_[a];
            if (it.kind == SidebarItem::Collection) {
                // Sammlungen: Klick oeffnet (beim Loslassen), Ziehen aendert die Reihenfolge
                sideDragItem_ = a;
                sideDragActive_ = false;
                sideDragY_ = y;
                sideInsert_ = -1;
            } else if (it.kind == SidebarItem::AllCollections) {
                ShowAllCollections();
            } else if (it.kind == SidebarItem::Bookmarks) {
                ShowBookmarks();
            } else if (it.kind == SidebarItem::SavedSearch) {
                OpenSavedSearch(it.idx);
            } else if (!it.path.empty()) {
                Navigate(it.path);
            }
            break;
        }
        case HitType::SidebarAdd:
            PickFolder();
            break;
        case HitType::SelectAll: {
            // Kaestchen im Spaltenkopf: sind schon alle ausgewaehlt, Auswahl aufheben, sonst alle auswaehlen
            bool all = !visible_.empty() && selected_.size() >= visible_.size();
            if (all)
                for (uint32_t i : visible_)
                    if (!selected_.count(i)) { all = false; break; }
            selected_.clear();
            if (!all)
                for (uint32_t i : visible_) selected_.insert(i);
            break;
        }
        default:
            break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (searchFocus_) AddSearchHistory(search_);
    searchFocus_ = false;
    showHistory_ = false;
    searchSelAll_ = false;

    if (settings_.viewMode == 2 && Inside(gridRc_, x, y) && !visible_.empty()) {
        CloudMouseDown(x, y);
        return;
    }
    if (settings_.viewMode == 3 && Inside(gridRc_, x, y) && TimelineMouseDown(x, y, keys)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    // Bildlaufleiste
    if (Inside(gridRc_, x, y) && x > gridRc_.right - 14 && contentH_ > gridRc_.bottom - gridRc_.top) {
        float gh = gridRc_.bottom - gridRc_.top;
        float trackH = gh - 8;
        float thumbH = std::max(32.0f, trackH * gh / contentH_);
        float maxScroll = contentH_ - gh;
        float ty = gridRc_.top + 4 + (trackH - thumbH) * (maxScroll > 0 ? scroll_ / maxScroll : 0);
        scrollDrag_ = true;
        scrollDragOffset_ = (y >= ty && y <= ty + thumbH) ? y - ty : thumbH / 2;
        OnMouseMove(x, y, keys);
        return;
    }

    int pos = GridPosAt(x, y);
    if (OnCheck(pos, x, y)) {   // Auswahlkaestchen: Element zur Auswahl hinzufuegen oder entfernen, nichts oeffnen
        ToggleCheck(pos, (keys & MK_SHIFT) != 0);
        return;
    }
    if (pos < 0) {
        if (Inside(gridRc_, x, y)) {
            // Auswahlrechteck auf leerer Flaeche
            marquee_ = true;
            marqueeBase_ = (keys & MK_CONTROL) ? selected_ : std::unordered_set<uint32_t>{};
            marqueeStart_ = D2D1::Point2F(x, y - gridRc_.top + scroll_);
            marqueeCur_ = marqueeStart_;
            GetCursorPos(&marqueeMouse_);
            ScreenToClient(hwnd_, &marqueeMouse_);
            UpdateMarquee();
        } else if (!(keys & (MK_CONTROL | MK_SHIFT))) {
            selected_.clear();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    lDown_ = true;
    downPt_ = pt;
    downPos_ = pos;
    downPlain_ = !(keys & (MK_CONTROL | MK_SHIFT));
    selectOnUp_ = false;
    // Detailansicht neben dem Namen: Auswahlrechteck. Auf einer bereits ausgewaehlten Zeile (ohne Strg/Umschalt)
    // dagegen ueberall die Auswahl ziehen, nicht nur am Dateinamen.
    rowMarquee_ = settings_.viewMode == 1 && !OverListLabel(pos, x) &&
                  !(selected_.count(visible_[pos]) && !(keys & (MK_CONTROL | MK_SHIFT)));
    if (rowMarquee_) {
        rowMarqueeStart_ = D2D1::Point2F(x, y - gridRc_.top + scroll_);
        rowMarqueeBase_ = (keys & MK_CONTROL) ? selected_ : std::unordered_set<uint32_t>{};
    }
    uint32_t idx = visible_[pos];
    if (keys & MK_CONTROL) {
        if (selected_.count(idx)) selected_.erase(idx);
        else selected_.insert(idx);
        focus_ = anchor_ = pos;
    } else if ((keys & MK_SHIFT) && anchor_ >= 0) {
        selected_.clear();
        int a = std::min(anchor_, pos), b = std::max(anchor_, pos);
        for (int p = a; p <= b; ++p) selected_.insert(visible_[p]);
        focus_ = pos;
    } else {
        if (selected_.count(idx)) selectOnUp_ = true;
        else {
            selected_.clear();
            selected_.insert(idx);
        }
        focus_ = anchor_ = pos;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnLButtonUp(float x, float y) {
    settingsScrollDrag_ = false;
    if (viewerPress_ != HitType::None) {
        HitType p = viewerPress_;
        viewerPress_ = HitType::None;
        if (GetCapture() == hwnd_) ReleaseCapture();
        const Hit* h = viewer_.open ? HitAt(x, y) : nullptr;
        if (h && h->type == p) {   // nur, wenn die Maus noch ueber dem Knopf ist
            if (p == HitType::ViewerBack) CloseViewer();                          // zur Ordneransicht
            else if (viewer_.shell) PostMessageW(hwnd_, WM_CLOSE, 0, 0);   // aus dem Explorer geoeffnet: beenden
            else CloseViewer();
        }
        return;
    }
    if (sheet_.open) { SheetMouseUp(); return; }
    if (atlas_.open) return;
    if (compare_.open) { CompareMouseUp(); return; }
    if (dup_.open) { DupMouseUp(); return; }
    if (TimelineMouseUp()) return;
    if (cloud_.down && !viewer_.open) {
        CloudMouseUp(x, y);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (viewer_.open && viewer_.cropDrag) {
        CropMouseUp();
        return;
    }
    if (viewer_.open) {
        // einfacher Klick: Leiste oben ein/aus (verzoegert, damit Doppelklick zoomen kann)
        if (viewer_.dragging && !viewer_.moved) SetTimer(hwnd_, kTimerViewerClick, GetDoubleClickTime(), nullptr);
        viewer_.dragging = false;
        return;
    }
    if (marquee_) {
        marquee_ = false;
        marqueeBase_.clear();
        if (focus_ >= 0) anchor_ = focus_;
    }
    if (colDrag_ >= 0) {
        colDrag_ = -1;
        settings_.Save();
    }
    if (sideDragItem_ >= 0) {
        int item = sideDragItem_;
        bool active = sideDragActive_;
        int insert = sideInsert_;
        sideDragItem_ = -1;
        sideDragActive_ = false;
        sideInsert_ = -1;
        if (item < static_cast<int>(sidebar_.size())) {
            std::wstring path = sidebar_[item].path;
            if (!active) {
                Navigate(path);
            } else {
                auto& c = settings_.collections;
                auto it = std::find_if(c.begin(), c.end(),
                                       [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; });
                if (it != c.end() && insert >= 0) {
                    int from = static_cast<int>(it - c.begin());
                    std::wstring moved = *it;
                    c.erase(it);
                    int to = insert > from ? insert - 1 : insert;
                    to = std::clamp(to, 0, static_cast<int>(c.size()));
                    c.insert(c.begin() + to, moved);
                    settings_.Save();
                    BuildSidebar();
                }
            }
        }
    }
    sliderDrag_ = false;
    scrollDrag_ = false;
    settingsScrollDrag_ = false;
    if (lDown_ && selectOnUp_ && downPos_ >= 0 && downPos_ < static_cast<int>(visible_.size())) {
        selected_.clear();
        selected_.insert(visible_[downPos_]);
    }
    // Oeffnen mit einem Klick (Einstellung): Klick ohne Strg/Umschalt, ohne Ziehen, auf demselben Element
    if (settings_.singleClick && lDown_ && downPlain_ && downPos_ >= 0 && downPos_ < static_cast<int>(visible_.size()) &&
        settings_.viewMode != 2) {
        POINT up;
        GetCursorPos(&up);
        ScreenToClient(hwnd_, &up);
        bool still = std::abs(up.x - downPt_.x) <= GetSystemMetrics(SM_CXDRAG) &&
                     std::abs(up.y - downPt_.y) <= GetSystemMetrics(SM_CYDRAG);
        if (still && GridPosAt(x, y) == downPos_) {
            int pos = downPos_;
            lDown_ = false;
            selectOnUp_ = false;
            rowMarquee_ = false;
            rowMarqueeBase_.clear();
            noDblUntil_ = GetTickCount() + GetDoubleClickTime();   // zweiter Klick eines Doppelklicks: nichts mehr oeffnen
            OpenPos(pos);
            return;
        }
    }
    lDown_ = false;
    selectOnUp_ = false;
    rowMarquee_ = false;
    rowMarqueeBase_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::OnDoubleClick(float x, float y) {
    if (viewer_.open && viewer_.crop) return;
    if (viewer_.open) {
        KillTimer(hwnd_, kTimerViewerClick);
        if (const Hit* h = HitAt(x, y)) {
            // schnelles Doppelklicken auf Zurueck/Weiter blaettert zweimal
            if (h->type == HitType::ViewerPrev) ViewerStep(-1);
            else if (h->type == HitType::ViewerNext) ViewerStep(1);
            return;
        }
        if (viewer_.slide) return;   // Diashow: kein Zoom
        if (x >= ViewerAreaW() || viewer_.pos < 0 || viewer_.pos >= static_cast<int>(visible_.size())) return;
        const FileEntry& e = entries_[visible_[viewer_.pos]];
        float iw, ih;
        float s0 = ViewerScale(e, iw, ih);
        viewer_.zoom = viewer_.zoom > 0 ? 0 : 1.0f;
        float s1 = ViewerScale(e, iw, ih);
        if (viewer_.zoom == 0) viewer_.pan = {0, 0};
        else {
            float cxv = ViewerAreaW() / 2;
            D2D1_POINT_2F c = D2D1::Point2F(cxv + viewer_.pan.x, H_ / 2 + viewer_.pan.y);
            D2D1_POINT_2F nc = D2D1::Point2F(x - (x - c.x) * s1 / s0, y - (y - c.y) * s1 / s0);
            viewer_.pan = D2D1::Point2F(nc.x - cxv, nc.y - H_ / 2);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (settingsOpen_) return;
    if (const Hit* h = HitAt(x, y)) {
        if (h->type == HitType::Search) {
            // Doppelklick markiert den gesamten Suchtext
            searchFocus_ = true;
            searchSelAll_ = !search_.empty();
            showHistory_ = false;
            caret_ = search_.size();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (h->type == HitType::ListResize) {
            settings_.colW[h->a] = std::max(h->a == 0 ? 360 : 140, ColumnMinWidth(h->a));   // Doppelklick: Standardbreite
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (settings_.viewMode == 2) {   // Doppelklick in der Wolke: wie ein weiterer Klick
        if (Inside(gridRc_, x, y) && !visible_.empty()) CloudMouseDown(x, y);
        return;
    }
    int pos = GridPosAt(x, y);
    if (OnCheck(pos, x, y)) {   // schnelles Doppelklicken auf ein Auswahlkaestchen: zweimal umschalten
        ToggleCheck(pos, false);
        return;
    }
    if (settings_.singleClick && GetTickCount() < noDblUntil_) return;   // schon mit dem ersten Klick geoeffnet
    if (pos >= 0) {
        lDown_ = false;
        selectOnUp_ = false;
        OpenPos(pos);
    }
}

void App::OnRButtonUp(float x, float y) {
    if (sheet_.open || atlas_.open) return;
    if (settingsOpen_) return;
    if (dup_.open && !compare_.open) return;
    POINT screen;
    GetCursorPos(&screen);
    if (compare_.open) {
        ShowCompareMenu(x, y, screen);
        return;
    }
    if (viewer_.open) {
        if (x < ViewerAreaW()) ShowViewerMenu(screen);
        return;
    }
    if (const Hit* h = HitAt(x, y)) {
        if (h->type == HitType::SidebarItem) ShowSidebarMenu(h->a, screen);
        return;
    }
    if (settings_.viewMode == 2 && CloudPick(x, y) >= 0) {
        CloudContextMenu(x, y);
        return;
    }
    int pos = GridPosAt(x, y);
    if (pos < 0) {
        // leere Flaeche: Einfuegen in den aktuellen Ordner
        if (!Inside(gridRc_, x, y) || collectionsView_ || bookmarksView_ || path_.empty()) return;
        std::vector<std::wstring> clip;
        bool mv = false;
        MenuItem m;
        m.id = 1;
        m.text = T(L"Einf\u00fcgen");
        m.right = T(L"Strg+V");
        m.glyph = 0xE77F;
        m.disabled = !FilesFromClipboard(hwnd_, clip, mv);
        MenuItem nf;
        nf.id = 5;
        nf.text = T(L"Neuer Ordner");
        nf.right = T(L"Strg+Umschalt+N");
        nf.glyph = 0xE8F4;
        MenuItem r;
        r.id = 2;
        r.text = T(L"Aktualisieren");
        r.right = L"F5";
        MenuItem ks;
        ks.id = 3;
        ks.text = T(L"Kontaktabzug aus diesem Ordner \u2026");
        ks.glyph = 0xECA5;
        MenuItem kt;
        kt.id = 4;
        kt.text = T(L"Kacheltextur aus diesem Ordner \u2026");
        kt.glyph = 0xE8A9;
        UINT cmd = ShowPopupMenu(hwnd_, {m, nf, MenuItem::Sep(), r, MenuItem::Sep(), ks, kt}, screen, accent_);
        if (cmd == 1) PasteFiles(path_);
        else if (cmd == 5) CreateNewFolder();
        else if (cmd == 2) Reload(true);
        else if (cmd == 3) OpenContactSheet({path_});
        else if (cmd == 4) OpenAtlas(path_);
        return;
    }
    if (!selected_.count(visible_[pos])) {
        selected_.clear();
        selected_.insert(visible_[pos]);
    }
    focus_ = anchor_ = pos;
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowContextMenu(pos, screen);
}

void App::OnWheel(float x, float y, int delta, WPARAM keys) {
    if (renameHost_) FinishRename(true);   // beim Blaettern wuerde das Eingabefeld nicht mehr ueber dem Namen stehen
    if (sheet_.open) { SheetWheel(x, y, delta); return; }
    if (atlas_.open) return;
    if (!compare_.open && dup_.open) { DupWheel(delta); return; }
    if (compare_.open) {   // synchron zoomen, Punkt unter dem Mauszeiger bleibt stehen
        CompareZoom(x, y, std::pow(1.15f, delta / 120.0f));
        return;
    }
    if (viewer_.open && viewer_.slide) return;   // Diashow: kein Zoom
    if (settingsOpen_) {
        {
            float maxS = std::max(0.0f, settingsContentH_ - H_);
            settingsScroll_ = std::clamp(settingsScroll_ - delta / 120.0f * 60.0f, 0.0f, maxS);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }
    if (viewer_.open) {
        if (x >= ViewerAreaW() || viewer_.pos < 0 || viewer_.pos >= static_cast<int>(visible_.size())) return;
        const FileEntry& e = entries_[visible_[viewer_.pos]];
        float iw, ih;
        float s0 = ViewerScale(e, iw, ih);
        float s100 = 1.0f / r_.scale();
        float factor = std::pow(1.2f, delta / 120.0f);
        float s1 = std::clamp(s0 * factor, s100 * 0.02f, s100 * 32.0f);
        viewer_.zoom = s1 / s100;
        float cxv = ViewerAreaW() / 2;
        D2D1_POINT_2F c = D2D1::Point2F(cxv + viewer_.pan.x, H_ / 2 + viewer_.pan.y);
        D2D1_POINT_2F nc = D2D1::Point2F(x - (x - c.x) * s1 / s0, y - (y - c.y) * s1 / s0);
        viewer_.pan = D2D1::Point2F(nc.x - cxv, nc.y - H_ / 2);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (settings_.viewMode == 3 && Inside(gridRc_, x, y) && !visible_.empty()) {
        TimelineWheel(delta);
        return;
    }
    if (settings_.viewMode == 2 && Inside(gridRc_, x, y)) {
        if (keys & MK_CONTROL) CloudSetSpread(cloud_.spread * (delta > 0 ? 1.12f : 1.0f / 1.12f));   // Abstand der Karten
        else CloudWheel(x, y, delta);
        return;
    }
    if ((keys & MK_CONTROL) && settings_.viewMode == 0) {
        SetTileSize(settings_.tileSize + (delta > 0 ? 16 : -16));
        return;
    }
    float gh = gridRc_.bottom - gridRc_.top;
    float maxScroll = std::max(0.0f, contentH_ - gh);
    // Kacheln: knapp eine Zeile je Raste; Detailansicht: zweieinhalb Zeilen (die Zeilen sind niedrig)
    float stepPx = settings_.viewMode == 1 ? cellH_ * 2.5f : cellH_ * 0.8f;
    scrollTarget_ = std::clamp(scrollTarget_ - delta / 120.0f * stepPx, 0.0f, maxScroll);
    StartAnimation();
}

// ---------------------------------------------------------------------------
// Tastatur
// ---------------------------------------------------------------------------

bool App::OnKeyDown(WPARAM vk) {
    bool ctrl = CtrlDown(), shift = ShiftDown(), alt = AltDown();
    if (slidePending_ >= 0) {   // Diashow blendet gerade ab: Esc bricht ab, sonst nichts
        if (vk == VK_ESCAPE) {
            KillTimer(hwnd_, kTimerSlideStart);
            slidePending_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return true;
    }
    if (vk == VK_F11) { ToggleFullscreen(); return true; }
    if (vk == VK_F12 && ctrl && shift && !alt) {   // Messanzeige fuer Bildzeiten (nicht dokumentiert)
        perfHud_ = !perfHud_;
        perfN_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (sheet_.open) return SheetKey(vk);
    if (atlas_.open) return AtlasKey(vk);
    if (compare_.open) return CompareKey(vk);
    if (dup_.open) return DupKey(vk);

    if (settingsOpen_) {
        if (vk == VK_ESCAPE || vk == VK_BACK || (alt && vk == VK_LEFT)) CloseSettings();
        return vk != VK_F4;
    }

    if (viewer_.open) {
        const FileEntry* e = (viewer_.pos >= 0 && viewer_.pos < static_cast<int>(visible_.size()))
                                 ? &entries_[visible_[viewer_.pos]]
                                 : nullptr;
        if (vk == VK_RETURN && alt && e) {   // Eigenschaften (wie im Explorer)
            ShowProperties({e->path});
            return true;
        }
        auto zoomBy = [&](float f) {
            if (!e) return;
            float iw, ih;
            float s0 = ViewerScale(*e, iw, ih);
            float s100 = 1.0f / r_.scale();
            float s1 = std::clamp(s0 * f, s100 * 0.02f, s100 * 32.0f);
            viewer_.zoom = s1 / s100;
            viewer_.pan.x *= s1 / s0;
            viewer_.pan.y *= s1 / s0;
        };
        auto jumpTo = [&](bool last) {
            int i = ViewerEdge(last);
            if (i < 0) return;
            viewer_.pos = i;
            viewer_.zoom = 0;
            viewer_.pan = {0, 0};
            viewer_.rotation = 0;
            ViewerPrefetch();
        };
        if (viewer_.crop) {
            if (vk == VK_ESCAPE) CancelCrop();
            else if (vk == VK_RETURN) SaveCrop();
            return true;
        }
        if (viewer_.slide) {   // Diashow: Leertaste haelt an, Esc beendet, Pfeile blaettern (mit Ueberblenden)
            if (vk == VK_SPACE) { ToggleSlidePause(); return true; }
            if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_BACK) { StopSlideshow(false); return true; }
        }
        switch (vk) {
        case VK_ESCAPE: case VK_RETURN: case VK_BACK: CloseViewer(); return true;
        case VK_LEFT: case VK_PRIOR: case VK_UP: ViewerStep(-1); return true;
        case VK_RIGHT: case VK_NEXT: case VK_DOWN: case VK_SPACE: ViewerStep(1); return true;
        case VK_DELETE: ViewerDeleteCurrent(); return true;
        case VK_HOME: jumpTo(false); break;
        case VK_END: jumpTo(true); break;
        case VK_ADD: case VK_OEM_PLUS: zoomBy(1.25f); break;
        case VK_SUBTRACT: case VK_OEM_MINUS: zoomBy(0.8f); break;
        case '0': case VK_NUMPAD0: viewer_.zoom = 0; viewer_.pan = {0, 0}; break;
        case '1': case VK_NUMPAD1: viewer_.zoom = 1.0f; viewer_.pan = {0, 0}; break;
        case 'I':
            settings_.viewerInfo = !settings_.viewerInfo;
            StartAnimation();
            break;
        case 'R':
            viewer_.rotation = (viewer_.rotation + (shift ? 3 : 1)) % 4;
            viewer_.zoom = 0;
            viewer_.pan = {0, 0};
            break;
        case 'C':
            if (ctrl && e) {
                if (shift) CopyTextToClipboard(hwnd_, e->path);
                else CopyFilesToClipboard({e->path});
                SetStatus(T(L"In die Zwischenablage kopiert."));
            }
            break;
        case 'E': if (ctrl && e) ShowInExplorer(e->path); break;
        default: return false;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (searchFocus_) {
        if (searchSelAll_ && (vk == VK_DELETE || vk == VK_BACK)) {
            EditSearch(L"", 0);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_HOME || vk == VK_END) searchSelAll_ = false;
        switch (vk) {
        case VK_RETURN:
        case VK_DOWN:
            AddSearchHistory(search_);
            showHistory_ = false;
            searchFocus_ = false;
            if (focus_ < 0 && !visible_.empty()) SetFocusPos(0, false, false);
            break;
        case VK_ESCAPE:
            if (!search_.empty()) EditSearch(L"", 0);
            else searchFocus_ = false;
            break;
        case VK_LEFT: if (caret_ > 0) --caret_; break;
        case VK_RIGHT: if (caret_ < search_.size()) ++caret_; break;
        case VK_HOME: caret_ = 0; break;
        case VK_END: caret_ = search_.size(); break;
        case VK_DELETE:
            if (caret_ < search_.size()) {
                std::wstring s = search_;
                s.erase(caret_, 1);
                EditSearch(s, caret_);
            }
            break;
        case 'V':
            if (ctrl) {
                std::wstring t = PasteTextFromClipboard(hwnd_);
                for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                if (searchSelAll_) { EditSearch(t, t.size()); break; }
                std::wstring s = search_;
                s.insert(caret_, t);
                EditSearch(s, caret_ + t.size());
                break;
            }
            return false;
        case 'A': if (ctrl) { searchSelAll_ = !search_.empty(); showHistory_ = false; break; } return false;
        default: return false;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    // Neuer Ordner (Strg+Umschalt+N wie im Explorer)
    if (vk == 'N' && ctrl && shift && !alt) {
        CreateNewFolder();
        return true;
    }
    // Umbenennen (F2) und Eigenschaften (Alt+Enter)
    if (vk == VK_F2 && !ctrl && !alt && focus_ >= 0) {
        BeginRename(focus_);
        return true;
    }
    if (vk == VK_RETURN && alt) {
        std::vector<std::wstring> sel = SelectedPaths();
        if (sel.empty() && focus_ >= 0 && focus_ < static_cast<int>(visible_.size())) sel.push_back(entries_[visible_[focus_]].path);
        ShowProperties(sel);
        return true;
    }
    if (settings_.viewMode == 2 && ctrl && !alt && !shift && (vk == '0' || vk == VK_NUMPAD0)) {
        CloudSetSpread(1.0f);   // Standardabstand
        return true;
    }
    if (settings_.viewMode == 3 && !ctrl && !alt && !settingsOpen_ && TimelineKey(vk)) return true;
    if (settings_.viewMode == 2 && !ctrl && !alt && CloudKey(vk)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    int n = static_cast<int>(visible_.size());
    int rowsPerPage = std::max(1, static_cast<int>((gridRc_.bottom - gridRc_.top) / cellH_));
    auto moveFocus = [&](int delta) {
        if (n == 0) return;
        int p = focus_ < 0 ? 0 : std::clamp(focus_ + delta, 0, n - 1);
        SetFocusPos(p, shift, ctrl);
    };
    switch (vk) {
    case VK_LEFT:
        if (alt) { GoBack(); return true; }
        moveFocus(-1); return true;
    case VK_RIGHT:
        if (alt) { GoForward(); return true; }
        moveFocus(1); return true;
    case VK_UP:
        if (alt) { GoUp(); return true; }
        if (n) SetFocusPos(focus_ < 0 ? 0 : VerticalNeighbor(focus_, -1), shift, ctrl);
        return true;
    case VK_DOWN:
        if (n) SetFocusPos(focus_ < 0 ? 0 : VerticalNeighbor(focus_, 1), shift, ctrl);
        return true;
    case VK_PRIOR: moveFocus(-cols_ * rowsPerPage); return true;
    case VK_NEXT: moveFocus(cols_ * rowsPerPage); return true;
    case VK_HOME: if (n) SetFocusPos(0, shift, false); return true;
    case VK_END: if (n) SetFocusPos(n - 1, shift, false); return true;
    case VK_BACK: GoBack(); return true;
    case VK_BROWSER_BACK: GoBack(); return true;
    case VK_BROWSER_FORWARD: GoForward(); return true;
    case VK_RETURN: if (focus_ >= 0) OpenPos(focus_); return true;
    case VK_SPACE:
        if (ctrl && focus_ >= 0) {
            uint32_t idx = visible_[focus_];
            if (selected_.count(idx)) selected_.erase(idx);
            else selected_.insert(idx);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (focus_ >= 0) {
            const FileEntry& e = entries_[visible_[focus_]];
            if (e.kind == Kind::Image && !TryZoomOpen(focus_)) OpenViewer(focus_);
        }
        return true;
    case VK_DELETE: DeleteSelection(); return true;
    case VK_ESCAPE:
        selected_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    case VK_F5:
        if (collectionsView_) indexer_.Rescan();
        Reload(true);
        return true;
    case VK_F9:
        settings_.sidebar = !settings_.sidebar;
        StartAnimation();
        return true;
    case VK_APPS:
        if (focus_ >= 0) {
            POINT pt;
            GetCursorPos(&pt);
            ShowContextMenu(focus_, pt);
        }
        return true;
    default: break;
    }
    if (ctrl) {
        switch (vk) {
        case 'F': searchFocus_ = true; caret_ = search_.size(); break;
        case 'I': settings_.infoPanel = !settings_.infoPanel; StartAnimation(); break;
        case '1': SetViewMode(0); break;
        case '2': SetViewMode(1); break;
        case '3': SetViewMode(2); break;
        case '4': SetViewMode(3); break;
        case 'A':
            selected_.clear();
            for (uint32_t i : visible_) selected_.insert(i);
            break;
        case 'C': {
            auto paths = SelectedPaths();
            if (paths.empty()) return true;
            if (shift) {
                std::wstring s;
                for (auto& p : paths) { if (!s.empty()) s += L"\r\n"; s += p; }
                CopyTextToClipboard(hwnd_, s);
                SetStatus(T(L"Pfad kopiert."));
            } else if (CopyFilesToClipboard(paths)) {
                SetStatus(std::to_wstring(paths.size()) + T(L" Datei(en) in die Zwischenablage kopiert."));
            }
            break;
        }
        case 'V':
            if (!collectionsView_ && !bookmarksView_) PasteFiles(path_);
            break;
        case 'E': {
            auto paths = SelectedPaths();
            ShowInExplorer(paths.empty() ? path_ : paths.front());
            break;
        }
        case 'R':
            if (!collectionsView_) { settings_.recursive = !settings_.recursive; Reload(false); }
            break;
        case VK_OEM_PLUS: case VK_ADD: SetTileSize(settings_.tileSize + 32); break;
        case VK_OEM_MINUS: case VK_SUBTRACT: SetTileSize(settings_.tileSize - 32); break;
        case '0': case VK_NUMPAD0: SetTileSize(160); break;
        case VK_OEM_COMMA: OpenSettings(); break;
        default: return false;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return false;
}

void App::OnChar(wchar_t c) {
    if (sheet_.open) { SheetChar(c); return; }
    if (atlas_.open || compare_.open || dup_.open) return;
    if (viewer_.open || settingsOpen_) return;
    if (c == 8) {
        if (searchFocus_ && searchSelAll_) { EditSearch(L"", 0); return; }
        if (searchFocus_ && caret_ > 0) {
            std::wstring s = search_;
            s.erase(caret_ - 1, 1);
            EditSearch(s, caret_ - 1);
        }
        return;
    }
    if (c < 32 || c == 127) return;
    if (CtrlDown() && !AltDown()) return;
    if (c == L' ' && !searchFocus_) return;
    if (!searchFocus_) {
        searchFocus_ = true;
        caret_ = search_.size();
    }
    if (searchSelAll_) { EditSearch(std::wstring(1, c), 1); return; }
    std::wstring s = search_;
    s.insert(caret_, 1, c);
    EditSearch(s, caret_ + 1);
}

// ---------------------------------------------------------------------------
// Menues (eigene, dunkle Popup-Menues)
// ---------------------------------------------------------------------------

static POINT BelowRect(HWND hwnd, const D2D1_RECT_F& rc, float scale, float gap) {
    POINT pt = {static_cast<LONG>(rc.left * scale), static_cast<LONG>((rc.bottom + gap) * scale)};
    ClientToScreen(hwnd, &pt);
    return pt;
}

std::vector<MenuItem> App::ChipMenuItems(ChipId id, UINT base) {
    ComputeDateBounds();
    std::vector<MenuItem> items;
    auto countOption = [&](int v) {
        uint32_t c = 0;
        for (auto& e : entries_)
            if (PassFilters(e, static_cast<int>(id)) && MatchOption(id, v, e)) ++c;
        return c;
    };
    auto add = [&](UINT cmd, const std::wstring& label, bool checked, int64_t count) {
        MenuItem m;
        m.id = cmd;
        m.text = label;
        m.checked = checked;
        if (count >= 0) m.right = std::to_wstring(count);
        m.disabled = count == 0 && !checked;
        items.push_back(m);
    };
    switch (id) {
    case ChipId::Type: {
        const wchar_t* n[] = {T(L"Alle"), T(L"Ordner"), T(L"Bilder"), T(L"SVG-Grafiken"), L"Videos", L"PDF", L"Photoshop (PSD)",
                              L"Illustrator (AI)", T(L"Sonstige Dateien")};
        const int v[] = {-1, 0, 1, 13, 2, 10, 11, 12, 3};
        for (int i = 0; i < 9; ++i) add(base + i, n[i], filters_.type == v[i], countOption(v[i]));
        break;
    }
    case ChipId::Date:
    case ChipId::Taken: {
        const wchar_t* n[] = {T(L"Alle"), T(L"Heute"), T(L"Letzte 7 Tage"), T(L"Letzte 30 Tage"), T(L"Dieses Jahr"), T(L"Letztes Jahr"),
                              T(L"Älter")};
        int cur = id == ChipId::Taken ? filters_.taken : filters_.date;
        for (int i = 0; i < 7; ++i) add(base + i, n[i], cur == i, countOption(i));
        items.push_back(MenuItem::Sep());
        MenuItem m;
        m.id = base + 7;
        m.text = cur == 7 ? T(L"Individuell: ") + SpanLabel(id == ChipId::Taken ? filters_.takenFrom : filters_.dateFrom,
                                                         id == ChipId::Taken ? filters_.takenTo : filters_.dateTo)
                          : T(L"Individuell \u2026");
        m.checked = cur == 7;
        m.glyph = 0xE787;
        items.push_back(m);
        break;
    }
    case ChipId::Size: {
        const wchar_t* n[] = {T(L"Alle"), T(L"Kleiner als 1 MB"), T(L"1 bis 10 MB"), T(L"10 bis 100 MB"), T(L"Größer als 100 MB")};
        for (int i = 0; i < 5; ++i) add(base + i, n[i], filters_.size == i, countOption(i));
        break;
    }
    case ChipId::Resolution: {
        const wchar_t* n[] = {T(L"Alle"), T(L"Unter HD (< 1280 px)"), T(L"Ab HD (≥ 1280 px)"), T(L"Ab Full HD (≥ 1920 px)"),
                              T(L"Ab 4K (≥ 3840 px)"), T(L"Über 20 Megapixel")};
        for (int i = 0; i < 6; ++i) add(base + i, n[i], filters_.res == i, countOption(i));
        break;
    }
    case ChipId::Orientation: {
        const wchar_t* n[] = {T(L"Alle"), T(L"Querformat"), T(L"Hochformat"), T(L"Quadratisch"), T(L"Panorama (ab 2:1)")};
        for (int i = 0; i < 5; ++i) add(base + i, n[i], filters_.orient == i, countOption(i));
        break;
    }
    case ChipId::Source:
        add(base, T(L"Alle"), filters_.source.empty(), -1);
        for (size_t i = 0; i < sourceFacets_.size() && i < 30; ++i)
            add(base + 1 + static_cast<UINT>(i), T(sourceFacets_[i].first), filters_.source == sourceFacets_[i].first,
                sourceFacets_[i].second);
        break;
    case ChipId::Pattern:
        add(base, T(L"Alle"), filters_.templ == UINT32_MAX, -1);
        for (size_t i = 0; i < templFacets_.size(); ++i)
            add(base + 1 + static_cast<UINT>(i), TPattern(templDict_.Word(templFacets_[i].id)), filters_.templ == templFacets_[i].id,
                templFacets_[i].count);
        break;
    default:
        break;
    }

    return items;
}

void App::ShowChipMenu(ChipId id, const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items = ChipMenuItems(id, 100);
    if (items.empty()) return;
    openMenu_ = static_cast<int>(id);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    openMenu_ = -1;
    lastMenu_ = static_cast<int>(id);
    lastMenuClose_ = GetTickCount();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!cmd) return;
    ApplyChipChoice(id, static_cast<int>(cmd) - 100, rc);
}

void App::ApplyChipChoice(ChipId id, int v, const D2D1_RECT_F& rc) {
    switch (id) {
    case ChipId::Type: { const int map[] = {-1, 0, 1, 13, 2, 10, 11, 12, 3}; if (v >= 0 && v < 9) filters_.type = map[v]; break; }
    case ChipId::Date:
    case ChipId::Taken:
        if (v == 7) { OpenDatePicker(id, rc); return; }
        (id == ChipId::Taken ? filters_.taken : filters_.date) = v;
        break;
    case ChipId::Size: filters_.size = v; break;
    case ChipId::Resolution: filters_.res = v; break;
    case ChipId::Orientation: filters_.orient = v; break;
    case ChipId::Source:
        filters_.source = v == 0 ? std::wstring() : sourceFacets_[static_cast<size_t>(v - 1)].first;
        break;
    case ChipId::Pattern:
        filters_.templ = v == 0 ? UINT32_MAX : templFacets_[static_cast<size_t>(v - 1)].id;
        break;
    default: break;
    }
    Refilter();
}

// "..." links neben "Sortieren": die Filter, die bei schmalem Fenster nicht mehr in die Leiste passen, jeweils
// mit ihrem Menue als Untermenue (aktiver Wert rechts daneben)
void App::ShowMoreFiltersMenu(const D2D1_RECT_F& rc) {
    if (chipOverflow_.empty()) return;
    std::vector<MenuItem> items;
    for (ChipId id : chipOverflow_) {
        MenuItem m;
        if (id == ChipId::Recursive) {
            m.id = 900;
            m.text = T(L"Unterordner");
            m.checked = settings_.recursive;
            m.glyph = 0xE8B7;
            items.push_back(m);
            continue;
        }
        static const wchar_t* names[] = {N_(L"Typ"), N_(L"Datum"), N_(L"Größe"), N_(L"Auflösung"), N_(L"Ausrichtung"),
                                         N_(L"Quelle"), N_(L"Muster")};
        int k = static_cast<int>(id);
        m.text = id == ChipId::Taken ? std::wstring(T(L"Aufgenommen")) : std::wstring(T(names[std::clamp(k, 0, 6)]));
        std::wstring cur = ChipLabel(id);
        if (cur != m.text) m.right = cur;   // aktiver Wert
        UINT base = 10000 + static_cast<UINT>(k) * 1000;   // je Filter eigener Bereich (Muster koennen viele sein)
        m.submenu = [this, id, base] { return ChipMenuItems(id, base); };
        items.push_back(m);
    }
    openMenu_ = 2000;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    openMenu_ = -1;
    lastMenu_ = 2000;
    lastMenuClose_ = GetTickCount();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (cmd == 900) {
        settings_.recursive = !settings_.recursive;
        Reload(false);
        return;
    }
    if (cmd < 10000) return;
    ApplyChipChoice(static_cast<ChipId>((cmd - 10000) / 1000), static_cast<int>((cmd - 10000) % 1000), rc);
}

void App::ShowSortMenu(const D2D1_RECT_F& rc) {
    if (settings_.viewMode == 3) {   // Zeitleiste: nur nach Aufnahmedatum, auf- oder absteigend
        std::vector<MenuItem> items;
        MenuItem k;
        k.id = 105;
        k.text = T(L"Aufgenommen");
        k.checked = true;
        items.push_back(k);
        items.push_back(MenuItem::Sep());
        MenuItem asc, desc;
        asc.id = 200; asc.text = T(L"Aufsteigend"); asc.checked = settings_.timelineAsc;
        desc.id = 201; desc.text = T(L"Absteigend"); desc.checked = !settings_.timelineAsc;
        items.push_back(asc);
        items.push_back(desc);
        openMenu_ = static_cast<int>(ChipId::Sort);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
        openMenu_ = -1;
        lastMenu_ = static_cast<int>(ChipId::Sort);
        lastMenuClose_ = GetTickCount();
        if (cmd == 200 || cmd == 201) {
            settings_.timelineAsc = cmd == 200;
            settings_.Save();
            tl_.version = ~0ull;   // neu anordnen, vorn beginnen
            tl_.scroll = tl_.target = 0;
            tl_.vel = 0;
            tl_.items.clear();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    const wchar_t* n[] = {L"Name", T(L"Datum"), T(L"Größe"), T(L"Auflösung"), T(L"Typ"), T(L"Aufgenommen")};
    std::vector<MenuItem> items;
    for (int i = 0; i < 6; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = n[i];
        m.checked = settings_.sortKey == i;
        items.push_back(m);
    }
    items.push_back(MenuItem::Sep());
    MenuItem asc, desc;
    asc.id = 200; asc.text = T(L"Aufsteigend"); asc.checked = !settings_.sortDesc;
    desc.id = 201; desc.text = T(L"Absteigend"); desc.checked = settings_.sortDesc;
    items.push_back(asc);
    items.push_back(desc);
    items.push_back(MenuItem::Sep());
    MenuItem grp;
    grp.id = 300;
    grp.text = T(L"Gruppieren");
    grp.checked = settings_.group;
    grp.glyph = 0xF168;
    items.push_back(grp);

    openMenu_ = static_cast<int>(ChipId::Sort);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    POINT pt = BelowRect(hwnd_, rc, r_.scale(), 4);
    UINT cmd = ShowPopupMenu(hwnd_, items, pt, accent_);
    openMenu_ = -1;
    lastMenu_ = static_cast<int>(ChipId::Sort);
    lastMenuClose_ = GetTickCount();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (cmd == 300) {
        settings_.group = !settings_.group;
        RememberSort();
        Layout();
        if (focus_ >= 0) ScrollToPos(focus_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (cmd == 200) settings_.sortDesc = false;
    else if (cmd == 201) settings_.sortDesc = true;
    else if (cmd >= 100 && cmd < 106) settings_.sortKey = static_cast<int>(cmd) - 100;
    else return;
    RememberSort();
    Refilter();
}

void App::ShowCrumbMenu(int index, const D2D1_RECT_F& rc) {
    if (index < 0 || index >= static_cast<int>(crumbPaths_.size())) return;
    std::wstring folder = crumbPaths_[index];
    std::wstring next = index + 1 < static_cast<int>(crumbPaths_.size()) ? crumbPaths_[index + 1] : std::wstring();

    std::vector<std::wstring> dirs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(LongPath(JoinPath(folder, L"*")).c_str(), FindExInfoBasic, &fd,
                                FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
                continue;
            if (!settings_.showHidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                continue;
            dirs.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd) && dirs.size() < 5000);
        FindClose(h);
    }
    std::sort(dirs.begin(), dirs.end(),
              [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });

    std::vector<MenuItem> items;
    std::wstring nextName = next.empty() ? std::wstring() : FileNameOf(next);
    for (size_t i = 0; i < dirs.size(); ++i) {
        MenuItem m;
        m.id = 1000 + static_cast<UINT>(i);
        m.text = dirs[i];
        m.checked = !nextName.empty() && _wcsicmp(dirs[i].c_str(), nextName.c_str()) == 0;
        items.push_back(m);
    }
    if (items.empty()) {
        MenuItem m;
        m.text = T(L"Keine Unterordner");
        m.disabled = true;
        items.push_back(m);
    }

    openMenu_ = 1000 + index;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 6), accent_);
    openMenu_ = -1;
    lastMenu_ = 1000 + index;
    lastMenuClose_ = GetTickCount();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (cmd >= 1000 && cmd - 1000 < dirs.size()) Navigate(JoinPath(folder, dirs[cmd - 1000]));
}

void App::ShowContextMenu(int pos, POINT screen) {
    if (pos < 0 || pos >= static_cast<int>(visible_.size())) return;
    const FileEntry e = entries_[visible_[pos]];
    auto item = [](UINT id, const std::wstring& text, const std::wstring& right = {}) {
        MenuItem m;
        m.id = id;
        m.text = text;
        m.right = right;
        return m;
    };
    std::vector<MenuItem> items;
    items.push_back(item(1, e.kind == Kind::Image ? T(L"Im Betrachter öffnen") : T(L"Öffnen"), L"Enter"));
    if (e.kind == Kind::Image) items.push_back(item(2, T(L"Mit Standard-App öffnen")));
    auto openWith = std::make_shared<std::vector<OpenWithApp>>();
    if (e.kind != Kind::Folder) items.push_back(OpenWithMenuItem(e, openWith));
    std::vector<std::wstring> cmpPaths = SelectedPaths();
    bool canCompare = cmpPaths.size() == 2;
    for (auto& p : cmpPaths) {
        auto it = byPath_.find(p);
        if (it == byPath_.end() || entries_[it->second].kind != Kind::Image || entries_[it->second].offline) canCompare = false;
    }
    if (canCompare) {
        MenuItem cm = item(32, T(L"Bilder vergleichen \u2026"));
        cm.glyph = 0xE81E;
        items.push_back(cm);
    }
    items.push_back(item(3, T(L"Im Explorer zeigen"), T(L"Strg+E")));
    items.push_back(MenuItem::Sep());
    items.push_back(item(4, T(L"Kopieren"), T(L"Strg+C")));
    {
        std::vector<std::wstring> clip;
        bool mv = false;
        bool canPaste = FilesFromClipboard(hwnd_, clip, mv) && (e.kind == Kind::Folder || (!collectionsView_ && !bookmarksView_));
        MenuItem pm = item(9, e.kind == Kind::Folder ? T(L"In diesen Ordner einf\u00fcgen") : T(L"Einf\u00fcgen"),
                           e.kind == Kind::Folder ? L"" : T(L"Strg+V"));
        pm.disabled = !canPaste;
        items.push_back(pm);
    }
    items.push_back(item(5, T(L"Pfad kopieren"), T(L"Strg+Umschalt+C")));
    items.push_back(MenuItem::Sep());
    if (SelectedPaths().size() <= 1 && !e.offline) {
        MenuItem rn = item(30, T(L"Umbenennen"), L"F2");
        rn.glyph = 0xE8AC;
        items.push_back(rn);
    }
    items.push_back(item(6, T(L"In den Papierkorb"), T(L"Entf")));
    items.push_back(MenuItem::Sep());
    bool marked = IsBookmarked(e.path);
    MenuItem bm = item(8, marked ? T(L"Von Merkliste entfernen") : T(L"Zur Merkliste hinzufügen"));
    bm.glyph = marked ? 0xE735 : 0xE734;
    items.push_back(bm);
    if (e.kind == Kind::Folder) items.push_back(item(7, T(L"Als Sammlung hinzufügen")));
    {
        // Kontaktabzug: nur, wenn Bilder oder Ordner ausgewaehlt sind
        bool any = false;
        for (auto& p : SelectedPaths()) {
            auto it = byPath_.find(p);
            if (it != byPath_.end() && (entries_[it->second].kind == Kind::Image || entries_[it->second].kind == Kind::Folder)) {
                any = true;
                break;
            }
        }
        if (any) {
            items.push_back(MenuItem::Sep());
            MenuItem ks = item(20, T(L"Kontaktabzug erstellen \u2026"));
            ks.glyph = 0xECA5;
            items.push_back(ks);
        }
        // Kacheltextur: nur fuer genau einen Ordner
        if (e.kind == Kind::Folder && SelectedPaths().size() <= 1) {
            MenuItem kt = item(21, T(L"Kacheltextur erstellen \u2026"));
            kt.glyph = 0xE8A9;
            items.push_back(kt);
        }
    }
    items.push_back(MenuItem::Sep());
    items.push_back(item(31, T(L"Eigenschaften"), L"Alt+Enter"));
    UINT cmd = ShowPopupMenu(hwnd_, items, screen, accent_);
    switch (cmd) {
    case 30: BeginRename(pos); break;
    case 32: OpenCompare(cmpPaths); break;
    case 31: {
        std::vector<std::wstring> sel = SelectedPaths();
        if (sel.empty()) sel.push_back(e.path);
        ShowProperties(sel);
        break;
    }
    case 1: OpenPos(pos); break;
    case 2: OpenWithDefaultApp(hwnd_, e.path); break;
    case 999: RunOpenWith(e, *openWith, cmd); break;
    case 3: ShowInExplorer(e.path); break;
    case 4: if (CopyFilesToClipboard(SelectedPaths())) SetStatus(T(L"In die Zwischenablage kopiert.")); break;
    case 5: {
        std::wstring s;
        for (auto& p : SelectedPaths()) { if (!s.empty()) s += L"\r\n"; s += p; }
        CopyTextToClipboard(hwnd_, s);
        SetStatus(T(L"Pfad kopiert."));
        break;
    }
    case 6: DeleteSelection(); break;
    case 7: AddCollection(e.path); break;
    case 9: PasteFiles(e.kind == Kind::Folder ? e.path : path_); break;
    case 20: {
        std::vector<std::wstring> sel = SelectedPaths();
        if (sel.empty()) sel.push_back(e.path);
        OpenContactSheet(sel);
        break;
    }
    case 21: OpenAtlas(e.path); break;
    case 8: {
        std::vector<std::wstring> sel = SelectedPaths();
        if (sel.empty()) sel.push_back(e.path);
        SetBookmarked(sel, !marked);
        break;
    }
    default:
        if (cmd >= 1000) RunOpenWith(e, *openWith, cmd);
        break;
    }
}

// "Oeffnen mit": Untermenue mit den registrierten Apps der Dateiendung (mit Symbol) und "Andere App auswaehlen"
MenuItem App::OpenWithMenuItem(const FileEntry& e, std::shared_ptr<std::vector<OpenWithApp>> apps) {
    MenuItem m;
    m.id = 10;
    m.text = T(L"Öffnen mit");
    std::wstring path = e.path;
    m.submenu = [apps, path]() {
        if (apps->empty()) *apps = GetOpenWithApps(path);
        std::vector<MenuItem> items;
        for (size_t i = 0; i < apps->size(); ++i) {
            MenuItem a;
            a.id = 1000 + static_cast<UINT>(i);
            a.text = (*apps)[i].name;
            a.iconPx = (*apps)[i].iconPx;
            a.iconW = (*apps)[i].iconW;
            a.iconH = (*apps)[i].iconH;
            if (a.iconPx.empty()) a.glyph = 0xE737;
            items.push_back(std::move(a));
        }
        if (!items.empty()) items.push_back(MenuItem::Sep());
        MenuItem other;
        other.id = 999;
        other.text = T(L"Andere App auswählen …");
        other.glyph = 0xE71D;
        items.push_back(other);
        return items;
    };
    return m;
}

void App::ShowThemeMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 3; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = T(kThemeNames[i]);
        m.checked = settings_.theme == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 102) return;
    settings_.theme = static_cast<int>(cmd) - 100;
    settings_.Save();
    ApplyTheme();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowLanguageMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 3; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = LanguageName(i);
        m.checked = settings_.language == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 102) return;
    settings_.language = static_cast<int>(cmd) - 100;
    langOverride_ = -1;
    settings_.Save();
    ApplyLanguage();
}

void App::ShowViewerFullscreenMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 2; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = T(kViewerFullscreenNames[i]);
        m.checked = (settings_.viewerFullscreen ? 0 : 1) == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 101) return;
    settings_.viewerFullscreen = cmd == 100;
    settings_.Save();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowSingleClickMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 2; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = T(kSingleClickNames[i]);
        m.checked = (settings_.singleClick ? 1 : 0) == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 101) return;
    settings_.singleClick = cmd == 101;
    settings_.Save();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowGeoClickMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 2; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = T(kGeoClickNames[i]);
        m.checked = settings_.geoClick == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 101) return;
    settings_.geoClick = static_cast<int>(cmd - 100);
    settings_.Save();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowCheckBoxMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 2; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = T(kCheckBoxNames[i]);
        m.checked = (settings_.checkBoxes ? 1 : 0) == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 101) return;
    settings_.checkBoxes = cmd == 101;
    settings_.Save();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowAnimationsMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 2; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = T(kCheckBoxNames[i]);   // Aus (Standard) / Ein
        m.checked = (settings_.animations ? 1 : 0) == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 101) return;
    settings_.animations = cmd == 101;
    settings_.Save();
    hoverFades_.clear();
    posFades_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::ShowViewerSmallMenu(const D2D1_RECT_F& rc) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 2; ++i) {
        MenuItem m;
        m.id = 100 + i;
        m.text = T(kViewerSmallNames[i]);
        m.checked = settings_.viewerSmall == i;
        items.push_back(m);
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, BelowRect(hwnd_, rc, r_.scale(), 4), accent_);
    if (cmd < 100 || cmd > 101) return;
    settings_.viewerSmall = static_cast<int>(cmd) - 100;
    settings_.Save();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Kontextmenue im Bildbetrachter (gilt fuer das angezeigte Bild)
void App::ShowViewerMenu(POINT screen) {
    int n = static_cast<int>(visible_.size());
    if (viewer_.pos < 0 || viewer_.pos >= n) return;
    const FileEntry e = entries_[visible_[viewer_.pos]];
    auto item = [](UINT id, const std::wstring& text, const std::wstring& right = {}, wchar_t glyph = 0) {
        MenuItem m;
        m.id = id;
        m.text = text;
        m.right = right;
        m.glyph = glyph;
        return m;
    };
    auto openWith = std::make_shared<std::vector<OpenWithApp>>();
    std::vector<MenuItem> items;
    items.push_back(item(2, T(L"Mit Standard-App \u00f6ffnen")));
    items.push_back(OpenWithMenuItem(e, openWith));
    items.push_back(item(3, T(L"Im Explorer zeigen"), T(L"Strg+E")));
    items.push_back(MenuItem::Sep());
    items.push_back(item(4, T(L"Kopieren"), T(L"Strg+C")));
    items.push_back(item(5, T(L"Pfad kopieren"), T(L"Strg+Umschalt+C")));
    items.push_back(MenuItem::Sep());
    items.push_back(item(12, viewer_.zoom > 0 ? T(L"Einpassen") : T(L"Originalgr\u00f6\u00dfe (100 %)"), viewer_.zoom > 0 ? L"0" : L"1"));
    MenuItem smooth = item(13, T(L"Beim Vergr\u00f6\u00dfern gl\u00e4tten"));
    smooth.checked = settings_.viewerSmooth;
    items.push_back(smooth);
    items.push_back(MenuItem::Sep());
    bool marked = IsBookmarked(e.path);
    items.push_back(item(8, marked ? T(L"Von Merkliste entfernen") : T(L"Zur Merkliste hinzuf\u00fcgen"), L"",
                         marked ? 0xE735 : 0xE734));
    items.push_back(item(6, T(L"In den Papierkorb"), T(L"Entf"), 0xE74D));
    items.push_back(MenuItem::Sep());
    items.push_back(item(14, T(L"Eigenschaften"), L"Alt+Enter"));
    UINT cmd = ShowPopupMenu(hwnd_, items, screen, accent_);
    switch (cmd) {
    case 2: OpenWithDefaultApp(hwnd_, e.path); break;
    case 3: ShowInExplorer(e.path); break;
    case 4: if (CopyFilesToClipboard({e.path})) SetStatus(T(L"In die Zwischenablage kopiert.")); break;
    case 5: CopyTextToClipboard(hwnd_, e.path); SetStatus(T(L"Pfad kopiert.")); break;
    case 6: ViewerDeleteCurrent(); break;
    case 8: SetBookmarked({e.path}, !marked); break;
    case 12:
        viewer_.zoom = viewer_.zoom > 0 ? 0 : 1.0f;
        viewer_.pan = {0, 0};
        break;
    case 13: settings_.viewerSmooth = !settings_.viewerSmooth; break;
    case 14: ShowProperties({e.path}); break;   // interpolieren oder Pixel vergroessern
    default:
        if (cmd == 999 || cmd >= 1000) RunOpenWith(e, *openWith, cmd, true);
        break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::RunOpenWith(const FileEntry& e, const std::vector<OpenWithApp>& apps, UINT cmd, bool onlyThis) {
    if (cmd == 999) {
        ShowOpenWithDialog(hwnd_, e.path);
        return;
    }
    size_t i = cmd - 1000;
    if (cmd < 1000 || i >= apps.size()) return;
    // markierte Dateien mit gleicher Endung gemeinsam oeffnen, sonst nur diese Datei
    std::vector<std::wstring> paths;
    if (!onlyThis)
    for (auto& p : SelectedPaths()) {
        const wchar_t* x = PathFindExtensionW(p.c_str());
        if (x && *x && _wcsicmp(x + 1, e.ext.c_str()) == 0) paths.push_back(p);
    }
    if (paths.empty()) paths.push_back(e.path);
    if (!OpenWithApp_Invoke(apps[i], paths)) SetStatus(T(L"Die App konnte nicht gestartet werden."));
}

void App::ShowSidebarMenu(int index, POINT screen) {
    if (index < 0 || index >= static_cast<int>(sidebar_.size())) return;
    const SidebarItem it = sidebar_[index];
    if (it.kind == SidebarItem::Header) return;
    auto item = [](UINT id, const std::wstring& text) {
        MenuItem m;
        m.id = id;
        m.text = text;
        return m;
    };
    std::vector<MenuItem> items;
    items.push_back(item(1, T(L"Öffnen")));
    if (it.kind == SidebarItem::Collection) {
        items.push_back(item(2, T(L"Neu indexieren")));
        items.push_back(MenuItem::Sep());
        items.push_back(item(3, T(L"Sammlung entfernen")));
    } else if (it.kind == SidebarItem::AllCollections) {
        items.push_back(item(2, T(L"Alle neu indexieren")));
    } else if (it.kind == SidebarItem::SavedSearch) {
        items.push_back(item(6, T(L"Umbenennen")));
        items.push_back(MenuItem::Sep());
        items.push_back(item(7, T(L"Suche l\u00f6schen")));
    } else if (!it.path.empty()) {
        items.push_back(item(4, T(L"Als Sammlung hinzufügen")));
        items.push_back(item(5, T(L"Im Explorer zeigen")));
    }
    UINT cmd = ShowPopupMenu(hwnd_, items, screen, accent_);
    switch (cmd) {
    case 1:
        if (it.kind == SidebarItem::AllCollections) ShowAllCollections();
        else if (it.kind == SidebarItem::SavedSearch) OpenSavedSearch(it.idx);
        else if (it.kind == SidebarItem::Bookmarks) ShowBookmarks();
        else Navigate(it.path);
        break;
    case 2: indexer_.Rescan(); SetStatus(T(L"Indexierung gestartet.")); break;
    case 3: RemoveCollection(it.path); break;
    case 4: AddCollection(it.path); break;
    case 5: ShowInExplorer(it.path); break;
    case 6: BeginRenameSearch(index); break;
    case 7: DeleteSavedSearch(it.idx); break;
    default: break;
    }
}

} // namespace peek
