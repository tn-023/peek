#pragma once
// Einstellungen verschluesselt in einstellungen.dat im Datenordner (keine Registry), siehe settings.cpp.
#include "common.h"

namespace peek {

struct Settings {
    int tileSize = 160;
    bool sidebar = true;
    bool infoPanel = false;
    bool viewerSmooth = false;   // Betrachter: beim Vergroessern glaetten statt Pixel vergroessern
    int viewMode = 0;       // 0 Vorschaubilder, 1 Details (Liste), 2 Bilderwolke
    // Seitenleiste links und Infobereich rechts je Ansicht (0 Vorschaubilder, 1 Liste, 2 Bilderwolke);
    // sidebar/infoPanel oben sind die Werte der aktuellen Ansicht
    bool sidebarView[3] = {true, true, false};
    bool infoView[3] = {false, false, false};
    // Bildbetrachter: Leiste oben und Infobereich an der Seite
    bool viewerBar = true, viewerInfo = false;
    int theme = 0;
    // Kontaktabzug
    bool sheetLandscape = false, sheetNames = true, sheetExts = true;
    int sheetCols = 4, sheetRows = 5, sheetQuality = 1;          // 0 wie Windows (Standard), 1 hell, 2 dunkel
    int colW[5] = {360, 150, 170, 100, 120};   // Name, Datum, Typ, Groesse, Aufloesung
    bool showHidden = false;
    bool recursive = false;
    int sortKey = 0;        // 0 Name, 1 Datum, 2 Groesse, 3 Aufloesung, 4 Typ
    bool sortDesc = false;
    bool group = false;          // Ansicht nach dem Sortierkriterium gruppieren
    int viewerSmall = 0;         // kleine Bilder im Betrachter: 0 = 100 %, 1 = eingepasst (vergroessert)
    std::vector<std::wstring> collections;
    std::vector<std::wstring> bookmarks;       // Merkliste
    std::vector<std::wstring> searchHistory;   // bis zu 10 Sucheingaben
    std::wstring lastFolder;
    std::wstring lastVersion;    // zuletzt gestartete Version (Symbol-Cache von Windows auffrischen)
    RECT window{};
    bool hasWindow = false;
    bool maximized = false;

    void Load();
    void Save() const;
};

} // namespace peek
