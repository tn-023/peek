#pragma once
// Verschluesselung des lokalen Index (index.db, thumbs.db).
//
// Ein zufaelliger 256-Bit-Schluessel liegt im Datenordner, geschuetzt mit der Windows-Datenschutz-API (DPAPI,
// an das Windows-Benutzerkonto gebunden). Auf einem anderen PC, unter einem anderen Konto oder nach einem
// zurueckgesetzten Kennwort ist er nicht lesbar: dann legt peek einen neuen Schluessel und einen leeren Index an
// (es ist nur ein Zwischenspeicher).
//
// - Pfade: jede Pfadkomponente einzeln, deterministisch verschluesselt (synthetischer IV = HMAC, AES-CTR). Gleiche
//   Ordner ergeben gleiche Kuerzel; so funktionieren Abfragen "alles unterhalb eines Ordners" weiter. Sichtbar
//   bleiben nur die Verschachtelung und die ungefaehre Laenge der Namen (auf 8 Byte aufgerundet).
// - Vorschaubilder: zufaelliger IV, AES-CTR, HMAC-SHA256 (Encrypt-then-MAC).
// Verwendet nur Windows CNG (bcrypt) und DPAPI (crypt32), keine Netzwerkfunktionen.
#include "common.h"

namespace peek {

class CacheCrypto {
public:
    // Laedt den Schluessel aus dir oder erzeugt einen neuen (fresh = true: bisherige Daten sind nicht lesbar)
    bool Init(const std::wstring& dir, bool& fresh);
    bool Ready() const { return ready_; }

    // Deterministisches Kuerzel fuer einen Text (ASCII, ohne '\' und ']')
    std::string Token(const std::wstring& s) const;
    bool FromToken(const std::string& t, std::wstring& out) const;

    // Pfad komponentenweise: "C:\A\b.jpg" -> "tok\tok\tok"
    std::string PathKey(const std::wstring& path) const;
    bool PathFromKey(const std::string& key, std::wstring& out) const;

    // Daten (Vorschaubilder): zufaellig, mit Integritaetspruefung
    std::vector<uint8_t> Seal(const uint8_t* p, size_t n) const;
    bool Open(const uint8_t* p, size_t n, std::vector<uint8_t>& out) const;

private:
    bool Ctr(const uint8_t key[32], const uint8_t iv[16], uint8_t* data, size_t n) const;
    bool Hmac(const uint8_t key[32], const uint8_t* p, size_t n, uint8_t out[32]) const;

    bool ready_ = false;
    uint32_t id_ = 0;   // unterscheidet Schluessel in den Thread-Zwischenspeichern
    uint8_t kName_[32] = {}, kNameMac_[32] = {}, kData_[32] = {}, kDataMac_[32] = {};
};

} // namespace peek
