// HEIF/HEIC-Dateikopf und EXIF lesen (siehe heif.h). Nur Lesen innerhalb der uebergebenen Puffer; alle Laengen
// und Anzahlen aus der Datei werden gegen die Puffergrenzen geprueft und gedeckelt.
#include "core/heif.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace peek {

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

// Lesen mit Grenzpruefung (Big Endian wie in ISO-BMFF); nach einem Fehler bleibt ok == false
struct Rd {
    const uint8_t* d;
    size_t pos, end;
    bool ok = true;
    bool Need(size_t n) {
        if (!ok || n > end - pos) { ok = false; return false; }
        return true;
    }
    uint8_t U8() { return Need(1) ? d[pos++] : 0; }
    uint16_t U16() {
        if (!Need(2)) return 0;
        uint16_t v = uint16_t((d[pos] << 8) | d[pos + 1]);
        pos += 2;
        return v;
    }
    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v = (uint32_t(d[pos]) << 24) | (uint32_t(d[pos + 1]) << 16) | (uint32_t(d[pos + 2]) << 8) | d[pos + 3];
        pos += 4;
        return v;
    }
    uint64_t U64() {
        uint64_t hi = U32();
        return (hi << 32) | U32();
    }
    uint64_t UN(int bytes) {   // Feldbreite 0, 4 oder 8 Byte (iloc)
        if (bytes == 0) return 0;
        if (bytes == 4) return U32();
        if (bytes == 8) return U64();
        ok = false;
        return 0;
    }
};

// naechster Kasten im Bereich [pos, end): Typ, Nutzdaten [payload, boxEnd). declaredEnd: Ende laut Datei
// (kann hinter end liegen, dann false)
bool NextBox(const uint8_t* d, size_t& pos, size_t end, uint32_t& type, size_t& payload, size_t& boxEnd,
             uint64_t& declaredEnd) {
    if (pos > end || end - pos < 8) return false;
    Rd r{d, pos, end};
    uint64_t size = r.U32();
    type = r.U32();
    if (size == 1) {
        size = r.U64();
        if (!r.ok || size < 16) return false;
    } else if (size == 0) {
        size = end - pos;   // bis zum Ende
    } else if (size < 8) {
        return false;
    }
    declaredEnd = pos + size;
    if (size > end - pos) return false;
    payload = r.pos;
    boxEnd = pos + static_cast<size_t>(size);
    pos = boxEnd;
    return true;
}

struct Loc { uint32_t id; uint64_t offset, length; };
struct Prop { uint32_t type; uint32_t a = 0, b = 0; };
struct Assoc { uint32_t id; uint16_t index; };

constexpr size_t kMaxItems = 20000;

void ParseIinf(const uint8_t* d, size_t p, size_t e, std::vector<std::pair<uint32_t, uint32_t>>& items) {
    Rd r{d, p, e};
    uint8_t ver = r.U8();
    r.pos += 3;
    if (!r.Need(0)) return;
    r.pos += (ver == 0 ? 2 : 4);   // Anzahl: die Kaesten selbst zaehlen
    if (r.pos > e) return;
    size_t pos = r.pos;
    uint32_t type;
    size_t pl, be;
    uint64_t de;
    while (items.size() < kMaxItems && NextBox(d, pos, e, type, pl, be, de)) {
        if (type != FourCC('i', 'n', 'f', 'e')) continue;
        Rd q{d, pl, be};
        uint8_t v = q.U8();
        q.pos += 3;
        if (v < 2) continue;   // alte Form ohne Typangabe
        uint32_t id = v == 2 ? q.U16() : q.U32();
        q.U16();   // Schutz
        uint32_t it = q.U32();
        if (q.ok) items.push_back({id, it});
    }
}

void ParseIloc(const uint8_t* d, size_t p, size_t e, std::vector<Loc>& locs) {
    Rd r{d, p, e};
    uint8_t ver = r.U8();
    r.pos += 3;
    if (ver > 2 || !r.Need(0)) return;
    uint8_t b1 = r.U8(), b2 = r.U8();
    int offSize = b1 >> 4, lenSize = b1 & 15, baseSize = b2 >> 4, idxSize = (ver == 1 || ver == 2) ? (b2 & 15) : 0;
    uint32_t count = ver < 2 ? r.U16() : r.U32();
    for (uint32_t i = 0; i < count && r.ok && locs.size() < kMaxItems; ++i) {
        uint32_t id = ver < 2 ? r.U16() : r.U32();
        int method = 0;
        if (ver == 1 || ver == 2) method = r.U16() & 15;
        r.U16();   // Datenverweis
        uint64_t base = r.UN(baseSize);
        uint16_t ext = r.U16();
        uint64_t off0 = 0, len0 = 0;
        for (uint16_t k = 0; k < ext && r.ok; ++k) {
            if (idxSize) r.UN(idxSize);
            uint64_t o = r.UN(offSize), l = r.UN(lenSize);
            if (k == 0) { off0 = o; len0 = l; }
        }
        // nur zusammenhaengende Eintraege direkt in der Datei (Aufbauart 0)
        if (r.ok && ext == 1 && method == 0 && len0 > 0 && base + off0 >= base) locs.push_back({id, base + off0, len0});
    }
}

void ParseIprp(const uint8_t* d, size_t p, size_t e, std::vector<Prop>& props, std::vector<Assoc>& assoc) {
    size_t pos = p;
    uint32_t type;
    size_t pl, be;
    uint64_t de;
    while (NextBox(d, pos, e, type, pl, be, de)) {
        if (type == FourCC('i', 'p', 'c', 'o')) {
            size_t q = pl;
            uint32_t pt;
            size_t ppl, pbe;
            uint64_t pde;
            while (props.size() < kMaxItems && NextBox(d, q, be, pt, ppl, pbe, pde)) {
                Prop pr{pt};
                Rd r{d, ppl, pbe};
                if (pt == FourCC('i', 's', 'p', 'e')) {
                    r.pos += 4;   // Version, Flags
                    pr.a = r.U32();
                    pr.b = r.U32();
                } else if (pt == FourCC('i', 'r', 'o', 't')) {
                    pr.a = r.U8() & 3;
                } else if (pt == FourCC('i', 'm', 'i', 'r')) {
                    pr.a = r.U8() & 1;
                }
                if (!r.ok) pr.type = 0;   // unvollstaendig: nicht verwenden
                props.push_back(pr);
            }
        } else if (type == FourCC('i', 'p', 'm', 'a')) {
            Rd r{d, pl, be};
            uint8_t ver = r.U8();
            r.U8();
            r.U8();
            uint8_t flags = r.U8();
            uint32_t count = r.U32();
            for (uint32_t i = 0; i < count && r.ok && assoc.size() < kMaxItems; ++i) {
                uint32_t id = ver < 1 ? r.U16() : r.U32();
                uint8_t n = r.U8();
                for (uint8_t k = 0; k < n && r.ok && assoc.size() < kMaxItems; ++k) {
                    uint16_t idx = (flags & 1) ? uint16_t(r.U16() & 0x7fff) : uint16_t(r.U8() & 0x7f);
                    if (r.ok) assoc.push_back({id, idx});
                }
            }
        }
    }
}

}  // namespace

int HeifOrientation(int angle, int mirrorAxis, bool mirrorFirst) {
    // Abbildungen als 2x2-Matrix [a b; c d] mit x' = a x + b y, y' = c x + d y (y nach unten)
    struct M { int a, b, c, d; };
    auto mul = [](M l, M r) {   // erst r, dann l
        return M{l.a * r.a + l.b * r.c, l.a * r.b + l.b * r.d, l.c * r.a + l.d * r.c, l.c * r.b + l.d * r.d};
    };
    M rot{1, 0, 0, 1};
    for (int i = 0; i < (angle & 3); ++i) rot = mul(M{0, 1, -1, 0}, rot);   // 90 Grad gegen den Uhrzeigersinn
    M mir{1, 0, 0, 1};
    if (mirrorAxis == 0) mir = M{1, 0, 0, -1};        // oben und unten tauschen
    else if (mirrorAxis == 1) mir = M{-1, 0, 0, 1};   // links und rechts tauschen
    M t = mirrorFirst ? mul(rot, mir) : mul(mir, rot);
    static const M kExif[9] = {{0, 0, 0, 0},  {1, 0, 0, 1},  {-1, 0, 0, 1}, {-1, 0, 0, -1}, {1, 0, 0, -1},
                               {0, 1, 1, 0},  {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
    for (int o = 1; o <= 8; ++o)
        if (kExif[o].a == t.a && kExif[o].b == t.b && kExif[o].c == t.c && kExif[o].d == t.d) return o;
    return 1;
}

bool HeifParse(const uint8_t* data, size_t size, HeifInfo& info) {
    info = HeifInfo{};
    if (!data || size < 16) return false;
    size_t pos = 0;
    uint32_t type;
    size_t pl, be;
    uint64_t de;
    if (!NextBox(data, pos, size, type, pl, be, de) || type != FourCC('f', 't', 'y', 'p')) return false;
    // meta-Kasten suchen (steht bei HEIC am Anfang)
    for (;;) {
        size_t before = pos;
        if (!NextBox(data, pos, size, type, pl, be, de)) {
            // Kasten reicht ueber den Puffer hinaus: benoetigte Laenge melden, falls es meta ist
            if (size - before >= 8) {
                Rd r{data, before, size};
                r.U32();
                if (r.U32() == FourCC('m', 'e', 't', 'a') && de > size) info.metaEnd = de;
            }
            return false;
        }
        if (type == FourCC('m', 'e', 't', 'a')) break;
    }
    info.metaEnd = be;
    uint32_t primary = 0;
    bool havePrimary = false;
    std::vector<std::pair<uint32_t, uint32_t>> items;
    std::vector<Loc> locs;
    std::vector<Prop> props;
    std::vector<Assoc> assoc;
    const size_t metaEnd = be;
    size_t mp = pl + 4;   // Version, Flags
    if (mp > metaEnd) return false;
    size_t cpl, cend;
    while (NextBox(data, mp, metaEnd, type, cpl, cend, de)) {
        if (type == FourCC('p', 'i', 't', 'm')) {
            Rd r{data, cpl, cend};
            uint8_t ver = r.U8();
            r.pos += 3;
            primary = ver == 0 ? r.U16() : r.U32();
            havePrimary = r.ok;
        } else if (type == FourCC('i', 'i', 'n', 'f')) {
            ParseIinf(data, cpl, cend, items);
        } else if (type == FourCC('i', 'l', 'o', 'c')) {
            ParseIloc(data, cpl, cend, locs);
        } else if (type == FourCC('i', 'p', 'r', 'p')) {
            ParseIprp(data, cpl, cend, props, assoc);
        }
    }
    if (!havePrimary) return false;
    int angle = 0, axis = -1;
    bool sawRot = false, mirrorFirst = false;
    for (const Assoc& a : assoc) {
        if (a.id != primary || a.index == 0 || a.index > props.size()) continue;
        const Prop& p = props[a.index - 1];
        if (p.type == FourCC('i', 's', 'p', 'e') && !info.width) {
            info.width = p.a;
            info.height = p.b;
        } else if (p.type == FourCC('i', 'r', 'o', 't')) {
            angle = static_cast<int>(p.a);
            sawRot = true;
        } else if (p.type == FourCC('i', 'm', 'i', 'r')) {
            axis = static_cast<int>(p.a);
            if (!sawRot) mirrorFirst = true;
        }
    }
    info.orientation = HeifOrientation(angle, axis, mirrorFirst);
    for (const auto& it : items) {
        if (it.second != FourCC('E', 'x', 'i', 'f')) continue;
        for (const Loc& l : locs)
            if (l.id == it.first) {
                info.exifOffset = l.offset;
                info.exifLength = l.length;
                break;
            }
        if (info.exifLength) break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// EXIF (TIFF-Aufbau)
// ---------------------------------------------------------------------------
namespace {

struct Tiff {
    const uint8_t* d;
    size_t n;
    bool le;
    bool U16(size_t at, uint16_t& v) const {
        if (at > n || n - at < 2) return false;
        v = le ? uint16_t(d[at] | (d[at + 1] << 8)) : uint16_t((d[at] << 8) | d[at + 1]);
        return true;
    }
    bool U32(size_t at, uint32_t& v) const {
        if (at > n || n - at < 4) return false;
        v = le ? (uint32_t(d[at]) | (uint32_t(d[at + 1]) << 8) | (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24))
               : ((uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3]);
        return true;
    }
};

struct Entry { uint16_t tag, type; uint32_t count; size_t at; };   // at: Lage des Wertes

bool ValueAt(const Tiff& t, size_t entry, Entry& e) {
    uint32_t off;
    if (!t.U16(entry, e.tag) || !t.U16(entry + 2, e.type) || !t.U32(entry + 4, e.count)) return false;
    static const uint8_t kSize[14] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};   // 13 = Verzeichnis
    uint64_t bytes = (e.type < 14 ? kSize[e.type] : 0) * uint64_t(e.count);
    if (bytes == 0) return false;
    if (bytes <= 4) {
        e.at = entry + 8;
    } else {
        if (!t.U32(entry + 8, off)) return false;
        e.at = off;
    }
    return e.at <= t.n && bytes <= t.n - e.at;
}

std::string Ascii(const Tiff& t, const Entry& e) {
    std::string s;
    if (e.type != 2) return s;
    size_t n = std::min<size_t>(e.count, 256);
    for (size_t i = 0; i < n; ++i) {
        char c = static_cast<char>(t.d[e.at + i]);
        if (c == 0) break;
        s.push_back(c);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

double Number(const Tiff& t, const Entry& e) {
    uint16_t s;
    uint32_t a, b;
    switch (e.type) {
    case 3: return t.U16(e.at, s) ? s : 0;
    case 4: return t.U32(e.at, a) ? a : 0;
    case 5:
        if (t.U32(e.at, a) && t.U32(e.at + 4, b) && b) return double(a) / b;
        return 0;
    case 10:
        if (t.U32(e.at, a) && t.U32(e.at + 4, b) && b) return double(int32_t(a)) / int32_t(b);
        return 0;
    default: return 0;
    }
}

// Eintraege eines Verzeichnisses; fn(entry) fuer jeden lesbaren Eintrag
template <typename Fn>
void ReadIfd(const Tiff& t, uint32_t off, Fn fn) {
    uint16_t count;
    if (!t.U16(off, count)) return;
    count = std::min<uint16_t>(count, 1000);
    for (uint16_t i = 0; i < count; ++i) {
        Entry e;
        if (ValueAt(t, size_t(off) + 2 + size_t(i) * 12, e)) fn(e);
    }
}

}  // namespace

bool ExifParse(const uint8_t* data, size_t size, ExifInfo& out) {
    out = ExifInfo{};
    if (!data || size < 8) return false;
    Tiff t{data, size, false};
    if (data[0] == 'I' && data[1] == 'I') t.le = true;
    else if (!(data[0] == 'M' && data[1] == 'M')) return false;
    uint16_t magic;
    uint32_t ifd0;
    if (!t.U16(2, magic) || magic != 42 || !t.U32(4, ifd0)) return false;
    uint32_t exifIfd = 0, gpsIfd = 0;
    std::string dateTime;
    ReadIfd(t, ifd0, [&](const Entry& e) {
        switch (e.tag) {
        case 271: out.make = Ascii(t, e); break;
        case 272: out.model = Ascii(t, e); break;
        case 306: dateTime = Ascii(t, e); break;
        case 274: out.orientation = static_cast<int>(Number(t, e)); break;
        case 34665: if (e.type == 4 || e.type == 13) t.U32(e.at, exifIfd); break;
        case 34853: if (e.type == 4 || e.type == 13) t.U32(e.at, gpsIfd); break;
        default: break;
        }
    });
    if (gpsIfd && gpsIfd != ifd0 && gpsIfd != exifIfd) {
        // Breite/Laenge als Grad, Minuten, Sekunden (drei Brueche), dazu N/S bzw. E/W
        auto dms = [&](const Entry& e) {
            if (e.type != 5 || e.count < 3) return -1.0;
            double v[3];
            for (int i = 0; i < 3; ++i) {
                Entry x = e;
                x.at = e.at + size_t(i) * 8;
                v[i] = Number(t, x);
            }
            return v[0] + v[1] / 60.0 + v[2] / 3600.0;
        };
        char latRef = 0, lonRef = 0;
        double lat = -1, lon = -1;
        ReadIfd(t, gpsIfd, [&](const Entry& e) {
            switch (e.tag) {
            case 1: { std::string r = Ascii(t, e); latRef = r.empty() ? 0 : r[0]; break; }
            case 2: lat = dms(e); break;
            case 3: { std::string r = Ascii(t, e); lonRef = r.empty() ? 0 : r[0]; break; }
            case 4: lon = dms(e); break;
            default: break;
            }
        });
        bool refs = (latRef == 'N' || latRef == 'S') && (lonRef == 'E' || lonRef == 'W');
        // 0/0 schreiben manche Geraete ohne Empfang: kein Ort
        if (refs && lat >= 0 && lat <= 90 && lon >= 0 && lon <= 180 && (lat > 1e-7 || lon > 1e-7)) {
            out.hasGps = true;
            out.lat = latRef == 'S' ? -lat : lat;
            out.lon = lonRef == 'W' ? -lon : lon;
        }
    }
    if (exifIfd && exifIfd != ifd0) {
        ReadIfd(t, exifIfd, [&](const Entry& e) {
            switch (e.tag) {
            case 36867: out.dateTaken = Ascii(t, e); break;
            case 33434: out.exposure = Number(t, e); break;
            case 33437: out.fnumber = Number(t, e); break;
            case 34855: out.iso = static_cast<uint32_t>(Number(t, e)); break;
            case 42036: out.lens = Ascii(t, e); break;
            default: break;
            }
        });
    }
    if (out.dateTaken.empty()) out.dateTaken = dateTime;
    if (out.orientation < 1 || out.orientation > 8) out.orientation = 0;
    return true;
}

bool JpegFindExif(const uint8_t* d, size_t n, size_t& off, size_t& len) {
    off = len = 0;
    if (!d || n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t p = 2;
    while (p + 4 <= n) {
        if (d[p] != 0xFF) return false;
        uint8_t m = d[p + 1];
        if (m == 0xFF) { ++p; continue; }                     // Fuellbytes
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { p += 2; continue; }   // ohne Laenge
        if (m == 0xDA || m == 0xD9) return false;             // Bilddaten beginnen: kein EXIF mehr
        size_t segLen = (size_t(d[p + 2]) << 8) | d[p + 3];
        if (segLen < 2) return false;
        size_t body = p + 4, bodyLen = segLen - 2;
        if (m == 0xE1 && bodyLen >= 6 && body + 6 <= n && std::memcmp(d + body, "Exif\0\0", 6) == 0) {
            if (body + bodyLen > n) return false;            // abgeschnitten
            off = body + 6;
            len = bodyLen - 6;
            return len >= 8;
        }
        p = body + bodyLen;
    }
    return false;
}

bool HeifExifParse(const uint8_t* item, size_t size, ExifInfo& out) {
    out = ExifInfo{};
    if (!item || size < 4) return false;
    uint32_t skip = (uint32_t(item[0]) << 24) | (uint32_t(item[1]) << 16) | (uint32_t(item[2]) << 8) | item[3];
    if (skip > size - 4) return false;
    return ExifParse(item + 4 + skip, size - 4 - skip, out);
}

}  // namespace peek
