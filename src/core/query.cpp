#include "core/query.h"
#include "core/textutil.h"

#include <cstdlib>

namespace peek {

bool NumCond::Test(double v) const {
    switch (op) {
    case Any: return true;
    case Lt: return v < a;
    case Le: return v <= a;
    case Gt: return v > a;
    case Ge: return v >= a;
    case Eq: return v == a;
    case Range: return v >= a && v < b;
    }
    return true;
}

bool Query::Empty() const {
    return terms.empty() && excludes.empty() && wildcards.empty() && exts.empty() && cams.empty() &&
           !width.active() && !height.active() && !megapixel.active() && !size.active() &&
           !durationSec.active() && !date.active() && type < 0;
}

static bool ParseNumber(std::wstring_view s, double& v) {
    std::wstring t(s);
    for (auto& c : t) if (c == L',') c = L'.';
    if (t.empty()) return false;
    wchar_t* end = nullptr;
    v = std::wcstod(t.c_str(), &end);
    return end && *end == 0;
}

bool ParseSize(std::wstring_view s, double& bytes) {
    std::wstring t = ToLower(s);
    double mul = 1;
    auto ends = [&](std::wstring_view suf) {
        return t.size() > suf.size() && t.compare(t.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (ends(L"gb")) { mul = 1024.0 * 1024 * 1024; t.resize(t.size() - 2); }
    else if (ends(L"mb")) { mul = 1024.0 * 1024; t.resize(t.size() - 2); }
    else if (ends(L"kb")) { mul = 1024.0; t.resize(t.size() - 2); }
    else if (ends(L"g")) { mul = 1024.0 * 1024 * 1024; t.resize(t.size() - 1); }
    else if (ends(L"m")) { mul = 1024.0 * 1024; t.resize(t.size() - 1); }
    else if (ends(L"k")) { mul = 1024.0; t.resize(t.size() - 1); }
    else if (ends(L"b")) { t.resize(t.size() - 1); }
    double v;
    if (!ParseNumber(t, v)) return false;
    bytes = v * mul;
    return true;
}

bool ParseDuration(std::wstring_view s, double& sec) {
    std::wstring t = ToLower(s);
    double mul = 1;
    if (!t.empty() && t.back() == L'h') { mul = 3600; t.pop_back(); }
    else if (!t.empty() && t.back() == L'm') { mul = 60; t.pop_back(); }
    else if (!t.empty() && t.back() == L's') { t.pop_back(); }
    double v;
    if (!ParseNumber(t, v)) return false;
    sec = v * mul;
    return true;
}

bool ParseDateSpan(std::wstring_view s, int64_t& from, int64_t& to) {
    int parts[3] = {0, 0, 0};
    int n = 0;
    size_t i = 0;
    while (i < s.size() && n < 3) {
        if (!IsDigit(s[i])) return false;
        int v = 0, len = 0;
        while (i < s.size() && IsDigit(s[i])) { v = v * 10 + (s[i] - L'0'); ++i; ++len; }
        if ((n == 0 && len != 4) || (n > 0 && (len < 1 || len > 2))) return false;
        parts[n++] = v;
        if (i < s.size()) {
            if (s[i] != L'-' && s[i] != L'.' && s[i] != L'/') return false;
            ++i;
        }
    }
    if (n == 0 || i != s.size()) return false;
    int y = parts[0];
    if (n == 1) {
        from = DaysFromCivil(y, 1, 1) * 86400;
        to = DaysFromCivil(y + 1, 1, 1) * 86400;
    } else if (n == 2) {
        unsigned m = static_cast<unsigned>(parts[1]);
        if (m < 1 || m > 12) return false;
        from = DaysFromCivil(y, m, 1) * 86400;
        to = (m == 12 ? DaysFromCivil(y + 1, 1, 1) : DaysFromCivil(y, m + 1, 1)) * 86400;
    } else {
        unsigned m = static_cast<unsigned>(parts[1]), d = static_cast<unsigned>(parts[2]);
        if (m < 1 || m > 12 || d < 1 || d > 31) return false;
        from = DaysFromCivil(y, m, d) * 86400;
        to = from + 86400;
    }
    return true;
}

// Liest Vergleichsoperator und Wert(e) mit einer Wertfunktion.
template <typename F>
static bool ParseCond(std::wstring_view v, NumCond& c, F parse) {
    size_t dots = v.find(L"..");
    if (dots != std::wstring_view::npos) {
        double a, b;
        if (!parse(v.substr(0, dots), a) || !parse(v.substr(dots + 2), b)) return false;
        c.op = NumCond::Range; c.a = a; c.b = b;
        return true;
    }
    NumCond::Op op = NumCond::Eq;
    if (v.rfind(L">=", 0) == 0) { op = NumCond::Ge; v.remove_prefix(2); }
    else if (v.rfind(L"<=", 0) == 0) { op = NumCond::Le; v.remove_prefix(2); }
    else if (v.rfind(L">", 0) == 0) { op = NumCond::Gt; v.remove_prefix(1); }
    else if (v.rfind(L"<", 0) == 0) { op = NumCond::Lt; v.remove_prefix(1); }
    else if (v.rfind(L"=", 0) == 0) { op = NumCond::Eq; v.remove_prefix(1); }
    double a;
    if (!parse(v, a)) return false;
    c.op = op; c.a = a;
    return true;
}

static bool ParseDateCond(std::wstring_view v, NumCond& c) {
    int64_t f1, t1, f2, t2;
    size_t dots = v.find(L"..");
    if (dots != std::wstring_view::npos) {
        if (!ParseDateSpan(v.substr(0, dots), f1, t1) || !ParseDateSpan(v.substr(dots + 2), f2, t2)) return false;
        c.op = NumCond::Range; c.a = static_cast<double>(f1); c.b = static_cast<double>(t2);
        return true;
    }
    NumCond::Op op = NumCond::Range;
    if (v.rfind(L">=", 0) == 0) { op = NumCond::Ge; v.remove_prefix(2); }
    else if (v.rfind(L"<=", 0) == 0) { op = NumCond::Le; v.remove_prefix(2); }
    else if (v.rfind(L">", 0) == 0) { op = NumCond::Gt; v.remove_prefix(1); }
    else if (v.rfind(L"<", 0) == 0) { op = NumCond::Lt; v.remove_prefix(1); }
    if (!ParseDateSpan(v, f1, t1)) return false;
    c.op = op;
    switch (op) {
    case NumCond::Range: c.a = static_cast<double>(f1); c.b = static_cast<double>(t1); break;
    case NumCond::Gt: c.op = NumCond::Ge; c.a = static_cast<double>(t1); break;   // nach dem Zeitraum
    case NumCond::Ge: c.a = static_cast<double>(f1); break;
    case NumCond::Lt: c.a = static_cast<double>(f1); break;
    case NumCond::Le: c.op = NumCond::Lt; c.a = static_cast<double>(t1); break;
    default: break;
    }
    return true;
}

static void SplitList(std::wstring_view v, std::vector<std::wstring>& out) {
    size_t s = 0;
    while (s <= v.size()) {
        size_t e = v.find(L',', s);
        if (e == std::wstring_view::npos) e = v.size();
        auto part = Trim(v.substr(s, e - s));
        if (!part.empty()) {
            std::wstring p = FoldForMatch(ToLower(part));
            if (!p.empty() && p[0] == L'.') p.erase(0, 1);
            out.push_back(p);
        }
        s = e + 1;
    }
}

Query ParseQuery(std::wstring_view text) {
    Query q;
    auto num = [](std::wstring_view s, double& v) { return ParseNumber(s, v); };
    for (auto& raw : SplitQueryWords(text)) {
        std::wstring w = ToLower(raw);
        size_t colon = w.find(L':');
        if (colon != std::wstring::npos && colon > 0 && colon + 1 < w.size()) {
            std::wstring key = w.substr(0, colon);
            std::wstring_view val = std::wstring_view(w).substr(colon + 1);
            bool ok = true;
            if (key == L"w" || key == L"breite") ok = ParseCond(val, q.width, num);
            else if (key == L"h" || key == L"hoehe" || key == L"h\u00f6he") ok = ParseCond(val, q.height, num);
            else if (key == L"mp" || key == L"megapixel") ok = ParseCond(val, q.megapixel, num);
            else if (key == L"size" || key == L"groesse" || key == L"gr\u00f6\u00dfe" || key == L"gr\u00f6sse")
                ok = ParseCond(val, q.size, ParseSize);
            else if (key == L"dur" || key == L"dauer") ok = ParseCond(val, q.durationSec, ParseDuration);
            else if (key == L"date" || key == L"datum") ok = ParseDateCond(val, q.date);
            else if (key == L"taken" || key == L"aufgenommen" || key == L"aufnahme") ok = ParseDateCond(val, q.taken);
            else if (key == L"ext" || key == L"endung") SplitList(val, q.exts);
            else if (key == L"cam" || key == L"kamera") SplitList(val, q.cams);
            else if (key == L"type" || key == L"typ") {
                if (val == L"image" || val == L"bild" || val == L"bilder") q.type = static_cast<int>(Kind::Image);
                else if (val == L"video" || val == L"videos") q.type = static_cast<int>(Kind::Video);
                else if (val == L"folder" || val == L"ordner") q.type = static_cast<int>(Kind::Folder);
                else if (val == L"other" || val == L"sonstige") q.type = static_cast<int>(Kind::Other);
                else ok = false;
            } else ok = false;
            if (ok) continue;
            // unbekannter Schluessel -> als normaler Begriff behandeln
        }
        std::wstring f = FoldForMatch(w);
        if (f.size() > 1 && f[0] == L'-') { q.excludes.push_back(f.substr(1)); continue; }
        if (f.find(L'*') != std::wstring::npos || f.find(L'?') != std::wstring::npos) {
            q.wildcards.push_back(f);
            continue;
        }
        if (!f.empty()) q.terms.push_back(f);
    }
    return q;
}

static bool Contains(std::wstring_view hay, std::wstring_view needle) {
    return hay.find(needle) != std::wstring_view::npos;
}

bool Matches(const Query& q, const MatchInput& in) {
    if (q.type >= 0 && static_cast<int>(in.kind) != q.type) return false;
    for (auto& t : q.terms)
        if (!Contains(in.name, t) && !Contains(in.folder, t) && !Contains(in.camera, t)) return false;
    for (auto& t : q.excludes)
        if (Contains(in.name, t) || Contains(in.folder, t)) return false;
    for (auto& w : q.wildcards)
        if (!WildcardMatch(w, in.name)) return false;
    if (!q.exts.empty()) {
        bool any = false;
        for (auto& e : q.exts) if (in.ext == e || (e == L"jpg" && in.ext == L"jpeg")) { any = true; break; }
        if (!any) return false;
    }
    if (!q.cams.empty()) {
        bool any = false;
        for (auto& c : q.cams) if (Contains(in.camera, c)) { any = true; break; }
        if (!any) return false;
    }
    if (q.width.active() && !q.width.Test(in.width)) return false;
    if (q.height.active() && !q.height.Test(in.height)) return false;
    if (q.megapixel.active() && !q.megapixel.Test(double(in.width) * in.height / 1e6)) return false;
    if (q.size.active() && !q.size.Test(static_cast<double>(in.size))) return false;
    if (q.durationSec.active() && !q.durationSec.Test(in.durationSec)) return false;
    if (q.date.active() && !q.date.Test(static_cast<double>(in.time))) return false;
    if (q.taken.active() && (!in.taken || !q.taken.Test(static_cast<double>(in.taken)))) return false;
    return true;
}

const std::vector<QueryParam>& QueryParams() {
    static const std::vector<QueryParam> p = {
        {L"Breite", L"Breite:>", L"Breite:>3000"},
        {L"H\u00f6he", L"H\u00f6he:>", L"H\u00f6he:>2000"},
        {L"Gr\u00f6\u00dfe", L"Gr\u00f6\u00dfe:>", L"Gr\u00f6\u00dfe:>10mb"},
        {L"Datum", L"Datum:", L"Datum:2024-08..2024-09"},
        {L"Aufgenommen", L"Aufgenommen:", L"Aufgenommen:2024"},
        {L"Dauer", L"Dauer:>", L"Dauer:>5min"},
        {L"Endung", L"Endung:", L"Endung:jpg,png"},
        {L"Kamera", L"Kamera:", L"Kamera:x-t5"},
    };
    return p;
}

} // namespace peek
