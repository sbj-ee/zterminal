#pragma once

#include <QRgb>

// Colour arithmetic for readable text: WCAG 2 relative luminance and contrast
// ratio, blending, and a minimum-contrast nudge. Used by Terminal::convert()
// for faint (SGR 2) text and for palette colours that would be near-invisible
// on the theme's own background.
namespace zterminal::contrast {

// Faint text is the foreground blended toward the cell background, keeping
// this much of the foreground (0.66 = two thirds foreground, one third background).
inline constexpr double kDimForegroundWeight = 0.66;

// Minimum contrast ratio (WCAG 2, 1..21) faint text keeps against its cell
// background. 3:1 is the WCAG AA level for large text / UI components: clearly
// dimmer than normal text, still readable.
inline constexpr double kDimMinContrast = 3.0;

// A 16/256-palette foreground on the theme's default background below this
// ratio is treated as indistinguishable (e.g. ANSI black #262626 on #000000,
// 1.4:1) and raised to kMinReadableContrast. Ordinary colours are untouched:
// the darkest standard one, PuTTY blue #0000bb on black, is 1.7:1.
inline constexpr double kIndistinguishableContrast = 1.5;
inline constexpr double kMinReadableContrast = 3.0;

// WCAG 2 relative luminance of an sRGB colour, 0 (black) .. 1 (white).
double luminance(QRgb c);
// WCAG 2 contrast ratio between two colours, 1 .. 21 (order doesn't matter).
double contrastRatio(QRgb a, QRgb b);
// Per-channel sRGB mix: `weightA` of a plus (1 - weightA) of b.
QRgb blend(QRgb a, QRgb b, double weightA);
// `fg` unchanged when it already has `minRatio` against `bg`; otherwise fg moved
// toward black or white (whichever side of bg has more room) just far enough
// to reach it, or as far as possible when it can't be reached.
QRgb ensureContrast(QRgb fg, QRgb bg, double minRatio);
// Faint text colour: fg blended toward bg (kDimForegroundWeight), then
// ensureContrast(kDimMinContrast).
QRgb dim(QRgb fg, QRgb bg);

} // namespace zterminal::contrast
