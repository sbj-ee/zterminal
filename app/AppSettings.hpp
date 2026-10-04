#pragma once

#include "MouseSettings.hpp"

#include <QFont>
#include <QString>

class QSettings;

namespace zterminal {

// Global preferences, stored with QSettings in ~/.config/zterminal/zterminal.ini
// (main() selects IniFormat + organization/application "zterminal").
struct AppSettings {
    QString fontFamily;
    int fontSize = 11;
    QString colorScheme = QStringLiteral("xterm");
    int scrollbackLines = 10000;
    MouseSettings mouse;
    // Stored now; the startup update checker itself lands in a later release.
    bool checkForUpdatesOnStartup = true;

    static constexpr int kDefaultFontSize = 11;
    static QString defaultFontFamily();
    QFont font() const;

    // The no-argument forms use the application's default QSettings
    // (~/.config/zterminal/zterminal.ini); the others read/write any store.
    static AppSettings load();
    static AppSettings load(const QSettings &s);
    void save() const;
    void save(QSettings &s) const;
    // Path of the default settings file (watched so open windows pick up changes).
    static QString filePath();

    bool operator==(const AppSettings &o) const;
    bool operator!=(const AppSettings &o) const { return !(*this == o); }
};

} // namespace zterminal
