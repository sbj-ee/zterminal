#pragma once

#include <QFile>
#include <QStandardPaths>
#include <QString>

namespace zterminal {

// Config root for vault, sessions, update-state. Honours $XDG_CONFIG_HOME when
// set (tests and Linux); otherwise QStandardPaths::GenericConfigLocation
// (~/.config on Linux, ~/Library/Application Support on macOS).
inline QString configHome()
{
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    if (!xdg.isEmpty()) {
        return QFile::decodeName(xdg);
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}

} // namespace zterminal
