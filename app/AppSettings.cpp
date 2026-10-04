#include "AppSettings.hpp"

#include <QFontDatabase>
#include <QSettings>

namespace zterminal {

QString AppSettings::defaultFontFamily()
{
    const QStringList families = QFontDatabase::families();
    for (const char *f : {"DejaVu Sans Mono", "Ubuntu Mono", "Liberation Mono", "Noto Mono"}) {
        if (families.contains(QString::fromLatin1(f))) {
            return QString::fromLatin1(f);
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

QFont AppSettings::font() const
{
    QFont f(fontFamily.isEmpty() ? defaultFontFamily() : fontFamily);
    f.setPointSize(fontSize > 0 ? fontSize : kDefaultFontSize);
    f.setStyleHint(QFont::TypeWriter);
    f.setFixedPitch(true);
    f.setKerning(false);
    return f;
}

AppSettings AppSettings::load()
{
    QSettings s;
    AppSettings a;
    a.fontFamily = s.value(QStringLiteral("appearance/fontFamily"), defaultFontFamily()).toString();
    a.fontSize = s.value(QStringLiteral("appearance/fontSize"), kDefaultFontSize).toInt();
    a.colorScheme = s.value(QStringLiteral("appearance/colorScheme"), a.colorScheme).toString();
    a.scrollbackLines = s.value(QStringLiteral("terminal/scrollbackLines"), a.scrollbackLines).toInt();
    a.menuBarVisible = s.value(QStringLiteral("view/menuBarVisible"), true).toBool();
    a.mouse.load(s);
    return a;
}

void AppSettings::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("appearance/fontFamily"), fontFamily);
    s.setValue(QStringLiteral("appearance/fontSize"), fontSize);
    s.setValue(QStringLiteral("appearance/colorScheme"), colorScheme);
    s.setValue(QStringLiteral("terminal/scrollbackLines"), scrollbackLines);
    s.setValue(QStringLiteral("view/menuBarVisible"), menuBarVisible);
    mouse.save(s);
}

} // namespace zterminal
