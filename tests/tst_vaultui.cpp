// Vault in the UI (offscreen): auto-lock, the create / unlock / change dialogs,
// Settings > Password Vault menu, the session dialog writing passwords to the
// vault (never the INI), SSH via SSH_ASKPASS against a fake `ssh` that checks
// its own /proc/<pid>/cmdline and environ, the locked-at-start prompt, and
// serial Send Stored Login over a socat pty pair.
#include "AppSettings.hpp"
#include "AskpassServer.hpp"
#include "MainWindow.hpp"
#include "PreferencesDialog.hpp"
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
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSerialPort>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

using namespace zterminal;

namespace {

const char *kSecret = "Sw0rdfish-\xe2\x9c\x93-askpass"; // "Sw0rdfish-✓-askpass"
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

// Answer the next unlock dialog with `password` (or Cancel when empty).
void answerUnlock(const QString &password, bool *seen = nullptr)
{
    whenModal([password, seen](QWidget *m) {
        auto *d = qobject_cast<UnlockVaultDialog *>(m);
        if (!d) {
            return false;
        }
        if (seen) {
            *seen = true;
        }
        if (password.isEmpty()) {
            d->reject();
        } else {
            child<QLineEdit>(d, "masterPassword")->setText(password);
            child<QPushButton>(d, "unlock")->click();
        }
        return true;
    });
}

QString screen(MainWindow &w)
{
    QString all;
    for (int r = 0; r < w.terminal()->totalLines(); ++r) {
        all += w.terminal()->lineText(r).trimmed() + QLatin1Char('\n');
    }
    return all;
}

QByteArray readFile(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

SecureBuffer sb(const char *s)
{
    return SecureBuffer::fromQString(QString::fromUtf8(s));
}

QString str(const SecureBuffer *b)
{
    return b ? QString::fromUtf8(reinterpret_cast<const char *>(b->data()), qsizetype(b->size())) : QString();
}

} // namespace

class TstVaultUi : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    SessionStore store;
    int n = 0;

    VaultManager &vm() { return VaultManager::instance(); }

    // Fresh vault file; created (and left unlocked) unless create=false.
    void freshVault(bool create = true)
    {
        vm().setPath(tmp.filePath(QStringLiteral("vault%1/vault.bin").arg(++n)));
        vm().setAutoLockIntervalMsForTests(0);
        vm().setAutoLockMinutes(15);
        if (create) {
            QVERIFY(vm().vault().create(sb(kMaster)));
            vm().noteUnlocked();
        }
    }

    SessionConfig sshSession(const QString &name)
    {
        SessionConfig s;
        s.name = name;
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("core-sw1.example");
        s.user = QStringLiteral("stevebj");
        s.useStoredPassword = true;
        return s;
    }

private slots:
    void initTestCase()
    {
        Vault::setKdfOverrideForTests(1, 8192);
        QVERIFY(!qEnvironmentVariable("ZTERMINAL_ASKPASS").isEmpty()); // set by CMake
    }

    void autoLockLocksAndWipes()
    {
        freshVault();
        QVERIFY(vm().vault().setSecret(QStringLiteral("ssh-password/x"), sb(kSecret)));
        QCOMPARE(vm().autoLockMinutes(), 15);
        QVERIFY(vm().autoLockArmed());
        vm().setAutoLockIntervalMsForTests(150);
        QTRY_VERIFY_WITH_TIMEOUT(!vm().isUnlocked(), 3000);
        QVERIFY(!vm().vault().secret(QStringLiteral("ssh-password/x")));
        QVERIFY(vm().vault().keys().isEmpty());
        QVERIFY(!vm().autoLockArmed());
    }

    void autoLockIsResetByInput()
    {
        freshVault();
        QWidget target;
        target.show();
        vm().setAutoLockIntervalMsForTests(400);
        for (int i = 0; i < 8; ++i) { // 8 x 100 ms of typing > 400 ms
            QTest::keyClick(&target, Qt::Key_A);
            QTest::qWait(100);
            QVERIFY2(vm().isUnlocked(), "locked although the user was typing");
        }
        QTRY_VERIFY_WITH_TIMEOUT(!vm().isUnlocked(), 3000); // then idle -> locks
        // 0 = never.
        freshVault();
        vm().setAutoLockMinutes(0);
        QVERIFY(!vm().autoLockArmed());
    }

    void preferencesSetAutoLock()
    {
        QCOMPARE(AppSettings{}.vaultAutoLockMinutes, 15);
        AppSettings s;
        s.vaultAutoLockMinutes = 5;
        PreferencesDialog dlg(s);
        auto *spin = child<QSpinBox>(&dlg, "vaultAutoLockMinutes");
        QCOMPARE(spin->value(), 5);
        spin->setValue(0);
        QCOMPARE(spin->text(), QStringLiteral("Never"));
        QCOMPARE(dlg.result().vaultAutoLockMinutes, 0);
        // A window applies the preference to the vault.
        freshVault();
        MainWindow w(LaunchRequest{}, {});
        AppSettings t = w.settings();
        t.vaultAutoLockMinutes = 7;
        w.setSettings(t);
        QCOMPARE(vm().autoLockMinutes(), 7);
        t.vaultAutoLockMinutes = 15;
        w.setSettings(t);
    }

    void createDialogWarnsAndValidates()
    {
        freshVault(false);
        CreateVaultDialog d(vm().vault());
        auto *warn = child<QLabel>(&d, "noRecoveryWarning");
        QVERIFY(warn->text().contains(QStringLiteral("NO recovery")));
        QVERIFY(warn->text().contains(QStringLiteral("lost")));
        auto *create = child<QPushButton>(&d, "create");
        auto *pw = child<QLineEdit>(&d, "masterPassword");
        auto *confirm = child<QLineEdit>(&d, "confirmPassword");
        auto *understand = child<QCheckBox>(&d, "understandNoRecovery");
        QCOMPARE(pw->echoMode(), QLineEdit::Password);
        QVERIFY(!create->isEnabled());
        pw->setText(QStringLiteral("short"));
        confirm->setText(QStringLiteral("short"));
        understand->setChecked(true);
        QVERIFY(!create->isEnabled()); // < 8 characters
        pw->setText(QString::fromUtf8(kMaster));
        confirm->setText(QStringLiteral("something else"));
        QVERIFY(!create->isEnabled()); // mismatch
        confirm->setText(QString::fromUtf8(kMaster));
        understand->setChecked(false);
        QVERIFY(!create->isEnabled()); // must acknowledge no recovery
        understand->setChecked(true);
        QVERIFY(create->isEnabled());
        QVERIFY(d.tryCreate());
        QVERIFY(pw->text().isEmpty() && confirm->text().isEmpty()); // fields cleared
        QVERIFY(vm().vault().isUnlocked());
        QVERIFY(QFileInfo::exists(vm().vault().path()));
    }

    void unlockDialogWrongThenRight()
    {
        freshVault();
        vm().lock();
        UnlockVaultDialog d(vm().vault(), QStringLiteral("Session \"x\" uses a stored password."));
        auto *pw = child<QLineEdit>(&d, "masterPassword");
        pw->setText(QStringLiteral("nope nope"));
        QVERIFY(!d.tryUnlock());
        QVERIFY(d.errorText().contains(QStringLiteral("Wrong master password")));
        QVERIFY(pw->text().isEmpty());
        QVERIFY(!vm().isUnlocked());
        pw->setText(QString::fromUtf8(kMaster));
        QVERIFY(d.tryUnlock());
        QVERIFY(vm().isUnlocked());
    }

    void changeMasterPasswordDialog()
    {
        freshVault();
        QVERIFY(vm().vault().setSecret(QStringLiteral("k"), sb("v")));
        ChangeMasterPasswordDialog d(vm().vault());
        child<QLineEdit>(&d, "currentPassword")->setText(QStringLiteral("wrong wrong"));
        child<QLineEdit>(&d, "newPassword")->setText(QStringLiteral("new master 1"));
        child<QLineEdit>(&d, "confirmPassword")->setText(QStringLiteral("new master 1"));
        QVERIFY(!d.tryChange());
        child<QLineEdit>(&d, "currentPassword")->setText(QString::fromUtf8(kMaster));
        child<QLineEdit>(&d, "newPassword")->setText(QStringLiteral("new master 1"));
        child<QLineEdit>(&d, "confirmPassword")->setText(QStringLiteral("new master 1"));
        QVERIFY2(d.tryChange(), qPrintable(d.errorText()));
        vm().lock();
        QVERIFY(!vm().vault().unlock(sb(kMaster)));
        QVERIFY(vm().vault().unlock(sb("new master 1")));
        QCOMPARE(str(vm().vault().secret(QStringLiteral("k"))), QStringLiteral("v"));
    }

    void vaultMenu()
    {
        freshVault(false);
        MainWindow w(LaunchRequest{}, {});
        QAction *unlock = w.action(QStringLiteral("unlockVault"));
        QAction *lock = w.action(QStringLiteral("lockVault"));
        QAction *change = w.action(QStringLiteral("changeMasterPassword"));
        QVERIFY(unlock && lock && change);
        QCOMPARE(lock->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+L")));
        QCOMPARE(unlock->text(), QStringLiteral("&Create Vault\u2026"));
        QVERIFY(!lock->isEnabled() && !change->isEnabled());
        QVERIFY(vm().vault().create(sb(kMaster)));
        vm().noteUnlocked();
        QVERIFY(lock->isEnabled() && change->isEnabled() && !unlock->isEnabled());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QTest::keyClick(w.view(), Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(!vm().isUnlocked());
        QCOMPARE(unlock->text(), QStringLiteral("&Unlock Vault\u2026"));
        QVERIFY(unlock->isEnabled());
        answerUnlock(QString::fromUtf8(kMaster));
        unlock->trigger();
        QVERIFY(vm().isUnlocked());
    }

    void sessionDialogWritesVaultNeverIni()
    {
        freshVault();
        vm().lock(); // the dialog has to ask
        SessionDialog d(store, sshSession(QStringLiteral("stored-pw")));
        auto *use = child<QCheckBox>(&d, "useStoredPassword");
        auto *pw = child<QLineEdit>(&d, "sshPassword");
        QVERIFY(use->isChecked());
        QVERIFY(pw->isEnabled());
        QCOMPARE(pw->echoMode(), QLineEdit::Password);
        pw->setText(QString::fromUtf8(kSecret));
        // Open alone never stores a password.
        QVERIFY(!d.openSession());
        QVERIFY(d.errorText().contains(QStringLiteral("Save")));
        bool asked = false;
        answerUnlock(QString::fromUtf8(kMaster), &asked);
        QVERIFY2(d.saveCurrent(), qPrintable(d.errorText()));
        QVERIFY(asked);
        QVERIFY(pw->text().isEmpty());
        // INI: only the flag, nothing secret.
        const QByteArray ini = readFile(store.filePathFor(QStringLiteral("stored-pw")));
        QVERIFY(!ini.isEmpty());
        QVERIFY(!ini.contains("Sw0rdfish"));
        QVERIFY(!ini.toLower().contains("pass"));
        QVERIFY(ini.contains("authFromVault=true"));
        const auto loaded = store.load(QStringLiteral("stored-pw"));
        QVERIFY(loaded && loaded->useStoredPassword);
        // Vault: the password, encrypted on disk.
        QCOMPARE(str(vm().vault().secret(QStringLiteral("ssh-password/stored-pw"))), QString::fromUtf8(kSecret));
        QVERIFY(!readFile(vm().vault().path()).contains("Sw0rdfish"));
        // Unticking (vault open) forgets it; Delete forgets too.
        use->setChecked(false);
        QVERIFY(!pw->isEnabled());
        QVERIFY(d.saveCurrent());
        QVERIFY(!vm().vault().secret(QStringLiteral("ssh-password/stored-pw")));
        // Locked + cancelled: session saved, password not, and it says so.
        use->setChecked(true);
        pw->setText(QStringLiteral("again"));
        vm().lock();
        answerUnlock(QString());
        QVERIFY(!d.saveCurrent());
        QVERIFY(d.errorText().contains(QStringLiteral("NOT stored")));
        QVERIFY(pw->text().isEmpty());
        QVERIFY(!readFile(store.filePathFor(QStringLiteral("stored-pw"))).contains("again"));
    }

    void sessionDialogCreatesVaultIfMissing()
    {
        freshVault(false);
        SessionDialog d(store, sshSession(QStringLiteral("first-pw")));
        child<QLineEdit>(&d, "sshPassword")->setText(QStringLiteral("pw1"));
        bool sawWarning = false;
        whenModal([&sawWarning](QWidget *m) {
            auto *c = qobject_cast<CreateVaultDialog *>(m);
            if (!c) {
                return false;
            }
            sawWarning = child<QLabel>(c, "noRecoveryWarning")->text().contains(QStringLiteral("NO recovery"));
            child<QLineEdit>(c, "masterPassword")->setText(QString::fromUtf8(kMaster));
            child<QLineEdit>(c, "confirmPassword")->setText(QString::fromUtf8(kMaster));
            child<QCheckBox>(c, "understandNoRecovery")->setChecked(true);
            child<QPushButton>(c, "create")->click();
            return true;
        });
        QVERIFY2(d.saveCurrent(), qPrintable(d.errorText()));
        QVERIFY(sawWarning);
        QCOMPARE(str(vm().vault().secret(QStringLiteral("ssh-password/first-pw"))), QStringLiteral("pw1"));
    }

    void sshAskpassEndToEnd()
    {
        // A fake `ssh` first in PATH: records its own /proc/<pid>/cmdline and
        // environ, then calls $SSH_ASKPASS twice like a real ssh would after a
        // rejected password.
        QTemporaryDir bin;
        const QString out = bin.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(out));
        {
            QFile f(bin.filePath(QStringLiteral("ssh")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\n"
                    "o=\"$ZT_FAKE_OUT\"\n"
                    "cat /proc/$$/cmdline > \"$o/cmdline\"\n"
                    "cat /proc/$$/environ > \"$o/environ\"\n"
                    "\"$SSH_ASKPASS\" \"stevebj@core-sw1's password: \" > \"$o/answer1\"; echo $? > \"$o/rc1\"\n"
                    "\"$SSH_ASKPASS\" \"stevebj@core-sw1's password: \" > \"$o/answer2\"; echo $? > \"$o/rc2\"\n"
                    "echo fake-ssh-finished\n");
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(bin.path()) + ':' + oldPath);
        qputenv("ZT_FAKE_OUT", QFile::encodeName(out));

        freshVault();
        QVERIFY(store.save(sshSession(QStringLiteral("core-sw1 pw"))));
        QVERIFY(vm().vault().setSecret(QStringLiteral("ssh-password/core-sw1 pw"), sb(kSecret)));
        vm().lock(); // locked when the session starts -> prompt
        {
            MainWindow w(parseCommandLine({QStringLiteral("core-sw1 pw")}), {QStringLiteral("core-sw1 pw")});
            w.show();
            bool asked = false;
            answerUnlock(QString::fromUtf8(kMaster), &asked);
            w.startSession();
            QVERIFY(asked);
            QVERIFY(w.askpassServer());
            const QString sock = w.askpassServer()->socketPath();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out + QStringLiteral("/rc1")), 10000);
            QCOMPARE(readFile(out + QStringLiteral("/answer1")), QByteArray(kSecret) + '\n');
            QCOMPARE(readFile(out + QStringLiteral("/rc1")).trimmed(), QByteArray("0"));
            QVERIFY(!QFileInfo::exists(sock)); // one shot
            // Second prompt: manual entry in the terminal (no loop, no resend).
            QTRY_VERIFY_WITH_TIMEOUT(screen(w).contains(QStringLiteral("stevebj@core-sw1's password:")), 10000);
            w.pty()->write("typed-by-hand\r");
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out + QStringLiteral("/rc2")), 10000);
            QCOMPARE(readFile(out + QStringLiteral("/answer2")), QByteArray("typed-by-hand\n"));
            QTRY_VERIFY_WITH_TIMEOUT(screen(w).contains(QStringLiteral("fake-ssh-finished")), 10000);
            QVERIFY(!screen(w).contains(QStringLiteral("typed-by-hand"))); // not echoed
            QVERIFY(!screen(w).contains(QString::fromUtf8(kSecret)));
        }
        // The secret is in neither the argv nor the environment of "ssh"...
        const QByteArray cmdline = readFile(out + QStringLiteral("/cmdline"));
        const QByteArray env = readFile(out + QStringLiteral("/environ"));
        QVERIFY(cmdline.contains("core-sw1.example"));
        QVERIFY(!cmdline.contains("Sw0rdfish"));
        QVERIFY(!env.contains("Sw0rdfish"));
        QVERIFY(env.contains("SSH_ASKPASS_REQUIRE=force"));
        QVERIFY(env.contains("ZTERMINAL_ASKPASS_SOCKET="));
        QVERIFY(env.contains(QByteArray("SSH_ASKPASS=") + qgetenv("ZTERMINAL_ASKPASS")));
        // ...nor in ours.
        QVERIFY(!readFile(QStringLiteral("/proc/self/environ")).contains("Sw0rdfish"));
        QVERIFY(!readFile(QStringLiteral("/proc/self/cmdline")).contains("Sw0rdfish"));

        // Cancelled unlock: no askpass at all, ssh asks as usual.
        QFile::remove(out + QStringLiteral("/environ"));
        QFile::remove(out + QStringLiteral("/rc1"));
        vm().lock();
        {
            MainWindow w(parseCommandLine({QStringLiteral("core-sw1 pw")}), {QStringLiteral("core-sw1 pw")});
            w.show();
            answerUnlock(QString());
            w.startSession();
            QVERIFY(!w.askpassServer());
            QVERIFY(screen(w).contains(QStringLiteral("vault locked; ssh will ask for the password")));
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(out + QStringLiteral("/environ")), 10000);
            QTest::qWait(200);
            QVERIFY(!readFile(out + QStringLiteral("/environ")).contains("SSH_ASKPASS"));
            w.pty()->terminate();
        }
        qputenv("PATH", oldPath);
        qunsetenv("ZT_FAKE_OUT");
    }

    void sendStoredLoginOverSerial()
    {
        if (!SocatPair::available()) {
            QSKIP("socat not installed");
        }
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig s;
        s.name = QStringLiteral("SG250 login");
        s.type = SessionConfig::Type::Serial;
        s.serialDevice = cable.a;
        s.loginUser = QStringLiteral("admin");
        s.localEcho = true; // the password must still not be echoed
        QVERIFY(store.save(s));
        QVERIFY(!readFile(store.filePathFor(s.name)).toLower().contains("pass"));
        freshVault();
        QVERIFY(vm().vault().setSecret(QStringLiteral("serial-password/SG250 login"), sb("c1sc0-Secret")));
        vm().lock();

        MainWindow w(parseCommandLine({s.name}), {s.name});
        w.show();
        w.startSession();
        QVERIFY(w.serial()->isOpen());
        QAction *act = w.action(QStringLiteral("sendStoredLogin"));
        QVERIFY(act && act->isEnabled());
        QSerialPort dev;
        dev.setPortName(cable.b);
        QVERIFY(dev.open(QIODevice::ReadWrite));
        QByteArray got;
        connect(&dev, &QSerialPort::readyRead, this, [&]() { got += dev.readAll(); });

        answerUnlock(QString::fromUtf8(kMaster));
        act->trigger();
        QVERIFY(w.loginPending());
        QTRY_COMPARE_WITH_TIMEOUT(got, QByteArray("admin\r"), 5000);
        QTest::qWait(200);
        QCOMPARE(got, QByteArray("admin\r")); // nothing more until the device asks
        dev.write("\r\nPassword: ");
        QTRY_COMPARE_WITH_TIMEOUT(got, QByteArray("admin\rc1sc0-Secret\r"), 5000);
        QVERIFY(!w.loginPending());
        QVERIFY(!screen(w).contains(QStringLiteral("c1sc0")));     // local echo skipped it
        QVERIFY(screen(w).contains(QStringLiteral("admin")));      // the user name was echoed

        // A prompt already on screen is answered right away (no user name sent).
        got.clear();
        dev.write("\r\nBad passwords.\r\nPassword:");
        QTRY_VERIFY_WITH_TIMEOUT(screen(w).contains(QStringLiteral("Bad passwords.")), 5000);
        QTest::qWait(100);
        QVERIFY(w.sendStoredLogin());
        QTRY_COMPARE_WITH_TIMEOUT(got, QByteArray("c1sc0-Secret\r"), 5000);
        // No prompt within the timeout: the password is NOT sent blind.
        got.clear();
        dev.write("\r\nswitch# ");
        QTest::qWait(100);
        QVERIFY(w.sendStoredLogin());
        QTRY_COMPARE_WITH_TIMEOUT(got, QByteArray("admin\r"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!w.loginPending(), 15000);
        QCOMPARE(got, QByteArray("admin\r"));
        QVERIFY(screen(w).contains(QStringLiteral("stored password NOT sent")));
    }

    void cleanupTestCase()
    {
        vm().lock();
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    TstVaultUi t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_vaultui.moc"
