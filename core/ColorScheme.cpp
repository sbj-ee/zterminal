#include "ColorScheme.hpp"

#include "BrandThemes.h"
#include "ConfigPaths.hpp"
#include "ThemeFile.h"

#include <vterm.h>

namespace zterminal {

namespace {
QRgb rgb(std::uint32_t v)
{
    return qRgb(int((v >> 16) & 0xff), int((v >> 8) & 0xff), int(v & 0xff));
}
} // namespace

ColorScheme ColorScheme::fromBrandTheme(const sbj::brand::Theme &t)
{
    ColorScheme s;
    s.id = QString::fromLatin1(t.id.data(), qsizetype(t.id.size()));
    s.name = QString::fromLatin1(t.name.data(), qsizetype(t.name.size()));
    for (size_t i = 0; i < s.ansi.size(); ++i) {
        s.ansi[i] = rgb(t.ansi[i]);
    }
    s.foreground = rgb(t.foreground);
    s.background = rgb(t.background);
    s.cursor = rgb(t.accent);
    s.selectionBackground = rgb(t.selection);
    s.selectionForeground = rgb(t.selectionText);
    return s;
}

QList<ColorScheme> ColorScheme::builtIn()
{
    QList<ColorScheme> list;

    ColorScheme xterm;
    xterm.id = QStringLiteral("xterm");
    xterm.name = QStringLiteral("xterm (default)");
    xterm.ansi = {qRgb(0, 0, 0),       qRgb(205, 0, 0),     qRgb(0, 205, 0),   qRgb(205, 205, 0),
                  qRgb(0, 0, 238),     qRgb(205, 0, 205),   qRgb(0, 205, 205), qRgb(229, 229, 229),
                  qRgb(127, 127, 127), qRgb(255, 0, 0),     qRgb(0, 255, 0),   qRgb(255, 255, 0),
                  qRgb(92, 92, 255),   qRgb(255, 0, 255),   qRgb(0, 255, 255), qRgb(255, 255, 255)};
    xterm.foreground = qRgb(229, 229, 229);
    xterm.background = qRgb(0, 0, 0);
    xterm.cursor = qRgb(229, 229, 229);
    xterm.selectionBackground = qRgb(176, 196, 222);
    xterm.selectionForeground = qRgb(0, 0, 0);
    list << xterm;

    ColorScheme putty;
    putty.id = QStringLiteral("putty");
    putty.name = QStringLiteral("PuTTY");
    putty.ansi = {qRgb(0, 0, 0),    qRgb(187, 0, 0),   qRgb(0, 187, 0),   qRgb(187, 187, 0),
                  qRgb(0, 0, 187),  qRgb(187, 0, 187), qRgb(0, 187, 187), qRgb(187, 187, 187),
                  qRgb(85, 85, 85), qRgb(255, 85, 85), qRgb(85, 255, 85), qRgb(255, 255, 85),
                  qRgb(85, 85, 255), qRgb(255, 85, 255), qRgb(85, 255, 255), qRgb(255, 255, 255)};
    putty.foreground = qRgb(187, 187, 187);
    putty.background = qRgb(0, 0, 0);
    putty.cursor = qRgb(0, 255, 0);
    putty.selectionBackground = qRgb(255, 255, 255);
    putty.selectionForeground = qRgb(0, 0, 0);
    list << putty;

    ColorScheme sol;
    sol.id = QStringLiteral("solarized-dark");
    sol.name = QStringLiteral("Solarized Dark");
    sol.ansi = {qRgb(7, 54, 66),    qRgb(220, 50, 47),   qRgb(133, 153, 0),   qRgb(181, 137, 0),
                qRgb(38, 139, 210), qRgb(211, 54, 130),  qRgb(42, 161, 152),  qRgb(238, 232, 213),
                qRgb(0, 43, 54),    qRgb(203, 75, 22),   qRgb(88, 110, 117),  qRgb(101, 123, 131),
                qRgb(131, 148, 150), qRgb(108, 113, 196), qRgb(147, 161, 161), qRgb(253, 246, 227)};
    sol.foreground = qRgb(131, 148, 150);
    sol.background = qRgb(0, 43, 54);
    sol.cursor = qRgb(147, 161, 161);
    sol.selectionBackground = qRgb(147, 161, 161);
    sol.selectionForeground = qRgb(0, 43, 54);
    list << sol;

    // Boilermakers, Badgers, Packers: the brand themes shared with zmail
    // (core/BrandThemes.h, docs/THEMES.md).
    for (const sbj::brand::Theme &t : sbj::brand::kThemes) {
        list << fromBrandTheme(t);
    }

    return list;
}

QString ColorScheme::userThemesDir()
{
    return sbj::theme::themesDir(configHome(), QStringLiteral("zterminal"));
}

QString ColorScheme::zmailThemesDir()
{
    return sbj::theme::themesDir(configHome(), QStringLiteral("zmail"));
}

ColorScheme ColorScheme::fromThemeFile(const sbj::theme::Theme &t, const QString &id)
{
    using namespace sbj::theme;
    const Terminal term = terminalOf(t);
    ColorScheme s;
    s.id = id;
    s.name = t.name;
    for (size_t i = 0; i < s.ansi.size(); ++i) {
        s.ansi[i] = rgb(term.ansi[i]);
    }
    s.foreground = rgb(t.roles[Foreground]);
    s.background = rgb(t.roles[Background]);
    s.cursor = rgb(term.cursor);
    s.selectionBackground = rgb(term.selection);
    s.selectionForeground = rgb(term.selectionText);
    s.cursorShape = term.cursorShape == QLatin1String("block")       ? VTERM_PROP_CURSORSHAPE_BLOCK
                    : term.cursorShape == QLatin1String("underline") ? VTERM_PROP_CURSORSHAPE_UNDERLINE
                    : term.cursorShape == QLatin1String("bar")       ? VTERM_PROP_CURSORSHAPE_BAR_LEFT
                                                                     : -1;
    s.cursorBlink = term.cursorBlink ? int(*term.cursorBlink) : -1;
    return s;
}

sbj::theme::Theme ColorScheme::toThemeFile() const
{
    using namespace sbj::theme;
    if (!path.isEmpty()) {
        if (std::optional<Theme> t = load(path)) {
            return *t;
        }
    }
    if (const sbj::brand::Theme *b = sbj::brand::findTheme(id.toStdString())) {
        return fromBrand(*b);
    }
    auto u = [](QRgb c) { return std::uint32_t(c & 0xFFFFFF); };
    Theme t;
    t.name = name;
    t.basedOn = id;
    std::array<bool, RoleCount> have{};
    for (auto [role, c] : {std::pair{Background, background}, std::pair{Foreground, foreground},
                           std::pair{Accent, cursor}, std::pair{Selection, selectionBackground},
                           std::pair{SelectionText, selectionForeground}}) {
        t.roles[size_t(role)] = u(c);
        have[size_t(role)] = true;
    }
    t.roles = deriveRoles(t.roles, have);
    Terminal term;
    for (size_t i = 0; i < ansi.size(); ++i) {
        term.ansi[i] = u(ansi[i]);
    }
    term.cursor = u(cursor);
    term.selection = u(selectionBackground);
    term.selectionText = u(selectionForeground);
    t.terminal = term;
    return t;
}

QList<ColorScheme> ColorScheme::all()
{
    QList<ColorScheme> list = builtIn();
    for (const sbj::theme::Entry &e : sbj::theme::scan(userThemesDir())) {
        ColorScheme s = fromThemeFile(e.theme, QStringLiteral("custom:") + e.stem);
        s.path = e.path;
        s.editable = true;
        list << s;
    }
    for (const sbj::theme::Entry &e : sbj::theme::scan(zmailThemesDir())) {
        ColorScheme s = fromThemeFile(e.theme, QStringLiteral("zmail:") + e.stem);
        s.name += QStringLiteral(" (zmail)");
        s.path = e.path;
        list << s;
    }
    return list;
}

ColorScheme ColorScheme::byId(const QString &id)
{
    if (id.startsWith(QLatin1String("custom:")) || id.startsWith(QLatin1String("zmail:"))) {
        const bool own = id.startsWith(QLatin1String("custom:"));
        const QString path = (own ? userThemesDir() : zmailThemesDir()) + QLatin1Char('/') +
                             id.section(QLatin1Char(':'), 1) + QLatin1String(sbj::theme::kSuffix);
        if (std::optional<sbj::theme::Theme> t = sbj::theme::load(path)) {
            ColorScheme s = fromThemeFile(*t, id);
            if (!own) {
                s.name += QStringLiteral(" (zmail)");
            }
            s.path = path;
            s.editable = own;
            return s;
        }
    }
    const QList<ColorScheme> all = builtIn();
    for (const ColorScheme &s : all) {
        if (s.id == id) {
            return s;
        }
    }
    return all.front();
}

QRgb xterm256(int n, const std::array<QRgb, 16> &ansi)
{
    if (n < 0 || n > 255) {
        return ansi[7];
    }
    if (n < 16) {
        return ansi[static_cast<size_t>(n)];
    }
    if (n < 232) {
        static constexpr int steps[6] = {0, 95, 135, 175, 215, 255};
        const int i = n - 16;
        return qRgb(steps[i / 36], steps[(i / 6) % 6], steps[i % 6]);
    }
    const int g = 8 + (n - 232) * 10;
    return qRgb(g, g, g);
}

} // namespace zterminal
