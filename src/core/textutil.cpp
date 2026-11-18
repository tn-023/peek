#include "core/textutil.h"
#include "core/lang.h"

#include <cstdio>
#include <cwchar>

namespace peek {

wchar_t LowerChar(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c + 32);
    if (c < 0xC0) return c;
    if (c <= 0xDE && c != 0xD7) return static_cast<wchar_t>(c + 32);  // Latin-1 Grossbuchstaben
    if (c >= 0x100 && c <= 0x17F) {                                       // Latin Extended-A
        if (c >= 0x139 && c <= 0x148) return (c % 2 == 1) ? static_cast<wchar_t>(c + 1) : c;
        if (c >= 0x179 && c <= 0x17E) return (c % 2 == 1) ? static_cast<wchar_t>(c + 1) : c;
        if (c == 0x178) return 0xFF;
        if (c < 0x138 || (c >= 0x14A && c <= 0x177)) return (c % 2 == 0) ? static_cast<wchar_t>(c + 1) : c;
    }
    return c;
}

std::wstring ToLower(std::wstring_view s) {
    std::wstring r;
    r.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) r[i] = LowerChar(s[i]);
    return r;
}

std::wstring FoldForMatch(std::wstring_view lower) {
    std::wstring r;
    r.reserve(lower.size() + 4);
    for (wchar_t c : lower) {
        switch (c) {
        case 0xE4: r += L"ae"; break;  // ae
        case 0xF6: r += L"oe"; break;  // oe
        case 0xFC: r += L"ue"; break;  // ue
        case 0xDF: r += L"ss"; break;  // ss
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE5: r += L'a'; break;
        case 0xE7: r += L'c'; break;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: r += L'e'; break;
        case 0xEC: case 0xED: case 0xEE: case 0xEF: r += L'i'; break;
        case 0xF1: r += L'n'; break;
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF8: r += L'o'; break;
        case 0xF9: case 0xFA: case 0xFB: r += L'u'; break;
        case 0xFD: case 0xFF: r += L'y'; break;
        default: r += c; break;
        }
    }
    return r;
}

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

bool IsLetter(wchar_t c) {
    if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) return true;
    if (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) return true;
    if (c >= 0x370 && c <= 0x3FF) return true;   // Griechisch
    if (c >= 0x400 && c <= 0x4FF) return true;   // Kyrillisch
    if (c >= 0x3040 && c <= 0x9FFF) return true; // CJK
    if (c >= 0xAC00 && c <= 0xD7AF) return true; // Hangul
    return false;
}

bool IsUpper(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return true;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return true;
    return false;
}

std::wstring_view Trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

bool WildcardMatch(std::wstring_view p, std::wstring_view t) {
    size_t pi = 0, ti = 0, star = std::wstring_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == t[ti])) { ++pi; ++ti; }
        else if (pi < p.size() && p[pi] == L'*') { star = pi++; mark = ti; }
        else if (star != std::wstring_view::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

std::vector<std::wstring> SplitQueryWords(std::wstring_view s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false;
    for (wchar_t c : s) {
        if (c == L'"') { quoted = !quoted; continue; }
        if (!quoted && (c == L' ' || c == L'\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Algorithmus nach Howard Hinnant (public domain)
int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void CivilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

std::wstring FormatSize(uint64_t b) {
    wchar_t buf[64];
    if (b < 1024) std::swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024ull * 1024) std::swprintf(buf, 64, L"%.1f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) std::swprintf(buf, 64, L"%.1f MB", b / (1024.0 * 1024));
    else std::swprintf(buf, 64, L"%.2f GB", b / (1024.0 * 1024 * 1024));
    std::wstring s = buf;
    if (CurrentLang() == Lang::De)
        for (auto& c : s) if (c == L'.') c = L',';   // deutsches Dezimalkomma
    return s;
}

std::wstring FormatDuration(uint32_t ms) {
    uint32_t s = ms / 1000;
    wchar_t buf[32];
    if (s >= 3600) std::swprintf(buf, 32, L"%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else std::swprintf(buf, 32, L"%u:%02u", s / 60, s % 60);
    return buf;
}

} // namespace peek
