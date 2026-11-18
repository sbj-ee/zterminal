#include "Contrast.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace zterminal::contrast {

namespace {

// sRGB channel (0..255) to linear light, as a table: luminance() runs for
// cells on every repaint.
const std::array<double, 256> &linearTable()
{
    static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double s = i / 255.0;
            t[i] = s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        }
        return t;
    }();
    return table;
}

} // namespace

double luminance(QRgb c)
{
    const auto &lin = linearTable();
    return 0.2126 * lin[qRed(c)] + 0.7152 * lin[qGreen(c)] + 0.0722 * lin[qBlue(c)];
}

double contrastRatio(QRgb a, QRgb b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QRgb blend(QRgb a, QRgb b, double weightA)
{
    weightA = std::clamp(weightA, 0.0, 1.0);
    const auto mix = [weightA](int x, int y) {
        return std::clamp(int(std::lround(x * weightA + y * (1.0 - weightA))), 0, 255);
    };
    return qRgb(mix(qRed(a), qRed(b)), mix(qGreen(a), qGreen(b)), mix(qBlue(a), qBlue(b)));
}

QRgb ensureContrast(QRgb fg, QRgb bg, double minRatio)
{
    if (contrastRatio(fg, bg) >= minRatio) {
        return fg;
    }
    const QRgb black = qRgb(0, 0, 0);
    const QRgb white = qRgb(255, 255, 255);
    // Move away from the background: toward white on a dark background,
    // toward black on a light one (the side with more contrast to gain).
    const bool towardWhite = contrastRatio(white, bg) >= contrastRatio(black, bg);
    const QRgb target = towardWhite ? white : black;
    if (contrastRatio(target, bg) < minRatio) {
        return target; // mid-grey background: the best that exists
    }
    // Smallest share of `target` that reaches minRatio (contrast grows
    // monotonically with it, since fg starts on bg's side of target).
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 20; ++i) {
        const double mid = (lo + hi) / 2;
        if (contrastRatio(blend(target, fg, mid), bg) >= minRatio) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    QRgb out = blend(target, fg, hi);
    // Rounding to 8 bits can land just under the ratio; step until it holds.
    for (int i = 0; i < 255 && contrastRatio(out, bg) < minRatio; ++i) {
        const auto step = [towardWhite](int v) { return std::clamp(v + (towardWhite ? 1 : -1), 0, 255); };
        out = qRgb(step(qRed(out)), step(qGreen(out)), step(qBlue(out)));
    }
    return out;
}

QRgb dim(QRgb fg, QRgb bg)
{
    return ensureContrast(blend(fg, bg, kDimForegroundWeight), bg, kDimMinContrast);
}

} // namespace zterminal::contrast
