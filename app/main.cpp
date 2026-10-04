#include "CommandLine.hpp"
#include "MainWindow.hpp"
#include "SessionStore.hpp"
#include "UpdateManager.hpp"
#include "version.hpp"

#include <QApplication>
#include <QSettings>

#include <cstdio>
#include <cstring>

int main(int argc, char *argv[])
{
    // --version / --help must work without a display (used by `zt` and tests).
    if (argc > 1) {
        QStringList early;
        for (int i = 1; i < argc; ++i) {
            early << QString::fromLocal8Bit(argv[i]);
        }
        // `zterminal --check ARGS...`: validate ARGS as zt would launch them
        // (used by zt before detaching, so mistakes are reported in the terminal).
        const bool checkOnly = early.front() == QLatin1String("--check");
        if (checkOnly) {
            early.removeFirst();
        }
        const zterminal::LaunchRequest r = zterminal::parseCommandLine(early);
        if (checkOnly && r.kind == zterminal::LaunchRequest::Kind::Error) {
            std::fprintf(stderr, "zterminal: %s\n", r.error.toLocal8Bit().constData());
            return 2;
        }
        if (r.kind == zterminal::LaunchRequest::Kind::Version) {
            std::printf("zterminal %s\n", zterminal::kVersionString);
            return 0;
        }
        if (r.kind == zterminal::LaunchRequest::Kind::Help) {
            std::fputs(zterminal::usageText().toLocal8Bit().constData(), stdout);
            return 0;
        }
        // Session lookups also run without a display (zt checks before detaching).
        const zterminal::SessionStore store;
        if (r.kind == zterminal::LaunchRequest::Kind::ListSessions) {
            for (const QString &n : store.names()) {
                std::printf("%s\n", n.toLocal8Bit().constData());
            }
            return 0;
        }
        if (r.kind == zterminal::LaunchRequest::Kind::CheckSession
            || r.kind == zterminal::LaunchRequest::Kind::SavedSession) {
            if (!store.contains(r.sessionName)) {
                std::fprintf(stderr, "zterminal: %s\n", store.unknownSessionMessage(r.sessionName).toLocal8Bit().constData());
                return 2;
            }
            if (r.kind == zterminal::LaunchRequest::Kind::CheckSession) {
                return 0;
            }
        }
        if (checkOnly) {
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QApplication::setApplicationVersion(QString::fromLatin1(zterminal::kVersionString));
    QApplication::setDesktopFileName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat); // ~/.config/zterminal/zterminal.ini

    const QStringList args = QApplication::arguments().mid(1);
    const zterminal::LaunchRequest request = zterminal::parseCommandLine(args);
    if (request.kind == zterminal::LaunchRequest::Kind::Error) {
        std::fprintf(stderr, "zterminal: %s\n\n%s", request.error.toLocal8Bit().constData(),
                     zterminal::usageText().toLocal8Bit().constData());
        return 2;
    }

    zterminal::MainWindow w(request, args);
    w.show();
    w.startSession();
    // Quietly, a few seconds in, if enabled in Preferences and a day has passed.
    zterminal::UpdateManager::instance().scheduleStartupCheck(&w);
    return QApplication::exec();
}
