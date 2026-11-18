#include "activity.h"
#include "thumbs.h"
#include "svgimaging.h"
#include "docimaging.h"
#include "sandbox.h"

namespace peek {

static std::wstring JobKey(ThumbJob job, const std::wstring& key) {
    wchar_t p = job == ThumbJob::Thumb ? L'T' : (job == ThumbJob::Full ? L'F' : L'I');
    return std::wstring(1, p) + key;
}

void ThumbService::Start(HWND notify, Database* db, int threads) {
    hwnd_ = notify;
    db_ = db;
    stop_ = false;
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { Worker(); });
}

void ThumbService::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

void ThumbService::Request(ThumbRequest r) {
    std::wstring jk = JobKey(r.job, r.key);
    r.prio = std::clamp(r.prio, 0, 2);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inflight_.count(jk)) return;
        auto it = pending_.find(jk);
        if (it != pending_.end() && it->second <= r.prio) return;
        pending_[jk] = r.prio;
        queues_[r.prio].push_back(std::move(r));
    }
    cv_.notify_one();
}

void ThumbService::CancelBelow(uint32_t gen) {
    std::lock_guard<std::mutex> lock(mutex_);
    minGen_ = gen;
    for (int p = 0; p < 2; ++p) {
        auto& q = queues_[p];
        std::deque<ThumbRequest> keep;
        for (auto& r : q) {
            if (r.gen != 0 && r.gen < gen) {
                auto it = pending_.find(JobKey(r.job, r.key));
                if (it != pending_.end() && it->second == p) pending_.erase(it);
            } else {
                keep.push_back(std::move(r));
            }
        }
        q.swap(keep);
    }
}

bool ThumbService::Pop(ThumbRequest& out) {
    for (int p = 0; p < 3; ++p) {
        auto& q = queues_[p];
        while (!q.empty()) {
            // sichtbare Auftraege: neueste zuerst (zuletzt angeforderte Zeilen), sonst FIFO
            ThumbRequest r;
            if (p == 0) { r = std::move(q.back()); q.pop_back(); }
            else { r = std::move(q.front()); q.pop_front(); }
            std::wstring jk = JobKey(r.job, r.key);
            auto it = pending_.find(jk);
            if (it == pending_.end() || it->second < p) continue;   // erledigt oder hoeher eingereiht
            pending_.erase(it);
            if (r.gen != 0 && r.gen < minGen_) continue;
            inflight_.insert(jk);
            out = std::move(r);
            return true;
        }
    }
    return false;
}

size_t ThumbService::Backlog() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queues_[0].size() + queues_[1].size() + queues_[2].size();
}

std::vector<ThumbResult> ThumbService::Take() {
    posted_ = false;
    std::lock_guard<std::mutex> lock(resultMutex_);
    std::vector<ThumbResult> out;
    out.swap(results_);
    return out;
}

bool ThumbService::Process(const ThumbRequest& r, ThumbResult& res) {
    switch (r.job) {
    case ThumbJob::Icon:
        return ShellIcon(r.path, r.isDir, r.perFile, res.px);
    case ThumbJob::Full:
        return SbDecodeImage(r.path, kFullEdge, res.px);
    case ThumbJob::SvgView:
        return SbSvgRegion(r.path, r.vk, r.vx, r.vy, r.vw, r.vh, res.px);
    case ThumbJob::Thumb: {
        std::vector<uint8_t> blob;
        if (db_ && db_->GetThumb(r.path, r.size, r.mtime, blob) && DecodeImageMemory(blob.data(), blob.size(), res.px))
            return true;
        bool ok = false;
        if (r.kind == Kind::Video) {
            ok = SbVideoThumbnail(r.path, kThumbEdge, res.px, &res.meta);
            res.hasMeta = ok;
        } else if (r.kind == Kind::Other) {
            ok = SbDocThumb(r.path, kThumbEdge, res.px, nullptr);   // PDF, Photoshop, Illustrator
        } else {
            ok = SbDecodeImage(r.path, kThumbEdge, res.px);
        }
        if (ok && db_) {
            std::vector<uint8_t> enc;
            if (EncodeThumb(res.px, enc)) db_->PutThumb(r.path, r.size, r.mtime, enc);
        }
        return ok;
    }
    }
    return false;
}

void ThumbService::Worker() {
    ComScope com;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        ThumbRequest r;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] {
                return stop_ || !queues_[0].empty() || !queues_[1].empty() || !queues_[2].empty();
            });
            if (stop_) return;
            if (!Pop(r)) continue;
        }
        if (r.gen != 0) MarkUiActivity();   // Auftraege der Ansicht (nicht des Indexers)
        ThumbResult res;
        res.key = r.key;
        res.job = r.job;
        res.path = r.path;
        res.ok = GuardedCall([&] { return Process(r, res); });
        if (!res.ok) res.px = {};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inflight_.erase(JobKey(r.job, r.key));
        }
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            results_.push_back(std::move(res));
        }
        if (!posted_.exchange(true)) PostMessageW(hwnd_, WM_APP_THUMB, 0, 0);
    }
}

} // namespace peek
