// Regression test for "the vault keeps asking for the master password while
// it is unlocked" (0.6.0/0.7.0, fixed in 0.7.1).
//
// Root cause: new windows were started as a *new zterminal process* (0.6.0:
// File > Open Saved Session, the session dialog's Open, Session > Duplicate;
// 0.7.0 with tabs: still File > New Window), whose own VaultManager had never
// been unlocked, so it asked again. Now new windows open in the running process
// and share its one unlocked vault. Plus idle-timer fixes (see the last tests).
//
// Each test: unlock once, then run a path and count master-password dialogs.
// A dialog that shows up when it shouldn't is counted and cancelled.
#include "AppSettings.hpp"
#include "AskpassServer.hpp"
#include "MainWindow.hpp"
#include "Pty.hpp"
#include "SecureBuffer.hpp"
#include "SerialBackend.hpp"
#include "SessionDialog.hpp"
#include "SessionStore.hpp"
#include "SocatPair.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "Vault.hpp"
#include "VaultDialogs.hpp"
#include "VaultManager.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QSerialPort>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace zterminal;

namespace {

const char *kSecret = "Sw0rdfish-shared-askpass";
const char *kSerialSecret = "c1sc0-Secret";
const char *kMaster = "correct horse battery";

SecureBuffer sb(const char *s)
{
    return SecureBuffer::fromQString(QString::fromUtf8(s));
}

QString str(const SecureBuffer *b)
{
    return b ? QString::fromUtf8(reinterpret_cast<const char *>(b->data()), qsizetype(b->size())) : QString();
}

QByteArray readFile(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QSet<MainWindow *> windows()
{
    QSet<MainWindow *> out;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *m = qobject_cast<MainWindow *>(w)) {
            out.insert(m);
        }
    }
    return out;
}

} // namespace

class TstVaultShared : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    QTemporaryDir bin;
    SessionStore store;
    QByteArray oldPath;
    QTimer watcher;
    int prompts = 0;          // master-password dialogs seen
    QString answer;           // what to type into the next one ("" = Cancel)
    int n = 0;

    VaultManager &vm() { return VaultManager::instance(); }

    void freshVault()
    {
        vm().setPath(tmp.filePath(QStringLiteral("vault%1/vault.bin").arg(++n)));
        vm().setAutoLockIntervalMsForTests(0);
        vm().setAutoLockMinutes(15);
        QVERIFY(vm().vault().create(sb(kMaster)));
        QVERIFY(vm().vault().setSecret(QStringLiteral("ssh-password/core-sw1 pw"), sb(kSecret)));
        vm().lock();
        QVERIFY(!vm().isUnlocked());
    }

    // Unlock exactly once, through the same menu item Stephen uses.
    void unlockOnceViaMenu(MainWindow &w)
    {
        answer = QString::fromUtf8(kMaster);
        w.action(QStringLiteral("unlockVault"))->trigger();
        QCOMPARE(prompts, 1);
        QVERIFY(vm().isUnlocked());
        answer.clear();
        prompts = 0;
    }

    SessionConfig sshSession()
    {
        SessionConfig s;
        s.name = QStringLiteral("core-sw1 pw");
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("core-sw1.example");
        s.user = QStringLiteral("stevebj");
        s.useStoredPassword = true;
        return s;
    }

    QString out(const QString &f) const { return bin.filePath(QStringLiteral("out/") + f); }

    // Opens `cfg` in a new tab of `w`, the way File > Open Saved Session does.
    SessionWidget *openTab(MainWindow &w, const SessionConfig &cfg)
    {
        QString err;
        if (!w.openInNewTab(cfg, &err)) {
            qWarning("openInNewTab failed: %s", qPrintable(err));
            return nullptr;
        }
        return w.currentSession();
    }

    // File > New Window; returns the new window if it opened in this process.
    MainWindow *newWindow(MainWindow &from)
    {
        const QSet<MainWindow *> before = windows();
        from.action(QStringLiteral("newWindow"))->trigger();
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 3000) {
            for (MainWindow *m : windows()) {
                if (!before.contains(m)) {
                    return m;
                }
            }
            QTest::qWait(20);
        }
        return nullptr;
    }

    void closeExtraWindows()
    {
        for (MainWindow *m : windows()) {
            if (m->testAttribute(Qt::WA_DeleteOnClose)) {
                while (m->isVisible() && m->tabCount() > 0) { // the last one closes the window
                    m->closeTab(m->tabCount() - 1, /*force=*/true); // no confirmation
                }
            }
        }
        QTest::qWait(50);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // The new window's fake ssh asked SSH_ASKPASS once and got the secret.
    void expectAskpassAnswered()
    {
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out(QStringLiteral("rc"))), 10000);
        QCOMPARE(readFile(out(QStringLiteral("rc"))).trimmed(), QByteArray("0"));
        QCOMPARE(readFile(out(QStringLiteral("answer"))), QByteArray(kSecret) + '\n');
        QFile::remove(out(QStringLiteral("rc")));
        QFile::remove(out(QStringLiteral("answer")));
    }

private slots:
    void initTestCase()
    {
        Vault::setKdfOverrideForTests(1, 8192);
        QVERIFY(!qEnvironmentVariable("ZTERMINAL_ASKPASS").isEmpty()); // set by CMake
        QVERIFY(SocatPair::available());                                 // CI installs socat: no skipping
        // A fake `ssh` that asks $SSH_ASKPASS once, like ssh's first password prompt.
        QVERIFY(QDir().mkpath(bin.filePath(QStringLiteral("out"))));
        QFile f(bin.filePath(QStringLiteral("ssh")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\n"
                "o=\"$ZT_FAKE_OUT\"\n"
                "\"$SSH_ASKPASS\" \"stevebj@core-sw1's password: \" > \"$o/answer\"; echo $? > \"$o/rc\"\n"
                "echo fake-ssh-finished\n"
                "exec sleep 30\n");
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("ZT_VAULTSHARED_CHILD", "1"); // inherited by anything we spawn
        oldPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(bin.path()) + ':' + oldPath);
        qputenv("ZT_FAKE_OUT", QFile::encodeName(bin.filePath(QStringLiteral("out"))));
        QVERIFY(store.save(sshSession()));

        // Any master-password dialog, wherever it comes from, is counted.
        watcher.setInterval(10);
        connect(&watcher, &QTimer::timeout, this, [this]() {
            QWidget *m = QApplication::activeModalWidget();
            auto *d = qobject_cast<UnlockVaultDialog *>(m);
            if (!d || d->property("zt-seen").toBool()) {
                if (auto *c = qobject_cast<CreateVaultDialog *>(m)) {
                    ++prompts;
                    c->reject();
                }
                return;
            }
            d->setProperty("zt-seen", true);
            ++prompts;
            if (answer.isEmpty()) {
                d->reject();
            } else {
                d->findChild<QLineEdit *>(QStringLiteral("masterPassword"))->setText(answer);
                d->findChild<QPushButton *>(QStringLiteral("unlock"))->click();
            }
        });
        watcher.start();
    }

    void init()
    {
        prompts = 0;
        answer.clear();
        freshVault();
    }

    void cleanup()
    {
        closeExtraWindows();
        vm().setAutoLockIntervalMsForTests(0);
        vm().lock();
    }

    // Stephen's repro: unlock, then connect to a saved session (a new tab since 0.7.0).
    void savedSessionAfterUnlockDoesNotPrompt()
    {
        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        SessionWidget *t = openTab(a, sshSession());
        QVERIFY(t);
        QCOMPARE(t->sessionName(), QStringLiteral("core-sw1 pw"));
        QCOMPARE(prompts, 0);
        QVERIFY(t->askpassServer());
        // SSH_ASKPASS path: the helper asked *this* app over the one-shot socket.
        expectAskpassAnswered();
        QCOMPARE(prompts, 0);
        QVERIFY(vm().isUnlocked());
    }

    // The same repro from File > New Window: until 0.7.0 that was a new
    // zterminal process with its own locked vault, so it asked again.
    void newWindowAfterUnlockDoesNotPrompt()
    {
        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        MainWindow *b = newWindow(a);
        QVERIFY2(b, "File > New Window did not open in this process (it would have its own locked vault)");
        QVERIFY(b->pty()->isRunning()); // its first tab (local shell) started
        QVERIFY(!b->action(QStringLiteral("unlockVault"))->isEnabled());
        QVERIFY(b->action(QStringLiteral("lockVault"))->isEnabled());
        SessionWidget *t = openTab(*b, sshSession());
        QVERIFY(t);
        QCOMPARE(prompts, 0);
        QVERIFY(t->askpassServer());
        expectAskpassAnswered();
        QCOMPARE(prompts, 0);
    }

    void duplicateAfterUnlockDoesNotPrompt()
    {
        MainWindow a(parseCommandLine({QStringLiteral("core-sw1 pw")}), {QStringLiteral("core-sw1 pw")});
        a.show();
        unlockOnceViaMenu(a);
        a.startSession();
        expectAskpassAnswered();
        a.action(QStringLiteral("duplicateSession"))->trigger();
        QCOMPARE(a.tabCount(), 2);
        expectAskpassAnswered();
        QCOMPARE(prompts, 0);
    }

    void vaultMenuAndSessionEditDoNotPrompt()
    {
        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        MainWindow *b = newWindow(a);
        QVERIFY(b);
        // Settings > Password Vault > Unlock while unlocked: nothing to ask.
        QAction *unlock = b->action(QStringLiteral("unlockVault"));
        unlock->setEnabled(true); // even if it were reachable
        unlock->trigger();
        QTest::qWait(100);
        QCOMPARE(prompts, 0);
        // Editing a session (saving a new password) from the second window.
        SessionDialog d(store, sshSession(), b);
        d.findChild<QLineEdit *>(QStringLiteral("sshPassword"))->setText(QStringLiteral("new-pw"));
        QVERIFY2(d.saveCurrent(), qPrintable(d.errorText()));
        QCOMPARE(prompts, 0);
        QCOMPARE(str(vm().vault().secret(QStringLiteral("ssh-password/core-sw1 pw"))), QStringLiteral("new-pw"));
    }

    void sendStoredLoginAfterUnlockDoesNotPrompt()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig s;
        s.name = QStringLiteral("SG250 shared");
        s.type = SessionConfig::Type::Serial;
        s.serialDevice = cable.a;
        s.loginUser = QStringLiteral("admin");
        QVERIFY(store.save(s));
        QVERIFY(vm().vault().unlock(sb(kMaster)));
        QVERIFY(vm().vault().setSecret(QStringLiteral("serial-password/SG250 shared"), sb(kSerialSecret)));
        vm().lock();

        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        MainWindow *b = newWindow(a);
        QVERIFY(b);
        SessionWidget *t = openTab(*b, s);
        QVERIFY(t);
        QTRY_VERIFY_WITH_TIMEOUT(t->serial()->isOpen(), 5000);
        QSerialPort dev;
        dev.setPortName(cable.b);
        QVERIFY(dev.open(QIODevice::ReadWrite));
        QByteArray got;
        connect(&dev, &QSerialPort::readyRead, this, [&]() { got += dev.readAll(); });
        b->action(QStringLiteral("sendStoredLogin"))->trigger();
        QCOMPARE(prompts, 0);
        QVERIFY(t->loginPending());
        QTRY_COMPARE_WITH_TIMEOUT(got, QByteArray("admin\r"), 5000);
        dev.write("\r\nPassword: ");
        QTRY_COMPARE_WITH_TIMEOUT(got, QByteArray("admin\r") + kSerialSecret + '\r', 5000);
        QCOMPARE(prompts, 0);
    }

    void manualLockPromptsAgain()
    {
        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        MainWindow *b = newWindow(a);
        QVERIFY(b);
        // Lock Vault in the second window locks it for the whole app.
        b->action(QStringLiteral("lockVault"))->trigger();
        QVERIFY(!vm().isUnlocked());
        QVERIFY(a.action(QStringLiteral("unlockVault"))->isEnabled());
        QVERIFY(!a.action(QStringLiteral("lockVault"))->isEnabled());
        // The next saved session asks once (answered), the one after doesn't.
        answer = QString::fromUtf8(kMaster);
        QVERIFY(openTab(a, sshSession()));
        QCOMPARE(prompts, 1);
        expectAskpassAnswered();
        answer.clear();
        QVERIFY(openTab(*b, sshSession()));
        expectAskpassAnswered();
        QCOMPARE(prompts, 1);
    }

    void idleTimeoutPromptsAgain()
    {
        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        QVERIFY(vm().autoLockArmed());
        vm().setAutoLockIntervalMsForTests(200); // simulated 15-minute idle
        QTRY_VERIFY_WITH_TIMEOUT(!vm().isUnlocked(), 3000);
        QVERIFY(a.action(QStringLiteral("unlockVault"))->isEnabled());
        vm().setAutoLockIntervalMsForTests(0);
        SessionWidget *t = openTab(a, sshSession()); // cancelled -> ssh would ask itself
        QVERIFY(t);
        QCOMPARE(prompts, 1);
        QVERIFY(!t->askpassServer());
    }

    // Applying unchanged settings (every new window does, and so does every
    // settings-file reload) must not push the idle deadline back.
    void idleTimerNotResetBySettingsApply()
    {
        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        vm().setAutoLockIntervalMsForTests(400);
        const AppSettings same = a.settings();
        for (int i = 0; i < 8; ++i) { // 8 x 100 ms > 400 ms, without any user input
            a.setSettings(same);
            QTest::qWait(100);
        }
        QVERIFY2(!vm().isUnlocked(), "re-applying unchanged settings kept postponing the auto-lock");
    }

    // Using a stored password counts as activity: it pushes the deadline back.
    void usingVaultResetsIdleTimer()
    {
        MainWindow a(LaunchRequest{}, {});
        a.show();
        unlockOnceViaMenu(a);
        QElapsedTimer sinceUnlock;
        sinceUnlock.start();
        vm().setAutoLockIntervalMsForTests(1500);
        QTest::qWait(1000);
        QVERIFY(vm().isUnlocked());
        SessionWidget *t = openTab(a, sshSession());
        QVERIFY(t && t->askpassServer()); // the stored password was used here
        QElapsedTimer sinceUse;
        sinceUse.start();
        expectAskpassAnswered();
        while (sinceUnlock.elapsed() < 1800) { // past the deadline counted from the unlock
            QTest::qWait(20);
        }
        QVERIFY2(sinceUse.elapsed() < 1400, "test machine too slow for these timings");
        QVERIFY2(vm().isUnlocked(), "using the vault did not reset the idle timer");
        QCOMPARE(prompts, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!vm().isUnlocked(), 3000);
    }

    void cleanupTestCase()
    {
        watcher.stop();
        closeExtraWindows();
        vm().lock();
        qputenv("PATH", oldPath);
        qunsetenv("ZT_FAKE_OUT");
    }
};

int main(int argc, char **argv)
{
    // Before the fix, File > New Window started a detached copy of the running
    // program (this test). Such a copy must do nothing.
    if (qEnvironmentVariableIsSet("ZT_VAULTSHARED_CHILD")) {
        return 0;
    }
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    TstVaultShared t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_vaultshared.moc"
