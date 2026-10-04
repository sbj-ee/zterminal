#include "CommandLine.hpp"

#include <QFileInfo>

namespace zterminal {

QString LaunchRequest::displayName() const
{
    switch (kind) {
    case Kind::LocalShell:
        return QStringLiteral("local shell");
    case Kind::SavedSession:
        return sessionName;
    case Kind::Ssh: {
        // The destination is the first non-option argument; skip the values of
        // ssh options that take one (ssh(1): -B -b -c -D -E -e -F -I -i -J -L -l
        // -m -O -o -p -Q -R -S -W -w).
        static const QString withValue = QStringLiteral("BbcDEeFIiJLlmOopQRSWw");
        QString dest;
        for (qsizetype i = 0; i < args.size(); ++i) {
            const QString &a = args.at(i);
            if (a == QLatin1String("--")) {
                if (i + 1 < args.size()) {
                    dest = args.at(i + 1);
                }
                break;
            }
            if (a.startsWith(QLatin1Char('-')) && a.size() >= 2) {
                if (a.size() == 2 && withValue.contains(a.at(1))) {
                    ++i; // value is the next argument
                }
                continue;
            }
            dest = a;
            break;
        }
        return dest.isEmpty() ? QStringLiteral("ssh") : QStringLiteral("ssh ") + dest;
    }
    case Kind::Command:
        return QFileInfo(program).fileName();
    case Kind::Version:
    case Kind::Help:
    case Kind::ListSessions:
    case Kind::CheckSession:
    case Kind::Error:
        break;
    }
    return {};
}

LaunchRequest parseCommandLine(const QStringList &args)
{
    LaunchRequest r;
    if (args.isEmpty()) {
        return r;
    }
    const QString first = args.front();
    if (first == QLatin1String("-v") || first == QLatin1String("--version")) {
        r.kind = LaunchRequest::Kind::Version;
        return r;
    }
    if (first == QLatin1String("-h") || first == QLatin1String("--help")) {
        r.kind = LaunchRequest::Kind::Help;
        return r;
    }
    if (first == QLatin1String("--list-sessions")) {
        r.kind = args.size() == 1 ? LaunchRequest::Kind::ListSessions : LaunchRequest::Kind::Error;
        r.error = QStringLiteral("--list-sessions takes no arguments");
        return r;
    }
    if (first == QLatin1String("--check-session")) {
        if (args.size() != 2) {
            r.kind = LaunchRequest::Kind::Error;
            r.error = QStringLiteral("--check-session needs exactly one session name");
            return r;
        }
        r.kind = LaunchRequest::Kind::CheckSession;
        r.sessionName = args.at(1);
        return r;
    }
    if (first == QLatin1String("-e") || first == QLatin1String("--command")) {
        if (args.size() < 2) {
            r.kind = LaunchRequest::Kind::Error;
            r.error = QStringLiteral("%1 needs a program to run").arg(first);
            return r;
        }
        r.kind = LaunchRequest::Kind::Command;
        r.program = args.at(1);
        r.args = args.mid(2);
        return r;
    }
    if (first == QLatin1String("ssh")) {
        if (args.size() < 2) {
            r.kind = LaunchRequest::Kind::Error;
            r.error = QStringLiteral("ssh needs a destination, e.g. zt ssh user@host");
            return r;
        }
        r.kind = LaunchRequest::Kind::Ssh;
        r.program = QStringLiteral("ssh");
        r.args = args.mid(1);
        return r;
    }
    if (first.startsWith(QLatin1Char('-'))) {
        r.kind = LaunchRequest::Kind::Error;
        r.error = QStringLiteral("unknown option: %1").arg(first);
        return r;
    }
    if (args.size() > 1) {
        r.kind = LaunchRequest::Kind::Error;
        r.error = QStringLiteral("too many arguments (a saved session name is one argument; quote it if it has spaces)");
        return r;
    }
    r.kind = LaunchRequest::Kind::SavedSession;
    r.sessionName = first;
    return r;
}

QString usageText()
{
    return QStringLiteral(
        "Usage: zterminal [SESSION | ssh [user@]host [ssh-args...] | -e command [args...]]\n"
        "       zterminal --version | --help\n"
        "\n"
        "  (no arguments)           open a local shell\n"
        "  SESSION                  open a saved session by name\n"
        "  ssh [user@]host [args]   open an ad-hoc SSH session using the system ssh\n"
        "  -e, --command prog args  run prog in the terminal\n"
        "  --list-sessions          print the saved session names and exit\n"
        "  --check-session SESSION  exit 0 if SESSION is saved, else list the saved ones\n"
        "  -v, --version            print the version and exit\n"
        "  -h, --help               print this help and exit\n"
        "\n"
        "Saved sessions live in ~/.config/zterminal/sessions/ (one .ini per session).\n"
        "Use `zt` to start zterminal detached from the calling shell.\n");
}

} // namespace zterminal
