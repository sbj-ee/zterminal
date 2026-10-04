#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace zterminal {

// A saved session (docs/PLAN.md §4.7). The INI file never holds a password.
// Optionally a password lives in the encrypted vault (core/Vault), keyed by
// session name; the INI only records *whether* to use it.
struct SessionConfig {
    enum class Type { LocalShell, Ssh, Serial };

    QString name;
    Type type = Type::LocalShell;

    // SSH (run through the system `ssh`, argv built by buildSshCommand()).
    QString host;
    QString user;      // empty: ssh's default (~/.ssh/config or the login name)
    int port = 22;
    QString keyFile;   // -i; a leading "~/" is expanded (no shell involved)
    QString jumpHost;  // -J; [user@]host[:port][,...]
    QString extraArgs; // further ssh *options*, split like a command line, never run by a shell
    bool useStoredPassword = false; // answer ssh's first password prompt from the vault

    // Serial (QSerialPort). Defaults: 9600 8N1, no flow control (Cisco console).
    QString serialDevice;            // e.g. /dev/ttyUSB0, /dev/ttyACM0
    int baudRate = 9600;
    int dataBits = 8;                // 5..8
    QString parity = QStringLiteral("none");     // none|even|odd|mark|space
    int stopBits = 1;                // 1 or 2
    QString flowControl = QStringLiteral("none"); // none|rtscts|xonxoff
    bool localEcho = false;
    QString enterSends = QStringLiteral("cr");    // cr|crlf|lf
    int charDelayMs = 0;             // paste pacing: after every character
    int lineDelayMs = 0;             // paste pacing: after every line end
    int breakMs = 300;               // Session > Send Break duration
    QString loginUser;               // Session > Send Stored Login (password in the vault)

    // Optional per-session appearance overrides (empty / 0: use Preferences).
    QString fontFamily;
    int fontSize = 0;
    QString colorScheme;

    static QString typeToString(Type t);
    static Type typeFromString(const QString &s); // unknown -> LocalShell

    bool operator==(const SessionConfig &o) const;
    bool operator!=(const SessionConfig &o) const { return !(*this == o); }
};

// Empty string when valid, otherwise a user-facing reason. A name must be
// usable as `zt <name>`: non-empty, no control characters, no leading '-',
// no surrounding whitespace, not "ssh".
QString validateSessionName(const QString &name);

// Per-field validation used to block ssh option injection.
QString validateSshHost(const QString &host);
QString validateSshUser(const QString &user);
QString validateJumpHost(const QString &jump);

// Empty when the serial fields are usable, else a user-facing reason.
QString validateSerial(const SessionConfig &s);
// The bytes the Enter key / a pasted line end becomes ("\r", "\r\n", "\n").
QByteArray enterSequence(const QString &enterSends);

struct SshCommand {
    QString program;  // "ssh"
    QStringList args; // argv[1..]; always ends with "--", host
    QString error;    // non-empty: refuse to run
    bool ok() const { return error.isEmpty(); }
};

// ssh [extra options] [-p port] [-l user] [-i key] [-J jump] -- host
// No shell is involved and every user-supplied value is either validated or
// passed as the separate value of an option, so nothing can become an option.
SshCommand buildSshCommand(const SessionConfig &s);

// Best-effort reverse of the above for "Save Session" on an ad-hoc `zt ssh ...`
// window. Returns nullopt (with *why set) when the arguments can't be
// represented, e.g. a remote command after the destination.
std::optional<SessionConfig> sessionFromSshArgs(const QStringList &args, QString *why = nullptr);

} // namespace zterminal
