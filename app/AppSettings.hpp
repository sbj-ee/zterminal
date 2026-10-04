#pragma once

#include "MouseSettings.hpp"

#include <QFont>
#include <QString>

namespace zterminal {

// Global preferences, stored with QSettings in ~/.config/zterminal/zterminal.ini
// (main() selects IniFormat + organization/application "zterminal").
struct AppSettings {
    QString fontFamily;
    int fontSize = 11;
    QString colorScheme = QStringLiteral("xterm");
    int scrollbackLines = 10000;
    bool menuBarVisible = true;
    MouseSettings mouse;

    static constexpr int kDefaultFontSize = 11;
    static QString defaultFontFamily();
    QFont font() const;

    static AppSettings load();
    void save() const;
};

} // namespace zterminal
