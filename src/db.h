#pragma once
// Lokale Ablage: SQLite fuer Metadaten (index.db) und Vorschaubilder (thumbs.db), verschluesselt (cachecrypt.h).
#include "fs.h"
#include "cachecrypt.h"

struct sqlite3;

namespace peek {

struct MetaRow {
    uint32_t width = 0, height = 0, durationMs = 0;
    int64_t taken = 0;
    std::wstring camera;
    bool hasGeo = false;   // Aufnahmeort (Grad, Norden/Osten positiv)
    double lat = 0, lon = 0;
    std::wstring ai;       // Bilderzeugung, kompakt (core/aimeta.h)
};

class Database {
public:
    ~Database();
    bool Open(const std::wstring& dir);
    void Close();

    // Metadaten: nur gueltig, wenn Groesse und Aenderungszeit uebereinstimmen.
    bool GetMeta(const std::wstring& path, uint64_t size, int64_t mtime, MetaRow& out);
    void PutFile(const FileEntry& e, const MetaRow* meta, int64_t seenStamp);
    void PutFiles(const std::vector<FileEntry>& entries, int64_t seenStamp);  // Basisdaten, Transaktion
    void PutFilesChunk(const std::vector<FileEntry>& entries, int64_t seen);
    void PutMetaBatch(const std::vector<std::pair<FileEntry, MetaRow>>& rows, int64_t seenStamp);
    void DeleteUnseenUnder(const std::wstring& root, int64_t seenStamp);
    void DeleteUnder(const std::wstring& root);
    // Datei/Ordner geloescht, verschoben oder umbenannt: Eintraege und Vorschaubilder sofort entfernen
    void Forget(const std::vector<std::wstring>& paths);
    // Nach dem Einlesen eines Ordners: Eintraege direkter Kinder entfernen, die es nicht mehr gibt
    void PruneFolder(const std::wstring& folder, const std::vector<std::wstring>& existing);

    // Alle bekannten Dateien unterhalb einer Wurzel (fuer die Sammlungsansicht)
    std::vector<FileEntry> LoadUnder(const std::wstring& root);
    uint64_t CountUnder(const std::wstring& root);

    // Vorschaubilder (JPEG)
    bool GetThumb(const std::wstring& path, uint64_t size, int64_t mtime, std::vector<uint8_t>& jpeg);
    bool HasThumb(const std::wstring& path, uint64_t size, int64_t mtime);
    // Fingerabdruck des Vorschaubilds (Duplikatsuche); nur zusammen mit einem Vorschaubild gespeichert
    bool GetHash(const std::wstring& path, uint64_t size, int64_t mtime, uint64_t& hash);
    void PutHash(const std::wstring& path, uint64_t size, int64_t mtime, uint64_t hash);
    void PutThumb(const std::wstring& path, uint64_t size, int64_t mtime, const std::vector<uint8_t>& jpeg);

    // Einstellungen-Seite
    struct Stats {
        uint64_t files = 0, media = 0, withMeta = 0, thumbs = 0;
    };
    Stats GetStats();
    void ClearAll();   // Index und Vorschaubilder loeschen

private:
    void PutFileLocked(const FileEntry& e, const MetaRow* meta, int64_t seenStamp);
    void DeleteThumbKeys(const std::vector<std::string>& keys);
    CacheCrypto crypto_;
    sqlite3* meta_ = nullptr;
    sqlite3* thumbs_ = nullptr;
    std::mutex metaMutex_, thumbMutex_;
};

// Pruefmodus fuer die CI (indextest.cpp): peek.exe --index-test <ordner> <bericht.txt>
int RunIndexTest(const std::wstring& dir, const std::wstring& report);

} // namespace peek
