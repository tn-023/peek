#include "indexer.h"
#include "core/phash.h"
#include "thumbs.h"
#include "docimaging.h"
#include "sandbox.h"
#include "activity.h"

namespace peek {

void Indexer::Start(HWND notify, Database* db) {
    hwnd_ = notify;
    db_ = db;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { Worker(); });
}

void Indexer::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
}

void Indexer::SetRoots(const std::vector<std::wstring>& roots, bool showHidden) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots_ = roots;
        showHidden_ = showHidden;
    }
    Rescan();
}

void Indexer::Rescan() {
    restart_ = true;
    SetEvent(wake_);
}

void Indexer::Notify(bool force) {
    DWORD now = GetTickCount();
    if (!force && now - lastNotify_ < 250) return;
    lastNotify_ = now;
    PostMessageW(hwnd_, WM_APP_INDEX, 0, 0);
}

void Indexer::WaitWhilePaused() {
    // pausiert (Einstellung) oder Oberflaeche gerade beschaeftigt: warten
    while (!ShouldAbort() && (paused_ || UiRecentlyActive(700))) Sleep(paused_ ? 100 : 40);
}

bool Indexer::IndexRoot(const std::wstring& root, int64_t stamp) {
    if (!IsLocalDrivePath(root) || !DirectoryExists(root)) return true;
    bool hidden;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hidden = showHidden_;
    }
    // 1) Struktur-Scan: alle Dateien erfassen (Name, Groesse, Datum; daraus Begriffe, Quelle, Muster)
    // Netzlaufwerk: nur diese Stufe. Inhalte (Metadaten, Vorschaubilder) liest peek erst, wenn ein Ordner geoeffnet
    // wird, und nur fuer diesen Ordner: kein Hintergrund-Scan, der den Server dauerhaft belastet.
    const bool network = IsNetworkPath(root);
    std::vector<FileEntry> media;
    bool complete = EnumerateFolder(root, true, hidden, [&] { return ShouldAbort(); }, [&](std::vector<FileEntry>&& part) {
        WaitWhilePaused();
        db_->PutFiles(part, stamp);
        for (auto& e : part)
            if (!network && e.HasThumb() && !e.offline) {
                media.push_back(std::move(e));
                ++total_;
            }
        Notify(false);
    });
    if (ShouldAbort()) return false;

    // 2) Metadaten und Vorschaubilder der Medien
    std::vector<std::pair<FileEntry, MetaRow>> batch;
    for (auto& e : media) {
        WaitWhilePaused();
        if (ShouldAbort()) break;
        MetaRow row;
        bool haveMeta = db_->GetMeta(e.path, e.size, e.mtime, row);
        bool haveThumb = db_->HasThumb(e.path, e.size, e.mtime);
        if (!haveMeta || !haveThumb) {
            Pixels px;
            bool ok = false;
            if (e.Doc()) {
                MetaRow dm;
                ok = !haveThumb && GuardedCall([&] { return SbDocThumb(e.path, ThumbService::kThumbEdge, px, &dm); });
                if (!haveMeta) {
                    if (!ok) GuardedCall([&] { return SbDocMeta(e.path, dm); });
                    row = dm;
                }
            } else if (e.kind == Kind::Video) {
                MetaRow vm;
                ok = GuardedCall([&] { return SbVideoThumbnail(e.path, ThumbService::kThumbEdge, px, &vm); });
                if (!haveMeta) {
                    row.width = vm.width;
                    row.height = vm.height;
                    row.durationMs = vm.durationMs;
                }
            } else {
                if (!haveMeta) GuardedCall([&] { return SbImageMeta(e.path, row); });
                if (!haveThumb) ok = GuardedCall([&] { return SbDecodeImage(e.path, ThumbService::kThumbEdge, px); });
            }
            if (!haveThumb && ok) {
                std::vector<uint8_t> enc;
                if (EncodeThumb(px, enc)) {
                    db_->PutThumb(e.path, e.size, e.mtime, enc);
                    if (e.kind == Kind::Image) db_->PutHash(e.path, e.size, e.mtime, DHash(px.data.data(), px.w, px.h, size_t(px.w) * 4));
                }
            }
            if (!haveMeta) batch.emplace_back(e, row);
            if (batch.size() >= 100) {
                db_->PutMetaBatch(batch, stamp);
                batch.clear();
            }
        }
        ++done_;
        Notify(false);
    }
    db_->PutMetaBatch(batch, stamp);
    if (ShouldAbort()) return false;

    // 3) Entfernte Dateien aus dem Index loeschen, nur nach vollstaendigem Scan
    // (ein zwischendurch nicht erreichbares Netzlaufwerk darf den Index nicht leeren)
    if (complete) db_->DeleteUnseenUnder(root, stamp);
    return true;
}

void Indexer::Worker() {
    ComScope com;
    // Niedrigste CPU-Prioritaet, aber kein Hintergrund-E/A-Modus: der drosselt auch die Datenbank-Schreibzugriffe
    // so stark, dass andere Threads lange auf die Datenbank warten (Prioritaetsumkehr).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    for (;;) {
        WaitForSingleObject(wake_, INFINITE);
        if (stop_) break;
        while (restart_ && !stop_) {
            restart_ = false;
            std::vector<std::wstring> roots;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                roots = roots_;
            }
            busy_ = true;
            done_ = 0;
            total_ = 0;
            Notify(true);
            int64_t stamp = NowUnix();
            for (auto& r : roots) {
                if (!IndexRoot(r, stamp)) break;
            }
            busy_ = false;
            Notify(true);
        }
    }
}

} // namespace peek
