#pragma once
// Indexiert Sammlungen im Hintergrund (nur solange peek laeuft): Dateien, Metadaten, Vorschaubilder.
#include "imaging.h"

#include <thread>

namespace peek {

class Indexer {
public:
    ~Indexer() { Stop(); }
    void Start(HWND notify, Database* db);
    void Stop();
    void SetRoots(const std::vector<std::wstring>& roots, bool showHidden);
    void Rescan();
    void Pause(bool paused) { paused_ = paused; }

    bool Busy() const { return busy_; }
    uint64_t Done() const { return done_; }
    uint64_t Total() const { return total_; }

private:
    void Worker();
    bool IndexRoot(const std::wstring& root, int64_t stamp);
    bool ShouldAbort() const { return stop_ || restart_; }
    void WaitWhilePaused();
    void Notify(bool force);

    std::mutex mutex_;
    std::vector<std::wstring> roots_;
    bool showHidden_ = false;
    HANDLE wake_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false}, restart_{false}, paused_{false}, busy_{false};
    std::atomic<uint64_t> done_{0}, total_{0};
    DWORD lastNotify_ = 0;
    HWND hwnd_ = nullptr;
    Database* db_ = nullptr;
};

} // namespace peek
