#pragma once

#include <QString>
#include <QStringList>

namespace zterminal {

// What the user asked zterminal (or the zt wrapper) to open.
struct LaunchRequest {
    enum class Kind { LocalShell, SavedSession, Ssh, Serial, Command, Version, Help, ListSessions, CheckSession, Error };
    Kind kind = Kind::LocalShell;
    QString sessionName; // SavedSession / CheckSession: the name
    QString program;     // Ssh/Command: executable; Serial: the device
    int baudRate = 9600; // Serial
    QStringList args;    // Ssh/Command: argv[1..]
    QString error;       // Error: message for stderr

    // Display name for the window title.
    QString displayName() const;
};

// Parses arguments after argv[0]:
//   (none)                 local shell
//   <name>                 saved session
//   ssh [ssh-args...] host  ad-hoc ssh via the system ssh (all args passed through)
//   serial <device> [baud] ad-hoc serial console (default 9600 8N1)
//   -e|--command prog [args...]
//   -v|--version, -h|--help
//   --list-sessions        print saved session names, one per line
//   --check-session <name> exit 0 if it exists, else print an error (used by zt)
LaunchRequest parseCommandLine(const QStringList &args);

QString usageText();

} // namespace zterminal
