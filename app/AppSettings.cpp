#include "AppSettings.hpp"

#include "SessionLog.hpp"

#include <QDir>

#include <algorithm>

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
    const QSettings s;
    return load(s);
}

void AppSettings::save() const
{
    QSettings s;
    save(s);
    s.sync();
}

QString AppSettings::effectiveLogDirectory() const
{
    QString d = logDirectory.trimmed();
    if (d.isEmpty()) {
        return SessionLog::defaultDirectory();
    }
    if (d == QLatin1String("~") || d.startsWith(QLatin1String("~/"))) {
        d = QDir::homePath() + d.mid(1);
    }
    return d;
}

QString AppSettings::filePath()
{
    return QSettings().fileName();
}

bool AppSettings::operator==(const AppSettings &o) const
{
    return fontFamily == o.fontFamily && fontSize == o.fontSize && colorScheme == o.colorScheme
        && scrollbackLines == o.scrollbackLines && checkForUpdatesOnStartup == o.checkForUpdatesOnStartup
        && vaultAutoLockMinutes == o.vaultAutoLockMinutes
        && logDirectory == o.logDirectory && logTimestamps == o.logTimestamps
        && mouse.middleClick == o.mouse.middleClick
        && mouse.copyOnSelectToClipboard == o.mouse.copyOnSelectToClipboard
        && mouse.wordDelimiters == o.mouse.wordDelimiters;
}

AppSettings AppSettings::load(const QSettings &s)
{
    AppSettings a;
    a.fontFamily = s.value(QStringLiteral("appearance/fontFamily"), defaultFontFamily()).toString();
    a.fontSize = s.value(QStringLiteral("appearance/fontSize"), kDefaultFontSize).toInt();
    a.colorScheme = s.value(QStringLiteral("appearance/colorScheme"), a.colorScheme).toString();
    a.scrollbackLines = s.value(QStringLiteral("terminal/scrollbackLines"), a.scrollbackLines).toInt();
    a.vaultAutoLockMinutes = std::clamp(
        s.value(QStringLiteral("vault/autoLockMinutes"), a.vaultAutoLockMinutes).toInt(), 0, 24 * 60);
    a.logDirectory = s.value(QStringLiteral("logging/directory")).toString();
    a.logTimestamps = s.value(QStringLiteral("logging/timestamps"), a.logTimestamps).toBool();
    a.checkForUpdatesOnStartup =
        s.value(QStringLiteral("updates/checkOnStartup"), a.checkForUpdatesOnStartup).toBool();
    a.mouse.load(s);
    return a;
}

void AppSettings::save(QSettings &s) const
{
    s.setValue(QStringLiteral("appearance/fontFamily"), fontFamily);
    s.setValue(QStringLiteral("appearance/fontSize"), fontSize);
    s.setValue(QStringLiteral("appearance/colorScheme"), colorScheme);
    s.setValue(QStringLiteral("terminal/scrollbackLines"), scrollbackLines);
    s.setValue(QStringLiteral("vault/autoLockMinutes"), vaultAutoLockMinutes);
    s.setValue(QStringLiteral("logging/directory"), logDirectory);
    s.setValue(QStringLiteral("logging/timestamps"), logTimestamps);
    s.setValue(QStringLiteral("updates/checkOnStartup"), checkForUpdatesOnStartup);
    // Menu-bar visibility is deliberately NOT persisted (see MainWindow::setMenuBarShown);
    // drop the key 0.1.0 wrote so an old "hidden" value can never come back.
    s.remove(QStringLiteral("view/menuBarVisible"));
    mouse.save(s);
}

} // namespace zterminal
