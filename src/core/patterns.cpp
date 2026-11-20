#include "core/patterns.h"
#include "core/textutil.h"
#include "core/lang.h"

#include <algorithm>
#include <array>

namespace peek {

uint32_t TokenDict::Id(const std::wstring& word) {
    auto it = ids_.find(word);
    if (it != ids_.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(words_.size());
    words_.push_back(word);
    ids_.emplace(word, id);
    return id;
}

bool IsStopword(std::wstring_view w) {
    static const std::array<std::wstring_view, 36> kStop = {
        L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"tif", L"tiff", L"webp", L"avif", L"heic", L"mp4", L"mov",
        L"copy", L"kopie", L"der", L"die", L"das", L"und", L"oder", L"the", L"and", L"of",
        L"von", L"mit", L"im", L"in", L"am", L"an", L"auf", L"zu", L"new", L"neu",
        L"final", L"file", L"datei", L"img"};
    for (auto s : kStop)
        if (s == w) return true;
    return false;
}

static void Push(std::vector<std::wstring>& out, std::wstring_view raw) {
    if (raw.size() < 2) return;
    bool allDigits = true;
    for (wchar_t c : raw)
        if (!IsDigit(c)) { allDigits = false; break; }
    if (allDigits) return;
    std::wstring t = FoldForMatch(ToLower(raw));
    if (IsStopword(t)) return;
    if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(std::move(t));
}

std::vector<std::wstring> TokenizeName(std::wstring_view s) {
    std::vector<std::wstring> out;
    size_t start = std::wstring_view::npos;
    auto flush = [&](size_t end) {
        if (start != std::wstring_view::npos && end > start) Push(out, s.substr(start, end - start));
        start = std::wstring_view::npos;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (!IsAlnum(c)) { flush(i); continue; }
        if (start == std::wstring_view::npos) { start = i; continue; }
        wchar_t p = s[i - 1];
        bool boundary = false;
        if (IsDigit(p) != IsDigit(c)) boundary = true;                       // img2041
        else if (IsLetter(p) && IsLetter(c)) {
            if (!IsUpper(p) && IsUpper(c)) boundary = true;                  // sommerUrlaub
            else if (IsUpper(p) && IsUpper(c) && i + 1 < s.size() &&
                     IsLetter(s[i + 1]) && !IsUpper(s[i + 1]) && (i - start) >= 2)
                boundary = true;                                             // JPEGImage
        }
        if (boundary) { flush(i); start = i; }
    }
    flush(s.size());
    return out;
}

std::wstring DetectSource(std::wstring_view stem) {
    struct P { std::wstring_view prefix; std::wstring_view name; };
    static const std::array<P, 14> kSources = {{
        {L"img_", N_(L"Kamera (IMG)")}, {L"img-", N_(L"Kamera (IMG)")}, {L"dsc", N_(L"Kamera (DSC)")},
        {L"pxl_", L"Google Pixel"}, {L"dji_", N_(L"DJI-Drohne")}, {L"gopr", L"GoPro"},
        {L"gh01", L"GoPro"}, {L"mvimg", N_(L"Bewegtfoto")}, {L"vid_", L"Video (VID)"},
        {L"screenshot", N_(L"Bildschirmfoto")}, {L"bildschirmfoto", N_(L"Bildschirmfoto")},
        {L"whatsapp", L"WhatsApp"}, {L"signal-", L"Signal"}, {L"telegram", L"Telegram"}}};
    std::wstring low = ToLower(stem.substr(0, std::min<size_t>(stem.size(), 16)));
    for (auto& p : kSources)
        if (low.rfind(p.prefix, 0) == 0) return std::wstring(p.name);
    return {};
}

static bool ValidDate(int y, int m, int d) {
    return y >= 1980 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

static int Num(std::wstring_view s, size_t pos, size_t len) {
    int v = 0;
    for (size_t i = 0; i < len; ++i) v = v * 10 + (s[pos + i] - L'0');
    return v;
}

// Prueft an Position i auf ein Datum; liefert Laenge des Treffers oder 0.
static size_t MatchDateAt(std::wstring_view s, size_t i, int& y, int& m, int& d) {
    auto digits = [&](size_t pos, size_t len) {
        if (pos + len > s.size()) return false;
        for (size_t k = 0; k < len; ++k)
            if (!IsDigit(s[pos + k])) return false;
        return true;
    };
    if (i > 0 && IsDigit(s[i - 1])) return 0;
    if (digits(i, 8) && (i + 8 == s.size() || !IsDigit(s[i + 8]))) {
        y = Num(s, i, 4); m = Num(s, i + 4, 2); d = Num(s, i + 6, 2);
        if (ValidDate(y, m, d)) return 8;
    }
    if (digits(i, 4) && i + 10 <= s.size() && (s[i + 4] == L'-' || s[i + 4] == L'_' || s[i + 4] == L'.') &&
        s[i + 7] == s[i + 4] && digits(i + 5, 2) && digits(i + 8, 2) &&
        (i + 10 == s.size() || !IsDigit(s[i + 10]))) {
        y = Num(s, i, 4); m = Num(s, i + 5, 2); d = Num(s, i + 8, 2);
        if (ValidDate(y, m, d)) return 10;
    }
    return 0;
}

std::wstring NameTemplate(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        int y, m, d;
        if (IsDigit(s[i])) {
            size_t len = MatchDateAt(s, i, y, m, d);
            if (len) { out += L"{Datum}"; i += len; continue; }
            while (i < s.size() && IsDigit(s[i])) { out += L'#'; ++i; }
            continue;
        }
        out += s[i++];
    }
    return out;
}

int64_t DateFromName(std::wstring_view s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (!IsDigit(s[i])) continue;
        int y, m, d;
        if (MatchDateAt(s, i, y, m, d))
            return DaysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
    }
    return -1;
}

std::vector<Facet> ComputeFacets(const std::vector<const std::vector<uint32_t>*>& docs,
                                 size_t maxFacets, uint32_t minDf, double maxShare) {
    std::unordered_map<uint32_t, uint32_t> df;
    for (auto* d : docs)
        for (uint32_t t : *d) ++df[t];
    const double limit = maxShare * static_cast<double>(docs.size());
    std::vector<Facet> out;
    for (auto& [id, n] : df)
        if (n >= minDf && (docs.size() < 5 || n <= limit)) out.push_back({id, n});
    std::sort(out.begin(), out.end(), [](const Facet& a, const Facet& b) {
        return a.count != b.count ? a.count > b.count : a.id < b.id;
    });
    if (out.size() > maxFacets) out.resize(maxFacets);
    return out;
}

} // namespace peek
