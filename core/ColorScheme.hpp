#pragma once

#include <QList>
#include <QRgb>
#include <QString>
#include <array>

namespace sbj::brand {
struct Theme;
}
namespace sbj::theme {
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
    // Custom themes only: the cursor style the theme asks for
    // (VTERM_PROP_CURSORSHAPE_*, blink 0/1); -1 = the app default.
    int cursorShape = -1;
    int cursorBlink = -1;
    // The *.ztheme.json file of a custom theme (empty for built-ins), and
    // whether zterminal may change it (its own themes dir, not zmail's).
    QString path;
    bool editable = false;

    // xterm, PuTTY, Solarized Dark, then the shared brand themes
    // (Boilermakers, Badgers, Packers; see BrandThemes.h / docs/THEMES.md).
    static QList<ColorScheme> builtIn();
    // Terminal scheme for a shared brand theme: its background/foreground,
    // cursor = accent, selection = selection on selectionText, its 16 ANSI colours.
    static ColorScheme fromBrandTheme(const sbj::brand::Theme &t);
    // builtIn(), then this user's themes (userThemesDir(), id "custom:<stem>")
    // and zmail's (zmailThemesDir(), id "zmail:<stem>", read-only). Bad
    // files are skipped. Used by every menu and dropdown.
    static QList<ColorScheme> all();
    // A theme file as a scheme: its terminal block, or ANSI colours derived
    // from the shared roles when it has none (e.g. a zmail theme).
    static ColorScheme fromThemeFile(const sbj::theme::Theme &t, const QString &id);
    // The other way (export / duplicate): a custom scheme's own file, a
    // brand theme's table entry, or roles derived from xterm/PuTTY/Solarized.
    sbj::theme::Theme toThemeFile() const;
    static QString userThemesDir();  // <config>/zterminal/themes
    static QString zmailThemesDir(); // <config>/zmail/themes
    // Any id from all(); falls back to the first built-in scheme for an unknown id.
    static ColorScheme byId(const QString &id);
};

// xterm's 256-colour palette entry n (0..255) using `ansi` for 0..15.
QRgb xterm256(int n, const std::array<QRgb, 16> &ansi);

} // namespace zterminal
