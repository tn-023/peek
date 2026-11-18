// Pruefmodus fuer die CI: peek.exe --index-test <leerer-ordner> <bericht.txt>
// Prueft den verschluesselten Index: Lesen/Schreiben, keine Klartexte in den Dateien, Aufraeumen beim Loeschen,
// Ersetzen alter unverschluesselter Dateien und Neuanfang bei fremdem Schluessel.
#include "db.h"
#include "settings.h"

#include <sqlite3.h>

namespace peek {

namespace {

bool FileContains(const std::wstring& path, const void* needle, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<uint8_t> d(static_cast<size_t>(sz.QuadPart));
    DWORD rd = 0;
    if (!d.empty()) ReadFile(h, d.data(), static_cast<DWORD>(d.size()), &rd, nullptr);
    CloseHandle(h);
    auto* p = static_cast<const uint8_t*>(needle);
    return std::search(d.begin(), d.end(), p, p + n) != d.end();
}

bool AnyFileContains(const std::wstring& dir, const std::wstring& text) {
    std::string u = WideToUtf8(text);
    for (const wchar_t* f : {L"index.db", L"index.db-wal", L"thumbs.db", L"thumbs.db-wal"}) {
        std::wstring p = JoinPath(dir, f);
        if (FileContains(p, text.data(), text.size() * sizeof(wchar_t)) || FileContains(p, u.data(), u.size())) return true;
    }
    return false;
}

FileEntry Entry(const std::wstring& path) {
    FileEntry e;
    e.path = path;
    e.name = FileNameOf(path);
    e.kind = Kind::Image;
    e.size = 1234;
    e.mtime = 1700000000;
    return e;
}

}  // namespace

int RunIndexTest(const std::wstring& dir, const std::wstring& report) {
    std::wstring out;
    int failures = 0;
    auto check = [&](bool ok, const wchar_t* what) {
        out += std::wstring(ok ? L"OK      " : L"FEHLER  ") + what + L"\n";
        if (!ok) ++failures;
    };
    CreateDirectoryW(dir.c_str(), nullptr);

    // 0) alte, unverschluesselte Datenbank (Schema 1) mit Klartext anlegen
    {
        sqlite3* db = nullptr;
        sqlite3_open16(JoinPath(dir, L"thumbs.db").c_str(), &db);
        sqlite3_exec(db, "CREATE TABLE thumbs(path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, data BLOB);"
                         "INSERT INTO thumbs VALUES('C:\\Alt\\Klartext-Geheimnis.jpg', 1, 1, x'FFD8FFE0');",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    const std::wstring secret = L"Geheimes Projekt Sonnenblume";
    const std::wstring folder = L"C:\\Bilder\\" + secret;
    const std::wstring a = folder + L"\\Urlaub am See.jpg", b = folder + L"\\Zweites Bild.jpg",
                       c = folder + L"\\Unterordner\\Drittes.jpg";
    std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0, 'J', 'F', 'I', 'F', 0};
    for (int i = 0; i < 200; ++i) jpeg.push_back(static_cast<uint8_t>('X'));
    {
        Database db;
        check(db.Open(dir), L"Index oeffnen (neuer Schluessel)");
        check(!AnyFileContains(dir, L"Klartext-Geheimnis"), L"alte unverschluesselte Datenbank ersetzt");
        MetaRow m;
        m.width = 4000;
        m.height = 3000;
        m.camera = L"Kamera Modell Zeta";
        db.PutFile(Entry(a), &m, 1);
        db.PutFiles({Entry(b), Entry(c)}, 1);
        db.PutThumb(a, 1234, 1700000000, jpeg);
        db.PutThumb(b, 1234, 1700000000, jpeg);
        db.PutThumb(c, 1234, 1700000000, jpeg);

        std::vector<uint8_t> got;
        check(db.GetThumb(a, 1234, 1700000000, got) && got == jpeg, L"Vorschaubild lesen");
        MetaRow r;
        check(db.GetMeta(a, 1234, 1700000000, r) && r.camera == m.camera && r.width == 4000, L"Metadaten lesen");
        auto all = db.LoadUnder(L"C:\\Bilder");
        bool found = false;
        for (auto& e : all) found |= e.path == a && e.name == L"Urlaub am See.jpg";
        check(all.size() == 3 && found, L"Pfade unterhalb eines Ordners (entschluesselt)");
        check(db.CountUnder(folder) == 3 && db.CountUnder(L"C:\\Bild") == 0, L"Bereichsabfragen");
    }
    // Dateien auf Klartext pruefen (nach dem Schliessen, inkl. WAL)
    check(!AnyFileContains(dir, secret) && !AnyFileContains(dir, L"Urlaub am See"), L"keine Pfade im Klartext");
    check(!AnyFileContains(dir, L"Kamera Modell Zeta"), L"kein Kameramodell im Klartext");
    {
        const uint8_t jfif[] = {'J', 'F', 'I', 'F', 0, 'X', 'X', 'X'};
        bool plain = false;
        for (const wchar_t* f : {L"thumbs.db", L"thumbs.db-wal"})
            plain |= FileContains(JoinPath(dir, f), jfif, sizeof(jfif));
        check(!plain, L"keine Bilddaten im Klartext");
    }
    {
        Database db;
        check(db.Open(dir), L"Index erneut oeffnen (gleicher Schluessel)");
        std::vector<uint8_t> got;
        check(db.GetThumb(a, 1234, 1700000000, got) && got == jpeg, L"Daten bleiben erhalten");
        // Datei geloescht: sofort entfernen
        db.Forget({a});
        check(!db.HasThumb(a, 1234, 1700000000) && db.CountUnder(folder) == 2, L"Loeschen entfernt Vorschau und Eintrag");
        // Ordner eingelesen, b gibt es nicht mehr; Unterordner bleibt unberuehrt
        db.PruneFolder(folder, {folder + L"\\Unterordner"});
        check(!db.HasThumb(b, 1234, 1700000000) && db.HasThumb(c, 1234, 1700000000), L"Aufraeumen beim Einlesen");
        db.Forget({folder});
        check(db.CountUnder(L"C:\\Bilder") == 0 && !db.HasThumb(c, 1234, 1700000000), L"Ordner entfernt alles darunter");
    }
    // Fremder Schluessel (anderer PC/Konto): Schluesseldatei ersetzen -> leerer Index, kein Fehler
    {
        Database db;
        db.Open(dir);
        db.PutThumb(a, 1, 1, jpeg);
    }
    DeleteFileW(JoinPath(dir, L"index.key").c_str());
    {
        Database db;
        check(db.Open(dir), L"Index mit neuem Schluessel oeffnen");
        check(!db.HasThumb(a, 1, 1), L"alte Daten mit fremdem Schluessel verworfen");
    }

    // Messung: 20.000 Dateien in 200 Ordnern
    {
        std::wstring pdir = dir + L"-mess";
        CreateDirectoryW(pdir.c_str(), nullptr);
        Database db;
        db.Open(pdir);
        std::vector<FileEntry> many;
        for (int f = 0; f < 200; ++f)
            for (int i = 0; i < 100; ++i)
                many.push_back(Entry(L"C:\\Fotos\\Jahr 2024\\Ordner " + std::to_wstring(f) + L"\\IMG_" + std::to_wstring(i) + L".jpg"));
        double t0 = NowMs();
        db.PutFiles(many, 1);
        double t1 = NowMs();
        auto loaded = db.LoadUnder(L"C:\\Fotos");
        double t2 = NowMs();
        for (int i = 0; i < 2000; ++i) db.PutThumb(many[static_cast<size_t>(i)].path, 1234, 1700000000, jpeg);
        double t3 = NowMs();
        std::vector<uint8_t> got;
        int hits = 0;
        for (int i = 0; i < 2000; ++i) hits += db.GetThumb(many[static_cast<size_t>(i)].path, 1234, 1700000000, got) ? 1 : 0;
        double t4 = NowMs();
        wchar_t line[256];
        swprintf_s(line, L"Messung: 20.000 Eintraege schreiben %.0f ms, alle laden %.0f ms (%zu), 2.000 Vorschauen schreiben %.0f ms, lesen %.0f ms (%d)\n",
                   t1 - t0, t2 - t1, loaded.size(), t3 - t2, t4 - t3, hits);
        out += line;
        check(loaded.size() == many.size() && hits == 2000, L"Messung vollstaendig");
        db.Close();
        auto size = [&](const wchar_t* f) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            return GetFileAttributesExW(JoinPath(pdir, f).c_str(), GetFileExInfoStandard, &fa)
                       ? (static_cast<uint64_t>(fa.nFileSizeHigh) << 32 | fa.nFileSizeLow) : 0ull;
        };
        uint64_t idx = size(L"index.db") + size(L"index.db-wal");
        swprintf_s(line, L"Groesse: index.db %.1f MB (%.0f Byte je Eintrag), thumbs.db %.1f MB\n", idx / 1048576.0,
                   static_cast<double>(idx) / many.size(), (size(L"thumbs.db") + size(L"thumbs.db-wal")) / 1048576.0);
        out += line;
    }

    // Einstellungen: alte Klartext-INI uebernehmen, verschluesselt speichern, Manipulation erkennen
    {
        std::wstring sdir = dir + L"-einstellungen";
        CreateDirectoryW(sdir.c_str(), nullptr);
        OverrideDataDirForTest(sdir);
        std::wstring ini = JoinPath(sdir, L"peek.ini");
        const std::wstring merk = L"C:\\Privat\\Merkliste Geheimbild.jpg";
        WritePrivateProfileStringW(L"Bookmarks", L"Count", L"1", ini.c_str());
        WritePrivateProfileStringW(L"Bookmarks", L"Path0", merk.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"Search", L"H0", L"geheime Suche Breite:>=3000", ini.c_str());
        WritePrivateProfileStringW(L"View", L"TileSize", L"222", ini.c_str());
        Settings s1;
        s1.Load();
        check(s1.bookmarks.size() == 1 && s1.bookmarks[0] == merk && s1.tileSize == 222 && s1.searchHistory.size() == 1 &&
                  s1.searchHistory[0] == L"geheime Suche Breite:>=3000",
              L"alte peek.ini uebernommen");
        s1.Save();
        std::wstring dat = JoinPath(sdir, L"einstellungen.dat");
        check(GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES, L"alte peek.ini geloescht");
        std::string u = WideToUtf8(merk);
        check(GetFileAttributesW(dat.c_str()) != INVALID_FILE_ATTRIBUTES && !FileContains(dat, u.data(), u.size()) &&
                  !FileContains(dat, merk.data(), merk.size() * sizeof(wchar_t)) && !FileContains(dat, "geheime", 7) &&
                  !FileContains(dat, "TileSize", 8),
              L"Einstellungen nur verschluesselt gespeichert");
        Settings s2;
        s2.Load();
        check(s2.bookmarks == s1.bookmarks && s2.searchHistory == s1.searchHistory && s2.tileSize == 222,
              L"Einstellungen verschluesselt gelesen");
        // ein Byte veraendern: Datei wird als ungueltig erkannt, Standardwerte
        {
            HANDLE h = CreateFileW(dat.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            uint8_t b = 0;
            DWORD n = 0;
            SetFilePointer(h, 40, nullptr, FILE_BEGIN);
            ReadFile(h, &b, 1, &n, nullptr);
            b ^= 0x5A;
            SetFilePointer(h, 40, nullptr, FILE_BEGIN);
            WriteFile(h, &b, 1, &n, nullptr);
            CloseHandle(h);
        }
        Settings s3;
        s3.Load();
        check(s3.bookmarks.empty() && s3.tileSize == 160, L"veraenderte Datei verworfen (Standardwerte)");
    }

    out += failures ? L"Ergebnis: " + std::to_wstring(failures) + L" Fehler\n" : L"Ergebnis: alles in Ordnung\n";
    std::string u = WideToUtf8(out);
    HANDLE h = CreateFileW(report.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &wr, nullptr);
        CloseHandle(h);
    }
    return failures ? 2 : 0;
}

} // namespace peek
