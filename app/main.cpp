#include "CommandLine.hpp"
#include "MainWindow.hpp"
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
        const zterminal::LaunchRequest r = zterminal::parseCommandLine(early);
        if (r.kind == zterminal::LaunchRequest::Kind::Version) {
            std::printf("zterminal %s\n", zterminal::kVersionString);
            return 0;
        }
        if (r.kind == zterminal::LaunchRequest::Kind::Help) {
            std::fputs(zterminal::usageText().toLocal8Bit().constData(), stdout);
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
    return QApplication::exec();
}
