#include "activity.h"
#include "meta.h"
#include "docimaging.h"
#include "sandbox.h"

namespace peek {

void MetaService::Start(HWND notify, Database* db, int threads) {
    hwnd_ = notify;
    db_ = db;
    stop_ = false;
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { Worker(); });
}

void MetaService::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void MetaService::SetGeneration(uint32_t gen) {
    std::lock_guard<std::mutex> lock(mutex_);
    gen_ = gen;
    covers_.clear();
    metas_.clear();
}

void MetaService::Enqueue(std::vector<MetaRequest>&& reqs) {
    if (reqs.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& r : reqs) {
            if (r.cover) covers_.push_back(std::move(r));
            else metas_.push_back(std::move(r));
        }
    }
    cv_.notify_all();
}

size_t MetaService::Pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    return covers_.size() + metas_.size() + active_;
}

std::vector<MetaResult> MetaService::Take() {
    posted_ = false;
    std::lock_guard<std::mutex> lock(resultMutex_);
    std::vector<MetaResult> out;
    out.swap(results_);
    return out;
}

static int CompareNames(const std::wstring& a, const std::wstring& b) {
    return StrCmpLogicalW(a.c_str(), b.c_str());
}

// Ein Ordner: erstes Medium nach Name; optional Unterordner sammeln
static bool ScanForCover(const std::wstring& dir, bool showHidden, MetaResult& out, std::vector<std::wstring>* subdirs) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(LongPath(JoinPath(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring best;
    FileEntry bestEntry;
    int seen = 0;
    do {
        if (++seen > 500) break;
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;
        if (!showHidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (subdirs && subdirs->size() < 32 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                subdirs->push_back(JoinPath(dir, fd.cFileName));
            continue;
        }
        FileEntry e = MakeEntry(dir, fd);
        if (!e.IsMedia() || e.offline) continue;
        if (best.empty() || CompareNames(e.name, best) < 0) {
            best = e.name;
            bestEntry = std::move(e);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best.empty()) return false;
    out.coverPath = bestEntry.path;
    out.coverSize = bestEntry.size;
    out.coverMtime = bestEntry.mtime;
    out.coverKind = bestEntry.kind;
    return true;
}

bool FindFolderCover(const std::wstring& folder, bool showHidden, MetaResult& out) {
    if (!IsLocalDrivePath(folder)) return false;
    std::vector<std::wstring> subdirs;
    if (ScanForCover(folder, showHidden, out, &subdirs)) return true;
    std::sort(subdirs.begin(), subdirs.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareNames(a, b) < 0;
    });
    int n = 0;
    for (auto& s : subdirs) {
        if (++n > 16) break;
        if (ScanForCover(s, showHidden, out, nullptr)) return true;
    }
    return false;
}

void MetaService::Flush(std::vector<std::pair<FileEntry, MetaRow>>& batch) {
    if (db_ && !batch.empty()) db_->PutMetaBatch(batch, 0);
    batch.clear();
}

void MetaService::Worker() {
    ComScope com;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<std::pair<FileEntry, MetaRow>> batch;
    DWORD lastFlush = GetTickCount();
    for (;;) {
        MetaRequest r;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (covers_.empty() && metas_.empty() && !batch.empty()) {
                lock.unlock();
                Flush(batch);
                lock.lock();
            }
            cv_.wait(lock, [&] { return stop_ || !covers_.empty() || !metas_.empty(); });
            if (stop_) break;
            if (!covers_.empty()) { r = std::move(covers_.front()); covers_.pop_front(); }
            else { r = std::move(metas_.front()); metas_.pop_front(); }
            ++active_;
        }
        MarkUiActivity();

        MetaResult res;
        res.path = r.entry.path;
        res.gen = r.gen;
        res.cover = r.cover;
        if (r.cover) {
            res.ok = GuardedCall([&] { return FindFolderCover(r.entry.path, r.showHidden, res); });
        } else if (db_ && db_->GetMeta(r.entry.path, r.entry.size, r.entry.mtime, res.row)) {
            res.ok = true;
        } else {
            const FileEntry& e = r.entry;
            res.ok = GuardedCall([&] {
                if (e.kind == Kind::Other) return SbDocMeta(e.path, res.row);   // Seitenzahl bzw. PSD-Abmessungen
                return e.kind == Kind::Video ? SbVideoMeta(e.path, res.row) : SbImageMeta(e.path, res.row);
            });
            // auch Fehlschlaege merken, damit defekte Dateien nicht erneut gelesen werden
            batch.emplace_back(e, res.row);
            if (batch.size() >= 200 || GetTickCount() - lastFlush > 1000) {
                Flush(batch);
                lastFlush = GetTickCount();
            }
        }
        --active_;
        if (res.gen == gen_) {
            {
                std::lock_guard<std::mutex> lock(resultMutex_);
                results_.push_back(std::move(res));
            }
            if (!posted_.exchange(true)) PostMessageW(hwnd_, WM_APP_META, 0, 0);
        }
    }
    Flush(batch);
}

} // namespace peek
