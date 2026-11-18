#pragma once
// Metadaten-Dienst fuer die aktuelle Ansicht: liest Kopfdaten (WIC/Media Foundation),
// nutzt den Index als Cache und sucht Ordner-Vorschaubilder.
#include "imaging.h"

#include <condition_variable>
#include <deque>
#include <thread>

namespace peek {

struct MetaRequest {
    FileEntry entry;          // nur Basisfelder werden genutzt
    bool cover = false;       // true: Ordner-Vorschaubild suchen
    bool showHidden = false;
    uint32_t gen = 0;
};

struct MetaResult {
    std::wstring path;
    uint32_t gen = 0;
    bool cover = false;
    bool ok = false;
    MetaRow row;
    std::wstring coverPath;
    uint64_t coverSize = 0;
    int64_t coverMtime = 0;
    Kind coverKind = Kind::Other;
};

class MetaService {
public:
    ~MetaService() { Stop(); }
    void Start(HWND notify, Database* db, int threads);
    void Stop();
    void SetGeneration(uint32_t gen);              // leert die Warteschlange
    void Enqueue(std::vector<MetaRequest>&& reqs);
    std::vector<MetaResult> Take();
    size_t Pending();

private:
    void Worker();
    void Flush(std::vector<std::pair<FileEntry, MetaRow>>& batch);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<MetaRequest> covers_, metas_;
    std::mutex resultMutex_;
    std::vector<MetaResult> results_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    std::atomic<uint32_t> gen_{0};
    std::atomic<size_t> active_{0};
    HWND hwnd_ = nullptr;
    Database* db_ = nullptr;
    std::atomic<bool> posted_{false};
};

// Sucht das erste Bild/Video eines Ordners (Name aufsteigend), sonst in direkten Unterordnern.
bool FindFolderCover(const std::wstring& folder, bool showHidden, MetaResult& out);

} // namespace peek
