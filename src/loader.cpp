#include "loader.h"

namespace peek {

void Loader::Start(HWND notify, Database* db) {
    hwnd_ = notify;
    db_ = db;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_ = false;
    thread_ = std::thread([this] { Worker(); });
}

void Loader::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
}

void Loader::LoadFolder(const std::wstring& path, bool recursive, bool showHidden, uint32_t gen) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = Job{};
        job_.valid = true;
        job_.path = path;
        job_.recursive = recursive;
        job_.showHidden = showHidden;
        job_.gen = gen;
    }
    currentGen_ = gen;
    SetEvent(wake_);
}

void Loader::LoadCollections(Database* db, const std::vector<std::wstring>& roots, uint32_t gen) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = Job{};
        job_.valid = true;
        job_.fromDb = true;
        job_.roots = roots;
        job_.db = db;
        job_.gen = gen;
    }
    currentGen_ = gen;
    SetEvent(wake_);
}

std::vector<LoadBatch> Loader::Take() {
    std::lock_guard<std::mutex> lock(resultMutex_);
    std::vector<LoadBatch> out;
    out.swap(results_);
    return out;
}

void Loader::Push(LoadBatch&& b) {
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        results_.push_back(std::move(b));
    }
    PostMessageW(hwnd_, WM_APP_LOAD, 0, 0);
}

void Loader::Worker() {
    ComScope com;
    HANDLE change = INVALID_HANDLE_VALUE;
    uint32_t watchGen = 0;
    for (;;) {
        HANDLE handles[2] = {wake_, change};
        DWORD n = change != INVALID_HANDLE_VALUE ? 2 : 1;
        DWORD w = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
        if (stop_) break;
        if (w == WAIT_OBJECT_0 + 1) {
            // Aenderung im ueberwachten Ordner
            if (watchGen == currentGen_) PostMessageW(hwnd_, WM_APP_FSCHANGE, watchGen, 0);
            if (!FindNextChangeNotification(change)) {
                FindCloseChangeNotification(change);
                change = INVALID_HANDLE_VALUE;
            }
            continue;
        }
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job = job_;
            job_.valid = false;
        }
        if (!job.valid) continue;
        if (change != INVALID_HANDLE_VALUE) {
            FindCloseChangeNotification(change);
            change = INVALID_HANDLE_VALUE;
        }
        uint32_t gen = job.gen;
        auto cancel = [&] { return stop_ || currentGen_ != gen; };

        if (job.fromDb) {
            for (auto& root : job.roots) {
                if (cancel()) break;
                LoadBatch b;
                b.gen = gen;
                b.entries = job.db->LoadUnder(root);
                Push(std::move(b));
            }
        }
        std::vector<std::wstring> seenPaths;   // fuer das Aufraeumen des Index (nur direkte Inhalte)
        bool complete = true;
        if (!job.fromDb) {
            complete = EnumerateFolder(job.path, job.recursive, job.showHidden, cancel, [&](std::vector<FileEntry>&& part) {
                if (!job.recursive)
                    for (auto& e : part) seenPaths.push_back(e.path);
                LoadBatch b;
                b.gen = gen;
                b.entries = std::move(part);
                Push(std::move(b));
            });
        }
        if (cancel()) continue;
        LoadBatch done;
        done.gen = gen;
        done.done = true;
        done.failed = !job.fromDb && !complete;
        Push(std::move(done));

        // Index: Eintraege und Vorschaubilder von Dateien entfernen, die es in diesem Ordner nicht mehr gibt
        // (geloescht, verschoben oder umbenannt, auch ausserhalb von peek)
        // nur nach vollstaendiger Auflistung (ein nicht erreichbares Netzlaufwerk darf den Index nicht leeren)
        if (!job.fromDb && !job.recursive && complete && db_ && IsLocalDrivePath(job.path)) db_->PruneFolder(job.path, seenPaths);

        if (!job.fromDb && IsLocalDrivePath(job.path)) {
            change = FindFirstChangeNotificationW(
                LongPath(job.path).c_str(), job.recursive ? TRUE : FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                    FILE_NOTIFY_CHANGE_LAST_WRITE);
            watchGen = gen;
        }
    }
    if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);
}

} // namespace peek
