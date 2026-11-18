#pragma once
// Lokale Mustererkennung in Dateinamen und Metadaten.
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace peek {

// Zuordnung Begriff <-> Nummer (nur vom UI-Thread benutzt).
class TokenDict {
public:
    uint32_t Id(const std::wstring& word);
    const std::wstring& Word(uint32_t id) const { return words_[id]; }
    size_t Size() const { return words_.size(); }
    void Clear() { ids_.clear(); words_.clear(); }
private:
    std::unordered_map<std::wstring, uint32_t> ids_;
    std::vector<std::wstring> words_;
};

// Zerlegt einen Dateinamen (ohne Endung) in normalisierte Begriffe:
// Trennzeichen, Gross/Klein-Wechsel, Buchstabe/Ziffer-Grenzen; Kleinschreibung,
// Umlaute gefaltet. Reine Zahlen, Einzelzeichen und Stoppwoerter entfallen.
std::vector<std::wstring> TokenizeName(std::wstring_view stem);

// Bekannte Quellen-Praefixe (IMG, DSC, PXL, Screenshot ...) als lesbarer Name, sonst leer.
std::wstring DetectSource(std::wstring_view stem);

// Namensvorlage: Ziffernfolgen -> '#', erkannte Datumsangaben -> {Datum}.
// Beispiel: "IMG_20240817_1430" -> "IMG_{Datum}_####"
std::wstring NameTemplate(std::wstring_view stem);

// Erkennt ein Datum im Namen (YYYYMMDD oder YYYY-MM-DD / YYYY_MM_DD). Liefert Tage seit 1970 oder -1.
int64_t DateFromName(std::wstring_view stem);

bool IsStopword(std::wstring_view word);

struct Facet {
    uint32_t id = 0;
    uint32_t count = 0;
};

// Haeufigste trennscharfe Begriffe einer Ansicht: df >= minDf, df <= maxShare * n.
// docs: je Dokument eine Liste von Begriffs-Nummern (ohne Doppelte).
std::vector<Facet> ComputeFacets(const std::vector<const std::vector<uint32_t>*>& docs,
                                 size_t maxFacets, uint32_t minDf = 3, double maxShare = 0.8);

} // namespace peek
