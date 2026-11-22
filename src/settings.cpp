#include "settings.h"
#include "cachecrypt.h"
#include "fs.h"

#include <map>

namespace peek {

// Einstellungen liegen verschluesselt in "einstellungen.dat" im Datenordner (gleicher Schluessel wie der Index,
// an das Windows-Konto gebunden, siehe cachecrypt.h). Inhalt: INI-Text (UTF-8). Ist die Datei nicht lesbar
// (anderer PC, anderes Konto, zurueckgesetztes Kennwort), startet peek mit den Standardwerten.
// Eine alte, unverschluesselte peek.ini wird einmalig uebernommen und danach geloescht.

namespace {

using IniMap = std::map<std::wstring, std::map<std::wstring, std::wstring>>;

constexpr wchar_t kFile[] = L"einstellungen.dat";
constexpr wchar_t kLegacy[] = L"peek.ini";

IniMap g_ini;   // beim Laden gefuellt, nur im UI-Thread benutzt

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r");
    return s.substr(a, b - a + 1);
}

IniMap Parse(const std::wstring& text) {
    IniMap m;
    std::wstring sec;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = Trim(text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos));
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == L';') continue;
        if (line.front() == L'[' && line.back() == L']') {
            sec = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        m[sec][Trim(line.substr(0, eq))] = line.substr(eq + 1);
    }
    return m;
}

std::wstring Serialize(const IniMap& m) {
    std::wstring t;
    for (auto& [sec, kv] : m) {
        t += L"[" + sec + L"]\n";
        for (auto& [k, v] : kv) {
            std::wstring clean = v;
            for (auto& c : clean)
                if (c == L'\n' || c == L'\r') c = L' ';
            t += k + L"=" + clean + L"\n";
        }
    }
    return t;
}

// Alte peek.ini (Klartext, Windows-INI-Funktionen) einlesen
IniMap ReadLegacy(const std::wstring& path) {
    IniMap m;
    std::wstring names(65536, L'\0');
    DWORD n = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), path.c_str());
    for (const wchar_t* sec = names.c_str(); sec < names.c_str() + n && *sec; sec += wcslen(sec) + 1) {
        std::wstring body(1 << 20, L'\0');
        DWORD k = GetPrivateProfileSectionW(sec, body.data(), static_cast<DWORD>(body.size()), path.c_str());
        for (const wchar_t* kv = body.c_str(); kv < body.c_str() + k && *kv; kv += wcslen(kv) + 1) {
            std::wstring line = kv;
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) m[sec][line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return m;
}

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (64ll << 20);
    if (ok) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD rd = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &rd, nullptr) && rd == out.size();
    }
    CloseHandle(h);
    return ok;
}

CacheCrypto& Crypto() {
    static CacheCrypto c;
    static bool tried = false;
    if (!tried) {
        tried = true;
        bool fresh = false;
        c.Init(DataDir(), fresh);
    }
    return c;
}

int ReadInt(const wchar_t* sec, const wchar_t* key, int def) {
    auto s = g_ini.find(sec);
    if (s == g_ini.end()) return def;
    auto k = s->second.find(key);
    if (k == s->second.end() || k->second.empty()) return def;
    wchar_t* end = nullptr;
    long v = wcstol(k->second.c_str(), &end, 10);
    return end && end != k->second.c_str() ? static_cast<int>(v) : def;
}

std::wstring ReadStr(const wchar_t* sec, const wchar_t* key) {
    auto s = g_ini.find(sec);
    if (s == g_ini.end()) return {};
    auto k = s->second.find(key);
    return k == s->second.end() ? std::wstring() : k->second;
}

}  // namespace

void Settings::Load() {
    g_ini.clear();
    std::wstring dir = DataDir();
    std::vector<uint8_t> blob, plain;
    if (ReadFileAll(JoinPath(dir, kFile), blob) && Crypto().Open(blob.data(), blob.size(), plain)) {
        g_ini = Parse(Utf8ToWide(std::string_view(reinterpret_cast<const char*>(plain.data()), plain.size())));
        SecureZeroMemory(plain.data(), plain.size());
    } else if (GetFileAttributesW(JoinPath(dir, kLegacy).c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_ini = ReadLegacy(JoinPath(dir, kLegacy));   // einmalige Uebernahme, Save() verschluesselt und loescht
    }
    tileSize = std::clamp(ReadInt(L"View", L"TileSize", 160), 64, 384);
    sidebar = ReadInt(L"View", L"Sidebar", 1) != 0;
    infoPanel = ReadInt(L"View", L"InfoPanel", 0) != 0;
    viewMode = std::clamp(ReadInt(L"View", L"ViewMode", 0), 0, 2);
    viewerSmooth = ReadInt(L"View", L"ViewerSmooth", 0) != 0;
    theme = std::clamp(ReadInt(L"View", L"Theme", 0), 0, 2);
    language = std::clamp(ReadInt(L"View", L"Language", 0), 0, 2);
    sheetLandscape = ReadInt(L"Sheet", L"Landscape", 0) != 0;
    sheetNames = ReadInt(L"Sheet", L"Names", 1) != 0;
    sheetExts = ReadInt(L"Sheet", L"Extensions", 1) != 0;
    sheetCols = std::clamp(ReadInt(L"Sheet", L"Cols", 4), 1, 8);
    sheetRows = std::clamp(ReadInt(L"Sheet", L"Rows", 5), 1, 8);
    sheetQuality = std::clamp(ReadInt(L"Sheet", L"Quality", 1), 0, 2);
    group = ReadInt(L"View", L"Group", 0) != 0;
    viewerSmall = std::clamp(ReadInt(L"View", L"ViewerSmall", 0), 0, 1);
    viewerFullscreen = ReadInt(L"View", L"ViewerFullscreen", 1) != 0;
    singleClick = ReadInt(L"View", L"SingleClick", 0) != 0;
    for (int i = 0; i < 5; ++i) {
        std::wstring key = L"Col" + std::to_wstring(i);
        colW[i] = std::clamp(ReadInt(L"View", key.c_str(), colW[i]), 60, 1200);
    }
    showHidden = ReadInt(L"View", L"ShowHidden", 0) != 0;
    recursive = ReadInt(L"View", L"Recursive", 0) != 0;
    sortKey = std::clamp(ReadInt(L"View", L"SortKey", 0), 0, 5);
    sortDesc = ReadInt(L"View", L"SortDesc", 0) != 0;
    // je Ansicht; aeltere Versionen kannten nur einen Wert (gilt dann fuer Vorschaubilder und Liste)
    for (int v = 0; v < 3; ++v) {
        std::wstring k = std::to_wstring(v);
        sidebarView[v] = ReadInt(L"View", (L"Sidebar" + k).c_str(), v < 2 ? (sidebar ? 1 : 0) : 0) != 0;
        infoView[v] = ReadInt(L"View", (L"Info" + k).c_str(), v < 2 ? (infoPanel ? 1 : 0) : 0) != 0;
    }
    sidebar = sidebarView[viewMode];
    infoPanel = infoView[viewMode];
    viewerBar = ReadInt(L"Viewer", L"Bar", 1) != 0;
    viewerInfo = ReadInt(L"Viewer", L"Info", 0) != 0;
    lastFolder = ReadStr(L"View", L"LastFolder");
    lastVersion = ReadStr(L"View", L"LastVersion");

    collections.clear();
    int n = ReadInt(L"Collections", L"Count", 0);
    for (int i = 0; i < n && i < 256; ++i) {
        std::wstring key = L"Path" + std::to_wstring(i);
        std::wstring p = ReadStr(L"Collections", key.c_str());
        if (!p.empty() && IsLocalDrivePath(p)) collections.push_back(p);
    }

    bookmarks.clear();
    int nb = ReadInt(L"Bookmarks", L"Count", 0);
    for (int i = 0; i < nb && i < 100000; ++i) {
        std::wstring key = L"Path" + std::to_wstring(i);
        std::wstring p = ReadStr(L"Bookmarks", key.c_str());
        if (!p.empty()) bookmarks.push_back(p);
    }
    folderSort.clear();
    int nf = ReadInt(L"FolderSort", L"Count", 0);
    for (int i = 0; i < nf && i < 2000; ++i) {
        std::wstring p = ReadStr(L"FolderSort", (L"P" + std::to_wstring(i)).c_str());
        int v = ReadInt(L"FolderSort", (L"V" + std::to_wstring(i)).c_str(), 0);
        if (!p.empty() && v > 0 && v < 64 && (v & 15) <= 5) folderSort.push_back({p, v});
    }
    searchHistory.clear();
    for (int i = 0; i < 10; ++i) {
        std::wstring key = L"H" + std::to_wstring(i);
        std::wstring h = ReadStr(L"Search", key.c_str());
        if (!h.empty()) searchHistory.push_back(h);
    }

    hasWindow = ReadInt(L"Window", L"Valid", 0) != 0;
    window.left = ReadInt(L"Window", L"Left", 100);
    window.top = ReadInt(L"Window", L"Top", 100);
    window.right = ReadInt(L"Window", L"Right", 1400);
    window.bottom = ReadInt(L"Window", L"Bottom", 900);
    maximized = ReadInt(L"Window", L"Maximized", 0) != 0;
}

void Settings::Save() const {
    IniMap m;
    auto WriteStr = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& v) { m[sec][key] = v; };
    auto WriteInt = [&](const wchar_t* sec, const wchar_t* key, int v) { m[sec][key] = std::to_wstring(v); };
    WriteInt(L"View", L"TileSize", tileSize);
    WriteInt(L"View", L"Sidebar", sidebar ? 1 : 0);
    WriteInt(L"View", L"InfoPanel", infoPanel ? 1 : 0);
    WriteInt(L"View", L"ViewMode", viewMode);
    WriteInt(L"View", L"ViewerSmooth", viewerSmooth ? 1 : 0);
    WriteInt(L"View", L"Theme", theme);
    WriteInt(L"View", L"Language", language);
    WriteInt(L"Sheet", L"Landscape", sheetLandscape ? 1 : 0);
    WriteInt(L"Sheet", L"Names", sheetNames ? 1 : 0);
    WriteInt(L"Sheet", L"Extensions", sheetExts ? 1 : 0);
    WriteInt(L"Sheet", L"Cols", sheetCols);
    WriteInt(L"Sheet", L"Rows", sheetRows);
    WriteInt(L"Sheet", L"Quality", sheetQuality);
    WriteInt(L"View", L"Group", group ? 1 : 0);
    WriteInt(L"View", L"ViewerSmall", viewerSmall);
    WriteInt(L"View", L"ViewerFullscreen", viewerFullscreen ? 1 : 0);
    WriteInt(L"View", L"SingleClick", singleClick ? 1 : 0);
    for (int i = 0; i < 5; ++i) {
        std::wstring key = L"Col" + std::to_wstring(i);
        WriteInt(L"View", key.c_str(), colW[i]);
    }
    WriteInt(L"View", L"ShowHidden", showHidden ? 1 : 0);
    WriteInt(L"View", L"Recursive", recursive ? 1 : 0);
    WriteInt(L"View", L"SortKey", sortKey);
    WriteInt(L"View", L"SortDesc", sortDesc ? 1 : 0);
    for (int v = 0; v < 3; ++v) {
        std::wstring k = std::to_wstring(v);
        bool sb = v == viewMode ? sidebar : sidebarView[v];
        bool in = v == viewMode ? infoPanel : infoView[v];
        WriteInt(L"View", (L"Sidebar" + k).c_str(), sb ? 1 : 0);
        WriteInt(L"View", (L"Info" + k).c_str(), in ? 1 : 0);
    }
    WriteInt(L"Viewer", L"Bar", viewerBar ? 1 : 0);
    WriteInt(L"Viewer", L"Info", viewerInfo ? 1 : 0);
    WriteStr(L"View", L"LastFolder", lastFolder);
    WriteStr(L"View", L"LastVersion", lastVersion);

    WriteInt(L"Collections", L"Count", static_cast<int>(collections.size()));
    for (size_t i = 0; i < collections.size(); ++i) {
        std::wstring key = L"Path" + std::to_wstring(i);
        WriteStr(L"Collections", key.c_str(), collections[i]);
    }

    WriteInt(L"Bookmarks", L"Count", static_cast<int>(bookmarks.size()));
    for (size_t i = 0; i < bookmarks.size(); ++i) {
        std::wstring key = L"Path" + std::to_wstring(i);
        WriteStr(L"Bookmarks", key.c_str(), bookmarks[i]);
    }
    for (size_t i = 0; i < searchHistory.size() && i < 10; ++i) {
        std::wstring key = L"H" + std::to_wstring(i);
        WriteStr(L"Search", key.c_str(), searchHistory[i]);
    }
    WriteInt(L"FolderSort", L"Count", static_cast<int>(folderSort.size()));
    for (size_t i = 0; i < folderSort.size(); ++i) {
        WriteStr(L"FolderSort", (L"P" + std::to_wstring(i)).c_str(), folderSort[i].first);
        WriteInt(L"FolderSort", (L"V" + std::to_wstring(i)).c_str(), folderSort[i].second);
    }

    WriteInt(L"Window", L"Valid", hasWindow ? 1 : 0);
    WriteInt(L"Window", L"Left", window.left);
    WriteInt(L"Window", L"Top", window.top);
    WriteInt(L"Window", L"Right", window.right);
    WriteInt(L"Window", L"Bottom", window.bottom);
    WriteInt(L"Window", L"Maximized", maximized ? 1 : 0);

    // verschluesselt schreiben (erst in eine neue Datei, dann ersetzen)
    if (!Crypto().Ready()) return;
    std::string text = WideToUtf8(Serialize(m));
    std::vector<uint8_t> sealed = Crypto().Seal(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    SecureZeroMemory(text.data(), text.size());
    if (sealed.empty()) return;
    std::wstring dir = DataDir();
    std::wstring path = JoinPath(dir, kFile), tmp = path + L".neu";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    bool ok = WriteFile(h, sealed.data(), static_cast<DWORD>(sealed.size()), &wr, nullptr) && wr == sealed.size();
    CloseHandle(h);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(JoinPath(dir, kLegacy).c_str());   // alte Klartext-Datei entfernen
        g_ini = m;
    } else {
        DeleteFileW(tmp.c_str());
    }
}

} // namespace peek

namespace peek {

int Settings::FolderSort(const std::wstring& key) const {
    for (const auto& f : folderSort)
        if (f.first == key) return f.second;
    return -1;
}

void Settings::SetFolderSort(const std::wstring& key, int value) {
    for (size_t i = 0; i < folderSort.size(); ++i)
        if (folderSort[i].first == key) {
            folderSort.erase(folderSort.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    if (value == 0) return;   // Standard: nicht speichern
    folderSort.push_back({key, value});
    if (folderSort.size() > 2000) folderSort.erase(folderSort.begin());   // aelteste Eintraege fallen weg
}

}  // namespace peek
