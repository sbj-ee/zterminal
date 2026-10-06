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
    // Keepalive (docs/PLAN.md §4.18): -o ServerAliveInterval / ServerAliveCountMax.
    // ssh gives up after interval x count seconds without an answer (default 30 s x 3).
    // 0 = no keepalive options (ssh's own default: off).
    int keepaliveInterval = 30;
    int keepaliveCountMax = 3;

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

    bool autoLog = false; // start Session > Logging when the session opens
    // After a drop (SSH network error), reconnect by itself with backoff
    // 2, 4, 8 ... 60 s. Off by default; serial ports always reopen when the
    // device comes back.
    bool autoReconnect = false;

    // Optional per-session appearance overrides (empty / 0: use Preferences).
    QString fontFamily;
    int fontSize = 0;
    QString colorScheme;

    // False for a session that arrived through Import Sessions and hasn't been
    // reviewed yet: it won't launch until the user approves it (the import
    // review dialog, or Save in the Sessions dialog). Sessions created in the
    // app are approved.
    bool approved = true;

    static QString typeToString(Type t);
    static Type typeFromString(const QString &s); // unknown -> LocalShell

    bool operator==(const SessionConfig &o) const;
    bool operator!=(const SessionConfig &o) const { return !(*this == o); }
};

// The target an ssh session really connects to: the user/host/port fields,
// overridden by -l / -p / -o User= / -o Port= / -o HostName= in the extra
// arguments (ssh uses the first value it sees and the extra arguments come
// first). `user` may be empty (ssh's default).
struct SshTarget {
    QString user;
    QString host;
    int port = 22;
    QString toString() const; // [user@]host:port
};
SshTarget effectiveSshTarget(const SessionConfig &s);

// What a stored vault secret is bound to (Vault entry "binding/<secret key>"):
// "ssh:[user@]host:port" (effective target) or "serial:<device>". A stored
// password is only ever used for a session whose binding matches.
QString vaultBindingFor(const SessionConfig &s);

// Extra checks for a session that came from an import file (untrusted input).
// Empty when acceptable, else the reason it is refused. Refuses extra ssh
// options that run local commands, load config files or libraries, defeat
// host-key checking, forward credentials, or point the session somewhere other
// than its Host field; see the list in Session.cpp.
QString validateImportedSession(const SessionConfig &s);

// Human-readable list of the settings an import review should show: anything
// that isn't a plain "ssh user@host" (extra arguments, jump host, key file,
// port, keepalive, auto-reconnect, auto-log, serial line settings...).
QStringList notableSessionSettings(const SessionConfig &s);

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

// ssh [extra options] [-o ServerAliveInterval=N -o ServerAliveCountMax=M]
//     [-p port] [-l user] [-i key] [-J jump] -- host
// (ssh takes the first value of an option, so -o in the extra options wins.)
// No shell is involved and every user-supplied value is either validated or
// passed as the separate value of an option, so nothing can become an option.
SshCommand buildSshCommand(const SessionConfig &s);

// Best-effort reverse of the above for "Save Session" on an ad-hoc `zt ssh ...`
// window. Returns nullopt (with *why set) when the arguments can't be
// represented, e.g. a remote command after the destination.
std::optional<SessionConfig> sessionFromSshArgs(const QStringList &args, QString *why = nullptr);

} // namespace zterminal
