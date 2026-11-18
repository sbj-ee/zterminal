// Emulation core: text, 16/256/truecolor SGR, attributes, scrollback, title,
// bracketed paste, mouse modes and key encoding (libvterm through Terminal).
#include "ColorScheme.hpp"
#include "Contrast.hpp"
#include "Terminal.hpp"
#include "WindowTitle.hpp"

#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTest>

using namespace zterminal;

class TstTerminal : public QObject
{
    Q_OBJECT

    static QByteArray collect(Terminal &t, const std::function<void()> &fn)
    {
        QByteArray out;
        auto c = QObject::connect(&t, &Terminal::output, [&out](const QByteArray &b) { out += b; });
        fn();
        QObject::disconnect(c);
        return out;
    }

private slots:
    void plainText()
    {
        Terminal t(5, 20);
        t.feed("hello\r\nworld");
        QCOMPARE(t.lineText(0), QStringLiteral("hello"));
        QCOMPARE(t.lineText(1), QStringLiteral("world"));
        QCOMPARE(t.cursorPos(), QPoint(5, 1));
        QCOMPARE(t.lineText(0, 1, 4), QStringLiteral("ell"));
    }

    void ansi16Colors()
    {
        Terminal t(5, 20);
        const ColorScheme s = t.colorScheme();
        t.feed("\x1b[31mR\x1b[92mG\x1b[0mD\x1b[44mB");
        QCOMPARE(t.cell(0, 0).fg, s.ansi[1]);
        QCOMPARE(t.cell(0, 1).fg, s.ansi[10]);
        QCOMPARE(t.cell(0, 2).fg, s.foreground);
        QVERIFY(t.cell(0, 2).defaultBg);
        QCOMPARE(t.cell(0, 3).bg, s.ansi[4]);
        QVERIFY(!t.cell(0, 3).defaultBg);
    }

    void colors256()
    {
        Terminal t(5, 20);
        t.feed("\x1b[38;5;196mA\x1b[38;5;21mB\x1b[48;5;244mC\x1b[38;5;3mD");
        QCOMPARE(t.cell(0, 0).fg, qRgb(255, 0, 0));
        QCOMPARE(t.cell(0, 1).fg, qRgb(0, 0, 255));
        QCOMPARE(t.cell(0, 2).bg, qRgb(128, 128, 128));
        QCOMPARE(t.cell(0, 3).fg, t.colorScheme().ansi[3]); // 0..15 follow the scheme
    }

    void truecolor()
    {
        Terminal t(5, 20);
        // Semicolon form and the colon sub-parameter form libvterm accepts (38:2:r:g:b).
        t.feed("\x1b[38;2;12;34;56;48;2;200;150;100mX\x1b[38:2:1:2:3mY");
        QCOMPARE(t.cell(0, 0).fg, qRgb(12, 34, 56));
        QCOMPARE(t.cell(0, 0).bg, qRgb(200, 150, 100));
        QCOMPARE(t.cell(0, 1).fg, qRgb(1, 2, 3));
    }

    void attributes()
    {
        Terminal t(5, 20);
        t.feed("\x1b[1mB\x1b[0;3mI\x1b[0;4mU\x1b[0;9mS\x1b[0;7mR");
        QVERIFY(t.cell(0, 0).bold);
        QVERIFY(t.cell(0, 1).italic);
        QVERIFY(t.cell(0, 2).underline);
        QVERIFY(t.cell(0, 3).strike);
        const Cell r = t.cell(0, 4);
        QCOMPARE(r.fg, t.colorScheme().background);
        QCOMPARE(r.bg, t.colorScheme().foreground);
        QVERIFY(!r.defaultBg);
    }

    // --- Faint (SGR 2) and minimum contrast (Contrast.hpp) -----------------

    void contrastMath()
    {
        using namespace contrast;
        const QRgb black = qRgb(0, 0, 0);
        const QRgb white = qRgb(255, 255, 255);
        QCOMPARE(contrastRatio(black, white), 21.0);
        QCOMPARE(contrastRatio(white, black), 21.0);
        QCOMPARE(contrastRatio(qRgb(90, 90, 90), qRgb(90, 90, 90)), 1.0);
        // #262626 on black: the unreadable grey in the bug report.
        QVERIFY(contrastRatio(qRgb(38, 38, 38), black) < 1.5);
        QCOMPARE(blend(white, black, 0.66), qRgb(168, 168, 168));
        QCOMPARE(blend(qRgb(200, 100, 50), qRgb(0, 0, 0), 1.0), qRgb(200, 100, 50));
        QCOMPARE(blend(qRgb(200, 100, 50), qRgb(10, 20, 30), 0.0), qRgb(10, 20, 30));
        // ensureContrast leaves a readable colour alone ...
        QCOMPARE(ensureContrast(qRgb(229, 229, 229), black, 3.0), qRgb(229, 229, 229));
        // ... and moves a dark one just far enough, away from the background.
        const QRgb up = ensureContrast(qRgb(38, 38, 38), black, 3.0);
        QVERIFY(contrastRatio(up, black) >= 3.0);
        QVERIFY(contrastRatio(up, black) < 3.2);
        QVERIFY(qRed(up) > 38);
        const QRgb down = ensureContrast(qRgb(235, 235, 235), white, 3.0);
        QVERIFY(contrastRatio(down, white) >= 3.0);
        QVERIFY(contrastRatio(down, white) < 3.2);
        QVERIFY(qRed(down) < 235);
        // Hue is kept as far as possible (blend toward white, not grey-out).
        const QRgb navy = ensureContrast(qRgb(0, 0, 90), black, 3.0);
        QVERIFY(qBlue(navy) > qRed(navy));
    }

    void dimBlendsTowardBackground()
    {
        // xterm default: #e5e5e5 on black -> 66 % foreground, already readable.
        Terminal t(5, 20);
        const ColorScheme s = t.colorScheme();
        t.feed("N\x1b[2mD\x1b[22mN\x1b[2;31mR\x1b[0mN");
        const Cell n = t.cell(0, 0);
        const Cell d = t.cell(0, 1);
        QVERIFY(!n.dim);
        QVERIFY(d.dim);
        QCOMPARE(n.fg, s.foreground);
        QCOMPARE(d.fg, contrast::blend(s.foreground, s.background, contrast::kDimForegroundWeight));
        QCOMPARE(d.fg, qRgb(151, 151, 151));
        QVERIFY(contrast::contrastRatio(d.fg, s.background) >= contrast::kDimMinContrast);
        QVERIFY(contrast::luminance(d.fg) < contrast::luminance(n.fg)); // still visibly dimmer
        QVERIFY(!t.cell(0, 2).dim); // SGR 22 ends faint
        QCOMPARE(t.cell(0, 2).fg, s.foreground);
        QVERIFY(t.cell(0, 3).dim);
        QCOMPARE(t.cell(0, 3).fg, contrast::dim(s.ansi[1], s.background));
        QVERIFY(!t.cell(0, 4).dim); // SGR 0 ends faint
        QCOMPARE(t.cell(0, 4).fg, s.foreground);
    }

    void dimKeepsMinimumContrastOnBlack()
    {
        // Boilermakers (the reporter's theme): gold on black, ANSI black #262626.
        Terminal t(5, 40);
        t.setColorScheme(ColorScheme::byId(QStringLiteral("boilermakers")));
        const ColorScheme s = t.colorScheme();
        QCOMPARE(s.background, qRgb(0, 0, 0));
        t.feed("\x1b[2mD\x1b[0;2;30mK\x1b[0;2;90mG");
        for (int col = 0; col < 3; ++col) {
            const Cell c = t.cell(0, col);
            QVERIFY(c.dim);
            QVERIFY2(contrast::contrastRatio(c.fg, s.background) >= contrast::kDimMinContrast,
                     qPrintable(QStringLiteral("col %1 %2").arg(col).arg(c.fg, 6, 16)));
        }
        // Dim default text is the gold at two thirds, not a near-black grey.
        QCOMPARE(t.cell(0, 0).fg, contrast::blend(s.foreground, s.background, contrast::kDimForegroundWeight));
    }

    void dimKeepsMinimumContrastOnLight()
    {
        ColorScheme light = ColorScheme::byId(QStringLiteral("xterm"));
        light.foreground = qRgb(0, 0, 0);
        light.background = qRgb(255, 255, 255);
        light.ansi[7] = qRgb(229, 229, 229);
        light.ansi[15] = qRgb(255, 255, 255);
        Terminal t(5, 40);
        t.setColorScheme(light);
        t.feed("N\x1b[2mD\x1b[0;2;37mW\x1b[0;2;38;2;240;240;240mT\x1b[0;2;38;5;254mI");
        QCOMPARE(t.cell(0, 0).fg, qRgb(0, 0, 0));
        // Black text dims toward white (lighter), stays well readable.
        const QRgb d = t.cell(0, 1).fg;
        QCOMPARE(d, contrast::blend(light.foreground, light.background, contrast::kDimForegroundWeight));
        QVERIFY(qRed(d) > 0);
        // Light foregrounds on a light background are pushed darker to 3:1.
        for (int col = 1; col < 5; ++col) {
            const Cell c = t.cell(0, col);
            QVERIFY(c.dim);
            QVERIFY2(contrast::contrastRatio(c.fg, light.background) >= contrast::kDimMinContrast,
                     qPrintable(QStringLiteral("col %1 %2").arg(col).arg(c.fg, 6, 16)));
        }
        QVERIFY(qRed(t.cell(0, 3).fg) < 240);
    }

    void dimWithDarkPaletteOrTruecolorForeground()
    {
        Terminal t(5, 40);
        const ColorScheme s = t.colorScheme(); // xterm: black background
        // 256-colour grey 235 (#262626), 232 (#080808), truecolor near-black,
        // and pure black text on black, all faint.
        t.feed("\x1b[2;38;5;235mA\x1b[0;2;38;5;232mB\x1b[0;2;38;2;30;30;30mC\x1b[0;2;38;2;0;0;0mD"
               "\x1b[0;2;38;2;0;0;100mE");
        for (int col = 0; col < 5; ++col) {
            const Cell c = t.cell(0, col);
            QVERIFY(c.dim);
            QVERIFY2(contrast::contrastRatio(c.fg, s.background) >= contrast::kDimMinContrast,
                     qPrintable(QStringLiteral("col %1 %2").arg(col).arg(c.fg, 6, 16)));
            QVERIFY(contrast::contrastRatio(c.fg, s.background) < 3.2); // lifted only as far as needed
        }
        // Dark blue keeps its hue.
        QVERIFY(qBlue(t.cell(0, 4).fg) > qRed(t.cell(0, 4).fg));
        // On an explicit background the floor is measured against that background.
        t.feed("\r\n\x1b[2;38;2;40;40;40;48;2;30;30;30mX");
        const Cell x = t.cell(1, 0);
        QCOMPARE(x.bg, qRgb(30, 30, 30));
        QVERIFY(contrast::contrastRatio(x.fg, x.bg) >= contrast::kDimMinContrast);
    }

    void dimWithInverseAndBold()
    {
        Terminal t(5, 40);
        const ColorScheme s = t.colorScheme();
        t.feed("\x1b[2;7mI\x1b[0;1;2;31mB\x1b[0;1;31mb\x1b[0;7;2;38;5;235mJ");
        // Inverse: the cell is drawn foreground-coloured, and the faint text
        // (the old background) is dimmed toward that, readable.
        const Cell inv = t.cell(0, 0);
        QVERIFY(inv.dim);
        QCOMPARE(inv.bg, s.foreground);
        QCOMPARE(inv.fg, contrast::dim(s.background, s.foreground));
        QVERIFY(contrast::contrastRatio(inv.fg, inv.bg) >= contrast::kDimMinContrast);
        QVERIFY(inv.fg != s.background); // dimmed, not plain reverse
        // Bold + faint: bold font, bright colour (bold-is-bright), then dimmed.
        const Cell bd = t.cell(0, 1);
        QVERIFY(bd.bold);
        QVERIFY(bd.dim);
        QCOMPARE(t.cell(0, 2).fg, s.ansi[9]);
        QCOMPARE(bd.fg, contrast::dim(s.ansi[9], s.background));
        // Inverse with a dark 256-colour foreground: that grey (raised, as
        // it is near-invisible on black) becomes the cell background and the
        // faint text must still read on it.
        const Cell j = t.cell(0, 3);
        QCOMPARE(j.bg, contrast::ensureContrast(qRgb(38, 38, 38), s.background, contrast::kMinReadableContrast));
        QVERIFY(contrast::contrastRatio(j.fg, j.bg) >= contrast::kDimMinContrast);
    }

    void dimSavedRestoredAndReported()
    {
        Terminal t(5, 40);
        // DECSC / DECRC keep faint with the rest of the pen.
        t.feed("\x1b[2m\x1b" "7\x1b[0m\x1b" "8X");
        QVERIFY(t.cell(0, 0).dim);
        // DECRQSS for SGR reports it (CSI 2 m among the attributes).
        const QByteArray reply = collect(t, [&t] { t.feed("\x1bP$qm\x1b\\"); });
        QCOMPARE(reply, QByteArray("\x1bP1$r2m\x1b\\"));
    }

    void normalColoursUnchanged()
    {
        // Only faint text and near-invisible palette colours on the default
        // background are adjusted; everything else renders exactly as sent.
        for (const QString &id : {QStringLiteral("xterm"), QStringLiteral("boilermakers"),
                                  QStringLiteral("solarized-dark"), QStringLiteral("putty")}) {
            Terminal t(5, 40);
            t.setColorScheme(ColorScheme::byId(id));
            const ColorScheme s = t.colorScheme();
            t.feed("N\x1b[31mR\x1b[32mG\x1b[34mB\x1b[37mW\x1b[94mL\x1b[0;38;2;30;30;30mT\x1b[0;38;5;196mX"
                   "\x1b[0;30;40mH\x1b[0;1mD");
            QCOMPARE(t.cell(0, 0).fg, s.foreground);
            QCOMPARE(t.cell(0, 1).fg, s.ansi[1]);
            QCOMPARE(t.cell(0, 2).fg, s.ansi[2]);
            QCOMPARE(t.cell(0, 3).fg, s.ansi[4]);
            QCOMPARE(t.cell(0, 4).fg, s.ansi[7]);
            QCOMPARE(t.cell(0, 5).fg, s.ansi[12]);
            QCOMPARE(t.cell(0, 6).fg, qRgb(30, 30, 30)); // truecolor is the program's choice
            QCOMPARE(t.cell(0, 7).fg, qRgb(255, 0, 0));
            QCOMPARE(t.cell(0, 8).fg, s.ansi[0]); // explicit fg/bg pair kept (e.g. hidden text)
            QCOMPARE(t.cell(0, 8).bg, s.ansi[0]);
            QCOMPARE(t.cell(0, 9).fg, s.foreground);
            for (int col = 0; col < 10; ++col) {
                QVERIFY(!t.cell(0, col).dim);
            }
        }
        // The darkest standard colours, xterm blue #0000ee (2.2:1) and PuTTY
        // blue #0000bb (1.7:1) on black, are left alone.
        Terminal x(2, 10);
        x.feed("\x1b[34mB");
        QCOMPARE(x.cell(0, 0).fg, qRgb(0, 0, 238));
        QVERIFY(contrast::contrastRatio(qRgb(0, 0, 187), qRgb(0, 0, 0)) >= contrast::kIndistinguishableContrast);
    }

    void nearInvisiblePaletteColourIsRaised()
    {
        // The reported case: a program falling back to ANSI colours draws
        // "subtle" text in ANSI black (Boilermakers #262626) or 256-colour
        // grey 235 on the black default background.
        Terminal t(5, 40);
        t.setColorScheme(ColorScheme::byId(QStringLiteral("boilermakers")));
        const ColorScheme s = t.colorScheme();
        t.feed("\x1b[30mK\x1b[0;38;5;235mG\x1b[0;38;5;0mZ\x1b[0;7;30mI");
        for (int col = 0; col < 3; ++col) {
            const QRgb fg = t.cell(0, col).fg;
            QVERIFY2(contrast::contrastRatio(fg, s.background) >= contrast::kMinReadableContrast,
                     qPrintable(QStringLiteral("col %1 %2").arg(col).arg(fg, 6, 16)));
            QVERIFY(contrast::contrastRatio(fg, s.background) < 3.2);
        }
        // Reverse video with ANSI black: the (raised) grey fills the cell.
        QCOMPARE(t.cell(0, 3).bg, t.cell(0, 0).fg);
        QCOMPARE(t.cell(0, 3).fg, s.background);
    }

    void schemeChangeRecolorsExistingCells()
    {
        Terminal t(5, 20);
        t.feed("\x1b[31mR");
        t.setColorScheme(ColorScheme::byId(QStringLiteral("solarized-dark")));
        QCOMPARE(t.cell(0, 0).fg, ColorScheme::byId(QStringLiteral("solarized-dark")).ansi[1]);
    }

    void wideChars()
    {
        Terminal t(5, 20);
        t.feed("a\xe4\xb8\xad" "b"); // a, U+4E2D (wide), b
        QCOMPARE(t.cell(0, 1).width, 2);
        QCOMPARE(t.cell(0, 2).width, 0);
        QCOMPARE(t.lineText(0), QStringLiteral("a\u4e2db"));
    }

    void scrollback()
    {
        Terminal t(5, 20);
        QSignalSpy spy(&t, &Terminal::scrolledIntoHistory);
        for (int i = 0; i < 10; ++i) {
            t.feed(QStringLiteral("line %1\r\n").arg(i).toUtf8());
        }
        QCOMPARE(t.scrollbackLines(), 6);
        QCOMPARE(t.totalLines(), 11);
        QCOMPARE(t.lineText(0), QStringLiteral("line 0"));
        QCOMPARE(t.lineText(9), QStringLiteral("line 9"));
        QVERIFY(spy.count() > 0);
    }

    void scrollbackLimitDropsOldest()
    {
        Terminal t(3, 20);
        t.setScrollbackLimit(4);
        int dropped = 0;
        connect(&t, &Terminal::scrolledIntoHistory, this, [&dropped](int, int d) { dropped += d; });
        for (int i = 0; i < 20; ++i) {
            t.feed(QStringLiteral("l%1\r\n").arg(i).toUtf8());
        }
        QCOMPARE(t.scrollbackLines(), 4);
        QCOMPARE(dropped, 20 - 2 - 4);
        QCOMPARE(t.lineText(0), QStringLiteral("l14"));
    }

    void clearScrollback()
    {
        Terminal t(3, 20);
        for (int i = 0; i < 10; ++i) {
            t.feed("x\r\n");
        }
        QVERIFY(t.scrollbackLines() > 0);
        t.clearScrollback();
        QCOMPARE(t.scrollbackLines(), 0);
        for (int i = 0; i < 10; ++i) {
            t.feed("x\r\n");
        }
        t.feed("\x1b[3J"); // ED 3: the program asks to erase scrollback
        QCOMPARE(t.scrollbackLines(), 0);
    }

    void resizeGrowPullsHistoryBack()
    {
        Terminal t(3, 20);
        for (int i = 0; i < 6; ++i) {
            t.feed(QStringLiteral("r%1\r\n").arg(i).toUtf8());
        }
        const int before = t.scrollbackLines();
        t.resize(6, 20);
        QCOMPARE(t.rows(), 6);
        QVERIFY(t.scrollbackLines() < before);
        QCOMPARE(t.lineText(0), QStringLiteral("r0"));
    }

    // Found by fuzzing: libvterm aborted ("screen_resize failed to update
    // cursor position") on output followed by a resize. Patched in
    // third_party/libvterm/src/screen.c.
    void resizeAfterOutputDoesNotAbort()
    {
        {
            Terminal t(1, 1);
            t.feed("55");
            t.resize(3, 3);
            QCOMPARE(t.rows(), 3);
        }
        // Small sizes, wrapped output, both screens, every resize direction.
        const QByteArray outputs[] = {"55", "abcdef\r\n12345", "\x1b[?1049hxyz\x1b[?1049l", "x\x1b[5;5Hy\r\nz",
                                      QByteArray(50, 'w')};
        for (const QByteArray &out : outputs) {
            for (int r0 = 1; r0 <= 4; ++r0) {
                for (int c0 = 1; c0 <= 4; ++c0) {
                    Terminal t(r0, c0);
                    t.feed(out);
                    for (int r1 = 1; r1 <= 5; ++r1) {
                        for (int c1 = 1; c1 <= 5; ++c1) {
                            t.resize(r1, c1);
                            t.feed(out.left(3));
                        }
                    }
                }
            }
        }
    }

    // Found by fuzzing: a wide character on a 1-column screen wrote through NULL.
    void wideCharInOneColumn()
    {
        Terminal t(3, 1);
        t.feed("\xe4\xb8\xad\xf0\x9f\x98\x80x\r\n\xe4\xb8\xad");
        t.resize(3, 2);
        t.feed("\xe4\xb8\xad\xe4\xb8\xad");
        t.resize(2, 1);
        t.feed("\xe4\xb8\xad");
        QCOMPARE(t.rows(), 2);
    }

    // Found by fuzzing: CSI numbers overflowed a signed long (UBSan).
    void hugeCsiArguments()
    {
        Terminal t(5, 10);
        t.feed("\x1b[" + QByteArray(60, '9') + "C\x1b[" + QByteArray(60, '4') + ";" + QByteArray(40, '7') + "Hx");
        t.feed("\x1b[" + QByteArray(30, '9') + "m\x1b[1;" + QByteArray(30, '8') + "r");
        t.feed("\x1b]" + QByteArray(30, '2') + ";title\x07");
        QCOMPARE(t.rows(), 5);
    }

    void tooManyCsiArguments()
    {
        // libvterm wrote past args[16] (heap overflow) for long ';' lists.
        Terminal t(5, 20);
        QByteArray semis;
        for (int i = 0; i < 2000; ++i)
            semis += "1;";
        t.feed("\x1b[" + semis + "1m");
        t.feed("\x1b[" + QByteArray(2000, ':') + "m");
        t.feed("\x1b[38:2:59:66:97;48:2:36:40:59;58:2:224:175:104;4:3mok");
        QCOMPARE(t.rows(), 5);
        QVERIFY(t.lineText(0).contains(QStringLiteral("ok")));
    }

    void scrollRegionAfterShrink()
    {
        // A scroll region below the new bottom (top 6 > 3 rows) used to make
        // scroll() call memmove with a negative size on the next line feed.
        Terminal t(9, 10);
        t.feed("\x1b[6;8r");
        t.resize(3, 10);
        t.feed("\x1b[3;1Ha\n\nb\x1b" "D\x1b" "M");
        t.resize(9, 10);
        t.feed("\x1b[?69h\x1b[6;9s\x1b[?6h");
        t.resize(9, 4);
        t.feed("\x1b[9;1Hc\n\x1b" "D\x1b[1Pok");
        QCOMPARE(t.rows(), 9);
    }

    void oscEscC0AtChunkEnd()
    {
        // ESC + C0 at the end of a chunk inside an OSC made libvterm pass a
        // SIZE_MAX-length fragment to the title callback.
        Terminal t(5, 20);
        t.feed(QByteArray("\x1b]2;abc\x1b\x17", 9));
        t.feed("def\x07");
        QVERIFY(t.title().size() < 64);
        t.feed("\x1b]2;ok\x07");
        QCOMPARE(t.title(), QStringLiteral("ok"));
    }

    void repWithoutPreviousChar()
    {
        // REP with no preceding character looped forever in libvterm.
        Terminal t(5, 20);
        t.feed("\x1b[5b");
        t.feed("x\x1b[3b");
        QCOMPARE(t.lineText(0), QStringLiteral("xxxx"));
    }

    void restoreCursorAfterShrink()
    {
        // DECRC restored a cursor saved before a shrink, past the new
        // bottom, and the next glyph wrote out of bounds in libvterm.
        Terminal t(25, 10);
        t.feed("\x1b[20;5H\x1b" "7");
        t.resize(5, 5);
        t.feed("\x1b" "8abcdefgh\r\nij");
        t.feed("\x1b[?1049h\x1b[?1049l");
        QCOMPARE(t.rows(), 5);
    }

    void utf8C1DoesNotMoveCursorBack()
    {
        // U+0082 as UTF-8 text had width -1: cursor column -1, then TBC
        // cleared a tab stop before libvterm's tabstops[] (heap overflow).
        Terminal t(4, 20);
        t.feed("\r\n\xc2\x82\x1b[g\x1b[0g\xc2\x82\xc2\x9b\x1b[Hok");
        QCOMPARE(t.lineText(0), QStringLiteral("ok"));
    }

    void doubleWidthLineInOneColumn()
    {
        // DECDHL/DECDWL on a 1-column screen: row width 0, cursor column -1.
        Terminal t(5, 1);
        t.feed("\x1b#3\x1b[@\x1b[K\x1b[P\x1b#6x\x1b[2@\x1b[X");
        t.resize(3, 7);
        t.feed("\x1b#3\x1b[@ok");
        QCOMPARE(t.rows(), 3);
    }

    void altScreenAndTitle()
    {
        Terminal t(5, 20);
        QSignalSpy spy(&t, &Terminal::titleChanged);
        t.feed("\x1b]2;vim foo.c\x07");
        QCOMPARE(t.title(), QStringLiteral("vim foo.c"));
        QCOMPARE(spy.count(), 1);
        t.feed("\x1b[?1049h");
        QVERIFY(t.altScreen());
        t.feed("\x1b[?1049l");
        QVERIFY(!t.altScreen());
    }

    void mouseModes()
    {
        Terminal t(5, 20);
        QCOMPARE(t.mouseMode(), int(VTERM_PROP_MOUSE_NONE));
        t.feed("\x1b[?1000h");
        QCOMPARE(t.mouseMode(), int(VTERM_PROP_MOUSE_CLICK));
        t.feed("\x1b[?1002h");
        QCOMPARE(t.mouseMode(), int(VTERM_PROP_MOUSE_DRAG));
        t.feed("\x1b[?1003h\x1b[?1006h");
        QCOMPARE(t.mouseMode(), int(VTERM_PROP_MOUSE_MOVE));
        const QByteArray out = collect(t, [&t]() {
            t.sendMouseMove(2, 4, VTERM_MOD_NONE);
            t.sendMouseButton(1, true, VTERM_MOD_NONE);
        });
        QVERIFY2(out.contains("\x1b[<0;5;3M"), out.toHex().constData()); // SGR, 1-based
        t.feed("\x1b[?1003l\x1b[?1002l\x1b[?1000l");
        QCOMPARE(t.mouseMode(), int(VTERM_PROP_MOUSE_NONE));
    }

    void pastePlainAndBracketed()
    {
        Terminal t(5, 20);
        QCOMPARE(collect(t, [&t]() { t.paste(QStringLiteral("a\nb\r\nc")); }), QByteArray("a\rb\rc"));
        t.feed("\x1b[?2004h");
        QCOMPARE(collect(t, [&t]() { t.paste(QStringLiteral("x\ny")); }),
                 QByteArray("\x1b[200~x\ry\x1b[201~"));
        // Embedded end marker cannot terminate the paste early.
        QCOMPARE(collect(t, [&t]() { t.paste(QStringLiteral("a\x1b[201~rm -rf\n")); }),
                 QByteArray("\x1b[200~arm -rf\r\x1b[201~"));
    }

    // Item 4: no control character but TAB/CR survives a paste.
    void pasteStripsControls()
    {
        // The split marker that the old single-pass removal re-assembled.
        QCOMPARE(Terminal::preparePasteBytes(QStringLiteral("x\x1b[20\x1b[201~1~y")), QByteArray("x[201~y"));
        QCOMPARE(Terminal::preparePasteBytes(QStringLiteral("a\x03" "b\x7f\x1b[31mred\u009b2Jx\tq\x1b]52;c;Zm9v\x07")),
                 QByteArray("ab[31mred2Jx\tq]52;c;Zm9v"));
        QCOMPARE(Terminal::preparePasteBytes(QStringLiteral("l1\r\nl2\nl3\r")), QByteArray("l1\rl2\rl3\r"));
        QCOMPARE(Terminal::preparePasteBytes(QStringLiteral("caf\u00e9 \U0001F600")), QStringLiteral("caf\u00e9 \U0001F600").toUtf8());
        QVERIFY(Terminal::preparePasteBytes(QString(QChar(0x1b))).isEmpty());
        // In bracketed mode the only end marker is ours, at the very end.
        Terminal t(5, 20);
        t.feed("\x1b[?2004h");
        const QByteArray out = collect(t, [&t]() { t.paste(QStringLiteral("\x1b[20\x1b[201~1~\x1b\x1b[201~~rm -rf /\n")); });
        QVERIFY(out.startsWith("\x1b[200~"));
        QVERIFY(out.endsWith("\x1b[201~"));
        QCOMPARE(out.count('\x1b'), 2);
    }

    // Fuzz-style: random mixes of controls, marker fragments and Unicode.
    void pasteFuzz()
    {
        QRandomGenerator rng(20261005);
        const QStringList atoms{QStringLiteral("\x1b"), QStringLiteral("["), QStringLiteral("200~"),
                                QStringLiteral("201~"), QStringLiteral("\x1b[201~"), QStringLiteral("\x1b[200~"),
                                QStringLiteral("\r"), QStringLiteral("\n"), QStringLiteral("\t"),
                                QStringLiteral("\x7f"), QStringLiteral("\u009b"), QStringLiteral("\u0090"),
                                QStringLiteral("\u009c"), QStringLiteral("a"), QStringLiteral("\u00e9"),
                                QStringLiteral("\u202e"), QStringLiteral("\U0001F600"), QStringLiteral("1~"),
                                QString(QChar(0)), QStringLiteral("\x03"), QStringLiteral("\x15")};
        Terminal t(5, 20);
        t.feed("\x1b[?2004h");
        for (int round = 0; round < 3000; ++round) {
            QString in;
            const int n = int(rng.bounded(1, 40));
            for (int i = 0; i < n; ++i) {
                if (rng.bounded(4) == 0) {
                    in += QChar(char16_t(rng.bounded(0, 0x250))); // anything in C0..Latin Extended
                } else {
                    in += atoms.at(int(rng.bounded(qsizetype(atoms.size()))));
                }
            }
            const QByteArray b = Terminal::preparePasteBytes(in);
            for (const char32_t u : QString::fromUtf8(b).toUcs4()) {
                const bool bad = (u < 0x20 && u != U'\t' && u != U'\r') || u == 0x7f || (u >= 0x80 && u < 0xa0);
                QVERIFY2(!bad, qPrintable(QStringLiteral("round %1: U+%2 survived in %3")
                                              .arg(round)
                                              .arg(uint(u), 4, 16, QLatin1Char('0'))
                                              .arg(QString::fromLatin1(b.toHex()))));
            }
            QVERIFY(!b.contains('\n'));
            const QByteArray out = collect(t, [&t, &in]() { t.paste(in); });
            if (!b.isEmpty()) {
                QCOMPARE(out, QByteArray("\x1b[200~") + b + QByteArray("\x1b[201~"));
            }
        }
    }

    // Item 7: program titles are capped and can't carry controls or bidi tricks.
    void titleSanitizedAndCapped()
    {
        Terminal t(5, 20);
        t.feed("\x1b]2;safe\u202eexe.txt\u2066x\u2069\u200f\u061c\x07");
        QCOMPARE(t.title(), QStringLiteral("safeexe.txtx"));
        // C1 controls (as UTF-8) inside the OSC string.
        t.feed("\x1b]2;a\xc2\x85" "b\xc2\x9b" "c\x07");
        QCOMPARE(t.title(), QStringLiteral("abc"));
        // A huge title, streamed in pieces, is cut at kMaxTitleBytes.
        QSignalSpy spy(&t, &Terminal::titleChanged);
        t.feed("\x1b]2;");
        const QByteArray chunk(1000, 'x');
        for (int i = 0; i < 2000; ++i) {
            t.feed(chunk);
        }
        t.feed("\x07");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(t.title().size(), qsizetype(Terminal::kMaxTitleBytes));
        // And the next title starts fresh.
        t.feed("\x1b]0;next\x07");
        QCOMPARE(t.title(), QStringLiteral("next"));
        // The window title sanitizes too (session names, any program title).
        QVERIFY(!makeWindowTitle(QStringLiteral("sw\u202e1"), QStringLiteral("a\x1b[2Jb\u2067")).contains(QChar(0x202e)));
        QVERIFY(makeWindowTitle(QStringLiteral("s"), QStringLiteral("a\x1b[2Jb\u2067")).endsWith(QStringLiteral("a[2Jb")));
    }

    void keyEncoding()
    {
        Terminal t(5, 20);
        QCOMPARE(collect(t, [&t]() { t.sendKey(VTERM_KEY_ENTER, VTERM_MOD_NONE); }), QByteArray("\r"));
        QCOMPARE(collect(t, [&t]() { t.sendKey(VTERM_KEY_UP, VTERM_MOD_NONE); }), QByteArray("\x1b[A"));
        QCOMPARE(collect(t, [&t]() { t.sendKey(VTERM_KEY_BACKSPACE, VTERM_MOD_NONE); }), QByteArray("\x7f"));
        QCOMPARE(collect(t, [&t]() { t.sendChar('x', VTERM_MOD_ALT); }), QByteArray("\x1bx"));
        QCOMPARE(collect(t, [&t]() { t.sendChar(0x00e9, VTERM_MOD_NONE); }), QByteArray("\xc3\xa9"));
        t.feed("\x1b[?1h"); // DECCKM: application cursor keys
        QCOMPARE(collect(t, [&t]() { t.sendKey(VTERM_KEY_UP, VTERM_MOD_NONE); }), QByteArray("\x1bOA"));
    }

    void cursorDefaultsToBlinkingBar()
    {
        Terminal t(5, 20);
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(t.cursorBlink());
        QCOMPARE(t.defaultCursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(t.defaultCursorBlink());
        t.feed("hello\r\n");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(t.cursorBlink());
    }

    void programCursorStyleIsHonouredAndDefaultRestores()
    {
        Terminal t(5, 20);
        QSignalSpy damaged(&t, &Terminal::damaged);
        t.feed("\x1b[2 q"); // DECSCUSR 2: steady block
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
        QVERIFY(!t.cursorBlink());
        QVERIFY(damaged.count() > 0); // the view repaints the cursor
        t.feed("\x1b[0 q"); // DECSCUSR 0: back to the user's default, not libvterm's block
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(t.cursorBlink());
        t.feed("\x1b[4 q");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_UNDERLINE));
        t.feed("\x1b[ q"); // DECSCUSR with no parameter (what Neovim sends on exit)
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        t.feed("\x1b[1 q"); // DECSCUSR 1 explicitly asks for a blinking block
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
        QVERIFY(t.cursorBlink());
        t.feed("\x1b[6 q\x1b[?12l"); // steady bar, then DECRST 12 (blink off)
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(!t.cursorBlink());
        t.feed("\x1b[?12h");
        QVERIFY(t.cursorBlink());
        // Order within one chunk matters: the later request wins.
        t.feed("\x1b[0 q\x1b[2 q");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
        t.feed("\x1b[2 q\x1b[0 qtext");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QCOMPARE(t.lineText(0), QStringLiteral("text"));
    }

    void cursorDefaultRestoreSurvivesSplitChunks()
    {
        Terminal t(5, 20);
        t.feed("\x1b[2 q");
        t.feed("ab\x1b");
        t.feed("[");
        t.feed("0");
        t.feed(" ");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
        t.feed("qcd");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(t.cursorBlink());
        QCOMPARE(t.lineText(0), QStringLiteral("abcd"));
        // Lookalikes don't reset the style.
        t.feed("\x1b[2 q\x1b[3q\x1b[10 q\x1b[0;1 q\x1b[0 p");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
    }

    void resetRestoresDefaultCursor()
    {
        Terminal t(5, 20);
        t.feed("\x1b[4 q");
        t.feed("\x1b" "c"); // RIS from the program (`reset`, `tput reset`)
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(t.cursorBlink());
        t.feed("\x1b[2 q");
        t.reset(); // Terminal > Reset
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BAR_LEFT));
        QVERIFY(t.cursorBlink());
    }

    void defaultCursorStyleIsConfigurable()
    {
        Terminal t(5, 20);
        t.setDefaultCursorStyle(VTERM_PROP_CURSORSHAPE_BLOCK, false);
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
        QVERIFY(!t.cursorBlink());
        t.feed("\x1b[6 q\x1b[ q");
        QCOMPARE(t.cursorShape(), int(VTERM_PROP_CURSORSHAPE_BLOCK));
        QVERIFY(!t.cursorBlink());
        t.setDefaultCursorStyle(99, true); // out of range -> built-in default shape
        QCOMPARE(t.cursorShape(), int(Terminal::kDefaultCursorShape));
    }
};

QTEST_GUILESS_MAIN(TstTerminal)
#include "tst_terminal.moc"
