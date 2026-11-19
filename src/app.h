#pragma once
// Hauptfenster von peek: Zustand, Navigation, Filter, Darstellung und Eingabe.
#include "core/lang.h"
#include "core/patterns.h"
#include "db.h"
#include "dragdrop.h"
#include "indexer.h"
#include "loader.h"
#include "meta.h"
#include "popup.h"
#include "renderer.h"
#include "settings.h"
#include "thumbs.h"
#include "pdfsheet.h"
#include "atlaswriter.h"
#include <map>
#include <unordered_map>

namespace peek {

// Auswahllisten der Einstellungen (deutsche Schluessel, Anzeige mit T())
extern const wchar_t* const kThemeNames[3];
extern const wchar_t* const kViewerSmallNames[2];
extern const wchar_t* const kViewerFullscreenNames[2];   // 0 = Vollbild, 1 = Taskleiste sichtbar
const wchar_t* LanguageName(int setting);   // 0 wie Windows, 1 Deutsch, 2 English

enum class HitType {
    None, SidebarToggle, NavBack, NavForward, NavUp, Refresh, Crumb, CrumbChevron, Search, SearchClear,
    Chip, Term, ResetFilters, SidebarItem, SidebarAdd, SettingsButton, InfoToggle, ZoomSlider,
    ZoomIn, ZoomOut, ViewGrid, ViewList, SortButton, ListHeader, ListResize,
    ViewerClose, ViewerPrev, ViewerNext, ViewerInfo, ViewerCrop, CropSave, CropCancel,
    ClearBookmarks, SearchHistItem, SearchHistDel, SearchHistClear, SearchParam,
    ViewCloud,
    SettingsBack, SettingsOpenData, SettingsRescan, SettingsDeleteIndex, SettingsTheme, SettingsShortcuts, SettingsViewerSmall, SettingsLanguage, SettingsViewerFullscreen,
    SheetCancel, SheetCreate, SheetOrient, SheetColsMinus, SheetColsPlus, SheetRowsMinus, SheetRowsPlus, SheetNames,
    SheetExts, SheetQuality, SheetPrev, SheetNext, SheetHeadline, SheetCheck, SheetCell, SheetStripCheck, SheetStrip,
    SheetOpenPdf, BookmarkSheet,
    AtlasCancel, AtlasCreate, AtlasColsMinus, AtlasColsPlus, AtlasRowsMinus, AtlasRowsPlus, AtlasTile,
    CompareMode, CompareClose
};

enum class ChipId { Type = 0, Date, Size, Resolution, Orientation, Source, Pattern, Sort, Recursive, Taken, Count };

struct Hit {
    D2D1_RECT_F rc{};
    HitType type = HitType::None;
    int a = 0;
};

struct SidebarItem {
    enum Kind { Header, Folder, Drive, Collection, AllCollections, Bookmarks } kind = Folder;
    std::wstring label;
    std::wstring path;
    wchar_t glyph = 0;
};

struct Bmp {
    ComPtr<ID2D1Bitmap1> bmp;
    uint32_t srcW = 0, srcH = 0;
    uint64_t lastUse = 0;
    size_t bytes = 0;
    bool requested = false;
    bool failed = false;
    int reqPrio = 9;
};

struct Filters {
    int type = -1;          // -1 alle, sonst Kind
    int date = 0;           // 0 alle, 1 heute, 2 7 Tage, 3 30 Tage, 4 dieses Jahr, 5 letztes Jahr, 6 aelter, 7 individuell
    int64_t dateFrom = 0, dateTo = 0;     // Zeitraum bei "Individuell" [von, bis)
    int taken = 0;          // Aufnahmedatum, Werte wie bei date
    int64_t takenFrom = 0, takenTo = 0;
    int size = 0;           // 0 alle, 1 <1MB, 2 1-10MB, 3 10-100MB, 4 >100MB
    int res = 0;            // 0 alle, 1 <HD, 2 >=HD, 3 >=FullHD, 4 >=4K, 5 >20MP
    int orient = 0;         // 0 alle, 1 quer, 2 hoch, 3 quadratisch, 4 Panorama
    std::wstring source;    // leer = alle
    uint32_t templ = UINT32_MAX;
    std::vector<uint32_t> terms;
    bool Any() const {
        return type >= 0 || date || taken || size || res || orient || !source.empty() || templ != UINT32_MAX || !terms.empty();
    }
};

struct ViewerState {
    bool open = false;
    int pos = -1;           // Position in visible_
    float zoom = 0;         // 0 = einpassen, sonst 1.0 = 100 %
    D2D1_POINT_2F pan{0, 0};
    int rotation = 0;       // 0..3 (x 90 Grad)
    bool dragging = false;
    bool moved = false;     // seit dem Druecken bewegt (dann kein Klick)
    POINT dragStart{};
    D2D1_POINT_2F panStart{};
    bool bar = true;        // transparente Leiste oben
    bool corner = false;    // Mauszeiger in der Ecke oben rechts -> Schliessen-Knopf
    DWORD controlsUntil = 0;// Blaetter-Pfeile sichtbar bis
    bool ownFullscreen = false;
    float animS = 0;        // angezeigter Massstab (weiche Zoom-Animation), 0 = noch nicht gesetzt
    D2D1_POINT_2F animPan{0, 0};
    // laufende Animation: von (fromS, fromPan) nach (toS, toPan), Fortschritt u (0..1)
    float fromS = 0, toS = 0, animU = 1;
    D2D1_POINT_2F fromPan{0, 0}, toPan{0, 0}, anchor{0, 0};
    bool anchored = false;  // Zoom um einen festen Punkt (sonst reines Verschieben)
    double animTick = 0;   // ms (QueryPerformanceCounter)
    // Zuschneiden
    bool crop = false;
    bool cropInset = false;          // ueber das Symbol gestartet: Bild etwas kleiner eingepasst
    D2D1_RECT_F cropN{0, 0, 1, 1};   // Ausschnitt relativ zum angezeigten Bild (0..1)
    int cropDrag = 0;                // 0 nichts, 1 neu aufziehen, 2 verschieben, 3..10 Griff (Ecken/Kanten)
    D2D1_POINT_2F cropFrom{};        // Startpunkt (relativ) beim Aufziehen/Verschieben
    D2D1_RECT_F cropStartN{};
    D2D1_RECT_F imgBox{};            // Bildschirmrechteck des angezeigten Bildes
    std::wstring toast;     // kurzer Hinweis oben (z. B. "In den Papierkorb verschoben")
    DWORD toastUntil = 0;
};

class App {
public:
    bool Init(HINSTANCE inst, int show);
    int Run();

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // ---- app.cpp: Navigation, Daten, Filter
    void Navigate(const std::wstring& path, bool history = true, const std::wstring& select = {});
    void ShowAllCollections(bool history = true);
    void ShowBookmarks(bool history = true);
    std::wstring CurrentLocation() const;
    void GoTo(const std::wstring& location, const std::wstring& select = {});
    bool IsBookmarked(const std::wstring& path) const;
    std::unordered_set<std::wstring> bookmarkSet_;   // Merkliste, Pfade klein (fuer das Stern-Symbol)
    void SetBookmarked(const std::vector<std::wstring>& paths, bool add);
    void AddSearchHistory(const std::wstring& text);
    void PaintSearchHistory(ID2D1DeviceContext* dc);
    void StartView();
    void Reload(bool keepState);
    void GoBack();
    void GoForward();
    void GoUp();
    void OnLoadResults();
    void OnThumbResults();
    void OnMetaResults();
    void PrepareEntry(FileEntry& e);
    void RequestMeta(size_t from);
    void ScheduleRefilter(DWORD delayMs);
    void Refilter();
    bool PassFilters(const FileEntry& e, int skip) const;
    bool MatchOption(ChipId id, int v, const FileEntry& e) const;
    void ComputeDateBounds();
    bool NeedsMeta() const;
    void PickFolder();
    void SortVisible();
    void UpdateFacets();
    Bmp& BmpFor(const std::wstring& key);
    void EnsureThumb(const FileEntry& e, int prio, uint64_t frame);
    void EnsureIcon(const FileEntry& e, int prio);
    std::wstring IconKey(const FileEntry& e) const;
    void TrimCaches();
    void ClearBitmaps();
    // Symbol der Standard-App je Dateiendung (fuer Dokument-Vorschaubilder), leer = keins
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> appIcons_;
    ID2D1Bitmap1* AppIconFor(const std::wstring& ext);
    void OpenPos(int pos);
    void OpenViewer(int pos);
    void CloseViewer();
    void ViewerStep(int delta);
    void ViewerPrefetch();
    void AddCollection(const std::wstring& path);
    void RemoveCollection(const std::wstring& path);
    void BuildSidebar();
    void UpdateTitle();
    std::vector<std::wstring> SelectedPaths() const;
    void DeleteSelection();
    void ToggleFullscreen(bool workArea = false);   // workArea: rahmenlos ueber dem Arbeitsbereich (Taskleiste bleibt sichtbar)
    void ApplyTheme();
    void ApplyLanguage();   // Sprache aus Einstellung bzw. --lang= setzen und Oberflaeche neu aufbauen
    void EndViewerStart();
    void CreateNewFolder();   // "Neuer Ordner" im aktuellen Ordner, danach gleich umbenennen
    DWORD OnDragOver(const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD allowed, bool leave);
    DWORD OnDrop(const std::vector<std::wstring>& paths, POINT screen, DWORD keys, DWORD allowed);
    DWORD CrumbDropEffect(const std::vector<std::wstring>& paths, const std::wstring& target, DWORD keys,
                          DWORD allowed) const;
    void DoPendingFileOp();
    std::wstring TypeName(const FileEntry& e);
    std::wstring CellText(const FileEntry& e, int col);
    int ColumnMinWidth(int col);
    float ListNameW() const;              // Detailansicht: Namensspalte fuellt die Breite, die uebrigen Spalten haengen rechts
    float ListColsRight() const;          // rechter Rand der Spalten
    bool OverListLabel(int pos, float x);   // Symbol und Name einer Zeile (Ziehen = Dateien ziehen)
    int colDragMin_ = 60;
    // Ordner hat sich geaendert: im Hintergrund neu einlesen, alte Ansicht bleibt bis zum Austausch stehen
    bool quietLoad_ = false;
    std::vector<FileEntry> stage_;
    std::unordered_map<std::wstring, uint32_t> stageIdx_;
    void QuietReload();
    void FinishQuietReload();
    // Start mit Sortierung nach Datum/Aufloesung: Anzeige erst, wenn die Metadaten da sind
    bool metaHold_ = false;
    int metaPending_ = 0;
    void ReleaseMetaHold();
    void ViewerDeleteCurrent();
    // Kontaktabzug (contactsheet.cpp, Zeichnen in app_paint.cpp)
    struct SheetItem { FileEntry e; bool on = true; };
    struct SheetState {
        bool open = false;
        std::vector<SheetItem> items;         // Reihenfolge = Reihenfolge im PDF
        std::wstring headline;
        size_t caret = 0;
        bool editing = false, selAll = false;
        int page = 0, focus = -1, hover = -1;
        // Ziehen
        int press = -1;
        bool pressStrip = false, dragging = false, dropOnPage = false;
        D2D1_POINT_2F downPt{}, mouse{};
        int dropCell = -1, dropStrip = -1;
        int flipDir = 0;
        DWORD flipSince = 0;
        float stripScroll = 0;
        // Geometrie der letzten Zeichnung (fuer Treffer)
        D2D1_RECT_F pageRc{}, stripRc{}, prevRc{}, nextRc{}, headRc{};
        float scale = 1, headX = 0;
        std::vector<D2D1_RECT_F> cellRc;     // alle Zellen der Seite (auch leere)
        std::vector<D2D1_RECT_F> stripItemRc;
        // PDF-Erstellung
        bool busy = false;
        std::atomic<int> progress{0};          // 0..1000
        std::atomic<bool> cancel{false};
        bool ok = false;
        std::wstring error, savedPath;
        DWORD savedUntil = 0;
    } sheet_;
    std::thread sheetThread_;
    std::unique_ptr<SheetFont> sheetFontR_, sheetFontB_;
    void OpenContactSheet(const std::vector<std::wstring>& paths);
    void CloseContactSheet();
    void PaintSheet(ID2D1DeviceContext* dc);
    void SheetMouseDown(float x, float y);
    void SheetMouseMove(float x, float y);
    void SheetMouseUp();
    void SheetDoubleClick(float x, float y);
    bool SheetKey(WPARAM vk);
    void SheetChar(wchar_t c);
    void SheetWheel(float x, float y, int delta);
    void SheetCreatePdf();
    void SheetDone();
    std::vector<int> SheetOnItems() const;   // angehakte Bilder (Indizes in items)
    int SheetPerPage() const;
    std::wstring SheetCaption(const FileEntry& e) const;
    void SheetMove(int from, int before);
    void SheetEditHeadline(const std::wstring& text, size_t caret);
    // Kacheltextur aus einer PNG-Sequenz eines Ordners (atlas.cpp, Zeichnen in app_paint.cpp)
    struct AtlasState {
        bool open = false;
        std::wstring folder;
        std::vector<FileEntry> items;          // Reihenfolge nach Dateinamen
        AtlasInfo info;
        std::wstring problem;                  // Grund, warum keine Textur moeglich ist
        int cols = 1;
        int hover = -1;
        D2D1_RECT_F texRc{};
        // Erstellung
        bool busy = false;
        std::atomic<int> progress{0};          // 0..1000
        std::atomic<bool> cancel{false};
        bool ok = false;
        std::wstring error, savedPath;
    } atlas_;
    std::thread atlasThread_;
    void OpenAtlas(const std::wstring& folder);
    void CloseAtlas();
    void PaintAtlas(ID2D1DeviceContext* dc);
    void AtlasMouseDown(float x, float y);
    void AtlasMouseMove(float x, float y);
    bool AtlasKey(WPARAM vk);
    void AtlasCreate();
    void AtlasDone();
    int AtlasRowCount() const;
    // Bilderwolke: alle Elemente der Ansicht als raeumlicher Haufen (cloud.cpp)
    struct CloudCard {
        std::wstring path;
        float x = 0, y = 0, z = 0, w = 1, h = 1, a = 0, fade = 1;   // aktuell
        float tx = 0, ty = 0, tz = 0, tw = 1, th = 1, ta = 1;       // Ziel
        float ex = 0, ey = 0;                                       // Ausweichversatz (geglaettet)
        float evAng = -1;                                           // feste Ausweichrichtung nahe am Kamerastrahl
        int rank = 0, slot = -1;
        int face = 0;            // Inhalt im Atlas: 0 leer, 1 Symbol/Platzhalter, 2 Vorschaubild
        bool xs = false;         // kleine Stufe gefuellt
        bool leaving = false;
    };
    struct CloudDrawn { D2D1_RECT_F rc; int card; };
    struct CloudState {
        std::vector<CloudCard> cards;
        std::unordered_map<std::wstring, int> index;   // Pfad -> Karte
        std::vector<int> order;                        // Karten nach Rang (0 = oben auf dem Haufen)
        uint64_t version = ~0ull;
        std::wstring folder;
        float va = 0;                                  // Seitenverhaeltnis der Flaeche beim letzten Anordnen
        float R = 0, D = 1, fit = 5;
        // Kamera (aktuell und Ziel)
        float cx = 0, cy = 0, cz = -5, tcx = 0, tcy = 0, tcz = -5;
        float tau = 110;
        bool camInit = false, camUser = false;
        float focusZ = 0;                              // Tiefe ohne Abdunklung (oben bzw. ausgewaehltes Bild)
        int block = -1;                                // Karte in der Bildmitte: weicht nie aus, die Kamera stoesst dagegen
        float selAnim = 0;                             // Info-Feld unter dem ausgewaehlten Bild (0..1)
        std::unordered_set<std::wstring> full;         // Bilder, die in voller Aufloesung geladen wurden
        std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> names;   // Dateinamen auf Karten (Text-Layout)
        int sel = -1, hover = -1;
        float before[3] = {0, 0, -5};
        bool haveBefore = false;
        // Maus
        bool down = false, dragging = false;
        D2D1_POINT_2F downPt{}, lastPt{};
        float dragZ = 0;
        // Atlas (Direct2D, Sprite-Batches)
        std::vector<ComPtr<ID2D1Bitmap1>> pagesS, pagesXS;
        int nextSlot = 0;
        std::vector<int> freeSlots;
        bool needRepack = false;
        DWORD lastRepack = 0;
        ComPtr<ID2D1SpriteBatch> batch;
        std::vector<CloudDrawn> drawn;
        D2D1_RECT_F view{};
        float f = 1;                                   // Brennweite in DIP
    } cloud_;
    std::wstring pendingCloudSel_;
    // --cloud-tour[=<Markerdatei>]: feste Kamerafahrt durch die Wolke (nur fuer die Aufnahme im README)
    bool cloudTour_ = false;
    std::wstring cloudTourMark_;
    float cloudTourWait_ = 0, cloudTourT_ = -1;
    float cloudTourPose_[3] = {0, 0, 0};
    double cloudTourStart_ = -1;   // Startzeit (Uhr), damit die Fahrt auch bei wenigen Bildern pro Sekunde im Takt bleibt
    bool CloudTourStep(float dt);
    int CloudTourPick(float zFrom, float zTo) const;
    // SVG im Betrachter: scharfer Ausschnitt in der aktuellen Zoomstufe (gerendert im Hintergrund)
    struct SvgDetail {
        std::wstring path;
        float k = 0;                    // Pixel je SVG-Einheit
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;   // Ausschnitt in SVG-Einheiten
        ComPtr<ID2D1Bitmap1> bmp;
    } svgDetail_;
    std::wstring svgPendingKey_;
    SvgDetail svgPending_;
    uint64_t svgSeq_ = 0;
    void PaintCloud(ID2D1DeviceContext* dc);
    void CloudSync();
    bool CloudStep(float dt);
    void CloudTargets();
    void CloudFit(bool immediate);
    void CloudFillAtlas(ID2D1DeviceContext* dc);
    void CloudRepack(ID2D1DeviceContext* dc);
    bool CloudEnsurePage(ID2D1DeviceContext* dc, int slot);
    void CloudDrawFace(ID2D1DeviceContext* dc, const CloudCard& c, const D2D1_RECT_F& rc, bool large);
    void CloudReleaseGpu();
    int CloudPick(float x, float y) const;
    int CloudPosOf(int card) const;
    void CloudSelect(int card);
    void CloudDeselect();
    void CloudMouseDown(float x, float y);
    void CloudMouseMove(float x, float y);
    void CloudMouseUp(float x, float y);
    void CloudWheel(float x, float y, int delta);
    bool CloudKey(WPARAM vk);
    void CloudContextMenu(float x, float y);
    void SetViewMode(int mode);
    // Bilder vergleichen (compare.cpp): nebeneinander oder uebereinander mit Trennlinie
    struct CompareState {
        bool open = false;
        std::wstring path[2], name[2], info[2];
        int mode = 0;                       // 0 nebeneinander, 1 uebereinander
        float z = 1, tz = 1;                // Zoom (1 = eingepasst), aktuell und Ziel
        D2D1_POINT_2F pan{}, tpan{};
        float splitX = -1;
        bool dragging = false;
        D2D1_POINT_2F last{};
        double tick = 0;
        bool ownFullscreen = false;
    } compare_;
    void OpenCompare(const std::vector<std::wstring>& paths);
    void CloseCompare();
    D2D1_RECT_F ComparePane(int i) const;
    void PaintCompare(ID2D1DeviceContext* dc);
    void CompareMouseDown(float x, float y);
    void CompareMouseMove(float x, float y);
    void CompareMouseUp();
    void CompareZoom(float x, float y, float factor);
    void CompareReset();
    bool CompareKey(WPARAM vk);
    void ShowCompareMenu(float x, float y, POINT screen);
    // Umbenennen an Ort und Stelle und Eigenschaften-Dialog (rename.cpp)
    HWND renameHost_ = nullptr, renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    std::wstring renamePath_;
    void BeginRename(int pos);
    void FinishRename(bool commit);
    void ShowProperties(const std::vector<std::wstring>& paths);
public:
    bool IsDarkTheme() const;
private:
    // Zuschneiden im Bildbetrachter (crop.cpp)
    void EnterCrop(bool fit = true);
    void CancelCrop();
    void SaveCrop();
    bool CropMouseDown(float x, float y, bool newRect);
    void CropMouseMove(float x, float y);
    void CropMouseUp();
    int CropHandleAt(float x, float y) const;
    LPCWSTR CropCursor(float x, float y) const;
    void PaintCrop(ID2D1DeviceContext* dc);
    D2D1_RECT_F CropScreenRect() const;
    // Datums-Picker ("Individuell"): Popup im Stil der Dropdowns, Jahre > Monate > Tage (datepicker.cpp)
    friend class DatePickerPanel;
    int64_t tzOffset_ = 0;
    void OpenDatePicker(ChipId chip, const D2D1_RECT_F& anchor);
    void ApplyDateSpan(ChipId chip, int64_t from, int64_t to);
    int64_t EntryTimeFor(ChipId chip, const FileEntry& e) const;
    std::wstring SpanLabel(int64_t from, int64_t to) const;
    void OpenSettings();
    void CloseSettings();
    void DeleteIndex();
    void RefreshSettingsInfo();
    bool AnimateStep(float dtMs);
    double animLast_ = 0;   // Zeitpunkt des letzten Animationsschritts (ms, hochaufloesend)
    void SetStatus(const std::wstring& msg);

    // ---- app_paint.cpp: Darstellung
    void Paint();
    void Layout();
    void PaintTopBar(ID2D1DeviceContext* dc);
    void PaintChips(ID2D1DeviceContext* dc);
    void PaintSidebar(ID2D1DeviceContext* dc);
    void PaintGrid(ID2D1DeviceContext* dc);
    void PaintTile(ID2D1DeviceContext* dc, int pos, const D2D1_RECT_F& cell);
    void PaintStatus(ID2D1DeviceContext* dc);
    void PaintViewer(ID2D1DeviceContext* dc);
    void PaintInfoPanel(ID2D1DeviceContext* dc, const D2D1_RECT_F& rc, const std::vector<uint32_t>& sel, bool dark);
    void PaintListHeader(ID2D1DeviceContext* dc);
    void PaintListRow(ID2D1DeviceContext* dc, int pos, const D2D1_RECT_F& row);
    std::vector<uint32_t> InfoSelection() const;
    float ViewerAreaW() const;
    void PaintSettings(ID2D1DeviceContext* dc);
    float SidebarWidth() const;
    float InfoWidth() const;
    bool DrawBitmapFit(ID2D1DeviceContext* dc, Bmp* b, const D2D1_RECT_F& box, float maxScale, float opacity = 1.0f,
                       D2D1_RECT_F* drawn = nullptr);
    void FillRound(ID2D1DeviceContext* dc, const D2D1_RECT_F& rc, float r, const D2D1_COLOR_F& c);
    void DrawText(ID2D1DeviceContext* dc, const std::wstring& s, const D2D1_RECT_F& rc, IDWriteTextFormat* f,
                  const D2D1_COLOR_F& c);
    std::wstring ChipLabel(ChipId id) const;
    float ViewerScale(const FileEntry& e, float& imgW, float& imgH) const;

    // ---- app_input.cpp: Eingabe
    void OnMouseMove(float x, float y, WPARAM keys);
    void OnLButtonDown(float x, float y, WPARAM keys);
    void OnLButtonUp(float x, float y);
    void OnDoubleClick(float x, float y);
    void OnRButtonUp(float x, float y);
    void OnWheel(float x, float y, int delta, WPARAM keys);
    bool OnKeyDown(WPARAM vk);
    void OnChar(wchar_t c);
    const Hit* HitAt(float x, float y) const;
    int GridPosAt(float x, float y) const;
    void ShowChipMenu(ChipId id, const D2D1_RECT_F& rc);
    void ShowContextMenu(int pos, POINT screen);
    void ShowSidebarMenu(int item, POINT screen);
    void ShowCrumbMenu(int index, const D2D1_RECT_F& rc);
    bool MenuJustClosed(int code);
    void SetTileFromSlider(float x);
    void RebuildRows();
    int RowOfPos(int pos) const;
    int FirstPosOfRow(int row) const;
    int RowAtY(float contentY) const;
    D2D1_RECT_F ItemRect(int pos) const;    // Inhaltskoordinaten (ohne Bildlauf)
    void UpdateMarquee();
    void SortByColumn(int col);
    void ShowSortMenu(const D2D1_RECT_F& rc);
    void StopAutoScroll();
    void ScrollToPos(int pos);
    void SetFocusPos(int pos, bool extend, bool toggle);
    void SetTileSize(int size);
    void StartAnimation();
    void EditSearch(const std::wstring& text, size_t caret);

    // ---- Zustand
    HWND hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    Renderer r_;
    Database db_;
    ThumbService thumbs_;
    MetaService meta_;
    Loader loader_;
    Indexer indexer_;
    Settings settings_;
    ComPtr<DropTarget> drop_;
    bool ownDrag_ = false;

    // Ansicht
    std::wstring path_;
    bool collectionsView_ = false;
    bool bookmarksView_ = false;
    D2D1_RECT_F bookmarkBarRc_{};
    bool dropBookmarks_ = false;
    bool showHistory_ = false;
    bool searchSelAll_ = false;
    D2D1_RECT_F searchRc_{};
    uint32_t gen_ = 1;
    bool loading_ = false;
    std::vector<std::wstring> back_, fwd_;
    // Gemerkter Zustand je Ort (Bildlauf, fokussiertes Element, Auswahl) fuer die exakte Rueckkehr
    struct ViewState { float scroll = 0; std::wstring focus; std::vector<std::wstring> selection; };
    std::map<std::wstring, ViewState> viewStates_;
    bool restoreNext_ = false;
    bool reloadAfterViewer_ = false;
    bool savingCrop_ = false;          // Zuschnitt wird gespeichert: kein Neueinlesen dazwischen
    std::wstring viewerSwitchTo_;      // nach dem Neueinlesen im Betrachter zu dieser Datei wechseln
    void SaveViewState();
    bool TakeRestore();
    void ApplyViewState(const std::wstring& loc);
    std::vector<FileEntry> entries_;
    std::unordered_map<std::wstring, uint32_t> byPath_;
    std::vector<uint32_t> visible_;
    std::unordered_set<uint32_t> selected_;
    int focus_ = -1, anchor_ = -1;
    std::wstring pendingSelect_;
    std::wstring pendingViewer_;
    bool viewerStart_ = false;   // mit einer Bilddatei gestartet (z. B. "Oeffnen mit"): gleich im Betrachter, Esc fuehrt zum Ordner
    // nur fuer Bildschirmfotos der Dokumentation: Begriff aktivieren, Suche vorbelegen/oeffnen, Filtermenue oeffnen
    std::wstring pendingTerm_, pendingSearch_;
    bool pendingSearchOpen_ = false;
    int pendingChip_ = -1;
    float pendingZoom_ = 0;
    bool pendingRename_ = false;
    bool renameAfterLoad_ = false;
    // Aufnahmedaten (Objektiv, Blende, Belichtung, ISO) fuer den Info-Bereich, bei Bedarf gelesen; leer = keine
    std::unordered_map<std::wstring, std::wstring> photoDetails_;
    std::unordered_set<std::wstring> photoDetailsAsked_;
    void PhotoDetailRows(const FileEntry& e, const std::function<void(const std::wstring&, const std::wstring&)>& row);   // neuer Ordner: nach dem Neuladen markieren und umbenennen
    std::wstring pendingCompare_;   // Dokumentation: "pfadA;pfadB"
    int pendingCompareMode_ = 0;
    std::wstring pendingDateRange_;   // Dokumentation: "JJJJ-MM-TT..JJJJ-MM-TT"
    bool pendingDatePicker_ = false;
    float pendingCloudZoom_ = 0;
    int langOverride_ = -1;      // --lang=de|en: 1 Deutsch, 2 Englisch (nur fuer diesen Start)
    bool pendingCrop_ = false;   // --crop: Betrachter gleich im Zuschnitt-Modus (Dokumentation)
    std::vector<std::wstring> keepSelection_;
    float keepScroll_ = -1;

    // Filter & Suche
    Filters filters_;
    std::wstring search_;
    size_t caret_ = 0;
    bool searchFocus_ = false;
    Query query_;
    TokenDict dict_, templDict_;
    std::vector<Facet> termFacets_;
    std::vector<std::pair<std::wstring, uint32_t>> sourceFacets_;
    std::vector<Facet> templFacets_;
    bool refilterPending_ = false;
    int64_t now_ = 0, todayStart_ = 0, yearStart_ = 0, lastYearStart_ = 0;

    // Bilder
    std::unordered_map<std::wstring, Bmp> bmps_;
    size_t bmpBytes_ = 0;
    uint64_t frame_ = 0;
    std::unordered_map<std::wstring, Bmp> fullBmps_;

    ViewerState viewer_;

    // Layout (DIP)
    float W_ = 0, H_ = 0;
    float sidebarW_ = 232, topH_ = 52, chipsH_ = 44, statusH_ = 30;
    D2D1_RECT_F gridRc_{};
    int cols_ = 1;
    float cellW_ = 160, cellH_ = 200, tileS_ = 160;
    float scroll_ = 0, scrollTarget_ = 0, contentH_ = 0;
    std::vector<Hit> hits_;
    std::vector<SidebarItem> sidebar_;
    std::vector<std::wstring> crumbPaths_;
    int hoverPos_ = -1;
    int hoverHit_ = -1;
    bool hoverScrollbar_ = false;

    // Maus
    bool lDown_ = false;
    POINT downPt_{};
    int downPos_ = -1;
    bool scrollDrag_ = false;
    float scrollDragOffset_ = 0;
    bool selectOnUp_ = false;

    // Zeilengeometrie (Zeilen variabler Hoehe wegen umbrochener Namen)
    std::vector<float> rowTop_;      // Groesse = Zeilen + 1, relativ zum Inhaltsanfang
    std::vector<int> rowFirst_;      // erste Position je Zeile (Groesse = Zeilen + 1); Gruppenkopf: keine Elemente
    std::vector<int> rowHeader_;     // Index in groupLabels_ fuer Gruppenkopf-Zeilen, sonst -1
    std::vector<std::wstring> groupLabels_;
    std::vector<uint32_t> groupCounts_;
    int rowsGroupKey_ = -1;
    std::wstring GroupLabel(const FileEntry& e) const;
    int RowEnd(int row) const { return rowFirst_[row + 1]; }
    int VerticalNeighbor(int pos, int dir) const;
    uint64_t visibleVersion_ = 0, rowsVersion_ = ~0ull;
    float rowsCellW_ = -1, rowsTile_ = -1;
    int rowsMode_ = -1, rowsCols_ = -1, rowsNameW_ = -1;
    static constexpr float kGroupH = 44.0f;   // Hoehe einer Gruppenkopf-Zeile
    D2D1_RECT_F headerRc_{};
    static constexpr float kNameSize = 12.5f;
    static constexpr float kLineH = 17.0f;

    // Auswahlrechteck
    bool marquee_ = false;
    D2D1_POINT_2F marqueeStart_{}, marqueeCur_{};   // Inhaltskoordinaten
    POINT marqueeMouse_{};
    std::unordered_set<uint32_t> marqueeBase_;
    bool rowMarquee_ = false;                       // Detailansicht: Ziehen neben dem Namen beginnt ein Auswahlrechteck
    D2D1_POINT_2F rowMarqueeStart_{};
    std::unordered_set<uint32_t> rowMarqueeBase_;

    // Sammlungen in der Seitenleiste verschieben
    int sideDragItem_ = -1;
    bool sideDragActive_ = false;
    float sideDragY_ = 0;
    int sideInsert_ = -1;            // Einfuegeposition in settings_.collections
    float sideInsertY_ = 0;

    // Spaltenbreite in der Detailansicht ziehen
    int colDrag_ = -1;
    float colDragX_ = 0;
    int colDragW_ = 0;

    // Animationen (0 = zu, 1 = offen)
    float sidebarAnim_ = 1.0f, infoAnim_ = 0.0f;

    // Menues der Pillen und Pfeile: erneutes Klicken schliesst
    int openMenu_ = -1;          // Code des gerade offenen Menues (fuer Hervorhebung)
    int lastMenu_ = -1;
    DWORD lastMenuClose_ = 0;

    // Statusleiste
    D2D1_RECT_F sliderRc_{};
    bool sliderDrag_ = false;
    std::unordered_map<std::wstring, std::wstring> typeNames_;

    // Mittlere Maustaste: automatisches Scrollen
    bool autoScroll_ = false, autoSticky_ = false;
    D2D1_POINT_2F autoAnchor_{};
    float autoMouseY_ = 0;
    DWORD autoStart_ = 0;

    // Drag & Drop in die App
    int dropCrumb_ = -1;
    bool dropSidebar_ = false;
    D2D1_RECT_F histRc_{};   // Suchliste unter dem Suchfeld (zuletzt gezeichnet)
    int dropPos_ = -1;        // Ordner in der Ansicht als Ablageziel
    int dropSideItem_ = -1;   // Ordner/Laufwerk/Sammlung in der Seitenleiste als Ablageziel
    bool FolderDropTarget(float x, float y, std::wstring& target, int& pos, int& side) const;
    void PasteFiles(const std::wstring& target);
    void ShowViewerMenu(POINT screen);
    void ShowThemeMenu(const D2D1_RECT_F& rc);
    void ShowViewerSmallMenu(const D2D1_RECT_F& rc);
    void ShowLanguageMenu(const D2D1_RECT_F& rc);
    void ShowViewerFullscreenMenu(const D2D1_RECT_F& rc);
    int settingsPage_ = 0;          // 0 Einstellungen, 1 Tastenkombinationen
    float settingsScroll_ = 0, settingsContentH_ = 0;
    void PaintShortcuts(ID2D1DeviceContext* dc);
    MenuItem OpenWithMenuItem(const FileEntry& e, std::shared_ptr<std::vector<OpenWithApp>> apps);
    void RunOpenWith(const FileEntry& e, const std::vector<OpenWithApp>& apps, UINT cmd, bool onlyThis = false);
    struct FileOp { std::vector<std::wstring> paths; std::wstring target; bool move = false; };
    std::vector<FileOp> fileOps_;

    // Einstellungen
    bool settingsOpen_ = false;
    Database::Stats dbStats_;
    uint64_t dbBytes_ = 0, thumbBytes_ = 0;
    std::vector<std::pair<std::wstring, uint64_t>> collectionCounts_;

    // Sonstiges
    bool dark_ = true;
    bool backdrop_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT prevPlacement_{sizeof(WINDOWPLACEMENT)};
    RECT prevRect_{};   // tatsaechliche Fensterlage (auch bei angedocktem Fenster)
    bool animating_ = false;
    D2D1_COLOR_F accent_{0.0f, 0.47f, 0.84f, 1.0f};
    std::wstring status_;
    DWORD statusUntil_ = 0;
    bool indexBusy_ = false;
    uint64_t indexDone_ = 0, indexTotal_ = 0;
};

// Zeitgeber-Kennungen
constexpr UINT_PTR kTimerAnim = 1;
constexpr UINT_PTR kTimerRefilter = 2;
constexpr UINT_PTR kTimerReload = 3;
constexpr UINT_PTR kTimerStatus = 4;
constexpr UINT_PTR kTimerViewerUi = 5;
constexpr UINT_PTR kTimerViewerClick = 6;
constexpr UINT_PTR kTimerMetaHold = 47;
constexpr UINT_PTR kTimerToast = 48;
constexpr UINT_PTR kTimerDocMenu = 49;   // Dokumentation: Filtermenue verzoegert oeffnen
constexpr UINT_PTR kTimerViewerStart = 50;   // Start mit einer Bilddatei: Notausgang, falls der Ordner nicht laedt
constexpr UINT WM_APP_FILEOP = WM_APP + 20;
constexpr UINT WM_APP_SHEET = WM_APP + 21;   // Fortschritt/Ende der PDF-Erstellung (Kontaktabzug)
constexpr UINT WM_APP_ATLAS = WM_APP + 22;   // Fortschritt/Ende der Kacheltextur
constexpr UINT WM_APP_RENAME = WM_APP + 23;  // Umbenennen beendet (wParam 1 = uebernehmen)

} // namespace peek
