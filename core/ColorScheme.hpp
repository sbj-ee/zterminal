#pragma once

#include <QList>
#include <QRgb>
#include <QString>
#include <array>

namespace sbj::brand {
struct Theme;
}

namespace zterminal {

// The 16 ANSI colours plus default foreground/background/cursor/selection.
// Indices 16..255 are fixed by xterm (6x6x6 cube + 24 greys); libvterm computes them.
struct ColorScheme {
    QString id;    // stable key stored in settings, e.g. "xterm"
    QString name;  // shown in View > Color Scheme
    std::array<QRgb, 16> ansi{};
    QRgb foreground = 0;
    QRgb background = 0;
    QRgb cursor = 0;
    QRgb selectionBackground = 0;
    QRgb selectionForeground = 0;

    // xterm, PuTTY, Solarized Dark, then the shared brand themes
    // (Boilermakers, Badgers, Packers; see BrandThemes.h / docs/THEMES.md).
    static QList<ColorScheme> builtIn();
    // Terminal scheme for a shared brand theme: its background/foreground,
    // cursor = accent, selection = selection on selectionText, its 16 ANSI colours.
    static ColorScheme fromBrandTheme(const sbj::brand::Theme &t);
    // Falls back to the first built-in scheme for an unknown id.
    static ColorScheme byId(const QString &id);
};

// xterm's 256-colour palette entry n (0..255) using `ansi` for 0..15.
QRgb xterm256(int n, const std::array<QRgb, 16> &ansi);

} // namespace zterminal
