// Emulation core: text, 16/256/truecolor SGR, attributes, scrollback, title,
// bracketed paste, mouse modes and key encoding (libvterm through Terminal).
#include "ColorScheme.hpp"
#include "Terminal.hpp"

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
