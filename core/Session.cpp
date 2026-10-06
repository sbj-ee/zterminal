#include "Session.hpp"

#include <QDir>
#include <QProcess>
#include <QRegularExpression>

namespace zterminal {

namespace {

// ssh(1) options that take a value as the next argument.
const QString kSshOptsWithValue = QStringLiteral("BbcDEeFIiJLlmOopQRSWw");

bool hasControlChars(const QString &s)
{
    for (const QChar c : s) {
        if (c.category() == QChar::Other_Control || c == QChar::LineSeparator || c == QChar::ParagraphSeparator) {
            return true;
        }
    }
    return false;
}

struct SshOpt {
    QChar letter;
    QString value; // empty for flags
};

// getopt-style walk over the extra arguments (same rules as buildSshCommand):
// in a cluster such as "-4vo", the first letter that takes a value swallows
// the rest of the cluster, or the next argument if it is the last letter.
// Returns false for anything that isn't an option.
bool parseSshOptions(const QStringList &extra, QList<SshOpt> *out)
{
    for (qsizetype i = 0; i < extra.size(); ++i) {
        const QString &a = extra.at(i);
        if (!a.startsWith(QLatin1Char('-')) || a.size() < 2 || a.startsWith(QLatin1String("--"))) {
            return false;
        }
        for (qsizetype j = 1; j < a.size(); ++j) {
            const QChar c = a.at(j);
            if (kSshOptsWithValue.contains(c)) {
                QString v;
                if (j == a.size() - 1) {
                    if (i + 1 >= extra.size()) {
                        return false;
                    }
                    v = extra.at(++i);
                } else {
                    v = a.mid(j + 1);
                }
                out->append({c, v});
                break;
            }
            out->append({c, QString()});
        }
    }
    return true;
}

// "-o Key=Value" / "-o Key Value" / "-oKey = Value": ssh splits the keyword
// at the first whitespace or '=' and matches it case-insensitively.
std::pair<QString, QString> splitSshConfigOption(const QString &opt)
{
    const QString t = opt.trimmed();
    qsizetype k = 0;
    while (k < t.size() && !t.at(k).isSpace() && t.at(k) != QLatin1Char('=')) {
        ++k;
    }
    const QString key = t.left(k).toLower();
    QString rest = t.mid(k).trimmed();
    if (rest.startsWith(QLatin1Char('='))) {
        rest = rest.mid(1).trimmed();
    }
    if (rest.size() >= 2 && rest.startsWith(QLatin1Char('"')) && rest.endsWith(QLatin1Char('"'))) {
        rest = rest.mid(1, rest.size() - 2);
    }
    return {key, rest};
}

QString expandTilde(const QString &path)
{
    if (path == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (path.startsWith(QLatin1String("~/"))) {
        return QDir::homePath() + path.mid(1);
    }
    return path;
}

} // namespace

QString SessionConfig::typeToString(Type t)
{
    switch (t) {
    case Type::Ssh:
        return QStringLiteral("ssh");
    case Type::Serial:
        return QStringLiteral("serial");
    case Type::LocalShell:
        break;
    }
    return QStringLiteral("local");
}

SessionConfig::Type SessionConfig::typeFromString(const QString &s)
{
    if (s == QLatin1String("ssh")) {
        return Type::Ssh;
    }
    if (s == QLatin1String("serial")) {
        return Type::Serial;
    }
    return Type::LocalShell;
}

bool SessionConfig::operator==(const SessionConfig &o) const
{
    return name == o.name && type == o.type && host == o.host && user == o.user && port == o.port
        && keyFile == o.keyFile && jumpHost == o.jumpHost && extraArgs == o.extraArgs
        && serialDevice == o.serialDevice && baudRate == o.baudRate && dataBits == o.dataBits
        && parity == o.parity && stopBits == o.stopBits && flowControl == o.flowControl
        && localEcho == o.localEcho && enterSends == o.enterSends && charDelayMs == o.charDelayMs
        && lineDelayMs == o.lineDelayMs && breakMs == o.breakMs
        && useStoredPassword == o.useStoredPassword && loginUser == o.loginUser && autoLog == o.autoLog
        && keepaliveInterval == o.keepaliveInterval && keepaliveCountMax == o.keepaliveCountMax
        && autoReconnect == o.autoReconnect && approved == o.approved
        && fontFamily == o.fontFamily && fontSize == o.fontSize && colorScheme == o.colorScheme;
}

QString SshTarget::toString() const
{
    QString h = host;
    if (h.contains(QLatin1Char(':')) && !h.startsWith(QLatin1Char('['))) {
        h = QLatin1Char('[') + h + QLatin1Char(']'); // IPv6
    }
    return (user.isEmpty() ? QString() : user + QLatin1Char('@')) + h + QLatin1Char(':') + QString::number(port);
}

SshTarget effectiveSshTarget(const SessionConfig &s)
{
    SshTarget t;
    t.user = s.user;
    t.host = s.host;
    t.port = s.port;
    QList<SshOpt> opts;
    if (!parseSshOptions(QProcess::splitCommand(s.extraArgs), &opts)) {
        return t; // buildSshCommand refuses these anyway
    }
    // First value wins (ssh semantics); the extra arguments precede the fields.
    bool haveUser = false;
    bool havePort = false;
    bool haveHost = false;
    for (const SshOpt &o : opts) {
        if (o.letter == QLatin1Char('l') && !haveUser) {
            t.user = o.value;
            haveUser = true;
        } else if (o.letter == QLatin1Char('p') && !havePort) {
            t.port = o.value.toInt();
            havePort = true;
        } else if (o.letter == QLatin1Char('o')) {
            const auto [key, val] = splitSshConfigOption(o.value);
            if (key == QLatin1String("user") && !haveUser) {
                t.user = val;
                haveUser = true;
            } else if (key == QLatin1String("port") && !havePort) {
                t.port = val.toInt();
                havePort = true;
            } else if (key == QLatin1String("hostname") && !haveHost) {
                t.host = val;
                haveHost = true;
            }
        }
    }
    return t;
}

QString vaultBindingFor(const SessionConfig &s)
{
    switch (s.type) {
    case SessionConfig::Type::Ssh:
        return QStringLiteral("ssh:") + effectiveSshTarget(s).toString();
    case SessionConfig::Type::Serial:
        return QStringLiteral("serial:") + s.serialDevice;
    case SessionConfig::Type::LocalShell:
        break;
    }
    return {};
}

QString validateImportedSession(const SessionConfig &s)
{
    if (s.type == SessionConfig::Type::Serial) {
        return validateSerial(s);
    }
    if (s.type != SessionConfig::Type::Ssh) {
        return {};
    }
    const SshCommand cmd = buildSshCommand(s); // host/user/jump/extra syntax
    if (!cmd.ok()) {
        return cmd.error;
    }
    QList<SshOpt> opts;
    if (!parseSshOptions(QProcess::splitCommand(s.extraArgs), &opts)) {
        return QStringLiteral("Extra arguments must be ssh options.");
    }
    auto refuse = [](const QString &what, const QString &why) {
        return QStringLiteral("imported extra argument %1 is not allowed (%2).").arg(what, why);
    };
    for (const SshOpt &o : opts) {
        const QString flag = QStringLiteral("-") + o.letter;
        switch (o.letter.toLatin1()) {
        case 'F':
            return refuse(flag, QStringLiteral("loads another ssh config file"));
        case 'E':
            return refuse(flag, QStringLiteral("writes ssh's log to a file"));
        case 'I':
            return refuse(flag, QStringLiteral("loads a PKCS#11 library"));
        case 'S':
        case 'M':
            return refuse(flag, QStringLiteral("connection sharing / control socket"));
        case 'A':
            return refuse(flag, QStringLiteral("forwards your ssh agent to the server"));
        case 'Y':
            return refuse(flag, QStringLiteral("trusted X11 forwarding gives the server your display"));
        case 'J':
            if (const QString e = validateJumpHost(o.value); !e.isEmpty()) {
                return e;
            }
            break;
        case 'o': {
            const auto [key, val] = splitSshConfigOption(o.value);
            const QString lv = val.toLower();
            const QString shown = QStringLiteral("-o ") + key;
            static const QStringList runsOrLoads{
                QStringLiteral("proxycommand"),       QStringLiteral("localcommand"),
                QStringLiteral("permitlocalcommand"), QStringLiteral("knownhostscommand"),
                QStringLiteral("match"),              QStringLiteral("include"),
                QStringLiteral("pkcs11provider"),     QStringLiteral("securitykeyprovider"),
                QStringLiteral("remotecommand"),      QStringLiteral("controlpath"),
                QStringLiteral("controlmaster"),      QStringLiteral("controlpersist"),
                QStringLiteral("sendenv"),            QStringLiteral("hostname"),
                QStringLiteral("forwardx11trusted"),
            };
            if (key.isEmpty()) {
                return refuse(QStringLiteral("-o"), QStringLiteral("empty option"));
            }
            if (runsOrLoads.contains(key)) {
                return refuse(shown, key == QLatin1String("hostname")
                                         ? QStringLiteral("connects somewhere other than the Host field")
                                         : QStringLiteral("runs a command, loads code or config, or shares connections"));
            }
            if (key == QLatin1String("forwardagent") && lv != QLatin1String("no")) {
                return refuse(shown, QStringLiteral("forwards your ssh agent to the server"));
            }
            if (key == QLatin1String("stricthostkeychecking")
                && (lv == QLatin1String("no") || lv == QLatin1String("off") || lv == QLatin1String("false"))) {
                return refuse(shown + QLatin1Char('=') + val, QStringLiteral("disables host-key checking"));
            }
            if (key == QLatin1String("userknownhostsfile") || key == QLatin1String("globalknownhostsfile")) {
                for (const QString &f : val.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    if (f == QLatin1String("/dev/null") || f.toLower() == QLatin1String("none")) {
                        return refuse(shown + QLatin1Char('=') + val, QStringLiteral("disables host-key checking"));
                    }
                }
            }
            if (key == QLatin1String("proxyjump") && lv != QLatin1String("none")) {
                if (const QString e = validateJumpHost(val); !e.isEmpty()) {
                    return e;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    return {};
}

QStringList notableSessionSettings(const SessionConfig &s)
{
    QStringList out;
    const SessionConfig d;
    if (s.type == SessionConfig::Type::Ssh) {
        out << QStringLiteral("connects to %1").arg(effectiveSshTarget(s).toString());
        if (!s.jumpHost.isEmpty()) {
            out << QStringLiteral("jump host: %1").arg(s.jumpHost);
        }
        if (!s.extraArgs.trimmed().isEmpty()) {
            out << QStringLiteral("extra ssh options: %1").arg(s.extraArgs.trimmed());
        }
        if (!s.keyFile.isEmpty()) {
            out << QStringLiteral("key file: %1").arg(s.keyFile);
        }
        if (s.keepaliveInterval != d.keepaliveInterval || s.keepaliveCountMax != d.keepaliveCountMax) {
            out << QStringLiteral("keepalive: %1 s x %2").arg(s.keepaliveInterval).arg(s.keepaliveCountMax);
        }
    } else if (s.type == SessionConfig::Type::Serial) {
        out << QStringLiteral("serial device: %1 at %2").arg(s.serialDevice).arg(s.baudRate);
        if (!s.loginUser.isEmpty()) {
            out << QStringLiteral("login user: %1").arg(s.loginUser);
        }
        if (s.charDelayMs || s.lineDelayMs) {
            out << QStringLiteral("paste delays: %1 ms/char, %2 ms/line").arg(s.charDelayMs).arg(s.lineDelayMs);
        }
    } else {
        out << QStringLiteral("local shell");
    }
    if (s.autoReconnect) {
        out << QStringLiteral("reconnects automatically");
    }
    if (s.autoLog) {
        out << QStringLiteral("starts logging automatically");
    }
    return out;
}

QString validateSessionName(const QString &name)
{
    if (name.trimmed().isEmpty()) {
        return QStringLiteral("Session name is empty.");
    }
    if (name != name.trimmed()) {
        return QStringLiteral("Session name must not start or end with spaces.");
    }
    if (hasControlChars(name)) {
        return QStringLiteral("Session name must not contain control characters.");
    }
    if (name.startsWith(QLatin1Char('-'))) {
        return QStringLiteral("Session name must not start with '-' (it would look like an option to zt).");
    }
    if (name == QLatin1String("ssh") || name == QLatin1String("serial")) {
        return QStringLiteral("\"%1\" is reserved (zt %1 ... opens an ad-hoc session).").arg(name);
    }
    if (name.size() > 200) {
        return QStringLiteral("Session name is too long.");
    }
    return {};
}

QString validateSshHost(const QString &host)
{
    if (host.isEmpty()) {
        return QStringLiteral("Host is empty.");
    }
    if (host.startsWith(QLatin1Char('-'))) {
        return QStringLiteral("Host must not start with '-'.");
    }
    // Names, IPv4, IPv6 (optionally in brackets, with a %zone).
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._:%\\[\\]-]+$"));
    if (!re.match(host).hasMatch()) {
        return QStringLiteral("Host may only contain letters, digits and . _ - : % [ ] "
                              "(put the user name in the User field).");
    }
    return {};
}

QString validateSshUser(const QString &user)
{
    if (user.isEmpty()) {
        return {}; // optional
    }
    if (user.startsWith(QLatin1Char('-'))) {
        return QStringLiteral("User must not start with '-'.");
    }
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9._$\\\\-]+$"));
    if (!re.match(user).hasMatch()) {
        return QStringLiteral("User may only contain letters, digits and . _ - $ \\.");
    }
    return {};
}

QString validateJumpHost(const QString &jump)
{
    if (jump.isEmpty()) {
        return {};
    }
    if (jump.startsWith(QLatin1Char('-'))) {
        return QStringLiteral("Jump host must not start with '-'.");
    }
    // [user@]host[:port] or ssh://[user@]host[:port], comma-separated.
    static const QRegularExpression hop(
        QStringLiteral("^(ssh://)?([A-Za-z0-9._$\\\\-]+@)?([A-Za-z0-9._%-]+|\\[[0-9A-Fa-f:.%]+\\])(:[0-9]{1,5})?$"));
    const QStringList hops = jump.split(QLatin1Char(','));
    for (const QString &h : hops) {
        if (h.isEmpty() || h.startsWith(QLatin1Char('-')) || !hop.match(h).hasMatch()) {
            return QStringLiteral("Jump host \"%1\" is not [user@]host[:port].").arg(h);
        }
    }
    return {};
}

QString validateSerial(const SessionConfig &s)
{
    if (s.serialDevice.isEmpty()) {
        return QStringLiteral("Serial device is empty (e.g. /dev/ttyUSB0).");
    }
    if (!s.serialDevice.startsWith(QLatin1Char('/')) || hasControlChars(s.serialDevice)) {
        return QStringLiteral("Serial device must be an absolute path such as /dev/ttyUSB0.");
    }
    if (s.baudRate < 50 || s.baudRate > 4000000) {
        return QStringLiteral("Baud rate must be between 50 and 4000000.");
    }
    if (s.dataBits < 5 || s.dataBits > 8) {
        return QStringLiteral("Data bits must be 5, 6, 7 or 8.");
    }
    static const QStringList parities{QStringLiteral("none"), QStringLiteral("even"), QStringLiteral("odd"),
                                      QStringLiteral("mark"), QStringLiteral("space")};
    if (!parities.contains(s.parity)) {
        return QStringLiteral("Unknown parity \"%1\".").arg(s.parity);
    }
    if (s.stopBits != 1 && s.stopBits != 2) {
        return QStringLiteral("Stop bits must be 1 or 2.");
    }
    static const QStringList flows{QStringLiteral("none"), QStringLiteral("rtscts"), QStringLiteral("xonxoff")};
    if (!flows.contains(s.flowControl)) {
        return QStringLiteral("Unknown flow control \"%1\".").arg(s.flowControl);
    }
    static const QStringList enters{QStringLiteral("cr"), QStringLiteral("crlf"), QStringLiteral("lf")};
    if (!enters.contains(s.enterSends)) {
        return QStringLiteral("Enter must send cr, crlf or lf.");
    }
    if (s.charDelayMs < 0 || s.charDelayMs > 1000 || s.lineDelayMs < 0 || s.lineDelayMs > 10000) {
        return QStringLiteral("Paste delays must be 0-1000 ms per character and 0-10000 ms per line.");
    }
    if (s.breakMs < 10 || s.breakMs > 5000) {
        return QStringLiteral("Break duration must be 10-5000 ms.");
    }
    // The login user is typed into the device followed by Enter: a CR/LF or
    // other control character in it would type further commands.
    if (hasControlChars(s.loginUser)) {
        return QStringLiteral("Login user must not contain control characters.");
    }
    return {};
}

QByteArray enterSequence(const QString &enterSends)
{
    if (enterSends == QLatin1String("crlf")) {
        return QByteArrayLiteral("\r\n");
    }
    if (enterSends == QLatin1String("lf")) {
        return QByteArrayLiteral("\n");
    }
    return QByteArrayLiteral("\r");
}

SshCommand buildSshCommand(const SessionConfig &s)
{
    SshCommand c;
    c.program = QStringLiteral("ssh");
    auto fail = [&c](const QString &why) {
        c.error = why;
        c.args.clear();
        return c;
    };
    if (QString e = validateSshHost(s.host); !e.isEmpty()) {
        return fail(e);
    }
    if (QString e = validateSshUser(s.user); !e.isEmpty()) {
        return fail(e);
    }
    if (QString e = validateJumpHost(s.jumpHost); !e.isEmpty()) {
        return fail(e);
    }
    if (s.port < 1 || s.port > 65535) {
        return fail(QStringLiteral("Port must be between 1 and 65535."));
    }
    if (s.keepaliveInterval < 0 || s.keepaliveInterval > 3600) {
        return fail(QStringLiteral("Keepalive interval must be between 0 (off) and 3600 seconds."));
    }
    if (s.keepaliveInterval > 0 && (s.keepaliveCountMax < 1 || s.keepaliveCountMax > 100)) {
        return fail(QStringLiteral("Keepalive count must be between 1 and 100."));
    }
    if (hasControlChars(s.keyFile) || hasControlChars(s.extraArgs)) {
        return fail(QStringLiteral("Key file and extra arguments must not contain control characters."));
    }

    // Extra arguments: split like a command line (quotes respected) but never
    // given to a shell. They must all be ssh options (or option values), so they
    // can't supply a destination or a remote command.
    const QStringList extra = QProcess::splitCommand(s.extraArgs);
    for (qsizetype i = 0; i < extra.size(); ++i) {
        const QString &a = extra.at(i);
        if (a == QLatin1String("--")) {
            return fail(QStringLiteral("Extra arguments must not contain \"--\"."));
        }
        if (!a.startsWith(QLatin1Char('-')) || a.size() < 2) {
            return fail(QStringLiteral("Extra arguments must be ssh options; \"%1\" is not "
                                       "(use the Host field for the destination).").arg(a));
        }
        if (a.startsWith(QLatin1String("--"))) {
            return fail(QStringLiteral("Extra argument \"%1\" is not an ssh option.").arg(a));
        }
        // getopt rules: in a cluster like "-4vo", the first letter that takes a
        // value swallows the rest of the cluster, or the next argument if it is
        // the last letter ("-o X", "-L spec"). Values are never checked further:
        // they can't become a destination.
        for (qsizetype j = 1; j < a.size(); ++j) {
            if (kSshOptsWithValue.contains(a.at(j))) {
                if (j == a.size() - 1) {
                    if (i + 1 >= extra.size()) {
                        return fail(QStringLiteral("Extra argument %1 needs a value.").arg(a));
                    }
                    ++i;
                }
                break;
            }
        }
    }
    c.args << extra;
    if (s.keepaliveInterval > 0) {
        c.args << QStringLiteral("-o") << QStringLiteral("ServerAliveInterval=%1").arg(s.keepaliveInterval)
               << QStringLiteral("-o") << QStringLiteral("ServerAliveCountMax=%1").arg(s.keepaliveCountMax);
    }
    if (s.port != 22) {
        c.args << QStringLiteral("-p") << QString::number(s.port);
    }
    if (!s.user.isEmpty()) {
        c.args << QStringLiteral("-l") << s.user;
    }
    if (!s.keyFile.trimmed().isEmpty()) {
        c.args << QStringLiteral("-i") << expandTilde(s.keyFile.trimmed());
    }
    if (!s.jumpHost.isEmpty()) {
        c.args << QStringLiteral("-J") << s.jumpHost;
    }
    // "--" ends option parsing: the host can never be read as an option.
    c.args << QStringLiteral("--") << s.host;
    return c;
}

std::optional<SessionConfig> sessionFromSshArgs(const QStringList &args, QString *why)
{
    SessionConfig s;
    s.type = SessionConfig::Type::Ssh;
    QStringList extra;
    QString dest;
    qsizetype i = 0;
    for (; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a == QLatin1String("--")) {
            ++i;
            if (i < args.size()) {
                dest = args.at(i++);
            }
            break;
        }
        if (a.startsWith(QLatin1Char('-')) && a.size() >= 2) {
            if (a.size() == 2 && kSshOptsWithValue.contains(a.at(1))) {
                if (i + 1 >= args.size()) {
                    break;
                }
                const QString v = args.at(++i);
                switch (a.at(1).toLatin1()) {
                case 'p':
                    s.port = v.toInt();
                    break;
                case 'l':
                    s.user = v;
                    break;
                case 'i':
                    s.keyFile = v;
                    break;
                case 'J':
                    s.jumpHost = v;
                    break;
                case 'o': {
                    // Keepalive options map to the session's fields.
                    const QString opt = v.section(QLatin1Char('='), 0, 0).trimmed();
                    const QString val = v.section(QLatin1Char('='), 1).trimmed();
                    bool num = false;
                    const int n = val.toInt(&num);
                    if (num && opt.compare(QLatin1String("ServerAliveInterval"), Qt::CaseInsensitive) == 0) {
                        s.keepaliveInterval = n;
                    } else if (num && opt.compare(QLatin1String("ServerAliveCountMax"), Qt::CaseInsensitive) == 0) {
                        s.keepaliveCountMax = n;
                    } else {
                        extra << a << v;
                    }
                    break;
                }
                default:
                    extra << a << v;
                }
            } else {
                extra << a;
            }
            continue;
        }
        dest = a;
        ++i;
        break;
    }
    auto fail = [why](const QString &m) -> std::optional<SessionConfig> {
        if (why) {
            *why = m;
        }
        return std::nullopt;
    };
    if (i < args.size()) {
        return fail(QStringLiteral("a remote command can't be saved in a session"));
    }
    if (dest.startsWith(QLatin1String("ssh://"))) {
        dest = dest.mid(6);
    }
    if (const qsizetype at = dest.lastIndexOf(QLatin1Char('@')); at >= 0) {
        s.user = dest.left(at);
        dest = dest.mid(at + 1);
    }
    s.host = dest;
    QStringList quoted;
    for (const QString &e : extra) {
        QString q = e;
        if (q.contains(QLatin1Char(' ')) || q.contains(QLatin1Char('"')) || q.contains(QLatin1Char('\''))) {
            q.replace(QLatin1Char('"'), QStringLiteral("\"\"\""));
            q = QLatin1Char('"') + q + QLatin1Char('"');
        }
        quoted << q;
    }
    s.extraArgs = quoted.join(QLatin1Char(' '));
    const SshCommand check = buildSshCommand(s);
    if (!check.ok()) {
        return fail(check.error);
    }
    return s;
}

} // namespace zterminal
