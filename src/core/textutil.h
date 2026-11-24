#pragma once
// Plattformunabhaengige Textwerkzeuge (wchar_t-basiert).
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace peek {

// Kleinschreibung fuer ASCII, Latin-1 und die gaengigen Zeichen von Latin Extended-A.
wchar_t LowerChar(wchar_t c);
std::wstring ToLower(std::wstring_view s);

// Faltet Umlaute und Sonderzeichen fuer den Vergleich: ae, oe, ue, ss, Akzente entfernt.
// Erwartet bereits klein geschriebenen Text.
std::wstring FoldForMatch(std::wstring_view lower);

bool IsDigit(wchar_t c);
bool IsLetter(wchar_t c);
bool IsUpper(wchar_t c);
inline bool IsAlnum(wchar_t c) { return IsDigit(c) || IsLetter(c); }

std::wstring_view Trim(std::wstring_view s);

// Platzhaltervergleich mit * und ? (Text und Muster bereits klein geschrieben).
bool WildcardMatch(std::wstring_view pattern, std::wstring_view text);

// Zerlegt an Leerzeichen, beachtet "Anfuehrungszeichen".
std::vector<std::wstring> SplitQueryWords(std::wstring_view s);

// Tage seit 1970-01-01 fuer ein (proleptisch gregorianisches) Datum.
int64_t DaysFromCivil(int y, unsigned m, unsigned d);
void CivilFromDays(int64_t z, int& y, unsigned& m, unsigned& d);

// Formatierung
std::wstring FormatSize(uint64_t bytes);
// Dateiname mit Kopie-Zusatz ("Bild - Kopie.jpg", "Bild (2).jpg", "Copy of Bild.jpg" …)
bool HasCopySuffix(const std::wstring& name);
std::wstring FormatDuration(uint32_t ms);

} // namespace peek
