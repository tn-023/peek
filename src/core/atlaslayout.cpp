#include "core/atlaslayout.h"

#include <algorithm>
#include <cmath>

namespace peek {

int AtlasRows(int n, int cols) {
    if (n <= 0) return 1;
    cols = std::clamp(cols, 1, n);
    return (n + cols - 1) / cols;
}

int AtlasAutoCols(int n, uint32_t w, uint32_t h) {
    if (n <= 1 || w == 0 || h == 0) return 1;
    int best = 1;
    double bestScore = 1e300;
    int bestEmpty = n;
    for (int c = 1; c <= n; ++c) {
        int r = AtlasRows(n, c);
        if (c > 1 && AtlasRows(n, c - 1) == r) continue;   // gleiche Zeilenzahl mit weniger Spalten ist kompakter
        double score = std::fabs(std::log(static_cast<double>(c) * w / (static_cast<double>(r) * h)));
        int empty = c * r - n;
        if (score < bestScore - 1e-9 || (std::fabs(score - bestScore) <= 1e-9 && empty <= bestEmpty)) {
            best = c;
            bestScore = score;
            bestEmpty = empty;
        }
    }
    return best;
}

int AtlasColsForMoreRows(int n, int cols) {
    int r = AtlasRows(n, cols);
    while (cols > 1 && AtlasRows(n, cols) <= r) --cols;
    return cols;
}

int AtlasColsForFewerRows(int n, int cols) {
    int r = AtlasRows(n, cols);
    while (cols < n && AtlasRows(n, cols) >= r) ++cols;
    return std::max(1, std::min(cols, std::max(1, n)));
}

} // namespace peek
