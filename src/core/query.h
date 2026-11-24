#pragma once
// Abfragesprache des Suchfelds, z. B.:  rom urlaub breite:>3000 größe:>10mb datum:2024-08..2024-09 -screenshot
// (die englischen Kurzformen w:, h:, size:, date: … gelten weiterhin)
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace peek {

enum class Kind : uint8_t { Folder = 0, Image = 1, Video = 2, Other = 3 };

struct NumCond {
    enum Op : uint8_t { Any, Lt, Le, Gt, Ge, Eq, Range } op = Any;
    double a = 0, b = 0;
    bool active() const { return op != Any; }
    bool Test(double v) const;
};

struct Query {
    std::vector<std::wstring> terms;      // Teilstrings (klein, gefaltet), alle muessen passen
    std::vector<std::wstring> excludes;   // -begriff
    std::vector<std::wstring> wildcards;  // mit * oder ?
    std::vector<std::wstring> exts;       // ext:jpg,png
    std::vector<std::wstring> cams;       // cam:x-t5
    std::vector<std::wstring> prompts, models, loras;   // Angaben zur Bilderzeugung
    NumCond width, height, megapixel, size, durationSec;
    NumCond date;                         // Unix-Sekunden
    NumCond taken;                        // Aufnahmedatum (nur Dateien mit Aufnahmedatum)
    int type = -1;                        // -1 alle, sonst Kind
    bool Empty() const;
};

Query ParseQuery(std::wstring_view text);

// Deutsche Suchparameter fuer die Auswahlpillen unter dem Suchfeld: Anzeige, einzufuegender Text, Beispiel
struct QueryParam { const wchar_t* label; const wchar_t* insert; const wchar_t* example; };
const std::vector<QueryParam>& QueryParams();

// Zu pruefende Eigenschaften einer Datei (Texte bereits klein + gefaltet).
struct MatchInput {
    std::wstring_view name;    // Dateiname inkl. Endung
    std::wstring_view folder;  // Ordnerpfad
    std::wstring_view ext;     // ohne Punkt
    std::wstring_view camera;
    std::wstring_view prompt, model, lora;   // Bilderzeugung (Modelle/LoRAs zeilenweise)
    Kind kind = Kind::Other;
    uint32_t width = 0, height = 0;
    uint64_t size = 0;
    int64_t time = 0;          // Unix-Sekunden (Aufnahme oder Aenderung)
    int64_t taken = 0;         // Aufnahmedatum oder 0
    double durationSec = 0;
};

bool Matches(const Query& q, const MatchInput& in);

// Hilfsfunktionen (auch fuer Tests)
bool ParseSize(std::wstring_view s, double& bytes);
bool ParseDuration(std::wstring_view s, double& seconds);
// Datum "2024", "2024-08", "2024-08-17" -> [von, bis) in Unix-Sekunden
bool ParseDateSpan(std::wstring_view s, int64_t& from, int64_t& to);

} // namespace peek
