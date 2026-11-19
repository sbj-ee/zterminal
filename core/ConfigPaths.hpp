#pragma once

#include <QFile>
#include <QStandardPaths>
#include <QString>

namespace zterminal {

// Config root for vault, sessions, groups, update-state and themes. Honours
// $XDG_CONFIG_HOME when set (any platform; the tests set it); otherwise
// QStandardPaths::GenericConfigLocation: ~/.config on Linux,
// ~/Library/Preferences on macOS. Note that zterminal.ini (QSettings,
// IniFormat) does not use this root: Qt puts it in $XDG_CONFIG_HOME or
// ~/.config on macOS too, so on a Mac it is ~/.config/zterminal/zterminal.ini
// while everything else is under ~/Library/Preferences/zterminal/.
inline QString configHome()
{
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    if (!xdg.isEmpty()) {
        return QFile::decodeName(xdg);
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}

} // namespace zterminal
