#include "core/phash.h"

#include <algorithm>
#include <numeric>

namespace peek {

uint64_t DHash(const uint8_t* bgra, uint32_t w, uint32_t h, size_t stride) {
    if (!bgra || w == 0 || h == 0) return 0;
    // Flaechenmittel je Zelle (9 Spalten, 8 Zeilen); Helligkeit nach Rec. 601
    double sum[8][9] = {};
    uint32_t cnt[8][9] = {};
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = bgra + size_t(y) * stride;
        const uint32_t cy = static_cast<uint32_t>(uint64_t(y) * 8 / h);
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = row + size_t(x) * 4;
            const uint32_t cx = static_cast<uint32_t>(uint64_t(x) * 9 / w);
            sum[cy][cx] += 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
            ++cnt[cy][cx];
        }
    }
    uint64_t hsh = 0;
    int bit = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x, ++bit) {
            double a = cnt[y][x] ? sum[y][x] / cnt[y][x] : 0;
            double b = cnt[y][x + 1] ? sum[y][x + 1] / cnt[y][x + 1] : 0;
            if (a > b) hsh |= uint64_t(1) << bit;
        }
    }
    return hsh;
}

int HammingDistance(uint64_t a, uint64_t b) {
    uint64_t v = a ^ b;
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}

bool DHashUsable(uint64_t h) {
    int ones = HammingDistance(h, 0);
    return ones > 3 && ones < 61;
}

std::vector<std::pair<uint32_t, uint32_t>> CandidatePairs(const std::vector<uint64_t>& hashes, int maxDist) {
    const size_t n = hashes.size();
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (n < 2) return out;
    maxDist = std::clamp(maxDist, 0, 20);
    const int parts = maxDist + 1;
    std::vector<std::pair<uint64_t, uint32_t>> keys;
    keys.reserve(n);
    for (int part = 0; part < parts; ++part) {
        const int b0 = part * 64 / parts, b1 = (part + 1) * 64 / parts;
        const uint64_t mask = (b1 - b0 >= 64) ? ~uint64_t(0) : (((uint64_t(1) << (b1 - b0)) - 1) << b0);
        keys.clear();
        for (size_t i = 0; i < n; ++i)
            if (DHashUsable(hashes[i])) keys.push_back({hashes[i] & mask, static_cast<uint32_t>(i)});
        std::sort(keys.begin(), keys.end());
        for (size_t a = 0; a < keys.size();) {
            size_t b = a + 1;
            while (b < keys.size() && keys[b].first == keys[a].first) ++b;
            for (size_t i = a; i < b; ++i)
                for (size_t j = i + 1; j < b; ++j) {
                    uint32_t x = keys[i].second, y = keys[j].second;   // nach Index sortiert: x < y
                    if (HammingDistance(hashes[x], hashes[y]) > maxDist) continue;
                    // nur im ersten Abschnitt melden, in dem das Paar uebereinstimmt (sonst mehrfach)
                    bool earlier = false;
                    for (int q = 0; q < part && !earlier; ++q) {
                        const int c0 = q * 64 / parts, c1 = (q + 1) * 64 / parts;
                        const uint64_t m = ((uint64_t(1) << (c1 - c0)) - 1) << c0;
                        earlier = (hashes[x] & m) == (hashes[y] & m);
                    }
                    if (!earlier) out.push_back({x, y});
                }
            a = b;
        }
    }
    return out;
}

void SmallRgb(const uint8_t* bgra, uint32_t w, uint32_t h, size_t stride, uint8_t out[kSmallRgbSize]) {
    double sum[16][16][3] = {};
    uint32_t cnt[16][16] = {};
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = bgra + size_t(y) * stride;
        const uint32_t cy = static_cast<uint32_t>(uint64_t(y) * 16 / h);
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = row + size_t(x) * 4;
            const uint32_t cx = static_cast<uint32_t>(uint64_t(x) * 16 / w);
            for (int c = 0; c < 3; ++c) sum[cy][cx][c] += p[c];
            ++cnt[cy][cx];
        }
    }
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            for (int c = 0; c < 3; ++c)
                out[(y * 16 + x) * 3 + c] = static_cast<uint8_t>(cnt[y][x] ? sum[y][x][c] / cnt[y][x] + 0.5 : 0);
}

bool SmallRgbSame(const uint8_t a[kSmallRgbSize], const uint8_t b[kSmallRgbSize]) {
    int total = 0, maxCell = 0;
    for (size_t i = 0; i < kSmallRgbSize; ++i) {
        int d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        total += d;
        maxCell = std::max(maxCell, d);
    }
    return total <= static_cast<int>(kSmallRgbSize) * 4 && maxCell <= 28;   // im Mittel hoechstens 4 von 255
}

}  // namespace peek
