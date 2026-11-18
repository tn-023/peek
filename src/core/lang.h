#pragma once
// Sprache der Oberflaeche (Deutsch, Englisch).
//
// Die deutschen Texte stehen direkt im Code und sind zugleich der Schluessel fuer die
// Uebersetzung. T(L"Ordner") liefert auf Deutsch den Text selbst, auf Englisch den
// Eintrag aus der Tabelle in lang_en.cpp (fehlt er, bleibt es beim deutschen Text).
//
//   T(L"...")            Text uebersetzen (Ergebnis bleibt gueltig, solange das Programm laeuft)
//   N_(L"...")           nur markieren, z. B. in statischen Tabellen; uebersetzt wird bei der
//                        Ausgabe mit T(tabelle[i])
//   TC(L"ctx", L"...")   Text mit Kontext, wenn derselbe deutsche Text je nach Stelle anders
//                        uebersetzt wird; in der Tabelle steht der Schluessel "ctx"
//   TF(L"... {0} ...", {a, b})  Text mit Platzhaltern {0}, {1} ..., damit die Wortstellung je
//                        Sprache stimmen kann
//
// tools/check-lang.py prueft in der CI, dass jeder markierte Text uebersetzt ist.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#define N_(s) s

namespace peek {

enum class Lang { De, En };

Lang CurrentLang();
void SetCurrentLang(Lang lang);
inline bool LangEn() { return CurrentLang() == Lang::En; }

const wchar_t* T(const wchar_t* de);
std::wstring T(const std::wstring& de);
const wchar_t* TC(const wchar_t* key, const wchar_t* de);
std::wstring TF(const wchar_t* de, std::initializer_list<std::wstring> args);

// Namensmuster anzeigen: Platzhalter {Datum} je Sprache ({date})
std::wstring TPattern(std::wstring pattern);

// Formatierung je Sprache
std::wstring FormatThousands(uint64_t n);                       // 12.345 / 12,345
const wchar_t* MonthName(unsigned m, bool shortName = false);   // m = 1..12
const wchar_t* WeekdayShort(unsigned d);                        // d = 0 (Montag) .. 6
std::wstring FormatDay(int y, unsigned m, unsigned d);          // 17.08.2024 / 17 Aug 2024
std::wstring FormatDayTime(int y, unsigned m, unsigned d, unsigned hh, unsigned mm);

// Anzahl der Tabelleneintraege (fuer Tests)
size_t EnglishEntryCount();

} // namespace peek
