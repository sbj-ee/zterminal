// Version/title, command line, mouse settings, colour palette.
#include "BrandThemes.h"
#include "ColorScheme.hpp"
#include "CommandLine.hpp"
#include "MouseSettings.hpp"
#include "WindowTitle.hpp"
#include "version.hpp"

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cmath>
#include <cstdint>

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

    // The brand themes shared with zmail (core/BrandThemes.h, docs/THEMES.md).
    // The hex values here are the table in docs/THEMES.md; zmail's tst_theme
    // checks the same table.
    void brandSchemesMatchSharedTable()
    {
        struct Row {
            const char *id, *name;
            QRgb bg, fg, cursor, selBg, selFg;
            std::array<QRgb, 16> ansi;
        };
        const Row rows[] = {
            {"boilermakers", "Boilermakers", 0x000000, 0xCFB991, 0xDAAA00, 0xCFB991, 0x000000,
             {0x262626, 0xE5534B, 0x8CC265, 0xDAAA00, 0x6CA0DC, 0xC792EA, 0x56B6C2, 0xC4BFC0,
              0x9D9795, 0xFF7B72, 0xB5E08A, 0xEBD99F, 0x9CC3F0, 0xE3B0F5, 0x8EDCE6, 0xFFFFFF}},
            {"badgers", "Badgers", 0x121212, 0xFFFFFF, 0xC5050C, 0x9B0000, 0xFFFFFF,
             {0x2A2A2A, 0xF0474E, 0x7FC97F, 0xF2C14E, 0x6FA8EC, 0xD88AD8, 0x5CC8C8, 0xE1E5E7,
              0x8A8D91, 0xFF7B80, 0xA8E6A3, 0xFFDA7A, 0x9DC4F5, 0xF0B0F0, 0x90E3E3, 0xFFFFFF}},
            {"packers", "Packers", 0x203731, 0xFFFFFF, 0xFFB612, 0xFFB612, 0x203731,
             {0x14241F, 0xFF8A80, 0x9EE07A, 0xFFB612, 0x8AB4F8, 0xE3A6F0, 0x7FE0D6, 0xE1E5E7,
              0x8FA89F, 0xFFB3AD, 0xC3F0A6, 0xFFD878, 0xB8D1FB, 0xF1C9F8, 0xB0F0E8, 0xFFFFFF}},
        };
        const QList<ColorScheme> all = ColorScheme::builtIn();
        QCOMPARE(all.size(), 6);
        QCOMPARE(int(sbj::brand::kThemes.size()), 3);
        auto opaque = [](QRgb v) { return v | 0xff000000u; };
        for (const Row &r : rows) {
            const ColorScheme s = ColorScheme::byId(QString::fromLatin1(r.id));
            QCOMPARE(s.id, QString::fromLatin1(r.id));
            QCOMPARE(s.name, QString::fromLatin1(r.name));
            QCOMPARE(s.background, opaque(r.bg));
            QCOMPARE(s.foreground, opaque(r.fg));
            QCOMPARE(s.cursor, opaque(r.cursor));
            QCOMPARE(s.selectionBackground, opaque(r.selBg));
            QCOMPARE(s.selectionForeground, opaque(r.selFg));
            for (size_t i = 0; i < 16; ++i) {
                QCOMPARE(s.ansi[i], opaque(r.ansi[i]));
            }
            const sbj::brand::Theme *t = sbj::brand::findTheme(r.id);
            QVERIFY(t);
            // Every UI role is set (only black may be 0, and only as a dark role).
            for (std::uint32_t v : {t->surface, t->foreground, t->muted, t->accent, t->link, t->selection,
                                    t->chromeText, t->header}) {
                QVERIFY(v != 0);
            }
        }
        QVERIFY(!sbj::brand::findTheme("nope"));
    }

    void brandSchemesAreReadable()
    {
        // WCAG 2.x contrast.
        auto lum = [](QRgb c) {
            auto ch = [](int v) {
                const double x = v / 255.0;
                return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * ch(qRed(c)) + 0.7152 * ch(qGreen(c)) + 0.0722 * ch(qBlue(c));
        };
        auto ratio = [&](QRgb a, QRgb b) {
            double la = lum(a), lb = lum(b);
            if (la < lb) {
                std::swap(la, lb);
            }
            return (la + 0.05) / (lb + 0.05);
        };
        for (const sbj::brand::Theme &t : sbj::brand::kThemes) {
            const ColorScheme s = ColorScheme::fromBrandTheme(t);
            const QByteArray id(t.id.data(), qsizetype(t.id.size()));
            QVERIFY2(ratio(s.foreground, s.background) >= 7.0, id.constData());   // WCAG AAA
            QVERIFY2(ratio(s.selectionForeground, s.selectionBackground) >= 7.0, id.constData());
            QVERIFY2(ratio(s.cursor, s.background) >= 3.0, id.constData());       // non-text UI
            // Every colour but black (0) is readable text on the background
            // (bright black 8 included), and no two of the 16 are the same.
            for (int i = 1; i < 16; ++i) {
                QVERIFY2(ratio(s.ansi[size_t(i)], s.background) >= 4.5, qPrintable(QStringLiteral("%1 ansi %2").arg(QString::fromLatin1(id)).arg(i)));
                for (int j = 0; j < i; ++j) {
                    QVERIFY(s.ansi[size_t(i)] != s.ansi[size_t(j)]);
                }
            }
        }
    }
};

QTEST_GUILESS_MAIN(TstMisc)
#include "tst_misc.moc"
