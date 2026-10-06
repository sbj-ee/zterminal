// Session logging in the UI (offscreen): Start/Stop Logging + REC indicator,
// Preferences folder/timestamps, auto-log for saved sessions, and that stored
// or typed passwords never reach the log (askpass, vault dialog, no-echo
// prompts, serial Send Stored Login).
#include "AppSettings.hpp"
#include "AskpassServer.hpp"
#include "MainWindow.hpp"
#include "PreferencesDialog.hpp"
#include "Pty.hpp"
#include "SecureBuffer.hpp"
#include "SerialBackend.hpp"
#include "SessionDialog.hpp"
#include "SessionLog.hpp"
#include "SessionStore.hpp"
#include "VaultBindings.hpp"
#include "SocatPair.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "Vault.hpp"
#include "VaultDialogs.hpp"
#include "VaultManager.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSerialPort>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

using namespace zterminal;

namespace {

const char *kSecret = "Sw0rdfish-LOG-askpass";
const char *kMaster = "correct horse battery";

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

void answerUnlock(const QString &password)
{
    whenModal([password](QWidget *m) {
        auto *d = qobject_cast<UnlockVaultDialog *>(m);
        if (!d) {
            return false;
        }
        child<QLineEdit>(d, "masterPassword")->setText(password);
        child<QPushButton>(d, "unlock")->click();
        return true;
    });
}

QByteArray readFile(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString screen(MainWindow &w)
{
    QString all;
    for (int r = 0; r < w.terminal()->totalLines(); ++r) {
        all += w.terminal()->lineText(r).trimmed() + QLatin1Char('\n');
    }
    return all;
}

SecureBuffer sb(const char *s)
{
    return SecureBuffer::fromQString(QString::fromUtf8(s));
}

LaunchRequest shCommand(const QString &script)
{
    return parseCommandLine({QStringLiteral("-e"), QStringLiteral("sh"), QStringLiteral("-c"), script});
}

} // namespace

class TstLogUi : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    SessionStore store;
    QString logDir;
    int n = 0;

    void useLogSettings(bool timestamps)
    {
        logDir = tmp.filePath(QStringLiteral("logs%1").arg(++n));
        AppSettings s = AppSettings::load();
        s.logDirectory = logDir;
        s.logTimestamps = timestamps;
        s.save();
    }

    QString onlyLog()
    {
        const QStringList files = QDir(logDir).entryList({QStringLiteral("*.log")}, QDir::Files);
        return files.size() == 1 ? QDir(logDir).filePath(files.front()) : QString();
    }

private slots:
    void initTestCase()
    {
        Vault::setKdfOverrideForTests(1, 8192);
        QVERIFY(!qEnvironmentVariable("ZTERMINAL_ASKPASS").isEmpty());
        // Release builds ignore $ZTERMINAL_ASKPASS (ZTERMINAL_DEV_OVERRIDES); CMake passes it to tests.
        AskpassServer::setHelperPathForTests(qEnvironmentVariable("ZTERMINAL_ASKPASS"));
    }

    void preferencesAndDefaults()
    {
        AppSettings d;
        QVERIFY(d.logDirectory.isEmpty());
        QVERIFY(!d.logTimestamps);
        QCOMPARE(d.effectiveLogDirectory(), QDir::homePath() + QStringLiteral("/zterminal-logs"));
        d.logDirectory = QStringLiteral("~/sw-logs");
        QCOMPARE(d.effectiveLogDirectory(), QDir::homePath() + QStringLiteral("/sw-logs"));
        PreferencesDialog dlg(AppSettings{});
        child<QLineEdit>(&dlg, "logDirectory")->setText(QStringLiteral("/tmp/x"));
        child<QCheckBox>(&dlg, "logTimestamps")->setChecked(true);
        QCOMPARE(dlg.result().logDirectory, QStringLiteral("/tmp/x"));
        QVERIFY(dlg.result().logTimestamps);
    }

    void toggleLoggingWithIndicator()
    {
        useLogSettings(true);
        MainWindow w(shCommand(QStringLiteral("sleep 0.5; printf '\\033[1;31mred\\033[0m text\\r\\nprogress 1%%\\rprogress 99%%\\r\\n'; sleep 30")), {});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QAction *a = w.action(QStringLiteral("toggleLogging"));
        QVERIFY(a);
        QCOMPARE(a->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+G")));
        QCOMPARE(a->text(), QStringLiteral("Start &Logging"));
        QVERIFY(!w.recIndicator()->isVisible());
        QVERIFY(!w.windowTitle().endsWith(QStringLiteral("[REC]")));
        w.startSession();
        QTest::keyClick(w.view(), Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(w.sessionLog()->isActive());
        QVERIFY(w.recIndicator()->isVisible());
        QCOMPARE(w.recIndicator()->text(), QStringLiteral("\u25CF REC"));
        QVERIFY(w.windowTitle().endsWith(QStringLiteral(" [REC]")));
        QCOMPARE(a->text(), QStringLiteral("Stop &Logging"));
        const QString path = w.sessionLog()->path();
        QVERIFY(path.startsWith(logDir + QStringLiteral("/sh_-c_sleep")) || path.startsWith(logDir + QLatin1Char('/')));
        QTRY_VERIFY_WITH_TIMEOUT(readFile(path).contains("progress 99%"), 5000);
        a->trigger(); // stop
        QVERIFY(!w.sessionLog()->isActive());
        QVERIFY(!w.recIndicator()->isVisible());
        QVERIFY(!w.windowTitle().endsWith(QStringLiteral("[REC]")));
        const QByteArray text = readFile(path);
        QVERIFY(!text.contains('\x1b'));
        QVERIFY(!text.contains("progress 1%"));
        QVERIFY(QRegularExpression(QStringLiteral("\\n\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3} red text\\n"))
                    .match(QString::fromUtf8(text))
                    .hasMatch());
        QCOMPARE(QFile::permissions(path) & ~(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser | QFileDevice::WriteUser),
                 QFileDevice::Permissions{});
        w.pty()->terminate();
    }

    void autoLogForSavedSessions()
    {
        useLogSettings(false);
        SessionConfig s;
        s.name = QStringLiteral("lab box/2");
        s.type = SessionConfig::Type::LocalShell;
        s.autoLog = true;
        QVERIFY(store.save(s));
        QVERIFY(readFile(store.filePathFor(s.name)).contains("[logging]\nauto=true"));
        // The session dialog round-trips the option.
        SessionDialog d(store, s);
        QVERIFY(child<QCheckBox>(&d, "autoLog")->isChecked());
        QVERIFY(d.config().autoLog);
        qputenv("SHELL", "/bin/sh");
        MainWindow w(parseCommandLine({s.name}), {s.name});
        w.show();
        w.startSession();
        QVERIFY(w.sessionLog()->isActive());
        QVERIFY(QFileInfo(w.sessionLog()->path()).fileName().startsWith(QStringLiteral("lab_box_2-")));
        w.pty()->write("echo auto-logged-$((40+2))\r");
        QTRY_VERIFY_WITH_TIMEOUT(readFile(w.sessionLog()->path()).contains("auto-logged-42"), 5000);
        w.pty()->terminate();
        // Without the option nothing is logged.
        s.autoLog = false;
        QVERIFY(store.save(s));
        MainWindow off(parseCommandLine({s.name}), {s.name});
        off.show();
        off.startSession();
        QVERIFY(!off.sessionLog()->isActive());
        off.pty()->write("echo shell-up\r");
        QTRY_VERIFY_WITH_TIMEOUT(screen(off).contains(QStringLiteral("shell-up")), 5000); // exec'd before we hang up
        off.pty()->terminate();
    }

    void noEchoPromptsAreNotLogged()
    {
        useLogSettings(false);
        // Even a program that prints while the terminal is in no-echo
        // (password) mode stays out of the log.
        MainWindow w(shCommand(QStringLiteral(
                         "sleep 0.4; echo before-prompt; sleep 0.4; stty -echo; printf 'Password: '; sleep 0.3; "
                         "echo HIDDEN-WHILE-NOECHO; sleep 0.5; stty echo; echo; echo after-prompt; sleep 30")),
                     {});
        w.show();
        QVERIFY(w.startLogging());
        w.startSession();
        QTRY_VERIFY_WITH_TIMEOUT(screen(w).contains(QStringLiteral("after-prompt")), 10000);
        QVERIFY(screen(w).contains(QStringLiteral("HIDDEN-WHILE-NOECHO"))); // shown on screen...
        QTRY_VERIFY2_WITH_TIMEOUT(readFile(w.sessionLog()->path()).contains("after-prompt"),
                                  readFile(w.sessionLog()->path()).constData(), 5000);
        const QByteArray text = readFile(w.sessionLog()->path());
        QVERIFY(!text.contains("HIDDEN")); // ...but not logged
        QVERIFY(text.contains("before-prompt"));
        QVERIFY(text.contains("[zterminal: logging paused: password prompt (terminal echo off)]"));
        w.pty()->terminate();
    }

    void storedPasswordNeverInLog()
    {
        // Fake ssh: prints a banner, asks $SSH_ASKPASS (answer1 = stored
        // password, via the socket), then a second prompt that the helper asks
        // on the terminal (typed by hand, echo off), then output after login.
        QTemporaryDir bin;
        const QString out = bin.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(out));
        {
            QFile f(bin.filePath(QStringLiteral("ssh")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\n"
                    "[ \"$1\" = -G ] && exit 1 # resolveTarget's `ssh -G`: use the session fields\n"
                    "o=\"$ZT_FAKE_OUT\"\n"
                    "echo 'Welcome to core-sw1'\n"
                    "\"$SSH_ASKPASS\" \"admin@core-sw1's password: \" > \"$o/answer1\"\n"
                    "\"$SSH_ASKPASS\" \"admin@core-sw1's password: \" > \"$o/answer2\"\n"
                    "echo 'core-sw1# logged in'\n"
                    "sleep 30\n");
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(bin.path()) + ':' + oldPath);
        qputenv("ZT_FAKE_OUT", QFile::encodeName(out));

        useLogSettings(true);
        VaultManager &vm = VaultManager::instance();
        vm.setPath(tmp.filePath(QStringLiteral("vault/vault.bin")));
        QVERIFY(vm.vault().create(sb(kMaster)));
        QVERIFY(vm.vault().setSecret(QStringLiteral("ssh-password/core-sw1 log"), sb(kSecret)));
        vm.lock();
        SessionConfig s;
        s.name = QStringLiteral("core-sw1 log");
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("core-sw1");
        s.user = QStringLiteral("admin");
        s.useStoredPassword = true;
        s.autoLog = true;
        QVERIFY(store.save(s));

        QString path;
        {
            MainWindow w(parseCommandLine({s.name}), {s.name});
            w.show();
            answerUnlock(QString::fromUtf8(kMaster));
            w.startSession(); // auto-log starts, then the unlock dialog (log paused)
            QVERIFY(w.sessionLog()->isActive());
            path = w.sessionLog()->path();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out + QStringLiteral("/answer1")), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(screen(w).contains(QStringLiteral("admin@core-sw1's password:")), 10000);
            w.pty()->write("typed-by-hand-pw\r");
            QTRY_VERIFY_WITH_TIMEOUT(readFile(path).contains("core-sw1# logged in"), 10000);
        }
        QCOMPARE(readFile(out + QStringLiteral("/answer1")), QByteArray(kSecret) + '\n'); // it did get there
        QCOMPARE(readFile(out + QStringLiteral("/answer2")), QByteArray("typed-by-hand-pw\n"));
        const QByteArray text = readFile(path);
        QVERIFY(!text.contains("Sw0rdfish"));
        QVERIFY(!text.contains("typed-by-hand"));
        QVERIFY(!text.contains(kMaster));
        QVERIFY(text.contains("Welcome to core-sw1"));
        QVERIFY(text.contains("[zterminal: logging paused: vault dialog open]"));
        QVERIFY(text.contains("[zterminal: logging paused: password prompt (terminal echo off)]"));
        QVERIFY(text.contains("=== log stopped"));
        qputenv("PATH", oldPath);
        qunsetenv("ZT_FAKE_OUT");
    }

    void sendStoredLoginIsNotLogged()
    {
        if (!SocatPair::available()) {
            QSKIP("socat not installed");
        }
        SocatPair cable;
        QVERIFY(cable.start());
        useLogSettings(false);
        VaultManager &vm = VaultManager::instance();
        vm.setPath(tmp.filePath(QStringLiteral("vault2/vault.bin")));
        QVERIFY(vm.vault().create(sb(kMaster)));
        vm.noteUnlocked();
        SessionConfig s;
        s.name = QStringLiteral("SG250 log");
        s.type = SessionConfig::Type::Serial;
        s.serialDevice = cable.a;
        s.loginUser = QStringLiteral("admin");
        s.autoLog = true;
        QVERIFY(store.save(s));
        QVERIFY(vaultbind::storeSecret(vm.vault(), s, sb("c1sc0-LOG-secret")));
        MainWindow w(parseCommandLine({s.name}), {s.name});
        w.show();
        w.startSession();
        QVERIFY(w.sessionLog()->isActive());
        QSerialPort dev;
        dev.setPortName(cable.b);
        QVERIFY(dev.open(QIODevice::ReadWrite));
        QByteArray got;
        // A careless device that echoes everything, including the password.
        connect(&dev, &QSerialPort::readyRead, this, [&]() {
            const QByteArray d = dev.readAll();
            got += d;
            dev.write(d);
            if (got == "admin\r") {
                dev.write("\r\nPassword: ");
            }
        });
        dev.write("\r\nSG250 ready\r\nUser Name: ");
        QTRY_VERIFY_WITH_TIMEOUT(readFile(w.sessionLog()->path()).contains("SG250 ready"), 5000);
        QVERIFY(w.sendStoredLogin());
        QTRY_COMPARE_WITH_TIMEOUT(got, QByteArray("admin\rc1sc0-LOG-secret\r"), 5000);
        QTest::qWait(1700); // the pause ends 1.5 s after the password went out
        dev.write("\r\nswitch# show clock\r\n");
        QTRY_VERIFY_WITH_TIMEOUT(readFile(w.sessionLog()->path()).contains("switch# show clock"), 5000);
        const QByteArray text = readFile(w.sessionLog()->path());
        QVERIFY(screen(w).contains(QStringLiteral("c1sc0-LOG-secret"))); // the device echoed it on screen...
        QVERIFY(!text.contains("c1sc0")); // ...the log never saw it
        QVERIFY(text.contains("[zterminal: logging paused: Send Stored Login]"));
        w.stopLogging();
    }

    void cleanupTestCase()
    {
        VaultManager::instance().lock();
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    TstLogUi t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_logui.moc"
