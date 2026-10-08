// GUI behaviour (offscreen): select = copy, right-click pastes CLIPBOARD,
// Ctrl+right-click menu, configurable middle-click, Shift bypasses mouse
// reporting, key encoding, shortcut policy, menu bar and versioned title.
#include "MainWindow.hpp"
#include "Pty.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "WindowTitle.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QImage>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

// Settings go to $XDG_CONFIG_HOME (set per test by CMake), never the real ~/.config.

using namespace zterminal;

class TstView : public QObject
{
    Q_OBJECT

    struct Fixture {
        Terminal term{10, 40};
        TerminalView view{&term};
        QByteArray out;
        Fixture()
        {
            view.resize(view.sizeHint());
            view.show();
            QObject::connect(&term, &Terminal::output, [this](const QByteArray &b) { out += b; });
        }
        // Pixel position of a cell boundary (row, col) inside the viewport.
        QPoint at(int row, int col, bool centre = false) const
        {
            const QSize c = view.cellSize();
            return {2 + col * c.width() + (centre ? c.width() / 2 : 1), 2 + row * c.height() + c.height() / 2};
        }
        void mouse(QEvent::Type type, const QPoint &p, Qt::MouseButton b, Qt::KeyboardModifiers m = {})
        {
            const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton
                                         : type == QEvent::MouseMove        ? Qt::MouseButtons(Qt::LeftButton)
                                                                            : Qt::MouseButtons(b);
            QMouseEvent e(type, QPointF(p), QPointF(view.viewport()->mapToGlobal(p)),
                          type == QEvent::MouseMove ? Qt::NoButton : b, held, m);
            QApplication::sendEvent(view.viewport(), &e);
        }
        void drag(const QPoint &from, const QPoint &to, Qt::KeyboardModifiers m = {})
        {
            mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, m);
            mouse(QEvent::MouseMove, to, Qt::LeftButton, m);
            mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, m);
        }
        void click(const QPoint &p, Qt::MouseButton b, Qt::KeyboardModifiers m = {})
        {
            mouse(QEvent::MouseButtonPress, p, b, m);
            mouse(QEvent::MouseButtonRelease, p, b, m);
        }
    };

private slots:
    void init()
    {
        QApplication::clipboard()->setText(QString(), QClipboard::Clipboard);
    }

    void dragSelectCopiesToClipboard()
    {
        Fixture f;
        f.term.feed("hello brave world");
        f.drag(f.at(0, 6), f.at(0, 11));
        QCOMPARE(f.view.selectedText(), QStringLiteral("brave"));
        QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard), QStringLiteral("brave"));
        if (QApplication::clipboard()->supportsSelection()) {
            QCOMPARE(QApplication::clipboard()->text(QClipboard::Selection), QStringLiteral("brave"));
        }
        QVERIFY(f.out.isEmpty()); // selecting never sends anything to the program
    }

    void copyOnSelectToClipboardCanBeTurnedOff()
    {
        Fixture f;
        MouseSettings m;
        m.copyOnSelectToClipboard = false;
        f.view.setMouseSettings(m);
        QApplication::clipboard()->setText(QStringLiteral("keep"), QClipboard::Clipboard);
        f.term.feed("hello brave world");
        f.drag(f.at(0, 0), f.at(0, 5));
        QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard), QStringLiteral("keep"));
        f.view.copySelection(); // Edit > Copy still fills CLIPBOARD
        QCOMPARE(QApplication::clipboard()->text(QClipboard::Clipboard), QStringLiteral("hello"));
    }

    void doubleAndTripleClick()
    {
        Fixture f;
        f.term.feed("ssh admin@sw1 -p 22");
        f.click(f.at(0, 6, true), Qt::LeftButton);
        f.click(f.at(0, 6, true), Qt::LeftButton);
        QCOMPARE(f.view.selectedText(), QStringLiteral("admin@sw1"));
        f.click(f.at(0, 6, true), Qt::LeftButton);
        QCOMPARE(f.view.selectedText(), QStringLiteral("ssh admin@sw1 -p 22"));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("ssh admin@sw1 -p 22"));
    }

    void plainClickClearsSelection()
    {
        Fixture f;
        f.term.feed("hello brave world");
        f.drag(f.at(0, 0), f.at(0, 5));
        QVERIFY(!f.view.selection().isEmpty());
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        f.click(f.at(0, 8), Qt::LeftButton);
        QVERIFY(f.view.selection().isEmpty());
    }

    void rightClickPastesClipboard()
    {
        Fixture f;
        QApplication::clipboard()->setText(QStringLiteral("show vlan\n"), QClipboard::Clipboard);
        f.click(f.at(1, 1), Qt::RightButton);
        QCOMPARE(f.out, QByteArray("show vlan\r"));
    }

    void ctrlRightClickOpensMenuInsteadOfPasting()
    {
        Fixture f;
        QApplication::clipboard()->setText(QStringLiteral("nope"), QClipboard::Clipboard);
        QSignalSpy spy(&f.view, &TerminalView::contextMenuRequested);
        f.click(f.at(1, 1), Qt::RightButton, Qt::ControlModifier);
        QCOMPARE(spy.count(), 1);
        QVERIFY(f.out.isEmpty());
        // Even when the program grabbed the mouse.
        f.term.feed("\x1b[?1000h");
        f.click(f.at(1, 1), Qt::RightButton, Qt::ControlModifier);
        QCOMPARE(spy.count(), 2);
        QVERIFY(f.out.isEmpty());
    }

    void middleClickIsConfigurable()
    {
        Fixture f;
        f.term.feed("alpha beta");
        f.drag(f.at(0, 0), f.at(0, 5)); // selection "alpha" -> PRIMARY (or internal fallback)
        QApplication::clipboard()->setText(QStringLiteral("CLIP"), QClipboard::Clipboard);

        f.out.clear();
        f.click(f.at(2, 0), Qt::MiddleButton); // default: paste PRIMARY
        QCOMPARE(f.out, QByteArray("alpha"));

        MouseSettings m;
        m.middleClick = MiddleClickAction::PasteClipboard;
        f.view.setMouseSettings(m);
        f.out.clear();
        f.click(f.at(2, 0), Qt::MiddleButton);
        QCOMPARE(f.out, QByteArray("CLIP"));

        m.middleClick = MiddleClickAction::Off;
        f.view.setMouseSettings(m);
        f.out.clear();
        f.click(f.at(2, 0), Qt::MiddleButton);
        QVERIFY(f.out.isEmpty());
    }

    void pasteUsesBracketedPasteWhenEnabled()
    {
        Fixture f;
        f.term.feed("\x1b[?2004h");
        QApplication::clipboard()->setText(QStringLiteral("a\nb"), QClipboard::Clipboard);
        f.click(f.at(0, 0), Qt::RightButton);
        QCOMPARE(f.out, QByteArray("\x1b[200~a\rb\x1b[201~"));
    }

    void mouseReportingAndShiftBypass()
    {
        Fixture f;
        f.term.feed("hello brave world\x1b[?1000h");
        f.click(f.at(0, 2, true), Qt::LeftButton);
        QVERIFY2(f.out.startsWith("\x1b[M"), f.out.toHex().constData()); // X10 encoding to the app
        QVERIFY(f.view.selection().isEmpty());

        f.out.clear();
        QApplication::clipboard()->setText(QStringLiteral("clip"), QClipboard::Clipboard);
        f.click(f.at(0, 2), Qt::RightButton);
        QVERIFY(f.out.startsWith("\x1b[M")); // right button also goes to the app
        QVERIFY(!f.out.contains("clip"));

        f.out.clear();
        f.drag(f.at(0, 6), f.at(0, 11), Qt::ShiftModifier); // Shift: local selection
        QVERIFY(f.out.isEmpty());
        QCOMPARE(f.view.selectedText(), QStringLiteral("brave"));
        f.click(f.at(0, 2), Qt::RightButton, Qt::ShiftModifier); // Shift+right: local paste
        QCOMPARE(f.out, QByteArray("brave"));
    }

    void keyboardEncoding()
    {
        // Ctrl+C must become 0x03 (VINTR). On macOS, main.cpp sets
        // AA_MacDontSwapCtrlAndMeta so Qt::ControlModifier is the physical
        // Control key (otherwise ⌃ arrives as MetaModifier and this path
        // never runs — ping ignores "Ctrl+C" and appears to hang).
        Fixture f;
        QTest::keyClick(&f.view, Qt::Key_A);
        QTest::keyClick(&f.view, Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(&f.view, Qt::Key_BracketLeft, Qt::ControlModifier);
        QTest::keyClick(&f.view, Qt::Key_Return);
        QTest::keyClick(&f.view, Qt::Key_Tab);
        QTest::keyClick(&f.view, Qt::Key_Up);
        QTest::keyClick(&f.view, Qt::Key_F1);
        QTest::keyClick(&f.view, Qt::Key_X, Qt::AltModifier);
        QCOMPARE(f.out, QByteArray("a\x03\x1b\r\t\x1b[A\x1bOP\x1bx"));
    }

    void shiftPageUpScrollsHistoryLocally()
    {
        Fixture f;
        for (int i = 0; i < 40; ++i) {
            f.term.feed(QStringLiteral("line %1\r\n").arg(i).toUtf8());
        }
        f.out.clear();
        QTest::keyClick(&f.view, Qt::Key_PageUp, Qt::ShiftModifier);
        QVERIFY(f.view.scrollOffset() > 0);
        QVERIFY(f.out.isEmpty());
        QTest::keyClick(&f.view, Qt::Key_A); // typing returns to the live screen
        QCOMPARE(f.view.scrollOffset(), 0);
    }

    void mainWindowMenusTitleAndShortcutPolicy()
    {
        LaunchRequest req;
        MainWindow w(req, {});
        w.show();
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("local shell")));
        QVERIFY(w.windowTitle().startsWith(QStringLiteral("zterminal " ZTERMINAL_EXPECTED_VERSION " \u2014 ")));

        QStringList menus;
        for (QAction *a : w.menuBar()->actions()) {
            menus << a->text().remove(QLatin1Char('&'));
        }
        QCOMPARE(menus, (QStringList{QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("View"),
                                     QStringLiteral("Session"), QStringLiteral("Settings"), QStringLiteral("Help")}));

        for (const char *name : {"newSession", "openSavedSession", "saveSession", "close", "quit", "copy", "paste", "selectAll", "fontLarger",
                                 "fontSmaller", "fontReset", "fullScreen", "showMenuBar", "duplicateSession",
                                 "restartSession", "clearScrollback", "resetTerminal", "changeSettings", "preferences",
                                 "about", "find", "checkForUpdates"}) {
            QVERIFY2(w.action(QString::fromLatin1(name)) && w.action(QString::fromLatin1(name))->isEnabled(), name);
        }
        for (const char *name : {"sendBreak"}) {
            QVERIFY2(w.action(QString::fromLatin1(name)) && !w.action(QString::fromLatin1(name))->isEnabled(), name);
        }
        QCOMPARE(w.action(QStringLiteral("copy"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+C")));
        QCOMPARE(w.action(QStringLiteral("paste"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+V")));

        // Plain Ctrl+C belongs to the session; Ctrl+Shift+C is an app shortcut.
        QKeyEvent ctrlC(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(w.view(), &ctrlC);
        QVERIFY(ctrlC.isAccepted());
        QKeyEvent ctrlShiftC(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        ctrlShiftC.ignore();
        QApplication::sendEvent(w.view(), &ctrlShiftC);
        QVERIFY(!ctrlShiftC.isAccepted());

#if defined(Q_OS_MACOS)
        // macOS: Cmd+C / Cmd+V (Qt's Meta, see main.cpp) are app shortcuts too.
        QVERIFY(w.action(QStringLiteral("copy"))->shortcuts().contains(QKeySequence(QStringLiteral("Meta+C"))));
        QVERIFY(w.action(QStringLiteral("paste"))->shortcuts().contains(QKeySequence(QStringLiteral("Meta+V"))));
        for (const Qt::Key key : {Qt::Key_C, Qt::Key_V}) {
            QKeyEvent cmd(QEvent::ShortcutOverride, key, Qt::MetaModifier);
            cmd.ignore();
            QApplication::sendEvent(w.view(), &cmd);
            QVERIFY(!cmd.isAccepted());
        }
        // Cmd+C copies the selection and sends nothing to the session.
        QByteArray sent;
        connect(w.terminal(), &Terminal::output, &w, [&sent](const QByteArray &b) { sent += b; });
        w.terminal()->feed("\x1b[2J\x1b[Hcopy me");
        w.action(QStringLiteral("selectAll"))->trigger();
        QApplication::clipboard()->setText(QStringLiteral("stale"), QClipboard::Clipboard);
        w.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&w));
        w.view()->setFocus();
        QTest::keyClick(w.view(), Qt::Key_C, Qt::MetaModifier);
        QTRY_COMPARE(QApplication::clipboard()->text(QClipboard::Clipboard).trimmed(), QStringLiteral("copy me"));
        QVERIFY(sent.isEmpty());
#endif

        // Program title is appended after the session name.
        w.terminal()->feed("\x1b]2;vim foo.c\x07");
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("local shell"), QStringLiteral("vim foo.c")));

        // Hiding the menu bar keeps shortcuts usable, and it can be restored.
        w.action(QStringLiteral("showMenuBar"))->setChecked(false);
        QVERIFY(!w.menuBar()->isVisible());
        QVERIFY(w.contextMenu()->actions().contains(w.action(QStringLiteral("showMenuBar"))));
        w.action(QStringLiteral("showMenuBar"))->setChecked(true);
        QVERIFY(w.menuBar()->isVisible());
    }

    void menuBarIsInWindowAndAlwaysVisibleOnStartup()
    {
        // A stale 0.1.0 setting that hid the menu bar must not hide it again.
        {
            QSettings s;
            s.setValue(QStringLiteral("view/menuBarVisible"), false);
        }
        LaunchRequest req;
        MainWindow w(req, {});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QMenuBar *bar = w.menuBar();
        QVERIFY(!bar->isNativeMenuBar()); // never exported to a global-menu service
        QVERIFY(bar->isVisible());
        QVERIFY(bar->height() > 0);
        QVERIFY(w.action(QStringLiteral("showMenuBar"))->isChecked());

        QStringList menus;
        for (QAction *a : bar->actions()) {
            QVERIFY(a->menu());
            QVERIFY(a->isVisible());
            menus << a->text().remove(QLatin1Char('&'));
        }
        QCOMPARE(menus, (QStringList{QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("View"),
                                     QStringLiteral("Session"), QStringLiteral("Settings"), QStringLiteral("Help")}));

        // Full screen does not hide it.
        w.action(QStringLiteral("fullScreen"))->setChecked(true);
        QVERIFY(bar->isVisible());
        w.action(QStringLiteral("fullScreen"))->setChecked(false);
        QVERIFY(bar->isVisible());

        // Hiding is per window and is not persisted: the next window shows it again.
        w.action(QStringLiteral("showMenuBar"))->setChecked(false);
        QVERIFY(!bar->isVisible());
        MainWindow w2(req, {});
        w2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w2));
        QVERIFY(w2.menuBar()->isVisible());
        QVERIFY(!w2.menuBar()->isNativeMenuBar());
        // Saving settings drops the obsolete 0.1.0 key.
        AppSettings::load().save();
        QVERIFY(!QSettings().contains(QStringLiteral("view/menuBarVisible")));
    }

    static void focus(TerminalView &v, bool in)
    {
        QFocusEvent e(in ? QEvent::FocusIn : QEvent::FocusOut, Qt::OtherFocusReason);
        QApplication::sendEvent(&v, &e);
    }

    // Pixel of the rendered viewport inside cell (row, col), dx/dy from its top-left.
    static QRgb cellPixel(TerminalView &v, int row, int col, int dx, int dy)
    {
        const QImage img = v.viewport()->grab().toImage();
        const qreal dpr = img.devicePixelRatio();
        const QSize c = v.cellSize();
        return img.pixel(int((2 + col * c.width() + dx) * dpr), int((2 + row * c.height() + dy) * dpr)) | 0xff000000u;
    }

    void cursorIsABlinkingBarByDefault()
    {
        Fixture f;
        f.term.feed("ab");
        focus(f.view, true);
        QVERIFY(f.view.cursorBlinking());
        QVERIFY(f.view.cursorBlinkPhaseOn());
        const QRgb cursor = f.term.colorScheme().cursor | 0xff000000u;
        const QRgb bg = f.term.colorScheme().background | 0xff000000u;
        const int h = f.view.cellSize().height();
        const int w = f.view.cellSize().width();
        // A thin vertical bar at the left edge of the cursor cell, not a block.
        QCOMPARE(cellPixel(f.view, 0, 2, 0, h / 2), cursor);
        QCOMPARE(cellPixel(f.view, 0, 2, 1, h / 2), cursor);
        QCOMPARE(cellPixel(f.view, 0, 2, w - 2, h / 2), bg);

        // It blinks: the "off" phase hides it, the next phase shows it again.
        const int interval = f.view.cursorBlinkInterval();
        QVERIFY(interval > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!f.view.cursorBlinkPhaseOn(), interval * 4);
        QCOMPARE(cellPixel(f.view, 0, 2, 0, h / 2), bg);
        QTRY_VERIFY_WITH_TIMEOUT(f.view.cursorBlinkPhaseOn(), interval * 4);
        QCOMPARE(cellPixel(f.view, 0, 2, 0, h / 2), cursor);
    }

    void cursorBlinkFollowsFocusTypingAndProgram()
    {
        Fixture f;
        QVERIFY(!f.view.cursorBlinking()); // never focused: no timer
        focus(f.view, true);
        QVERIFY(f.view.cursorBlinking());
        QTRY_VERIFY_WITH_TIMEOUT(!f.view.cursorBlinkPhaseOn(), f.view.cursorBlinkInterval() * 4);
        // Typing (and cursor movement) shows the cursor solid again at once.
        QTest::keyClick(&f.view, Qt::Key_X);
        QVERIFY(f.view.cursorBlinkPhaseOn());
        QTRY_VERIFY_WITH_TIMEOUT(!f.view.cursorBlinkPhaseOn(), f.view.cursorBlinkInterval() * 4);
        f.term.feed("z");
        QVERIFY(f.view.cursorBlinkPhaseOn());
        // A steady style from the program stops blinking; the default brings it back.
        f.term.feed("\x1b[2 q");
        QVERIFY(!f.view.cursorBlinking());
        QVERIFY(f.view.cursorBlinkPhaseOn());
        f.term.feed("\x1b[0 q");
        QVERIFY(f.view.cursorBlinking());
        // Unfocused: hollow block, solid, no timer.
        focus(f.view, false);
        QVERIFY(!f.view.cursorBlinking());
        QVERIFY(f.view.cursorBlinkPhaseOn());
    }

    void mainWindowRunsLocalShell()
    {
        qputenv("SHELL", "/bin/sh");
        LaunchRequest req;
        req.kind = LaunchRequest::Kind::Command;
        req.program = QStringLiteral("/bin/sh");
        req.args = {QStringLiteral("-c"), QStringLiteral("printf '\\033[38;2;255;100;0mTRUECOLOR\\033[0m'")};
        MainWindow w(req, {});
        w.show();
        w.startSession();
        QTRY_VERIFY_WITH_TIMEOUT(w.terminal()->lineText(0).contains(QStringLiteral("TRUECOLOR")), 10000);
        QCOMPARE(w.terminal()->cell(0, 0).fg, qRgb(255, 100, 0));
        QTRY_VERIFY_WITH_TIMEOUT(!w.pty()->isRunning(), 10000);
    }
};

QTEST_MAIN(TstView)
#include "tst_view.moc"
