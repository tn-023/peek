#pragma once
// Vorschaubild-Dienst: priorisierte Warteschlange, mehrere Dekoder-Threads, Ablage in thumbs.db.
#include "imaging.h"

#include <condition_variable>
#include <deque>
#include <thread>

namespace peek {

enum class ThumbJob : uint8_t { Thumb, Full, Icon, SvgView, Details };   // SvgView: scharfer SVG-Ausschnitt fuer den Betrachter; Details: Aufnahmedaten

struct ThumbRequest {
    std::wstring key;
    std::wstring path;
    Kind kind = Kind::Other;
    ThumbJob job = ThumbJob::Thumb;
    uint64_t size = 0;
    int64_t mtime = 0;
    bool isDir = false;
    bool perFile = false;
    int prio = 0;       // 0 sichtbar, 1 Umgebung, 2 Hintergrund
    uint32_t gen = 0;   // Generation der Ansicht; 0 = nie verwerfen
    float vk = 1, vx = 0, vy = 0;   // SvgView: Massstab und Versatz
    uint32_t vw = 0, vh = 0;        // SvgView: Groesse in Pixeln
    bool sharp = false;             // Netzlaufwerk: zweite Stufe (ganze Datei) nach dem eingebetteten Vorschaubild
};

struct ThumbResult {
    std::wstring key;
    ThumbJob job = ThumbJob::Thumb;
    bool ok = false;
    Pixels px;
    bool hasMeta = false;
    std::wstring path;
    MetaRow meta;
    bool quick = false;   // erste Stufe (eingebettetes Vorschaubild), die scharfe Fassung folgt
    std::wstring details; // Details: Objektiv, Blende, Belichtung, ISO (siehe ReadPhotoDetails)
};

class ThumbService {
public:
    static constexpr uint32_t kThumbEdge = 320;
    static constexpr uint32_t kFullEdge = 8192;

    ~ThumbService() { Stop(); }
    void Start(HWND notify, Database* db, int threads);
    void Stop();

    void Request(ThumbRequest r);
    void CancelBelow(uint32_t gen);   // verwirft sichtbare/benachbarte Auftraege aelterer Ansichten
    std::vector<ThumbResult> Take();
    size_t Backlog();

private:
    void Worker();
    bool Pop(ThumbRequest& out);
    bool Process(const ThumbRequest& r, ThumbResult& res);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ThumbRequest> queues_[3];
    std::unordered_map<std::wstring, int> pending_;   // job+key -> beste Prioritaet
    std::unordered_set<std::wstring> inflight_;
    std::mutex resultMutex_;
    std::vector<ThumbResult> results_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    HWND hwnd_ = nullptr;
    Database* db_ = nullptr;
    std::atomic<uint32_t> minGen_{0};
    std::atomic<bool> posted_{false};
};

} // namespace peek
