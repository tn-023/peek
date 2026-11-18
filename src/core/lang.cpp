#include "core/lang.h"

#include <atomic>
#include <cwchar>
#include <unordered_map>

namespace peek {

struct LangPair { const wchar_t* de; const wchar_t* en; };
extern const LangPair kEnglish[];
extern const size_t kEnglishCount;

namespace {

std::atomic<int> g_lang{static_cast<int>(Lang::De)};

const std::unordered_map<std::wstring_view, const wchar_t*>& EnglishMap() {
    static const std::unordered_map<std::wstring_view, const wchar_t*> m = [] {
        std::unordered_map<std::wstring_view, const wchar_t*> r;
        r.reserve(kEnglishCount * 2);
        for (size_t i = 0; i < kEnglishCount; ++i) r.emplace(kEnglish[i].de, kEnglish[i].en);
        return r;
    }();
    return m;
}

const wchar_t* Lookup(std::wstring_view de) {
    auto& m = EnglishMap();
    auto it = m.find(de);
    return it == m.end() ? nullptr : it->second;
}

}  // namespace

Lang CurrentLang() { return static_cast<Lang>(g_lang.load(std::memory_order_relaxed)); }
void SetCurrentLang(Lang lang) { g_lang.store(static_cast<int>(lang), std::memory_order_relaxed); }

const wchar_t* T(const wchar_t* de) {
    if (!de || CurrentLang() == Lang::De) return de;
    // Zeichenkettenliterale haben eine feste Adresse: Ergebnis je Adresse merken
    thread_local std::unordered_map<const wchar_t*, const wchar_t*> cache;
    auto it = cache.find(de);
    if (it != cache.end()) return it->second;
    const wchar_t* en = Lookup(de);
    const wchar_t* r = en ? en : de;
    cache.emplace(de, r);
    return r;
}

std::wstring T(const std::wstring& de) {
    if (CurrentLang() == Lang::De || de.empty()) return de;
    const wchar_t* en = Lookup(de);
    return en ? std::wstring(en) : de;
}

const wchar_t* TC(const wchar_t* key, const wchar_t* de) {
    if (CurrentLang() == Lang::De) return de;
    const wchar_t* en = Lookup(key);
    return en ? en : de;
}

std::wstring TF(const wchar_t* de, std::initializer_list<std::wstring> args) {
    std::wstring_view f = T(de);
    std::wstring out;
    out.reserve(f.size() + 32);
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == L'{' && i + 2 < f.size() && f[i + 1] >= L'0' && f[i + 1] <= L'9' && f[i + 2] == L'}') {
            size_t k = static_cast<size_t>(f[i + 1] - L'0');
            if (k < args.size()) out += *(args.begin() + k);
            i += 2;
            continue;
        }
        out += f[i];
    }
    return out;
}

std::wstring TPattern(std::wstring pattern) {
    if (CurrentLang() == Lang::De) return pattern;
    static const std::wstring_view from = L"{Datum}";
    const wchar_t* to = T(L"{Datum}");
    for (size_t pos = pattern.find(from); pos != std::wstring::npos; pos = pattern.find(from, pos + 1))
        pattern.replace(pos, from.size(), to);
    return pattern;
}

std::wstring FormatThousands(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    wchar_t sep = CurrentLang() == Lang::En ? L',' : L'.';
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), 1, sep);
    return s;
}

const wchar_t* MonthName(unsigned m, bool shortName) {
    static const wchar_t* deLong[12] = {L"Januar", L"Februar", L"März", L"April", L"Mai", L"Juni",
                                        L"Juli", L"August", L"September", L"Oktober", L"November", L"Dezember"};
    static const wchar_t* deShort[12] = {L"Jan", L"Feb", L"Mär", L"Apr", L"Mai", L"Jun",
                                         L"Jul", L"Aug", L"Sep", L"Okt", L"Nov", L"Dez"};
    static const wchar_t* enLong[12] = {L"January", L"February", L"March", L"April", L"May", L"June",
                                        L"July", L"August", L"September", L"October", L"November", L"December"};
    static const wchar_t* enShort[12] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                         L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (m < 1 || m > 12) return L"";
    bool en = CurrentLang() == Lang::En;
    return shortName ? (en ? enShort : deShort)[m - 1] : (en ? enLong : deLong)[m - 1];
}

const wchar_t* WeekdayShort(unsigned d) {
    static const wchar_t* de[7] = {L"Mo", L"Di", L"Mi", L"Do", L"Fr", L"Sa", L"So"};
    static const wchar_t* en[7] = {L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa", L"Su"};
    if (d > 6) return L"";
    return (CurrentLang() == Lang::En ? en : de)[d];
}

std::wstring FormatDay(int y, unsigned m, unsigned d) {
    wchar_t buf[48];
    if (CurrentLang() == Lang::En)
        std::swprintf(buf, 48, L"%u %ls %d", d, MonthName(m, true), y);
    else
        std::swprintf(buf, 48, L"%02u.%02u.%04d", d, m, y);
    return buf;
}

std::wstring FormatDayTime(int y, unsigned m, unsigned d, unsigned hh, unsigned mm) {
    wchar_t buf[24];
    std::swprintf(buf, 24, L", %02u:%02u", hh, mm);
    return FormatDay(y, m, d) + buf;
}

size_t EnglishEntryCount() { return kEnglishCount; }

} // namespace peek
