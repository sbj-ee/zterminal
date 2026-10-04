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
        && fontFamily == o.fontFamily && fontSize == o.fontSize && colorScheme == o.colorScheme;
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
    if (name == QLatin1String("ssh")) {
        return QStringLiteral("\"ssh\" is reserved (zt ssh ... opens an ad-hoc SSH session).");
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
