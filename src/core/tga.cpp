#include "core/tga.h"

#include <cstring>

namespace peek {

namespace {

struct Header {
    uint8_t idLength, mapType, type;
    uint16_t mapFirst, mapLength;
    uint8_t mapBits;
    uint16_t width, height;
    uint8_t bits, desc;
};

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool ParseHeader(const uint8_t* d, size_t n, Header& h) {
    if (!d || n < 18) return false;
    h.idLength = d[0];
    h.mapType = d[1];
    h.type = d[2];
    h.mapFirst = Le16(d + 3);
    h.mapLength = Le16(d + 5);
    h.mapBits = d[7];
    h.width = Le16(d + 12);
    h.height = Le16(d + 14);
    h.bits = d[16];
    h.desc = d[17];
    if (!h.width || !h.height || h.mapType > 1 || (h.desc & 0xC0)) return false;   // Interleaving gibt es nicht
    uint8_t base = h.type & ~8u;
    if ((h.type & ~11u) != 0 || base == 0) return false;   // erlaubt: 1, 2, 3, 9, 10, 11
    if (base == 1) {   // Palette
        if (h.mapType != 1 || h.bits != 8 || !h.mapLength) return false;
        if (h.mapBits != 15 && h.mapBits != 16 && h.mapBits != 24 && h.mapBits != 32) return false;
    } else if (base == 2) {   // Echtfarben
        if (h.bits != 15 && h.bits != 16 && h.bits != 24 && h.bits != 32) return false;
    } else {   // Graustufen
        if (h.bits != 8) return false;
    }
    return true;
}

// Ein Pixel (in Dateireihenfolge) nach BGRA (Alpha wird spaeter je nach Kopf verwendet oder ignoriert)
void ToBgra(const uint8_t* p, int bytes, bool gray, uint8_t out[4]) {
    if (gray) {
        out[0] = out[1] = out[2] = p[0];
        out[3] = 255;
    } else if (bytes == 2) {   // ARRRRRGG GGGBBBBB (Little Endian)
        uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
        auto x5 = [](unsigned c) { return static_cast<uint8_t>((c << 3) | (c >> 2)); };
        out[0] = x5(v & 31);
        out[1] = x5((v >> 5) & 31);
        out[2] = x5((v >> 10) & 31);
        out[3] = (v & 0x8000) ? 255 : 0;
    } else {
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = bytes == 4 ? p[3] : 255;
    }
}

}  // namespace

bool TgaReadInfo(const uint8_t* data, size_t size, TgaInfo& info) {
    Header h;
    if (!ParseHeader(data, size, h)) return false;
    info.width = h.width;
    info.height = h.height;
    uint8_t base = h.type & ~8u;
    int bits = base == 1 ? h.mapBits : h.bits;
    info.alpha = (bits == 32 || bits == 16) && (h.desc & 15) != 0;
    return true;
}

bool TgaDecode(const uint8_t* data, size_t size, uint64_t maxPixels, std::vector<uint8_t>& bgra, uint32_t& width,
               uint32_t& height) {
    Header h;
    if (!ParseHeader(data, size, h)) return false;
    const uint64_t count = static_cast<uint64_t>(h.width) * h.height;
    if (count > maxPixels) return false;
    const uint8_t base = h.type & ~8u;
    const bool rle = (h.type & 8) != 0;
    const bool gray = base == 3;
    const int alphaBits = h.desc & 15;

    size_t pos = 18 + static_cast<size_t>(h.idLength);
    // Palette (wird auch bei Echtfarbbildern uebersprungen, falls vorhanden)
    std::vector<uint8_t> map;   // BGRA je Eintrag
    if (h.mapType == 1) {
        int eb = (h.mapBits + 7) / 8;
        size_t mapBytes = static_cast<size_t>(h.mapLength) * eb;
        if (pos > size || size - pos < mapBytes) return false;
        if (base == 1) {
            map.resize(static_cast<size_t>(h.mapLength) * 4);
            for (size_t i = 0; i < h.mapLength; ++i) ToBgra(data + pos + i * eb, eb, false, &map[i * 4]);
        }
        pos += mapBytes;
    }
    const int pb = base == 1 ? 1 : (base == 3 ? 1 : (h.bits + 7) / 8);   // Bytes je Pixel in der Datei

    // Pixel in Dateireihenfolge lesen
    std::vector<uint8_t> px(static_cast<size_t>(count) * 4);
    auto decodeOne = [&](const uint8_t* p, uint8_t* out) -> bool {
        if (base == 1) {
            unsigned idx = p[0];
            if (idx < h.mapFirst || idx - h.mapFirst >= h.mapLength) return false;
            memcpy(out, &map[(idx - h.mapFirst) * 4], 4);
            return true;
        }
        ToBgra(p, pb, gray, out);
        return true;
    };
    uint64_t done = 0;
    if (!rle) {
        if (pos > size || (size - pos) / static_cast<size_t>(pb) < count) return false;
        for (uint64_t i = 0; i < count; ++i)
            if (!decodeOne(data + pos + i * pb, &px[i * 4])) return false;
    } else {
        while (done < count) {
            if (pos >= size) return false;
            uint8_t hdr = data[pos++];
            uint64_t n = (hdr & 0x7F) + 1u;
            if (n > count - done) return false;   // Paket laeuft ueber das Bildende
            if (hdr & 0x80) {   // Wiederholung
                if (size - pos < static_cast<size_t>(pb)) return false;
                uint8_t c[4];
                if (!decodeOne(data + pos, c)) return false;
                pos += pb;
                for (uint64_t i = 0; i < n; ++i) memcpy(&px[(done + i) * 4], c, 4);
            } else {   // Rohdaten
                if ((size - pos) / static_cast<size_t>(pb) < n) return false;
                for (uint64_t i = 0; i < n; ++i)
                    if (!decodeOne(data + pos + i * pb, &px[(done + i) * 4])) return false;
                pos += static_cast<size_t>(n) * pb;
            }
            done += n;
        }
    }

    // Alpha: ohne Angabe im Kopf deckend; ist der Kanal ueberall 0 (haeufig bei 32 Bit), ebenfalls deckend
    int srcBits = base == 1 ? h.mapBits : (gray ? 8 : h.bits);
    bool useAlpha = (srcBits == 32 || srcBits == 16) && alphaBits > 0;
    if (useAlpha) {
        bool any = false;
        for (uint64_t i = 0; i < count && !any; ++i) any = px[i * 4 + 3] != 0;
        if (!any) useAlpha = false;
    }

    // Ausrichtung: Bit 5 = von oben, Bit 4 = von rechts
    const bool top = (h.desc & 0x20) != 0, right = (h.desc & 0x10) != 0;
    width = h.width;
    height = h.height;
    bgra.assign(static_cast<size_t>(count) * 4, 0);
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t sy = top ? y : height - 1 - y;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t sx = right ? width - 1 - x : x;
            const uint8_t* s = &px[(static_cast<size_t>(sy) * width + sx) * 4];
            uint8_t* d = &bgra[(static_cast<size_t>(y) * width + x) * 4];
            uint8_t a = useAlpha ? s[3] : 255;
            d[0] = static_cast<uint8_t>((s[0] * a + 127) / 255);
            d[1] = static_cast<uint8_t>((s[1] * a + 127) / 255);
            d[2] = static_cast<uint8_t>((s[2] * a + 127) / 255);
            d[3] = a;
        }
    }
    return true;
}

} // namespace peek
