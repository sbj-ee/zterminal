// Tabs (offscreen): each tab is an independent session (local, SSH, serial)
// with its own log/REC state, askpass server and paste "don't ask again";
// tab shortcuts don't take terminal keys; the window title follows the active
// tab; closing a live tab or a window with live tabs asks first.
#include "AppSettings.hpp"
#include "AskpassServer.hpp"
#include "MainWindow.hpp"
#include "Pty.hpp"
#include "SecureBuffer.hpp"
#include "SerialBackend.hpp"
#include "SessionLog.hpp"
#include "SessionStore.hpp"
#include "SessionWidget.hpp"
#include "SocatPair.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "Vault.hpp"
#include "VaultManager.hpp"
#include "WindowTitle.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSerialPort>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

using namespace zterminal;

namespace {

template <typename T>
T *child(QObject *o, const char *name)
{
    T *w = o->findChild<T *>(QString::fromLatin1(name));
    if (!w) {
        qFatal("missing child %s", name);
    }
    return w;
}

void whenModal(const std::function<bool(QWidget *)> &fn)
{
    auto *timer = new QTimer;
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, [timer, fn]() {
        if (QWidget *w = QApplication::activeModalWidget(); w && fn(w)) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

// Answer the next QMessageBox with `button`; records its text.
void answerBox(QMessageBox::StandardButton button, QString *text = nullptr)
{
    whenModal([button, text](QWidget *m) {
        auto *box = qobject_cast<QMessageBox *>(m);
        if (!box) {
            return false;
        }
        if (text) {
            *text = box->text();
        }
        box->button(button)->click();
        return true;
    });
}

QString screen(Terminal *t)
{
    QString all;
    for (int r = 0; r < t->totalLines(); ++r) {
        all += t->lineText(r).trimmed() + QLatin1Char('\n');
    }
    return all;
}

QStringList shArgs(const QString &script)
{
    return {QStringLiteral("-e"), QStringLiteral("sh"), QStringLiteral("-c"), script};
}

QByteArray readFile(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TstTabs : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    SessionStore store;

    // MainWindow with a first tab running `script` (started).
    static void startFirst(MainWindow &w)
    {
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.startSession();
    }

private slots:
    void initTestCase()
    {
        Vault::setKdfOverrideForTests(1, 8192);
        AppSettings{}.save();
    }

    void shortcutsLeaveTerminalKeysAlone()
    {
        MainWindow w(LaunchRequest{}, {});
        QCOMPARE(w.action(QStringLiteral("newTab"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+T")));
        QCOMPARE(w.action(QStringLiteral("closeTab"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+W")));
        QVERIFY(w.action(QStringLiteral("nextTab"))->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_PageDown)));
        QVERIFY(w.action(QStringLiteral("nextTab"))->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Shift+]"))));
        QVERIFY(w.action(QStringLiteral("previousTab"))->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::Key_PageUp)));
        QVERIFY(w.action(QStringLiteral("previousTab"))->shortcuts().contains(QKeySequence(QStringLiteral("Ctrl+Shift+["))));

        // No shortcut is used twice.
        QSet<int> seen;
        for (QAction *a : w.findChildren<QAction *>()) {
            for (const QKeySequence &k : a->shortcuts()) {
                QVERIFY2(!seen.contains(k[0].toCombined()), qPrintable(k.toString()));
                seen.insert(k[0].toCombined());
            }
        }
        // Plain Ctrl+T / Ctrl+W / Ctrl+Tab / Shift+PgUp stay with the session...
        auto toSession = [&w](int key, Qt::KeyboardModifiers mods) {
            QKeyEvent e(QEvent::ShortcutOverride, key, mods);
            e.ignore();
            QApplication::sendEvent(w.view(), &e);
            return e.isAccepted();
        };
        QVERIFY(toSession(Qt::Key_T, Qt::ControlModifier));
        QVERIFY(toSession(Qt::Key_W, Qt::ControlModifier));
        QVERIFY(toSession(Qt::Key_Tab, Qt::ControlModifier));
        // ...the tab shortcuts are the app's.
        QVERIFY(!toSession(Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier));
        QVERIFY(!toSession(Qt::Key_PageDown, Qt::ControlModifier));
        QVERIFY(!toSession(Qt::Key_PageUp, Qt::ControlModifier));
    }

    void tabsAreIndependentSessions()
    {
        MainWindow w(parseCommandLine(shArgs(QStringLiteral("echo FIRST-TAB; exec cat"))), {});
        startFirst(w);
        QCOMPARE(w.tabCount(), 1);
        QVERIFY(w.tabs()->tabBar()->isHidden()); // one tab: looks like the old window
        SessionWidget *a = w.currentSession();

        w.action(QStringLiteral("newTab"))->trigger(); // local shell
        QCOMPARE(w.tabCount(), 2);
        QVERIFY(!w.tabs()->tabBar()->isHidden());
        QCOMPARE(w.tabs()->tabText(1), QStringLiteral("local shell"));
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("local shell")));
        SessionWidget *b = w.addTab(parseCommandLine(shArgs(QStringLiteral("echo THIRD-TAB; exec cat"))), {});
        QCOMPARE(w.tabCount(), 3);
        QVERIFY(a->terminal() != b->terminal());
        QVERIFY(a->pty()->pid() != b->pty()->pid());
        QTRY_VERIFY_WITH_TIMEOUT(screen(a->terminal()).contains(QStringLiteral("FIRST-TAB")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(screen(b->terminal()).contains(QStringLiteral("THIRD-TAB")), 10000);
        QVERIFY(!screen(a->terminal()).contains(QStringLiteral("THIRD-TAB")));

        // Typing goes to the current tab only.
        QCOMPARE(w.currentSession(), b);
        QTest::keyClicks(w.view(), QStringLiteral("only-b"));
        QTRY_VERIFY_WITH_TIMEOUT(screen(b->terminal()).contains(QStringLiteral("only-b")), 5000);
        QTest::qWait(100);
        QVERIFY(!screen(a->terminal()).contains(QStringLiteral("only-b")));

        // Next / previous wrap around.
        w.nextTab();
        QCOMPARE(w.currentSession(), a);
        w.previousTab();
        QCOMPARE(w.currentSession(), b);
        QTest::keyClick(w.view(), Qt::Key_PageUp, Qt::ControlModifier);
        QCOMPARE(w.tabs()->currentIndex(), 1);
        QTest::keyClick(w.view(), Qt::Key_PageDown, Qt::ControlModifier);
        QCOMPARE(w.currentSession(), b);

        // Restart in one tab doesn't touch the others.
        const qint64 pidA = a->pty()->pid();
        a->pty()->terminate();
        QTRY_VERIFY_WITH_TIMEOUT(!a->pty()->isRunning(), 5000);
        QVERIFY(b->pty()->isRunning());
        QVERIFY(pidA > 0);
    }

    void titleFollowsActiveTab()
    {
        QVERIFY(store.save([] {
            SessionConfig s;
            s.name = QStringLiteral("lab box");
            return s;
        }()));
        MainWindow w(parseCommandLine({QStringLiteral("lab box")}), {QStringLiteral("lab box")});
        startFirst(w);
        QCOMPARE(w.tabs()->tabText(0), QStringLiteral("lab box"));
        w.addTab(parseCommandLine(shArgs(QStringLiteral("printf '\\033]0;vim notes.txt\\007'; exec cat"))), {});
        QCOMPARE(w.tabs()->tabText(1), QStringLiteral("sh"));
        QTRY_COMPARE_WITH_TIMEOUT(w.windowTitle(), makeWindowTitle(QStringLiteral("sh"), QStringLiteral("vim notes.txt")), 5000);
        w.tabs()->setCurrentIndex(0);
        // (The user's shell may set its own title; it's appended after the session.)
        QVERIFY2(w.windowTitle().startsWith(makeWindowTitle(QStringLiteral("lab box"))), qPrintable(w.windowTitle()));
        QVERIFY(!w.windowTitle().contains(QStringLiteral("vim notes.txt")));
        QVERIFY(w.windowTitle().startsWith(QStringLiteral("zterminal " ZTERMINAL_EXPECTED_VERSION " \u2014 ")));
    }

    void loggingAndRecArePerTab()
    {
        const QString logDir = tmp.filePath(QStringLiteral("logs"));
        AppSettings s;
        s.logDirectory = logDir;
        s.save();
        MainWindow w(parseCommandLine(shArgs(QStringLiteral("sleep 0.5; echo ALPHA-OUT; exec cat"))), {});
        startFirst(w);
        SessionWidget *a = w.currentSession();
        QVERIFY(w.startLogging());
        QVERIFY(!w.recIndicator()->isHidden());
        QVERIFY(w.windowTitle().endsWith(QStringLiteral(" [REC]")));

        SessionWidget *b = w.addTab(parseCommandLine(shArgs(QStringLiteral("sleep 0.5; echo BRAVO-OUT; exec cat"))), {});
        QVERIFY(w.recIndicator()->isHidden()); // the active tab isn't logging
        QVERIFY(!w.windowTitle().endsWith(QStringLiteral(" [REC]")));
        QVERIFY(w.tabs()->tabText(0).startsWith(QStringLiteral("\u25CF "))); // the other tab still records
        QVERIFY(!w.tabs()->tabText(1).startsWith(QStringLiteral("\u25CF ")));
        QCOMPARE(w.action(QStringLiteral("toggleLogging"))->text(), QStringLiteral("Start &Logging"));
        QVERIFY(b->sessionLog() != a->sessionLog());

        w.action(QStringLiteral("toggleLogging"))->trigger(); // logs tab b too, to its own file
        QVERIFY(b->isLogging());
        QVERIFY(a->sessionLog()->path() != b->sessionLog()->path());
        QTRY_VERIFY_WITH_TIMEOUT(screen(a->terminal()).contains(QStringLiteral("ALPHA-OUT")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(screen(b->terminal()).contains(QStringLiteral("BRAVO-OUT")), 10000);
        w.tabs()->setCurrentIndex(0);
        QVERIFY(!w.recIndicator()->isHidden());
        w.stopLogging(); // only tab a
        QVERIFY(!a->isLogging());
        QVERIFY(b->isLogging());
        QVERIFY(w.recIndicator()->isHidden());
        b->stopLogging();
        const QByteArray la = readFile(a->sessionLog()->path());
        const QByteArray lb = readFile(b->sessionLog()->path());
        QVERIFY(la.contains("ALPHA-OUT"));
        QVERIFY(!la.contains("BRAVO-OUT"));
        QVERIFY(lb.contains("BRAVO-OUT"));
        QVERIFY(!lb.contains("ALPHA-OUT"));
        AppSettings{}.save();
    }

    void pasteDontAskAgainIsPerTab()
    {
        MainWindow w(parseCommandLine(shArgs(QStringLiteral("exec cat"))), {});
        startFirst(w);
        SessionWidget *a = w.currentSession();
        SessionWidget *b = w.addTab(parseCommandLine(shArgs(QStringLiteral("exec cat"))), {});
        QApplication::clipboard()->setText(QStringLiteral("one\ntwo\n"));
        w.tabs()->setCurrentIndex(0);
        whenModal([](QWidget *m) {
            if (m->objectName() != QLatin1String("pasteConfirmDialog")) {
                return false;
            }
            child<QCheckBox>(m, "dontAskAgain")->setChecked(true);
            child<QPushButton>(m, "pasteButton")->click();
            return true;
        });
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(a->pasteConfirmSkipped());
        QVERIFY(!b->pasteConfirmSkipped());
        // Tab b still asks.
        w.tabs()->setCurrentIndex(1);
        bool asked = false;
        whenModal([&asked](QWidget *m) {
            if (m->objectName() != QLatin1String("pasteConfirmDialog")) {
                return false;
            }
            asked = true;
            QMetaObject::invokeMethod(m, "reject");
            return true;
        });
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(asked);
    }

    void closingLiveTabsAsks()
    {
        MainWindow w(parseCommandLine(shArgs(QStringLiteral("exec cat"))), {});
        startFirst(w);
        w.addTab(parseCommandLine(shArgs(QStringLiteral("exec cat"))), {});
        SessionWidget *done = w.addTab(parseCommandLine(shArgs(QStringLiteral("true"))), {});
        QTRY_VERIFY_WITH_TIMEOUT(!done->isLive(), 5000);
        QCOMPARE(w.liveTabCount(), 2);

        // A finished session closes without a question.
        w.action(QStringLiteral("closeTab"))->trigger();
        QCOMPARE(w.tabCount(), 2);

        // Live: Cancel keeps it, Close ends it.
        QString text;
        answerBox(QMessageBox::Cancel, &text);
        w.action(QStringLiteral("closeTab"))->trigger();
        QCOMPARE(w.tabCount(), 2);
        QVERIFY2(text.contains(QStringLiteral("still running")), qPrintable(text));
        SessionWidget *victim = w.currentSession();
        const qint64 pid = victim->pty()->pid();
        answerBox(QMessageBox::Close);
        w.action(QStringLiteral("closeTab"))->trigger();
        QCOMPARE(w.tabCount(), 1);
        QVERIFY(pid > 0);

        // Closing the window with a live tab asks too.
        answerBox(QMessageBox::Cancel, &text);
        w.close();
        QVERIFY(w.isVisible());
        QVERIFY2(text.contains(QStringLiteral("1 tab has a running session")), qPrintable(text));
        w.addTab(parseCommandLine(shArgs(QStringLiteral("exec cat"))), {});
        answerBox(QMessageBox::Cancel, &text);
        w.close();
        QVERIFY(text.contains(QStringLiteral("2 tabs have running sessions")));
        QVERIFY(w.isVisible());
        answerBox(QMessageBox::Close);
        w.close();
        QVERIFY(!w.isVisible());
        QCOMPARE(w.liveTabCount(), 0);
    }

    void closingTheLastTabClosesTheWindow()
    {
        MainWindow w(parseCommandLine(shArgs(QStringLiteral("exec cat"))), {});
        startFirst(w);
        answerBox(QMessageBox::Close); // one question only (the tab's)
        w.action(QStringLiteral("closeTab"))->trigger();
        QVERIFY(!w.isVisible());
        QVERIFY(!w.currentSession()->isLive());
    }

    void localSshAndSerialTabsWithPerTabAskpass()
    {
        if (!SocatPair::available()) {
            QSKIP("socat not installed");
        }
        // Fake ssh: asks its askpass once and shows the answer.
        QTemporaryDir bin;
        {
            QFile f(bin.filePath(QStringLiteral("ssh")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\nfor a in \"$@\"; do h=\"$a\"; done\n"
                    "printf 'ssh to %s got [%s]\\n' \"$h\" \"$(\"$SSH_ASKPASS\" \"$h password: \")\"\nexec cat\n");
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(bin.path()) + ':' + oldPath);

        VaultManager &vm = VaultManager::instance();
        vm.setPath(tmp.filePath(QStringLiteral("vault/vault.bin")));
        vm.setAutoLockIntervalMsForTests(0);
        QVERIFY(vm.vault().create(SecureBuffer::fromQString(QStringLiteral("master pw"))));
        vm.noteUnlocked();
        for (const char *n : {"sw-a", "sw-b"}) {
            SessionConfig s;
            s.name = QString::fromLatin1(n);
            s.type = SessionConfig::Type::Ssh;
            s.host = s.name + QStringLiteral(".example");
            s.useStoredPassword = true;
            QVERIFY(store.save(s));
            QVERIFY(vm.vault().setSecret(QStringLiteral("ssh-password/") + s.name,
                                         SecureBuffer::fromQString(QStringLiteral("pw-of-") + s.name)));
        }
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig con;
        con.name = QStringLiteral("SG250 console");
        con.type = SessionConfig::Type::Serial;
        con.serialDevice = cable.a;
        QVERIFY(store.save(con));

        MainWindow w(LaunchRequest{}, {}); // local shell
        startFirst(w);
        SessionWidget *local = w.currentSession();
        SessionWidget *a = w.addTab(parseCommandLine({QStringLiteral("sw-a")}), {QStringLiteral("sw-a")});
        SessionWidget *b = w.addTab(parseCommandLine({QStringLiteral("sw-b")}), {QStringLiteral("sw-b")});
        SessionWidget *ser = w.addTab(parseCommandLine({con.name}), {con.name});
        QCOMPARE(w.tabCount(), 4);
        QCOMPARE(w.tabs()->tabText(3), QStringLiteral("SG250 console"));
        QVERIFY(a->askpassServer() && b->askpassServer());
        QVERIFY(a->askpassServer() != b->askpassServer());
        QVERIFY(!local->askpassServer());
        QTRY_VERIFY_WITH_TIMEOUT(screen(a->terminal()).contains(QStringLiteral("ssh to sw-a.example got [pw-of-sw-a]")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(screen(b->terminal()).contains(QStringLiteral("ssh to sw-b.example got [pw-of-sw-b]")), 10000);

        // Serial tab: its own port; Send Break / Send Stored Login follow the tab.
        QVERIFY(ser->serial()->isOpen());
        QVERIFY(!a->serial()->isOpen());
        QVERIFY(w.action(QStringLiteral("sendBreak"))->isEnabled());
        QVERIFY(w.action(QStringLiteral("sendStoredLogin"))->isEnabled());
        QSerialPort dev;
        dev.setPortName(cable.b);
        QVERIFY(dev.open(QIODevice::ReadWrite));
        dev.write("SG250-core#");
        QTRY_VERIFY_WITH_TIMEOUT(screen(ser->terminal()).contains(QStringLiteral("SG250-core#")), 5000);
        QVERIFY(!screen(local->terminal()).contains(QStringLiteral("SG250-core#")));
        w.tabs()->setCurrentIndex(1);
        QVERIFY(!w.action(QStringLiteral("sendBreak"))->isEnabled());
        QVERIFY(!w.action(QStringLiteral("sendStoredLogin"))->isEnabled());
        QCOMPARE(w.liveTabCount(), 4);

        // The banner belongs to the serial tab only.
        cable.stop();
        QTRY_VERIFY_WITH_TIMEOUT(!ser->banner()->isHidden(), 5000);
        QVERIFY(ser->bannerText().contains(QStringLiteral("Disconnected")));
        QVERIFY(a->banner()->isHidden());
        QVERIFY(local->banner()->isHidden());
        qputenv("PATH", oldPath);
    }
};

QTEST_MAIN(TstTabs)
#include "tst_tabs.moc"
