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
    // History lines per tab (0 = none, -1 = unlimited; see docs/PLAN.md §4.16).
    int scrollbackLines = 100000;
    static constexpr int kDefaultScrollbackLines = 100000;
    static constexpr int kUnlimitedScrollback = -1;
    MouseSettings mouse;
    // Stored now; the startup update checker itself lands in a later release.
    bool checkForUpdatesOnStartup = true;
    // Lock the password vault after this many idle minutes (0 = never).
    int vaultAutoLockMinutes = 15;
    static constexpr int kDefaultVaultAutoLockMinutes = 15;
    // Session logs: folder (empty = ~/zterminal-logs) and per-line timestamps.
    QString logDirectory;
    bool logTimestamps = false;
    QString effectiveLogDirectory() const;
    // Safe copy and paste (0.6.0): trim trailing whitespace from copied lines,
    // and confirm pastes that contain a line break.
    bool trimCopiedWhitespace = true;
    bool confirmMultilinePaste = true;

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
