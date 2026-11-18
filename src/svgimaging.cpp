// SVG mit resvg. Der Baum wird je Datei einmal geparst und kurz zwischengespeichert, damit der Betrachter beim
// Zoomen nur noch rendern muss. Vor dem Parsen werden alle href-Verweise geprueft: nur Datenverweise (data:) und
// interne Verweise (#id) bleiben; Verweise auf Dateien, Internetadressen und Netzwerkpfade werden entfernt.
#include "svgimaging.h"
#include "core/svgcheck.h"
#include "fileinput.h"

#include <resvg.h>

#include <cmath>
#include <memory>

namespace peek {

namespace {

// Zwei Optionssaetze: ohne Schriften (schnell) und mit Systemschriften. Das Laden aller Schriften dauert beim
// ersten Mal bis zu einigen Sekunden und ist nur fuer SVG-Dateien mit Text noetig.
std::once_flag g_optOnce, g_fontOnce;
resvg_options* g_opt = nullptr;
resvg_options* g_optFonts = nullptr;

resvg_options* NewOptions() {
    resvg_options* o = resvg_options_create();
    resvg_options_set_dpi(o, 96.0f);
    resvg_options_set_sans_serif_family(o, "Segoe UI");
    resvg_options_set_font_family(o, "Segoe UI");
    return o;
}

const resvg_options* Options(bool fonts) {
    if (!fonts) {
        std::call_once(g_optOnce, [] { g_opt = NewOptions(); });
        return g_opt;
    }
    std::call_once(g_fontOnce, [] {
        g_optFonts = NewOptions();
        resvg_options_load_system_fonts(g_optFonts);   // C:\Windows\Fonts (im Dekoder-Prozess nur Systemschriften)
    });
    return g_optFonts;
}

bool ReadFileBytes(const std::wstring& path, std::string& out, int64_t& mtime) {
    std::vector<uint8_t> d;
    if (!ReadInputAll(path, 256ull << 20, d, &mtime)) return false;
    out.assign(reinterpret_cast<const char*>(d.data()), d.size());
    return true;
}

// Wert eines href-Attributs pruefen: Datenverweise (data:) und interne Verweise (#id) bleiben, alles andere wird
// entfernt (leer). Externe Dateien bindet peek bewusst nicht ein: der Dekoder-Prozess darf nur die SVG-Datei selbst
// lesen, und so kann eine SVG-Datei weder andere Dateien noch Netzwerkpfade oeffnen.
std::string CheckHref(const std::string& v) {
    size_t i = 0;
    while (i < v.size() && (v[i] == ' ' || v[i] == '\t' || v[i] == '\n' || v[i] == '\r')) ++i;
    if (i >= v.size() || v[i] == '#') return v;
    std::string low;
    for (char c : v.substr(i, 5)) low += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (low == "data:") return v;
    return {};
}

// Alle href="..."-Werte (auch xlink:href) pruefen und ersetzen
std::string SanitizeHrefs(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 64);
    size_t i = 0;
    while (i < in.size()) {
        size_t p = in.find("href", i);
        if (p == std::string::npos) break;
        size_t q = p + 4;
        while (q < in.size() && (in[q] == ' ' || in[q] == '\t' || in[q] == '\n' || in[q] == '\r')) ++q;
        bool attr = (p == 0 || in[p - 1] == ' ' || in[p - 1] == ':' || in[p - 1] == '\t' || in[p - 1] == '\n' ||
                     in[p - 1] == '\r') && q < in.size() && in[q] == '=';
        if (!attr) {
            out.append(in, i, p + 4 - i);
            i = p + 4;
            continue;
        }
        ++q;
        while (q < in.size() && (in[q] == ' ' || in[q] == '\t' || in[q] == '\n' || in[q] == '\r')) ++q;
        if (q >= in.size() || (in[q] != '"' && in[q] != '\'')) {
            out.append(in, i, q - i);
            i = q;
            continue;
        }
        char quote = in[q];
        size_t end = in.find(quote, q + 1);
        if (end == std::string::npos) break;
        std::string val = in.substr(q + 1, end - q - 1);
        out.append(in, i, q + 1 - i);
        out += CheckHref(val);
        out += quote;
        i = end + 1;
    }
    out.append(in, i, std::string::npos);
    return out;
}

struct Tree {
    resvg_render_tree* t = nullptr;
    float w = 0, h = 0;
    ~Tree() { if (t) resvg_tree_destroy(t); }
};

struct CacheEntry {
    std::wstring path;
    int64_t mtime = 0;
    std::shared_ptr<Tree> tree;
};
std::mutex g_cacheMutex;
std::vector<CacheEntry> g_cache;   // die zuletzt benutzten Baeume (Betrachter)

std::shared_ptr<Tree> Load(const std::wstring& path, bool cache) {
    std::string data;
    int64_t mtime = 0;
    if (!ReadFileBytes(path, data, mtime)) return nullptr;
    if (cache) {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        for (auto& c : g_cache)
            if (c.mtime == mtime && _wcsicmp(c.path.c_str(), path.c_str()) == 0) return c.tree;
    }
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);   // UTF-8-BOM
    if (!SvgEntitiesSafe(data)) return nullptr;   // Entities mit Markup koennten Verweise verstecken
    std::string clean = SanitizeHrefs(data);
    auto tree = std::make_shared<Tree>();
    bool hasText = clean.find("<text") != std::string::npos || clean.find(":text") != std::string::npos;
    if (resvg_parse_tree_from_data(clean.data(), clean.size(), Options(hasText), &tree->t) != RESVG_OK || !tree->t)
        return nullptr;
    resvg_size sz = resvg_get_image_size(tree->t);
    tree->w = sz.width;
    tree->h = sz.height;
    if (!(tree->w > 0) || !(tree->h > 0)) return nullptr;
    if (cache) {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache.insert(g_cache.begin(), {path, mtime, tree});
        if (g_cache.size() > 3) g_cache.resize(3);
    }
    return tree;
}

bool RenderTo(const Tree& tree, resvg_transform tr, uint32_t w, uint32_t h, Pixels& out) {
    if (!w || !h || static_cast<uint64_t>(w) * h > (64ull << 20)) return false;
    out.w = w;
    out.h = h;
    out.data.assign(static_cast<size_t>(w) * h * 4, 0);
    if (!resvg_is_image_empty(tree.t))
        resvg_render(tree.t, tr, w, h, reinterpret_cast<char*>(out.data.data()));
    // RGBA -> BGRA (beides vormultipliziert)
    uint8_t* p = out.data.data();
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i, p += 4) std::swap(p[0], p[2]);
    out.srcW = static_cast<uint32_t>(std::lround(tree.w));
    out.srcH = static_cast<uint32_t>(std::lround(tree.h));
    return true;
}

}  // namespace

bool IsSvgPath(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(path.c_str() + dot, L".svg") == 0;
}

bool SvgSize(const std::wstring& path, float& w, float& h) {
    auto t = Load(path, false);
    if (!t) return false;
    w = t->w;
    h = t->h;
    return true;
}

bool DecodeSvg(const std::wstring& path, uint32_t maxEdge, Pixels& out) {
    bool viewer = maxEdge >= 4096;
    auto t = Load(path, viewer);
    if (!t) return false;
    float nat = std::max(t->w, t->h);
    float edge = viewer ? std::clamp(nat, 2048.0f, 4096.0f) : static_cast<float>(maxEdge ? maxEdge : nat);
    float k = edge / nat;
    uint32_t w = std::max(1u, static_cast<uint32_t>(std::lround(t->w * k)));
    uint32_t h = std::max(1u, static_cast<uint32_t>(std::lround(t->h * k)));
    resvg_transform tr = {k, 0, 0, k, 0, 0};
    return RenderTo(*t, tr, w, h, out);
}

bool RenderSvgRegion(const std::wstring& path, float k, float tx, float ty, uint32_t w, uint32_t h, Pixels& out) {
    auto t = Load(path, true);
    if (!t) return false;
    resvg_transform tr = {k, 0, 0, k, tx, ty};
    return RenderTo(*t, tr, w, h, out);
}

} // namespace peek
