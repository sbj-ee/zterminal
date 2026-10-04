// Version/title, command line, mouse settings, colour palette.
#include "ColorScheme.hpp"
#include "CommandLine.hpp"
#include "MouseSettings.hpp"
#include "WindowTitle.hpp"
#include "version.hpp"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace zterminal;

class TstMisc : public QObject
{
    Q_OBJECT
private slots:
    void versionComesFromCMake()
    {
        QCOMPARE(QString::fromLatin1(kVersionString), QStringLiteral(ZTERMINAL_EXPECTED_VERSION));
        QCOMPARE(QStringLiteral("%1.%2.%3").arg(kVersionMajor).arg(kVersionMinor).arg(kVersionPatch),
                 QStringLiteral(ZTERMINAL_EXPECTED_VERSION));
    }

    void windowTitle()
    {
        const QString v = QStringLiteral(ZTERMINAL_EXPECTED_VERSION);
        QCOMPARE(makeWindowTitle(QStringLiteral("local shell")),
                 QStringLiteral("zterminal %1 \u2014 local shell").arg(v));
        QCOMPARE(makeWindowTitle(QStringLiteral("SG250 console"), QStringLiteral("vim foo.c")),
                 QStringLiteral("zterminal %1 \u2014 SG250 console \u2014 vim foo.c").arg(v));
        QCOMPARE(makeWindowTitle(QStringLiteral("x"), QStringLiteral("x")),
                 QStringLiteral("zterminal %1 \u2014 x").arg(v));
        QCOMPARE(makeWindowTitle(QString()), QStringLiteral("zterminal %1").arg(v));
    }

    void commandLine_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("display");
        using K = LaunchRequest::Kind;
        QTest::newRow("none") << QStringList{} << int(K::LocalShell) << QStringLiteral("local shell");
        QTest::newRow("saved") << QStringList{QStringLiteral("SG250")} << int(K::SavedSession) << QStringLiteral("SG250");
        QTest::newRow("ssh") << QStringList{QStringLiteral("ssh"), QStringLiteral("-p"), QStringLiteral("2222"),
                                            QStringLiteral("admin@sw1")}
                             << int(K::Ssh) << QStringLiteral("ssh admin@sw1");
        QTest::newRow("ssh-jump") << QStringList{QStringLiteral("ssh"), QStringLiteral("-J"), QStringLiteral("vertex"),
                                                 QStringLiteral("-4"), QStringLiteral("sbj@dragon"), QStringLiteral("uptime")}
                                  << int(K::Ssh) << QStringLiteral("ssh sbj@dragon");
        QTest::newRow("ssh-no-host") << QStringList{QStringLiteral("ssh")} << int(K::Error) << QString();
        QTest::newRow("cmd") << QStringList{QStringLiteral("-e"), QStringLiteral("/usr/bin/htop")}
                             << int(K::Command) << QStringLiteral("htop");
        QTest::newRow("version") << QStringList{QStringLiteral("--version")} << int(K::Version) << QString();
        QTest::newRow("help") << QStringList{QStringLiteral("-h")} << int(K::Help) << QString();
        QTest::newRow("bad") << QStringList{QStringLiteral("--bogus")} << int(K::Error) << QString();
        QTest::newRow("two") << QStringList{QStringLiteral("a"), QStringLiteral("b")} << int(K::Error) << QString();
    }

    void commandLine()
    {
        QFETCH(QStringList, args);
        QFETCH(int, kind);
        QFETCH(QString, display);
        const LaunchRequest r = parseCommandLine(args);
        QCOMPARE(int(r.kind), kind);
        if (!display.isEmpty()) {
            QCOMPARE(r.displayName(), display);
        }
    }

    void sshArgsPassThroughUnchanged()
    {
        const QStringList in{QStringLiteral("ssh"), QStringLiteral("-J"), QStringLiteral("vertex"),
                             QStringLiteral("admin@10.0.0.1; rm -rf ~")};
        const LaunchRequest r = parseCommandLine(in);
        QCOMPARE(r.program, QStringLiteral("ssh"));
        QCOMPARE(r.args, in.mid(1)); // an argv list: no shell ever sees it
    }

    void mouseSettingsDefaultsAndRoundTrip()
    {
        const MouseSettings d;
        QCOMPARE(d.middleClick, MiddleClickAction::PastePrimary);
        QVERIFY(d.copyOnSelectToClipboard);
        QCOMPARE(MouseSettings::middleClickFromString(QStringLiteral("nonsense")), MiddleClickAction::PastePrimary);

        QTemporaryDir dir;
        QSettings s(dir.filePath(QStringLiteral("z.ini")), QSettings::IniFormat);
        MouseSettings m;
        m.middleClick = MiddleClickAction::Off;
        m.copyOnSelectToClipboard = false;
        m.save(s);
        MouseSettings back;
        back.load(s);
        QCOMPARE(back.middleClick, MiddleClickAction::Off);
        QVERIFY(!back.copyOnSelectToClipboard);
        m.middleClick = MiddleClickAction::PasteClipboard;
        m.save(s);
        back.load(s);
        QCOMPARE(back.middleClick, MiddleClickAction::PasteClipboard);
    }

    void xterm256Palette()
    {
        const auto ansi = ColorScheme::byId(QStringLiteral("xterm")).ansi;
        QCOMPARE(xterm256(1, ansi), ansi[1]);
        QCOMPARE(xterm256(16, ansi), qRgb(0, 0, 0));
        QCOMPARE(xterm256(21, ansi), qRgb(0, 0, 255));
        QCOMPARE(xterm256(208, ansi), qRgb(255, 135, 0));
        QCOMPARE(xterm256(231, ansi), qRgb(255, 255, 255));
        QCOMPARE(xterm256(232, ansi), qRgb(8, 8, 8));
        QCOMPARE(xterm256(255, ansi), qRgb(238, 238, 238));
        QCOMPARE(ColorScheme::byId(QStringLiteral("unknown")).id, QStringLiteral("xterm"));
    }
};

QTEST_GUILESS_MAIN(TstMisc)
#include "tst_misc.moc"
