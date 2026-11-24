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
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <map>
#include <tuple>
#include <unordered_map>

namespace peek {

// Auswahllisten der Einstellungen (deutsche Schluessel, Anzeige mit T())
extern const wchar_t* const kThemeNames[3];
extern const wchar_t* const kViewerSmallNames[2];
extern const wchar_t* const kViewerFullscreenNames[2];   // 0 = Vollbild, 1 = Taskleiste sichtbar
extern const wchar_t* const kGeoClickNames[2];           // 0 = kopieren (Standard), 1 = OpenStreetMap
extern const wchar_t* const kCheckBoxNames[2];           // 0 = aus (Standard), 1 = Auswahlkaestchen anzeigen
extern const wchar_t* const kSingleClickNames[2];        // 0 = Doppelklick (Standard), 1 = ein Klick
const wchar_t* LanguageName(int setting);   // 0 wie Windows, 1 Deutsch, 2 English

enum class HitType {
    None, SidebarToggle, NavBack, NavForward, NavUp, Refresh, Crumb, CrumbChevron, Search, SearchClear,
    Chip, Term, ResetFilters, SidebarItem, SidebarAdd, SettingsButton, InfoToggle, ZoomSlider,
    ZoomIn, ZoomOut, ViewGrid, ViewList, SortButton, MoreFilters, ListHeader, ListResize,
    ViewerClose, ViewerBack, ViewerMin, ViewerMax, ViewerPrev, ViewerNext, ViewerInfo, ViewerCrop, CropSave, CropCancel,
    ClearBookmarks, SearchHistItem, SearchHistDel, SearchHistClear, SearchParam,
    ViewCloud,
    SettingsBack, SettingsOpenData, SettingsRescan, SettingsDeleteIndex, SettingsTheme, SettingsShortcuts, SettingsViewerSmall, SettingsLanguage, SettingsViewerFullscreen, SettingsSingleClick, SettingsCheckBoxes, SettingsAnimations, SelectAll, SlideshowStart, ViewerSlideshow, InfoGeo, SettingsGeoClick, TimelineGroup, TimelineScrub, ViewTimeline,
    SheetCancel, SheetCreate, SheetOrient, SheetColsMinus, SheetColsPlus, SheetRowsMinus, SheetRowsPlus, SheetNames,
    SheetExts, SheetQuality, SheetPrev, SheetNext, SheetHeadline, SheetCheck, SheetCell, SheetStripCheck, SheetStrip,
    BookmarkSheet,
    SaveSearch, DeleteSearch, InfoStripMeta,
    DupSearch, DupCancel, DupDelete, DupCheck, DupThumb, DupScrollbar, DupSlider, DupZoomIn, DupZoomOut,
    AtlasCancel, AtlasCreate, AtlasColsMinus, AtlasColsPlus, AtlasRowsMinus, AtlasRowsPlus, AtlasTile,
    CompareMode, CompareClose
};

enum class ChipId { Type = 0, Date, Size, Resolution, Orientation, Source, Pattern, Sort, Recursive, Taken, Count };

struct Hit {
    D2D1_RECT_F rc{};
    HitType type = HitType::None;
    int a = 0;
    const wchar_t* tip = nullptr;   // Hinweis beim Verweilen (deutscher Schluessel, wird uebersetzt)
};

struct SidebarItem {
    enum Kind { Header, Folder, Drive, Collection, AllCollections, Bookmarks, SavedSearch } kind = Folder;
    std::wstring label;
    std::wstring path;
    wchar_t glyph = 0;
    int idx = -1;   // gespeicherte Suche: Index in Settings::searches
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
    float barAnim = 0;      // Leiste oben: 0 = ausgeblendet, 1 = ganz da (gleitet von oben herein)
    bool shell = false;     // aus dem Explorer geoeffnet: Leiste immer sichtbar, links "Zurueck" zum Ordner, X beendet peek
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
    // Diashow: Bilder blenden langsam ineinander ueber, jedes zoomt dabei leicht heran
    bool timeOrder = false;  // aus der Zeitleiste geoeffnet: Reihenfolge wie dort (Datum absteigend)
    bool slide = false;
    bool slidePaused = false;
    bool slideFromGrid = false;  // aus der Ordneransicht gestartet: Esc fuehrt dorthin zurueck
    double slideT0 = 0;          // ms: Beginn des aktuellen Bildes (Einblenden)
    int slidePrev = -1;          // vorheriges Bild, blendet aus
    double slidePrevT0 = 0;
    double slidePausedAt = 0;
    double slideIntroEnd = 0;
    std::wstring slideCaption;   // Datum ueber dem ersten Bild eines Tages (nur bei Sortierung nach Aufnahmedatum)    // ms: bis dahin blendet die bisherige Ansicht nach Schwarz ab
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
    void DrainThumbs(double budgetMs);   // fertige Vorschaubilder hochladen, begrenzt je Bild
    std::deque<ThumbResult> thumbBacklog_;
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
    void StartSlideshow(bool fromGrid);
    void SlideshowFromGrid();
    void BeginSlideshow(bool fromGrid, bool intro);
    bool slidePosted_ = false;
    int slidePending_ = -1;             // Diashow aus der Ordneransicht: Startbild, waehrend nach Schwarz abgeblendet wird
    double slidePendingEnd_ = 0;
    void StopSlideshow(bool keepViewer);
    void ToggleSlidePause();
    int SlideNext(int pos, int dir) const;
    bool ViewerBefore(int a, int b) const;     // Bild a kommt im Betrachter vor b
    int ViewerNext(int pos, int dir) const;    // naechstes/vorheriges Bild oder -1
    int ViewerEdge(bool last) const;           // erstes/letztes Bild oder -1
    void ViewerRank(int pos, int& index, int& count) const;     // naechstes Bild in der Ansicht (am Ende wieder von vorn)
    void SlideAdvance(int dir);
    std::wstring SlideCaption(int pos, bool first) const;
    void PaintSlides(ID2D1DeviceContext* dc, float VW);
    struct SlideCache {                        // Bild in Bildschirmaufloesung (einmal hochwertig gefiltert)
        std::wstring path;
        ID2D1Bitmap1* src = nullptr;
        ComPtr<ID2D1Bitmap1> bmp;
        float w = 0, h = 0, dpi = 0;
    };
    SlideCache slideCache_[2];
    void SetCaptionless(bool on);   // Betrachter/Vergleich im Fenster: ohne Titelleiste, die obere Leiste zieht das Fenster
    LRESULT CaptionHitTest(LPARAM lp, LRESULT def);
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
    float ListX0() const;                 // Detailansicht: linker Rand der Namensspalte (mit Auswahlkaestchen hinter deren Spalte)
    D2D1_RECT_F CheckRect(int pos) const; // Auswahlkaestchen eines Elements (Koordinaten wie ItemRect)
    bool OnCheck(int pos, float x, float y) const;   // Maus ueber dem Auswahlkaestchen (nur mit der Einstellung Auswahlkaestchen)
    void ToggleCheck(int pos, bool shift);
    void PaintCheck(ID2D1DeviceContext* dc, const D2D1_RECT_F& rc, int state, bool hot);   // 0 leer, 1 Haken, 2 teilweise
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
    // Duplikatsuche (dup.cpp): eigene Ansicht mit einer Liste visuell identischer Bilder
    struct DupState {
        bool open = false, busy = false;
        std::wstring title;                       // "Duplikate in „Reise“ mit Unterordnern"
        struct Item { FileEntry e; bool checked = false; };
        std::vector<std::vector<Item>> groups;    // je Gruppe das beste Bild zuerst
        float scroll = 0, target = 0;   // Bildlauf: angezeigt und Ziel (weich nachgefuehrt)
        std::wstring toast;
        DWORD toastUntil = 0;
        wchar_t toastGlyph = 0xE74D;
        bool sbDrag = false, sliderDrag = false;
        float sbOffset = 0;
        D2D1_RECT_F sbTrack{}, sbThumb{}, slider{};
    } dup_;
    struct DupInput { std::wstring path; uint64_t size; int64_t mtime; };
    std::thread dupThread_;
    std::atomic<uint64_t> dupGen_{0};
    std::atomic<int> dupDone_{0}, dupTotal_{0};
    std::mutex dupMutex_;
    std::vector<std::vector<uint32_t>> dupRes_;   // Ergebnis des Threads (Indizes in dupIn_)
    uint64_t dupResGen_ = 0;
    std::vector<FileEntry> dupIn_;                // durchsuchte Bilder (Kopien, im UI-Thread)
    // Animationen (Einstellung "Animationen", anim.cpp): Hereingleiten in Kacheln/Liste, Hover blendet aus
    struct HoverFade { HitType type; int a; D2D1_RECT_F rc; DWORD t0; };
    std::vector<HoverFade> hoverFades_;
    std::vector<std::pair<int, DWORD>> posFades_;   // Kachel/Zeile (Position) und Beginn des Ausblendens
    bool lastHovValid_ = false;
    HitType lastHovType_{};
    int lastHovA_ = 0;
    D2D1_RECT_F lastHovRc_{};
    int lastHoverPos_ = -1;
    bool enterPending_ = false, enterActive_ = false;
    DWORD enterT0_ = 0;
    std::wstring enterSig_;
    int enterFirst_ = 0, enterCount_ = 1;            // sichtbare Positionen im aktuellen Bild
    float Hov(int idx) const;                        // 1 = unter der Maus, dazwischen: blendet aus
    float PosHov(int pos) const;
    void TrackHoverFades();
    bool AnimFadesStep();
    void RequestEnter();
    // Bildbetrachter oeffnen/schliessen: das Bild fliegt zwischen Vorschaubild und Vollbild (mit Animationen)
    struct ZoomAnim {
        bool active = false, closing = false;
        int pos = -1;
        double t0 = 0;
        POINT snapOrigin{};                 // Bildschirm-Pixel der Fensterecke beim Festhalten der Ansicht
        D2D1_RECT_F fromScr{};              // Bildschirm-Pixel: Vorschaubild (oeffnen) bzw. Bild im Betrachter (schliessen)
        ComPtr<ID2D1Bitmap1> snap;          // festgehaltene Ordneransicht (oeffnen)
        ComPtr<ID2D1Bitmap> bmp;            // fliegendes Bild (schliessen)
    } zoom_;
    std::unordered_map<int, D2D1_RECT_F> drawnThumbs_;   // Position -> gezeichnetes Vorschaubild (dieses Bild)
    int viewerOpenReq_ = -1;               // Betrachter nach dem naechsten Bild oeffnen (Ansicht festhalten)
    bool TryZoomOpen(int pos);
    void SnapForZoom(ID2D1DeviceContext* dc);
    void ZoomOpenNow(int pos);
    void ZoomCloseBegin();                 // in CloseViewer, vor dem Schliessen
    void PaintZoomOpen(ID2D1DeviceContext* dc);
    void PaintZoomOverlay(ID2D1DeviceContext* dc);   // schliessen: ueber der Ordneransicht
    D2D1_RECT_F ScreenToDip(const D2D1_RECT_F& r) const;
    D2D1_RECT_F DipToScreen(const D2D1_RECT_F& r) const;
    float EnterProgress(int pos) const;              // 0 .. 1 (geglaettet), 1 = an seinem Platz
    // Metadaten entfernen (stripmeta.cpp)
    bool CanStripMetadata(const FileEntry& e) const;
    void RemoveMetadata(uint32_t index);
    // Gespeicherte Suchen (savedsearch.cpp)
    int forceRecursive_ = -1;             // Navigate: Unterordner wie gespeichert (-1 = wie sonst)
    int pendingSaved_ = -1;               // nach dem Einlesen: Filter dieser Suche anwenden
    uint32_t pendingSavedGen_ = 0;
    int renameSearch_ = -1;               // Umbenennen in der Seitenleiste: Index der Suche
    bool SearchActive() const;            // Suchergebnis (Suchtext oder Filter) an einem speicherbaren Ort
    std::wstring FiltersText() const;
    Filters ParseFiltersText(const std::wstring& text);
    Settings::SavedSearch CurrentSearch() const;
    int MatchingSavedSearch() const;      // angezeigte Suche ist gespeichert: Index, sonst -1
    std::wstring SavedSearchName(const Settings::SavedSearch& s) const;
    void SaveCurrentSearch();
    void DeleteSavedSearch(int i);
    void OpenSavedSearch(int i);
    void ApplySavedPending();
    void BeginRenameSearch(int sidebarIndex);
    void OpenRenameField(D2D1_RECT_F nr, float fieldH, const std::wstring& text, size_t selEnd);
    void OpenDuplicates();
    void CloseDuplicates();
    void StopDupScan();
    void DupWorker(std::vector<DupInput> items, uint64_t gen, HWND hwnd);
    void OnDupMessage(WPARAM wp);
    void PaintDup(ID2D1DeviceContext* dc);
    float DupContentH() const;
    size_t DupShown() const;              // angezeigte Gruppen (hoechstens 100 Duplikate)
    float DupRowH() const;
    float DupListTop() const { return 90; }
    float DupListBottom() const { return H_ - 30; }
    void DupScrollTo(float s, bool smooth = false);
    void DupSetThumb(int edge);
    void DupMouseUp();
    void DupMouseDown(float x, float y, bool dbl);
    void DupMouseMove(float x, float y);
    void DupWheel(int delta);
    bool DupKey(WPARAM vk);
    void DupDelete();
    void DupForget(const std::vector<std::wstring>& paths);
    void DupCompare(int g, int i);
    void CompareStepGroup(int dir);
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
        // Dateiname auf der Karte: Text-Layout der aktuellen Breitenstufe (je Karte, ohne Suche ueber den Pfad)
        mutable ComPtr<IDWriteTextLayout> nameLay;
        mutable int nameBucket = -1;
        mutable float nameH = 0;
        mutable float nameHs = -1;   // angezeigte Texthoehe: laeuft nameH weich nach (kein Sprung beim Zeilenwechsel)
        mutable GlyphRuns nameRuns;   // aufgezeichnete Glyphenlaeufe des Layouts (guenstiger als das Layout je Bild)
        mutable float nameW = 0;      // Bezugsbreite des Layouts
        mutable float nameNat = -1;   // Breite des Namens in einer Zeile (Bezugsgroesse), -1 = noch nicht gemessen
        mutable uint64_t nameUse = 0; // zuletzt mit Namen gezeichnet (Bildnummer): ferne Karten geben ihn wieder ab
    };
    struct CloudDrawn { D2D1_RECT_F rc; int card; };
    // Gruppierte Wolke: hoechstens 6 senkrechte Streifen (benachbarte Gruppen zusammengefasst)
    struct CloudGroup { std::wstring label; int start = 0, count = 0; };
    struct CloudStrip { std::wstring label; int count = 0; float x0 = 0, x1 = 0; float sl = 0, sr = 0; };   // sl/sr: Bildschirm
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
        bool nameMoving = false;                       // eine Karte gleicht ihre Texthoehe noch an
        float nameK = 1;                               // Annaeherung je Bild
        ComPtr<IDWriteInlineObject> ellipsis;          // Auslassungszeichen fuer gekuerzte Dateinamen
        // Dateisymbole in Zielgroesse: einmal hochwertig verkleinert, danach guenstig gezeichnet. Viele Karten teilen
        // sich Symbol und Groesse; je Karte und Bild neu zu filtern war der groesste Posten bei Karten ohne Vorschau.
        struct IconScaled { ComPtr<ID2D1Bitmap1> src, bmp; uint64_t lastUse = 0; };
        std::map<std::tuple<ID2D1Bitmap1*, int, int>, IconScaled> icons;
        size_t iconBytes = 0;
        std::vector<CloudDrawn> drawn;
        D2D1_RECT_F view{};
        float f = 1;                                   // Brennweite in DIP
        int groupKey = -1;                             // Gruppierung beim letzten Anordnen (-1 = keine Streifen)
        std::vector<CloudStrip> strips;                // leer = Wolke ohne Gruppen (wie bisher)
        float spread = 1;                              // Abstand der Karten (Strg+Mausrad), beim Wechsel zur Wolke 1
        // Grosse Wolken: nur bewegte Karten animieren, im Ruhezustand nur sichtbare Karten anfassen
        std::vector<int> active;                       // Karten, die sich noch bewegen
        bool allActive = true;                         // alle pruefen (neue Ziele, Auswahl geaendert)
        int stepSel = -2;
        bool posMoving = true;                         // im letzten Schritt hat sich eine Karte verschoben
        bool fading = false;                           // Karten gleiten noch vor dem ausgewaehlten Bild zur Seite
        struct Grid {                                  // Raster ueber x/y, je Zelle Karten nach Tiefe sortiert
            bool valid = false;
            float x0 = 0, y0 = 0, cs = 1, maxHalf = 0, maxSize = 0;
            int nx = 0, ny = 0;
            std::vector<int> start;                    // je Zelle Beginn in items (nx*ny+1 Eintraege)
            std::vector<int> items;
            std::vector<float> itemZ;
            std::vector<int> byZ;                      // alle Karten nach Tiefe (fuer das Ausweichen)
            std::vector<float> byZz;
        } grid;
        std::vector<int> evaded;                       // Karten mit Ausweichversatz (zum Zuruecksetzen)
        std::vector<uint32_t> visStamp, fillStamp;     // sichtbar in diesem Bild / im Atlas-Durchlauf betrachtet
        uint32_t stamp = 0, fillMark = 0;
        size_t fillCursor = 0;                         // Atlas: Karten ausserhalb des Bildes schrittweise pruefen
        std::vector<std::wstring> arrived;             // gerade fertige Vorschaubilder: zuerst in den Atlas
        double nameT0 = 0;                             // Dateinamen: Zeitbudget je Bild
        int nameMade = 0;
        bool namePending = false;
        // Zeichenlisten (bleiben ueber die Bilder erhalten: keine grossen Speicheranforderungen je Bild)
        std::vector<D2D1_RECT_F> sDst;
        std::vector<D2D1_RECT_U> sSrc;
        std::vector<D2D1_COLOR_F> sCol;
        float stripH = 0;                              // Hoehe der Streifen (Welt)
    } cloud_;
    // ---- timeline.cpp: Zeitleiste (Ansicht 3)
    struct TimelineItem {
        int pos = -1;            // Position in visible_ (zusammengefasste Gruppe: erstes Element)
        float x = 0, y = 0, s = 0;   // Weltkoordinaten (x ab Beginn des Zeitstrahls, y ab Oberkante)
        int group = 0;
        int col = -1;            // Spalte (tl_.cols)
        bool stack = false;      // Kachel fuer eine zusammengefasste Gruppe
    };
    struct TimelineCol {
        float x = 0, s = 0;      // Lage und Kachelgroesse ohne Schrumpfen
        int first = 0, count = 0, group = 0;   // Kacheln first .. first+count-1 in tl_.items
    };
    struct TimelineGroup {
        int64_t key = 0;
        std::wstring big, sub, full;   // Zeitangabe gross, Zusatz klein, vollstaendig
        std::wstring caps;             // fuer die Diashow: "2026", "FEBRUAR 2017", "17. MAERZ 2024" (Versalien)
        int count = 0, firstItem = 0, itemCount = 0;
        bool collapsed = false;
        std::vector<int> mosaic;         // zusammengefasst: Vorschau aus den ersten Bildern
        float x0 = 0, x1 = 0, fs = 0, bigW = 0, subW = 0, labelW = 0;
        float fromX0 = 0;                // Lage vor dem Auf-/Zuklappen (Animation)
        int firstCol = 0, lastCol = 0;
        int level = 0;                   // 0 Tag, 1 Monat, 2 Jahr
    };
    struct TimelineState {
        std::wstring folder;
        uint64_t version = ~0ull;
        float viewH = 0;
        bool langEn = false, dirty = false;
        std::vector<TimelineItem> items;     // nach x geordnet
        std::vector<TimelineCol> cols;
        std::vector<int> groupOf;            // je Position in visible_: Gruppe (-1 = nicht in der Zeitleiste)
        std::vector<float> colX, colK, colFog;   // je Spalte: Lage nach dem Aneinanderreihen, Groesse, Abdunklung
        std::vector<int> drawList;
        std::vector<TimelineGroup> groups;
        std::unordered_set<int64_t> expanded;   // aufgeklappte grosse Gruppen
        float scroll = 0, target = 0, contentW = 0, maxS = 0;
        bool scrub = false, pan = false, panMoved = false;   // scrub: Ziehen in der Uebersicht unten
        int pressPos = -1;                   // gedrueckt auf diesem Element (Klick ohne Ziehen waehlt es aus)
        WPARAM pressKeys = 0;
        float vel = 0;                       // Weiterlaufen nach dem Anschieben (DIP je ms)
        float lastX = 0;
        double lastT = 0;
        bool anim = false;                   // Auf-/Zuklappen laeuft
        double animT0 = 0;
        std::vector<D2D1_RECT_F> from;       // je Kachel: Lage vor dem Auf-/Zuklappen (Welt)
        std::vector<float> delay;            // ms
        float panX = 0, panScroll = 0, chartL = 0, chartW = 1;   // Uebersicht unten: linker Rand, Breite
    } tl_;
    void TimelineSync();
    int TimelineFirstAt(float wx) const;
    int TimelinePick(float x, float y) const;
    void TimelineToggle(int group);
    void TimelineScrubAt(float x, bool immediate);
    bool TimelineMouseDown(float x, float y, WPARAM keys);
    float TimelineShrink(float x, float s, float& fog) const;
    void TimelineMapColumns();
    D2D1_RECT_F TimelineItemRect(const TimelineItem& it) const;
    int TimelineItemAt(float x, float y) const;
    bool TimelineMouseMove(float x, float y);
    bool TimelineMouseUp();
    void TimelineWheel(int delta);
    bool TimelineKey(WPARAM vk);
    bool TimelineStep(float dt);
    void PaintTimeline(ID2D1DeviceContext* dc);
    static constexpr float kCloudGap = 1.2f;   // Abstand zwischen zwei Streifen (Welt)
    int CloudGroupKey() const;
    std::vector<CloudGroup> CloudGroups();
    void CloudFocusStrip(int i);
    void PaintCloudHeaders(ID2D1DeviceContext* dc, bool titles);   // app_paint.cpp
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
    // rad > 0: Karte selbst mit runden Ecken fuellen (ohne Zwischenebene), sonst liefert die Ebene die Ecken
    void CloudFillIcons(ID2D1DeviceContext* dc);
    void CloudBuildGrid();
    void CloudSetSpread(float spread);
    void ApplySpread();
    void CloudPan(float fx, float fy);
    void CloudMarkVisible();
    void CloudDrawFace(ID2D1DeviceContext* dc, const CloudCard& c, const D2D1_RECT_F& rc, bool large, float rad = 0);
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
        std::vector<FileEntry> group;       // aus der Duplikatsuche: rechts durch die Gruppe blaettern (links das beste)
        int groupPos = -1;
    } compare_;
    void OpenCompare(const std::vector<std::wstring>& paths);
    void OpenCompareEntries(const FileEntry& a, const FileEntry& b);
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
    std::vector<MenuItem> ChipMenuItems(ChipId id, UINT base);        // Eintraege eines Filtermenues (ids base + Wert)
    void ApplyChipChoice(ChipId id, int v, const D2D1_RECT_F& rc);     // gewaehlten Wert uebernehmen
    void ShowMoreFiltersMenu(const D2D1_RECT_F& rc);                   // "..." mit den Filtern, die nicht mehr passen
    std::vector<ChipId> chipOverflow_;                                 // beim Zeichnen ermittelt
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
    std::wstring SortStoreKey() const;   // Schluessel fuer die Sortierung je Ordner
    void ApplyFolderSort();             // gespeicherte Sortierung des Ordners (sonst Name aufsteigend)
    void RememberSort();                // aktuelle Sortierung fuer diesen Ordner merken
    bool sortFromCli_ = false;          // Sortierung per Befehlszeile vorgegeben: beim Start nicht ueberschreiben
    HitType viewerPress_ = HitType::None;   // Betrachter: Knopf, der beim Loslassen ausgeloest wird
    void ShowSortMenu(const D2D1_RECT_F& rc);
    void StopAutoScroll();
    void ScrollToPos(int pos);
    void SetFocusPos(int pos, bool extend, bool toggle);
    void SetTileSize(int size);
    void StartAnimation();
    void EditSearch(const std::wstring& text, size_t caret);

    // ---- Zustand
    HWND hwnd_ = nullptr;
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
    bool CurrentFolderTarget(std::wstring& target) const;   // angezeigter Ordner als Ablageziel (nicht Lesezeichen/Sammlungen)
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
    std::wstring shellLastFolder_;   // zuletzt geoeffneter Ordner vor dem Start aus dem Explorer (beim Beenden behalten)
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
    bool hoverCheck_ = false;         // Maus ueber dem Auswahlkaestchen des Elements unter hoverPos_
    int hoverHit_ = -1;
    bool hoverScrollbar_ = false;

    // Maus
    bool lDown_ = false;
    POINT downPt_{};
    int downPos_ = -1;
    bool downPlain_ = false;          // Maustaste ohne Strg/Umschalt gedrueckt (Oeffnen mit einem Klick)
    DWORD noDblUntil_ = 0;            // nach Oeffnen mit einem Klick: folgenden Doppelklick ignorieren
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
    float rowsTile_ = -1;
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
    float autoMouseY_ = 0, autoMouseX_ = 0;   // Zeitleiste: waagerecht
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
    void ShowSingleClickMenu(const D2D1_RECT_F& rc);
    void ShowCheckBoxMenu(const D2D1_RECT_F& rc);
    void ShowAnimationsMenu(const D2D1_RECT_F& rc);
    void ShowGeoClickMenu(const D2D1_RECT_F& rc);
    std::wstring GeoText(const FileEntry& e);
    void GeoClick(uint32_t idx);
    int settingsPage_ = 0;          // 0 Einstellungen, 1 Tastenkombinationen
    float settingsScroll_ = 0, settingsContentH_ = 0;
    bool settingsScrollDrag_ = false;   // Bildlaufleiste der Einstellungen wird gezogen
    float settingsDragOffset_ = 0;
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
    bool inSizeMove_ = false;           // Fenster wird gerade mit der Maus in der Groesse geaendert
    bool tipShown_ = false;             // Hinweis (Tooltip) zum Treffer unter der Maus sichtbar
    void PaintTip(ID2D1DeviceContext* dc);
    bool captionless_ = false;          // Titelleiste ausgeblendet (Betrachter im Fenster)
    D2D1_RECT_F viewerBarRc_{};         // obere Leiste des Betrachters (zum Ziehen des Fensters), leer wenn verborgen
    WINDOWPLACEMENT prevPlacement_{sizeof(WINDOWPLACEMENT)};
    RECT prevRect_{};   // tatsaechliche Fensterlage (auch bei angedocktem Fenster)
    bool animating_ = false;
    bool animFresh_ = false;
    // kurze Ein-/Ausblendungen (Leiste im Betrachter, Liste unter dem Suchfeld): v laeuft linear in ms gegen 0 bzw. 1
    bool UiAnim(float& v, bool on, float ms);
    bool uiAnimPending_ = false;
    float histAnim_ = 0;        // Animation gerade gestartet: erster Schritt = eine Bildperiode
    float FrameDt(double rawMs);    // Zeitschritt am Takt des Bildschirms ausrichten
    double RefreshPeriodMs();
    // Messanzeige (Strg+Umschalt+F12): Bildabstaende und Rechenzeit der letzten Bilder
    bool perfHud_ = false;
    float perfFrame_[120] = {}, perfCpu_[120] = {};
    // Aufschluesselung: 0 = Wolke Atlas, 1 = Wolke Karten berechnen, 2 = Wolke nahe Karten zeichnen, 3 = Abschluss (EndDraw)
    float perfPart_[4][120] = {};
    float perfNow_[4] = {};
    int perfCount_[5] = {};   // Wolke: sichtbare Karten, nahe Bilder, nahe Namenskarten, Ebenen, Sprites
    int perfN_ = 0;
    double perfLast_ = 0;
    void PaintPerfHud(ID2D1DeviceContext* dc, double paintStart);
    // Betrachter: fertig verkleinertes Bild der ruhenden Ansicht (gleiche Pixel, kein erneutes Filtern je Bild)
    struct ViewCache {
        ComPtr<ID2D1Bitmap1> bmp;
        std::wstring path;
        ID2D1Bitmap1* src = nullptr;
        D2D1_RECT_F dst{};
        int rotation = 0, x0 = 0, y0 = 0;
        UINT w = 0, h = 0;
        float dpi = 0;
        D2D1_RECT_F seen{};   // Lage im vorigen Bild
        float seenW = 0;
    } viewCache_;
    // Zeilengeometrie: Gruppen und Namensbreiten haengen nicht von der Fensterbreite ab (eigener Zwischenspeicher)
    std::vector<int> grpStart_;
    std::vector<float> nameNatural_;
    uint64_t grpVersion_ = ~0ull, natVersion_ = ~0ull;
    int grpKey_ = -2;
    D2D1_COLOR_F accent_{0.0f, 0.47f, 0.84f, 1.0f};
    std::wstring status_;
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
constexpr UINT_PTR kTimerTip = 51;
constexpr UINT_PTR kTimerSlideStart = 52;     // Diashow: nach dem Abblenden den Betrachter oeffnen           // Hinweis nach kurzem Verweilen zeigen
constexpr UINT_PTR kTimerViewerStart = 50;   // Start mit einer Bilddatei: Notausgang, falls der Ordner nicht laedt
constexpr UINT WM_APP_FILEOP = WM_APP + 20;
constexpr UINT WM_APP_SHEET = WM_APP + 21;   // Fortschritt/Ende der PDF-Erstellung (Kontaktabzug)
constexpr UINT WM_APP_ATLAS = WM_APP + 22;   // Fortschritt/Ende der Kacheltextur
constexpr UINT WM_APP_RENAME = WM_APP + 23;  // Umbenennen beendet (wParam 1 = uebernehmen)
constexpr UINT WM_APP_DUP = WM_APP + 24;     // Duplikatsuche: Fortschritt (0) bzw. Ergebnis (1)
constexpr UINT WM_APP_ZOOMOPEN = WM_APP + 25; // Betrachter oeffnen, nachdem die Ansicht festgehalten wurde (wParam = Position)

} // namespace peek
