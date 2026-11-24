#include "core/metastrip.h"
#include "core/heif.h"

#include <algorithm>
#include <cstring>

namespace peek {

namespace {

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
void PutLe32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    v[at] = x & 255; v[at + 1] = (x >> 8) & 255; v[at + 2] = (x >> 16) & 255; v[at + 3] = (x >> 24) & 255;
}

bool StartsWith(const uint8_t* p, size_t n, const char* s) {
    size_t l = std::strlen(s);
    return n >= l && std::memcmp(p, s, l) == 0;
}

// EXIF nur mit der Drehung (APP1-Abschnitt ab 0xFFE1)
std::vector<uint8_t> OrientationApp1(int orientation) {
    const uint8_t seg[] = {0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
                           'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08,   // TIFF-Kopf, IFD0 bei 8
                           0x00, 0x01,                                     // ein Eintrag
                           0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, // Orientation, SHORT, 1
                           0x00, static_cast<uint8_t>(orientation), 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00};                        // kein weiteres IFD
    return std::vector<uint8_t>(seg, seg + sizeof(seg));
}

bool StripJpeg(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    // Drehung aus dem EXIF merken
    int orientation = 0;
    {
        size_t off = 0, len = 0;
        ExifInfo ex;
        if (JpegFindExif(d, n, off, len) && ExifParse(d + off, len, ex)) orientation = ex.orientation;
    }
    out.clear();
    out.reserve(n);
    out.push_back(0xFF);
    out.push_back(0xD8);
    bool orientationDone = orientation < 2 || orientation > 8;
    size_t i = 2;
    bool first = true;
    while (i < n) {
        if (d[i] != 0xFF) return false;
        while (i < n && d[i] == 0xFF) ++i;   // Fuellbytes
        if (i >= n) return false;
        const uint8_t m = d[i++];
        if (m == 0xD9) {   // Ende: alles danach (angehaengte Daten, weitere Bilder) entfaellt
            out.push_back(0xFF);
            out.push_back(0xD9);
            return true;
        }
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            out.push_back(0xFF);
            out.push_back(m);
            continue;
        }
        if (i + 2 > n) return false;
        const size_t len = (size_t(d[i]) << 8) | d[i + 1];
        if (len < 2 || i + len > n) return false;
        const uint8_t* body = d + i + 2;
        const size_t bl = len - 2;
        bool keep = true;
        if (m == 0xE0) keep = StartsWith(body, bl, "JFIF") || StartsWith(body, bl, "JFXX");
        else if (m == 0xE2) keep = StartsWith(body, bl, "ICC_PROFILE");
        else if (m == 0xEE) keep = StartsWith(body, bl, "Adobe");   // Farbumrechnung (CMYK/YCCK)
        else if (m >= 0xE1 && m <= 0xEF) keep = false;               // EXIF, XMP, IPTC, MPF, Herstellerdaten
        else if (m == 0xFE) keep = false;                            // Kommentar
        // Drehung nach JFIF (falls vorhanden) bzw. vor dem ersten uebrigen Abschnitt einfuegen
        if (!orientationDone && !(first && m == 0xE0 && keep)) {
            auto app1 = OrientationApp1(orientation);
            out.insert(out.end(), app1.begin(), app1.end());
            orientationDone = true;
        }
        first = false;
        if (keep) {
            out.push_back(0xFF);
            out.push_back(m);
            out.insert(out.end(), d + i, d + i + len);
        }
        i += len;
        if (m == 0xDA) {   // Bilddaten bis zum naechsten Marker unveraendert uebernehmen
            size_t s = i;
            while (i + 1 < n) {
                if (d[i] == 0xFF) {
                    uint8_t x = d[i + 1];
                    if (x != 0x00 && x != 0xFF && !(x >= 0xD0 && x <= 0xD7)) break;
                }
                ++i;
            }
            if (i + 1 >= n) i = n;
            out.insert(out.end(), d + s, d + i);
        }
    }
    return true;   // ohne Endmarker (abgeschnittene Datei): so viel wie vorhanden
}

bool StripPng(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n < 8 || std::memcmp(d, sig, 8) != 0) return false;
    // Zusatzbloecke, die die Darstellung betreffen
    static const char* keep[] = {"iCCP", "sRGB", "gAMA", "cHRM", "sBIT", "bKGD", "tRNS", "hIST", "sPLT",
                                 "cICP", "mDCv", "cLLi", "mDCV", "cLLI", "acTL", "fcTL", "fdAT"};
    out.assign(d, d + 8);
    size_t i = 8;
    bool sawHeader = false;
    while (i + 12 <= n) {
        const uint32_t len = Be32(d + i);
        if (len > 0x7FFFFFFFu || i + 12 + size_t(len) > n) return false;
        const char* type = reinterpret_cast<const char*>(d + i + 4);
        bool critical = (type[0] & 0x20) == 0;
        bool k = critical;
        for (auto* kk : keep) k = k || std::memcmp(type, kk, 4) == 0;
        if (!sawHeader && std::memcmp(type, "IHDR", 4) != 0) return false;
        sawHeader = true;
        if (k) out.insert(out.end(), d + i, d + i + 12 + len);
        i += 12 + size_t(len);
        if (std::memcmp(type, "IEND", 4) == 0) return true;
    }
    return false;   // ohne IEND nicht anfassen
}

bool StripWebP(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WEBP", 4) != 0) return false;
    size_t end = std::min(n, size_t(Le32(d + 4)) + 8);
    out.assign(d, d + 12);
    size_t i = 12;
    size_t vp8x = 0;
    while (i + 8 <= end) {
        const uint32_t len = Le32(d + i + 4);
        const size_t total = 8 + size_t(len) + (len & 1);
        if (i + 8 + size_t(len) > end) return false;
        const char* fcc = reinterpret_cast<const char*>(d + i);
        bool k = false;
        for (const char* kk : {"VP8 ", "VP8L", "VP8X", "ALPH", "ANIM", "ANMF", "ICCP"}) k = k || std::memcmp(fcc, kk, 4) == 0;
        if (k) {
            if (std::memcmp(fcc, "VP8X", 4) == 0) vp8x = out.size();
            out.insert(out.end(), d + i, d + std::min(end, i + total));
            if (total > 8 + size_t(len) && i + total > end) out.push_back(0);   // fehlendes Fuellbyte
        }
        i += total;
    }
    if (vp8x && vp8x + 8 < out.size()) out[vp8x + 8] &= static_cast<uint8_t>(~(0x04 | 0x08));   // keine EXIF/XMP-Bloecke mehr
    PutLe32(out, 4, static_cast<uint32_t>(out.size() - 8));
    return out.size() > 12;
}

}  // namespace

StripFormat DetectStripFormat(const uint8_t* d, size_t n) {
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return StripFormat::Jpeg;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return StripFormat::Png;
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return StripFormat::WebP;
    return StripFormat::None;
}

bool StripSupportedExt(const std::wstring& ext) {
    return ext == L"jpg" || ext == L"jpeg" || ext == L"jpe" || ext == L"jfif" || ext == L"png" || ext == L"webp";
}

bool StripMetadata(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    bool ok = false;
    switch (DetectStripFormat(data, size)) {
    case StripFormat::Jpeg: ok = StripJpeg(data, size, out); break;
    case StripFormat::Png: ok = StripPng(data, size, out); break;
    case StripFormat::WebP: ok = StripWebP(data, size, out); break;
    default: break;
    }
    if (!ok) out.clear();
    return ok;
}

} // namespace peek
