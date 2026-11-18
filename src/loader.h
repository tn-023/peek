#pragma once
// Laedt Ordnerinhalte im Hintergrund (in Teilen) und ueberwacht den aktuellen Ordner auf Aenderungen.
#include "db.h"

#include <thread>

namespace peek {

struct LoadBatch {
    uint32_t gen = 0;
    std::vector<FileEntry> entries;
    bool done = false;
};

class Loader {
public:
    ~Loader() { Stop(); }
    void Start(HWND notify, Database* db);
    void Stop();
    void LoadFolder(const std::wstring& path, bool recursive, bool showHidden, uint32_t gen);
    void LoadCollections(Database* db, const std::vector<std::wstring>& roots, uint32_t gen);
    std::vector<LoadBatch> Take();

private:
    struct Job {
        bool valid = false;
        bool fromDb = false;
        std::wstring path;
        std::vector<std::wstring> roots;
        bool recursive = false, showHidden = false;
        uint32_t gen = 0;
        Database* db = nullptr;
    };
    void Worker();
    void Push(LoadBatch&& b);

    std::mutex mutex_;
    Job job_;
    HANDLE wake_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> currentGen_{0};
    std::mutex resultMutex_;
    std::vector<LoadBatch> results_;
    HWND hwnd_ = nullptr;
    Database* db_ = nullptr;   // zum Aufraeumen verwaister Index-Eintraege
};

} // namespace peek
