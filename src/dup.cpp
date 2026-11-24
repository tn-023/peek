// Duplikatsuche: eigene Ansicht mit einer Liste visuell identischer Bilder. Gesucht wird unter den Bildern, die
// die Ansicht gerade zeigt (Ordner, mit Unterordnern, Sammlung, alle Sammlungen, Suchergebnis, mit Filtern).
// Visuell identisch heisst: dasselbe Bild, auch verkleinert, neu gespeichert, in einem anderen Format, mit anderen
// Metadaten, anderem Namen oder Ort. Serienbilder, bearbeitete, beschnittene, gespiegelte oder gedrehte Fassungen
// sind keine Duplikate.
//   1. Kandidaten: Fingerabdruck (dHash) des Vorschaubilds weicht in hoechstens 8 von 64 Bit ab
//   2. Pruefung:   gleiches Seitenverhaeltnis und eine kleine Farbfassung (16 x 16) stimmt fast genau ueberein
// Fehlende Fingerabdruecke berechnet der Thread aus dem gespeicherten Vorschaubild (sonst ueber den Decoder in der
// Sandbox) und legt sie im Index ab. Cloud-Dateien, die nicht auf dem Geraet liegen, bleiben aussen vor.
#include "app.h"
#include "core/lang.h"
#include "core/phash.h"
#include "core/textutil.h"
#include "imaging.h"
#include "sandbox.h"

#include <unordered_set>
#include <cmath>
#include <numeric>

namespace peek {

namespace {

bool Inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

std::wstring FoldPath(const std::wstring& p) { return ToLower(p); }

// Reihenfolge in einer Gruppe: hoechste Aufloesung, dann groessere Datei, dann aeltere Datei, dann ohne Kopie-Zusatz
bool BetterCopy(const FileEntry& x, const FileEntry& y) {
    uint64_t px = uint64_t(x.width) * x.height, py = uint64_t(y.width) * y.height;
    if (px != py) return px > py;
    if (x.size != y.size) return x.size > y.size;
    if (x.mtime != y.mtime) return x.mtime < y.mtime;
    bool cx = HasCopySuffix(x.name), cy = HasCopySuffix(y.name);
    if (cx != cy) return !cx;
    if (x.name.size() != y.name.size()) return x.name.size() < y.name.size();
    return _wcsicmp(x.path.c_str(), y.path.c_str()) < 0;
}

struct UnionFind {
    std::vector<uint32_t> p;
    explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
    uint32_t Find(uint32_t x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    void Join(uint32_t a, uint32_t b) {
        a = Find(a);
        b = Find(b);
        if (a != b) p[std::max(a, b)] = std::min(a, b);
    }
};

}  // namespace

void App::OpenDuplicates() {
    StopDupScan();
    auto& d = dup_;
    dupIn_.clear();
    for (uint32_t i : visible_) {
        const FileEntry& e = entries_[i];
        if (e.kind == Kind::Image && !e.offline && !e.gone) dupIn_.push_back(e);
    }
    // Ueberschrift: wo gesucht wird
    std::wstring place;
    if (bookmarksView_) place = T(L"Merkliste");
    else if (collectionsView_) place = T(L"Alle Sammlungen");
    else {
        place = FileNameOf(path_);
        if (place.empty()) place = path_;
    }
    d.title = TF(L"Duplikate in „{0}“", {place});
    if (!collectionsView_ && !bookmarksView_ && settings_.recursive) d.title += T(L" mit Unterordnern");
    if (!search_.empty() || filters_.Any()) d.title += T(L" (gefiltert)");
    d.groups.clear();
    d.scroll = 0;
    d.toast.clear();
    d.open = true;
    d.busy = dupIn_.size() >= 2;
    searchFocus_ = false;
    showHistory_ = false;
    hoverHit_ = -1;
    if (d.busy) {
        std::vector<DupInput> items;
        items.reserve(dupIn_.size());
        for (const auto& e : dupIn_) items.push_back({e.path, e.size, e.mtime});
        const uint64_t gen = ++dupGen_;
        dupDone_ = 0;
        dupTotal_ = static_cast<int>(items.size());
        HWND hwnd = hwnd_;
        dupThread_ = std::thread([this, items = std::move(items), gen, hwnd]() mutable { DupWorker(std::move(items), gen, hwnd); });
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::CloseDuplicates() {
    if (!dup_.open) return;
    StopDupScan();
    dup_.open = false;
    dup_.busy = false;
    dup_.groups.clear();
    dupIn_.clear();
    hoverHit_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::StopDupScan() {
    ++dupGen_;
    if (dupThread_.joinable()) dupThread_.join();
}

void App::DupWorker(std::vector<DupInput> items, uint64_t gen, HWND hwnd) {
    ComScope com;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    auto cancelled = [&] { return dupGen_.load() != gen; };
    DWORD lastPost = 0;
    auto progress = [&] {
        ++dupDone_;
        DWORD now = GetTickCount();
        if (now - lastPost > 120) {
            lastPost = now;
            PostMessageW(hwnd, WM_APP_DUP, 0, 0);
        }
    };
    // Vorschaubild aus dem Index, sonst neu erzeugen (und gleich speichern)
    auto loadThumb = [&](const DupInput& it, Pixels& px) {
        std::vector<uint8_t> blob;
        if (db_.GetThumb(it.path, it.size, it.mtime, blob) && DecodeImageMemory(blob.data(), blob.size(), px)) return true;
        if (!GuardedCall([&] { return SbDecodeImage(it.path, ThumbService::kThumbEdge, px); })) return false;
        std::vector<uint8_t> enc;
        if (EncodeThumb(px, enc)) db_.PutThumb(it.path, it.size, it.mtime, enc);
        return true;
    };

    // 1. Fingerabdruecke
    const size_t n = items.size();
    std::vector<uint64_t> hashes(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (cancelled()) return;
        const DupInput& it = items[i];
        uint64_t hv = 0;
        if (!db_.GetHash(it.path, it.size, it.mtime, hv)) {
            Pixels px;
            if (loadThumb(it, px) && px.w && px.h) {
                hv = DHash(px.data.data(), px.w, px.h, size_t(px.w) * 4);
                db_.PutHash(it.path, it.size, it.mtime, hv);
            }
        }
        hashes[i] = hv;
        progress();
    }
    if (cancelled()) return;

    // 2. Kandidaten genau pruefen (kleine Farbfassung nur fuer Bilder, die in einem Paar vorkommen)
    auto pairs = CandidatePairs(hashes, 8);
    struct Small {
        bool tried = false, ok = false;
        float aspect = 0;
        uint8_t rgb[kSmallRgbSize];
    };
    std::vector<Small> small(n);
    auto get = [&](uint32_t i) -> const Small& {
        Small& s = small[i];
        if (!s.tried) {
            s.tried = true;
            Pixels px;
            if (loadThumb(items[i], px) && px.w && px.h) {
                SmallRgb(px.data.data(), px.w, px.h, size_t(px.w) * 4, s.rgb);
                s.aspect = static_cast<float>(px.w) / px.h;
                s.ok = true;
            }
        }
        return s;
    };
    UnionFind uf(n);
    for (auto [a, b] : pairs) {
        if (cancelled()) return;
        const Small& x = get(a);
        const Small& y = get(b);
        if (!x.ok || !y.ok) continue;
        if (std::fabs(x.aspect - y.aspect) > 0.03f * std::max(x.aspect, y.aspect)) continue;   // beschnitten
        if (!SmallRgbSame(x.rgb, y.rgb)) continue;
        uf.Join(a, b);
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> byRoot;
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t r = uf.Find(i);
        auto& v = byRoot[r];
        if (v.empty()) roots.push_back(r);
        v.push_back(i);
    }
    std::vector<std::vector<uint32_t>> res;
    for (uint32_t r : roots)
        if (byRoot[r].size() >= 2) res.push_back(std::move(byRoot[r]));
    if (cancelled()) return;
    {
        std::lock_guard<std::mutex> lock(dupMutex_);
        dupRes_ = std::move(res);
        dupResGen_ = gen;
    }
    PostMessageW(hwnd, WM_APP_DUP, 1, 0);
}

void App::OnDupMessage(WPARAM wp) {
    if (!dup_.open) return;
    if (wp == 1) {
        std::vector<std::vector<uint32_t>> res;
        {
            std::lock_guard<std::mutex> lock(dupMutex_);
            if (dupResGen_ != dupGen_.load()) return;
            res = std::move(dupRes_);
        }
        if (dupThread_.joinable()) dupThread_.join();
        dup_.busy = false;
        dup_.groups.clear();
        for (auto& g : res) {
            std::vector<DupState::Item> items;
            for (uint32_t i : g)
                if (i < dupIn_.size()) items.push_back({dupIn_[i], true});
            if (items.size() < 2) continue;
            std::sort(items.begin(), items.end(), [](const DupState::Item& a, const DupState::Item& b) { return BetterCopy(a.e, b.e); });
            items[0].checked = false;   // das beste Bild bleibt, alle anderen sind zum Loeschen vorgemerkt
            dup_.groups.push_back(std::move(items));
        }
        // Gruppen in der Reihenfolge des Speicherorts, dann des Namens des besten Bildes
        std::sort(dup_.groups.begin(), dup_.groups.end(), [](const auto& a, const auto& b) {
            return _wcsicmp(a[0].e.path.c_str(), b[0].e.path.c_str()) < 0;
        });
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Eingaben
// ---------------------------------------------------------------------------

void App::DupMouseDown(float x, float y, bool dbl) {
    const Hit* h = HitAt(x, y);
    if (!h) return;
    const int g = h->a >> 16, i = h->a & 0xFFFF;
    auto valid = [&] { return g >= 0 && g < static_cast<int>(dup_.groups.size()) && i < static_cast<int>(dup_.groups[g].size()); };
    switch (h->type) {
    case HitType::DupCancel: CloseDuplicates(); return;
    case HitType::DupDelete: if (!dbl) DupDelete(); break;
    case HitType::DupCheck:
        if (valid()) dup_.groups[g][i].checked = !dup_.groups[g][i].checked;
        break;
    case HitType::DupThumb:
        if (dbl && valid()) DupCompare(g, i);
        break;
    default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::DupMouseMove(float x, float y) {
    int hh = -1;
    for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i)
        if (hits_[i].type != HitType::None && Inside(hits_[i].rc, x, y)) { hh = i; break; }
    if (hh != hoverHit_) {
        hoverHit_ = hh;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void App::DupWheel(int delta) {
    const float maxS = std::max(0.0f, DupContentH() - (H_ - 90));
    dup_.scroll = std::clamp(dup_.scroll - delta / 120.0f * 72.0f, 0.0f, maxS);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool App::DupKey(WPARAM vk) {
    const float page = std::max(60.0f, H_ - 140);
    switch (vk) {
    case VK_ESCAPE:
    case VK_BACK: CloseDuplicates(); return true;
    case VK_DELETE: DupDelete(); return true;
    case VK_UP: DupWheel(60); return true;
    case VK_DOWN: DupWheel(-60); return true;
    case VK_PRIOR: DupWheel(static_cast<int>(page / 72.0f * 120)); return true;
    case VK_NEXT: DupWheel(-static_cast<int>(page / 72.0f * 120)); return true;
    case VK_HOME: dup_.scroll = 0; InvalidateRect(hwnd_, nullptr, FALSE); return true;
    case VK_END: DupWheel(-1000000); return true;
    default: return vk != VK_F4;
    }
}

void App::DupDelete() {
    auto& d = dup_;
    if (d.busy) return;
    std::vector<std::wstring> paths;
    for (auto& g : d.groups) {
        size_t cnt = 0;
        for (auto& it : g) cnt += it.checked ? 1 : 0;
        if (cnt == g.size()) {   // nie alle Bilder einer Gruppe: eines muss bleiben
            d.toast = TF(L"Mindestens ein Bild jeder Gruppe muss bleiben – bitte bei „{0}“ eines abwählen.", {g[0].e.name});
            d.toastGlyph = 0xE946;
            d.toastUntil = GetTickCount() + 3800;
            SetTimer(hwnd_, kTimerToast, 3900, nullptr);
            // zur Gruppe blaettern
            float y = 0;
            for (auto& g2 : d.groups) {
                if (&g2 == &g) break;
                y += 38 + 56.0f * g2.size();
            }
            d.scroll = std::clamp(y, 0.0f, std::max(0.0f, DupContentH() - (H_ - 90)));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        for (auto& it : g)
            if (it.checked) paths.push_back(it.e.path);
    }
    if (paths.empty()) return;
    if (!RecycleFiles(hwnd_, paths)) return;
    DupForget(paths);
    d.toast = paths.size() == 1 ? T(L"1 Datei in den Papierkorb gelegt")
                                : TF(L"{0} Dateien in den Papierkorb gelegt", {FormatThousands(paths.size())});
    d.toastGlyph = 0xE74D;
    d.toastUntil = GetTickCount() + 2600;
    SetTimer(hwnd_, kTimerToast, 2700, nullptr);
    QuietReload();   // die Ansicht dahinter zeigt die Dateien nicht mehr
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Dateien sind weg (geloescht, verschoben): aus den Gruppen nehmen, Gruppen mit nur einem Bild verschwinden
void App::DupForget(const std::vector<std::wstring>& paths) {
    if (!dup_.open || paths.empty()) return;
    std::unordered_set<std::wstring> gone;
    for (auto& p : paths) gone.insert(FoldPath(p));
    auto& gs = dup_.groups;
    for (auto& g : gs) {
        g.erase(std::remove_if(g.begin(), g.end(), [&](const DupState::Item& it) { return gone.count(FoldPath(it.e.path)) > 0; }),
                g.end());
        if (!g.empty() && std::none_of(g.begin(), g.end(), [](const DupState::Item& it) { return !it.checked; }))
            g[0].checked = false;   // das verbleibende beste Bild nie vorgemerkt lassen
    }
    gs.erase(std::remove_if(gs.begin(), gs.end(), [](const auto& g) { return g.size() < 2; }), gs.end());
    dup_.scroll = std::clamp(dup_.scroll, 0.0f, std::max(0.0f, DupContentH() - (H_ - 90)));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Vergleich: links das beste Bild der Gruppe, rechts das gewaehlte (beim besten selbst: das naechste)
void App::DupCompare(int g, int i) {
    const auto& grp = dup_.groups[g];
    if (grp.size() < 2) return;
    int right = i <= 0 ? 1 : i;
    OpenCompareEntries(grp[0].e, grp[right].e);
    if (!compare_.open) return;
    compare_.group.clear();
    for (auto& it : grp) compare_.group.push_back(it.e);
    compare_.groupPos = right;
}

// Pfeiltasten im Vergleich aus der Duplikatsuche: rechts das naechste/vorige Bild der Gruppe (ohne das beste)
void App::CompareStepGroup(int dir) {
    auto& c = compare_;
    const int n = static_cast<int>(c.group.size());
    if (n < 3) return;
    int pos = c.groupPos - 1;                  // 0 .. n-2
    pos = ((pos + dir) % (n - 1) + (n - 1)) % (n - 1);
    c.groupPos = pos + 1;
    const FileEntry& e = c.group[c.groupPos];
    if (!viewer_.open && c.path[1] != c.path[0]) fullBmps_.erase(c.path[1]);
    c.path[1] = e.path;
    c.name[1] = e.name;
    c.info[1].clear();
    EnsureThumb(e, 0, frame_);
    if (!fullBmps_.count(e.path)) {
        fullBmps_[e.path].requested = true;
        ThumbRequest r;
        r.key = e.path;
        r.path = e.path;
        r.kind = e.kind;
        r.job = ThumbJob::Full;
        r.prio = 0;
        r.gen = 0;
        thumbs_.Request(std::move(r));
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace peek
