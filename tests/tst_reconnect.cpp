// Keepalive and reconnect (docs/PLAN.md §4.18): ssh keepalive options per saved
// session, drop classification, the per-tab "Disconnected" banner, backoff on
// a mocked clock, auto-reconnect with the vault password and no extra prompt,
// serial unplug/replug through socat (by-id path preferred), scrollback and
// the log file kept with disconnected/reconnected markers, and no reconnect
// after a clean exit.
#include "AppSettings.hpp"
#include "CommandLine.hpp"
#include "Reconnect.hpp"
#include "SecureBuffer.hpp"
#include "SerialBackend.hpp"
#include "Session.hpp"
#include "SessionDialog.hpp"
#include "SessionLog.hpp"
#include "SessionExport.hpp"
#include "SessionStore.hpp"
#include "VaultBindings.hpp"
#include "SessionWidget.hpp"
#include "SocatPair.hpp"
#include "Terminal.hpp"
#include "Vault.hpp"
#include "VaultManager.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QPushButton>
#include <QSerialPort>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

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

QString screen(Terminal *t)
{
    QString all;
    for (int r = 0; r < t->totalLines(); ++r) {
        all += t->lineText(r).trimmed() + QLatin1Char('\n');
    }
    return all;
}

QByteArray readFile(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A fake `ssh` on PATH. Invocation n does what line n of the plan says:
//   drop    print "connected N", then die like a lost link (exit 255)
//   refused "Connection refused" (exit 255)
//   auth    "Permission denied" (exit 255)
//   clean   "logout", exit 0
//   stay    "connected N", stay up (cat)
// With ASKPASS=1 it first asks its askpass helper and prints the answer.
struct FakeSsh {
    QTemporaryDir dir;
    QByteArray oldPath = qgetenv("PATH");
    explicit FakeSsh(const QStringList &plan, bool askpass = false)
    {
        QFile p(dir.filePath(QStringLiteral("plan")));
        if (p.open(QIODevice::WriteOnly)) {
            p.write(plan.join(QLatin1Char('\n')).toUtf8() + '\n');
        }
        p.close();
        QFile f(dir.filePath(QStringLiteral("ssh")));
        if (f.open(QIODevice::WriteOnly)) {
            const QByteArray st = QFile::encodeName(dir.path());
            QByteArray s = "#!/bin/sh\nS='" + st + "'\n"
                           "n=$(cat \"$S/count\" 2>/dev/null || echo 0); n=$((n+1)); echo $n > \"$S/count\"\n"
                           "printf '%s\\n' \"$*\" > \"$S/args$n\"\n";
            if (askpass) {
                s += "printf 'attempt %s got [%s]\\n' $n \"$(\"$SSH_ASKPASS\" \"password: \")\"\n";
            }
            s += "m=$(sed -n \"${n}p\" \"$S/plan\")\n"
                 "case \"$m\" in\n"
                 " drop) echo \"connected $n\"; sleep 0.3; echo 'Timeout, server not responding.' >&2; exit 255;;\n"
                 " refused) echo 'ssh: connect to host sw port 22: Connection refused' >&2; exit 255;;\n"
                 " auth) echo 'admin@sw: Permission denied (publickey,password).' >&2; exit 255;;\n"
                 " clean) echo logout; exit 0;;\n"
                 " *) echo \"connected $n\"; exec cat;;\n"
                 "esac\n";
            f.write(s);
        }
        f.close();
        QFile::setPermissions(f.fileName(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", QFile::encodeName(dir.path()) + ':' + oldPath);
    }
    ~FakeSsh() { qputenv("PATH", oldPath); }
    int count() const { return readFile(dir.filePath(QStringLiteral("count"))).trimmed().toInt(); }
    QString args(int n) const
    {
        return QString::fromUtf8(readFile(dir.filePath(QStringLiteral("args%1").arg(n)))).trimmed();
    }
};

// Fails the test if any modal dialog (e.g. a vault unlock prompt) shows up.
struct NoDialogs {
    QTimer timer;
    QString seen;
    NoDialogs()
    {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [this]() {
            if (QWidget *m = QApplication::activeModalWidget()) {
                seen = m->windowTitle() + QLatin1Char('/') + QString::fromLatin1(m->metaObject()->className());
                m->close();
            }
        });
        timer.start();
    }
};

} // namespace

class TstReconnect : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    SessionStore store;
    qint64 fakeNow = 0;

    SessionConfig sshSession(const QString &name, bool autoReconnect, bool stored = false)
    {
        SessionConfig s;
        s.name = name;
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("sw.example");
        s.user = QStringLiteral("admin");
        s.autoReconnect = autoReconnect;
        s.useStoredPassword = stored;
        return s;
    }
    AppSettings settings() const
    {
        AppSettings a;
        a.logDirectory = tmp.filePath(QStringLiteral("logs"));
        return a;
    }
    // A tab for the saved session, with the scheduler on the fake clock.
    std::unique_ptr<SessionWidget> open(const QString &name)
    {
        auto w = std::make_unique<SessionWidget>(parseCommandLine({name}), QStringList{name}, settings());
        w->resize(700, 400);
        w->show();
        w->reconnectScheduler()->setTickInterval(0);
        w->reconnectScheduler()->setClock([this]() { return fakeNow; });
        return w;
    }
    void advance(SessionWidget *w, qint64 ms)
    {
        fakeNow += ms;
        w->reconnectScheduler()->tick();
    }

private slots:
    void initTestCase()
    {
        Vault::setKdfOverrideForTests(1, 8192);
        AppSettings{}.save();
        SessionWidget::setReconnectStableMsForTests(400);
    }
    void init()
    {
        QDir(store.directory()).removeRecursively();
        QDir(tmp.filePath(QStringLiteral("logs"))).removeRecursively();
    }

    void backoffAndMockedClock()
    {
        const Backoff b;
        QList<int> d;
        for (int i = 1; i <= 8; ++i) {
            d << b.delaySeconds(i);
        }
        QCOMPARE(d, (QList<int>{2, 4, 8, 16, 32, 60, 60, 60}));

        ReconnectScheduler s;
        s.setTickInterval(0);
        qint64 now = 1000;
        s.setClock([&now]() { return now; });
        QSignalSpy due(&s, &ReconnectScheduler::attemptDue);
        QSignalSpy count(&s, &ReconnectScheduler::countdown);
        s.scheduleNext();
        QCOMPARE(s.attempt(), 1);
        QVERIFY(s.isWaiting());
        QCOMPARE(s.secondsLeft(), 2);
        now += 999;
        s.tick();
        QCOMPARE(s.secondsLeft(), 2); // rounded up
        now += 1;
        s.tick();
        QCOMPARE(s.secondsLeft(), 1);
        now += 999;
        s.tick();
        QCOMPARE(due.count(), 0);
        now += 1; // exactly 2 s
        s.tick();
        QCOMPARE(due.count(), 1);
        QCOMPARE(due.at(0).at(0).toInt(), 1);
        QVERIFY(s.isAttempting());
        QVERIFY(!s.isWaiting());
        QCOMPARE(count.count(), 2); // "2", "1": once per second shown
        // The attempt failed: 4 s, then 8 s.
        s.scheduleNext();
        QCOMPARE(s.attempt(), 2);
        QCOMPARE(s.secondsLeft(), 4);
        now += 3999;
        s.tick();
        QCOMPARE(due.count(), 1);
        now += 1;
        s.tick();
        QCOMPARE(due.count(), 2);
        s.scheduleNext();
        QCOMPARE(s.secondsLeft(), 8);
        // Cancel stops it; reset after success starts over at attempt 1.
        s.cancel();
        QVERIFY(!s.isWaiting());
        now += 100000;
        s.tick();
        QCOMPARE(due.count(), 2);
        s.scheduleNext();
        QCOMPARE(s.attempt(), 1);
        QCOMPARE(s.secondsLeft(), 2);
    }

    void classifiesSshExits()
    {
        using K = SshExit::Kind;
        QCOMPARE(SshExit::classify(0, false, "logout\r\n").kind, K::Clean);
        QCOMPARE(SshExit::classify(1, false, "").kind, K::RemoteStatus);
        QCOMPARE(SshExit::classify(130, false, "").kind, K::RemoteStatus);
        QCOMPARE(SshExit::classify(129, true, "").kind, K::Killed);
        SshExit t = SshExit::classify(255, false, "prompt$ \r\nTimeout, server not responding.\r\n");
        QCOMPARE(t.kind, K::Dropped);
        QCOMPARE(t.reason, QStringLiteral("Timeout, server not responding."));
        QVERIFY(SshExit::classify(255, false, "client_loop: send disconnect: Broken pipe\r\n").isDrop());
        QVERIFY(SshExit::classify(255, false, "Connection to sw closed by remote host.\r\n").isDrop());
        QVERIFY(SshExit::classify(255, false, "ssh: connect to host sw port 22: Connection refused\r\n").isDrop());
        QVERIFY(SshExit::classify(255, false, "").isDrop());
        QCOMPARE(SshExit::classify(255, false, "admin@sw: Permission denied (publickey).\r\n").kind, K::Refused);
        QCOMPARE(SshExit::classify(255, false, "Host key verification failed.\r\n").kind, K::Refused);
    }

    void keepaliveOptions()
    {
        SessionConfig s = sshSession(QStringLiteral("k"), false);
        QCOMPARE(s.keepaliveInterval, 30);
        QCOMPARE(s.keepaliveCountMax, 3);
        QVERIFY(!s.autoReconnect);
        SshCommand c = buildSshCommand(s);
        QVERIFY(c.ok());
        QCOMPARE(c.args, (QStringList{QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"), QStringLiteral("-o"),
                                      QStringLiteral("ServerAliveCountMax=3"), QStringLiteral("-l"), QStringLiteral("admin"),
                                      QStringLiteral("--"), QStringLiteral("sw.example")}));
        // An explicit -o in the extra options comes first, and ssh uses the first value.
        s.extraArgs = QStringLiteral("-o ServerAliveInterval=5");
        c = buildSshCommand(s);
        QVERIFY(c.args.indexOf(QStringLiteral("ServerAliveInterval=5")) < c.args.indexOf(QStringLiteral("ServerAliveInterval=30")));
        s.extraArgs.clear();
        s.keepaliveInterval = 0; // off: no options at all
        c = buildSshCommand(s);
        QVERIFY(!c.args.join(QLatin1Char(' ')).contains(QStringLiteral("ServerAlive")));
        s.keepaliveInterval = 4000;
        QVERIFY(!buildSshCommand(s).ok());
        s.keepaliveInterval = 15;
        s.keepaliveCountMax = 0;
        QVERIFY(!buildSshCommand(s).ok());

        // Saved and loaded.
        s.keepaliveCountMax = 5;
        s.autoReconnect = true;
        QVERIFY(store.save(s));
        const auto back = store.load(s.name);
        QVERIFY(back);
        QCOMPARE(back->keepaliveInterval, 15);
        QCOMPARE(back->keepaliveCountMax, 5);
        QVERIFY(back->autoReconnect);
        QCOMPARE(*back, s);
        // `zt ssh -o ServerAliveInterval=10 host` saved: the fields, not extra options.
        const auto adhoc = sessionFromSshArgs({QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=10"), QStringLiteral("-o"),
                                               QStringLiteral("ServerAliveCountMax=2"), QStringLiteral("host")});
        QVERIFY(adhoc);
        QCOMPARE(adhoc->keepaliveInterval, 10);
        QCOMPARE(adhoc->keepaliveCountMax, 2);
        QVERIFY(adhoc->extraArgs.isEmpty());
    }

    void sessionEditorFields()
    {
        SessionConfig s = sshSession(QStringLiteral("edit-me"), true);
        s.keepaliveInterval = 20;
        s.keepaliveCountMax = 6;
        SessionDialog d(store, s);
        auto *interval = child<QSpinBox>(&d, "keepaliveInterval");
        auto *countMax = child<QSpinBox>(&d, "keepaliveCountMax");
        auto *autoRe = child<QCheckBox>(&d, "autoReconnect");
        QCOMPARE(interval->value(), 20);
        QCOMPARE(countMax->value(), 6);
        QVERIFY(autoRe->isChecked());
        interval->setValue(45);
        countMax->setValue(4);
        autoRe->setChecked(false);
        SessionConfig r = d.config();
        QCOMPARE(r.keepaliveInterval, 45);
        QCOMPARE(r.keepaliveCountMax, 4);
        QVERIFY(!r.autoReconnect);
        // The command preview shows the options; 0 = off disables the count.
        QVERIFY(child<QLineEdit>(&d, "commandPreview")->text().contains(QStringLiteral("ServerAliveInterval=45")));
        interval->setValue(0);
        QVERIFY(!countMax->isEnabled());
        QVERIFY(!child<QLineEdit>(&d, "commandPreview")->text().contains(QStringLiteral("ServerAlive")));
        // New sessions start at 30 s x 3, auto-reconnect off.
        SessionDialog fresh(store, SessionConfig{});
        QCOMPARE(child<QSpinBox>(&fresh, "keepaliveInterval")->value(), 30);
        QCOMPARE(child<QSpinBox>(&fresh, "keepaliveCountMax")->value(), 3);
        QVERIFY(!child<QCheckBox>(&fresh, "autoReconnect")->isChecked());
    }

    void dropShowsBannerAndManualReconnect()
    {
        FakeSsh ssh({QStringLiteral("drop"), QStringLiteral("stay")});
        QVERIFY(store.save(sshSession(QStringLiteral("manual"), false)));
        auto w = open(QStringLiteral("manual"));
        QVERIFY(w->startLogging());
        const QString logPath = w->sessionLog()->path();
        w->startSession();
        QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("connected 1")), 5000);
        QVERIFY(ssh.args(1).contains(QStringLiteral("-o ServerAliveInterval=30 -o ServerAliveCountMax=3")));
        QTRY_VERIFY_WITH_TIMEOUT(w->isDisconnected(), 5000);
        // Banner: "Disconnected at HH:mm:ss — reason", Reconnect, no countdown (auto off).
        QVERIFY(!w->banner()->isHidden());
        const QString text = w->bannerText();
        QVERIFY2(text.contains(QStringLiteral("Disconnected")), qPrintable(text));
        QVERIFY2(text.contains(w->droppedAt().toString(QStringLiteral("HH:mm:ss"))), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("Timeout, server not responding.")), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("Reconnecting in")));
        QVERIFY(child<QPushButton>(w.get(), "cancelReconnect")->isHidden());
        QVERIFY(!w->reconnectScheduler()->isWaiting());
        QVERIFY(!w->isLive());
        // Nothing happens by itself.
        advance(w.get(), 120000);
        QTest::qWait(200);
        QCOMPARE(ssh.count(), 1);

        // Reconnect: same tab, scrollback kept, log continues in the same file.
        child<QPushButton>(w.get(), "reconnect")->click();
        QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("connected 2")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!w->isDisconnected(), 5000); // stayed up 400 ms (test setting)
        QVERIFY(w->banner()->isHidden());
        QVERIFY(screen(w->terminal()).contains(QStringLiteral("connected 1")));
        QCOMPARE(w->sessionLog()->path(), logPath);
        QVERIFY(w->isLogging());
        const QString log = QString::fromUtf8(readFile(logPath));
        static const QRegularExpression disc(QStringLiteral(
            "--- disconnected \\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d \\(Timeout, server not responding\\.\\) ---"));
        static const QRegularExpression reco(QStringLiteral("--- reconnected \\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d ---"));
        QVERIFY2(log.contains(disc), qPrintable(log));
        QVERIFY2(log.contains(reco), qPrintable(log));
        QVERIFY(log.indexOf(QStringLiteral("connected 1")) < log.indexOf(QStringLiteral("--- disconnected")));
        QVERIFY(log.indexOf(QStringLiteral("--- disconnected")) < log.indexOf(QStringLiteral("connected 2")));
        QVERIFY(log.indexOf(QStringLiteral("connected 2")) < log.indexOf(QStringLiteral("--- reconnected")));
        QCOMPARE(w->reconnectCount(), 1);
    }

    void autoReconnectBacksOffOnMockedClock()
    {
        FakeSsh ssh({QStringLiteral("drop"), QStringLiteral("refused"), QStringLiteral("refused"), QStringLiteral("stay")});
        QVERIFY(store.save(sshSession(QStringLiteral("auto"), true)));
        auto w = open(QStringLiteral("auto"));
        w->startSession();
        QTRY_VERIFY_WITH_TIMEOUT(w->isDisconnected(), 5000);
        ReconnectScheduler *s = w->reconnectScheduler();
        QVERIFY(s->isWaiting());
        QCOMPARE(s->attempt(), 1);
        QVERIFY2(w->bannerText().contains(QStringLiteral("Reconnecting in 2 s (attempt 1)")), qPrintable(w->bannerText()));
        QVERIFY(!child<QPushButton>(w.get(), "cancelReconnect")->isHidden());
        advance(w.get(), 1000);
        QVERIFY2(w->bannerText().contains(QStringLiteral("Reconnecting in 1 s (attempt 1)")), qPrintable(w->bannerText()));
        QCOMPARE(ssh.count(), 1);
        advance(w.get(), 1000); // 2 s: attempt 1 (refused)
        QTRY_COMPARE_WITH_TIMEOUT(ssh.count(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(s->isWaiting(), 5000);
        QCOMPARE(s->attempt(), 2);
        QCOMPARE(s->secondsLeft(), 4);
        QVERIFY(screen(w->terminal()).contains(QStringLiteral("reconnect attempt 1 failed")));
        advance(w.get(), 3999);
        QTest::qWait(100);
        QCOMPARE(ssh.count(), 2); // not before 4 s
        advance(w.get(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(ssh.count(), 3, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(s->isWaiting(), 5000);
        QCOMPARE(s->attempt(), 3);
        QCOMPARE(s->secondsLeft(), 8);
        QVERIFY(w->bannerText().contains(QStringLiteral("attempt 3")));
        advance(w.get(), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(ssh.count(), 4, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!w->isDisconnected(), 5000);
        QVERIFY(w->banner()->isHidden());
        QCOMPARE(s->attempt(), 0); // the next drop starts over at 2 s
        QVERIFY(screen(w->terminal()).contains(QStringLiteral("connected 4")));
    }

    void cancelStopsAutoReconnect()
    {
        FakeSsh ssh({QStringLiteral("drop"), QStringLiteral("stay")});
        QVERIFY(store.save(sshSession(QStringLiteral("cancel"), true)));
        auto w = open(QStringLiteral("cancel"));
        w->startSession();
        QTRY_VERIFY_WITH_TIMEOUT(w->reconnectScheduler()->isWaiting(), 5000);
        child<QPushButton>(w.get(), "cancelReconnect")->click();
        QVERIFY(!w->reconnectScheduler()->isWaiting());
        QVERIFY(w->isDisconnected());
        QVERIFY(w->bannerText().contains(QStringLiteral("cancelled")));
        QVERIFY(child<QPushButton>(w.get(), "cancelReconnect")->isHidden());
        advance(w.get(), 120000);
        QTest::qWait(200);
        QCOMPARE(ssh.count(), 1);
    }

    void noReconnectAfterCleanExitOrAuthFailure()
    {
        {
            FakeSsh ssh({QStringLiteral("clean"), QStringLiteral("stay")});
            QVERIFY(store.save(sshSession(QStringLiteral("bye"), true)));
            auto w = open(QStringLiteral("bye"));
            w->startSession();
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("exited with code 0")), 5000);
            QVERIFY(!w->isDisconnected());
            QVERIFY(w->banner()->isHidden());
            QVERIFY(!w->reconnectScheduler()->isWaiting());
            advance(w.get(), 120000);
            QTest::qWait(200);
            QCOMPARE(ssh.count(), 1);
        }
        {
            // A drop, then the retry is refused for good (password changed): stop retrying.
            FakeSsh ssh({QStringLiteral("drop"), QStringLiteral("auth"), QStringLiteral("stay")});
            QVERIFY(store.save(sshSession(QStringLiteral("auth"), true)));
            auto w = open(QStringLiteral("auth"));
            w->startSession();
            QTRY_VERIFY_WITH_TIMEOUT(w->reconnectScheduler()->isWaiting(), 5000);
            advance(w.get(), 2000);
            QTRY_COMPARE_WITH_TIMEOUT(ssh.count(), 2, 5000);
            QTRY_VERIFY_WITH_TIMEOUT(w->bannerText().contains(QStringLiteral("Reconnect failed")), 5000);
            QVERIFY(w->bannerText().contains(QStringLiteral("Permission denied")));
            QVERIFY(!w->reconnectScheduler()->isWaiting());
            advance(w.get(), 120000);
            QTest::qWait(200);
            QCOMPARE(ssh.count(), 2);
        }
    }

    void reconnectUsesVaultPasswordWithoutPrompt()
    {
        FakeSsh ssh({QStringLiteral("drop"), QStringLiteral("stay")}, /*askpass=*/true);
        VaultManager &vm = VaultManager::instance();
        vm.setPath(tmp.filePath(QStringLiteral("vault/vault.bin")));
        vm.setAutoLockIntervalMsForTests(0);
        QVERIFY(vm.vault().create(SecureBuffer::fromQString(QStringLiteral("master pw"))));
        vm.noteUnlocked();
        const SessionConfig s = sshSession(QStringLiteral("vaulted"), true, /*stored=*/true);
        QVERIFY(store.save(s));
        QVERIFY(vaultbind::storeSecret(vm.vault(), s, SecureBuffer::fromQString(QStringLiteral("s3cret"))));
        NoDialogs guard;
        auto w = open(s.name);
        w->startSession();
        QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("attempt 1 got [s3cret]")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(w->reconnectScheduler()->isWaiting(), 5000);
        advance(w.get(), 2000);
        // The retry gets a fresh one-shot askpass server with the same stored password.
        QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("attempt 2 got [s3cret]")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!w->isDisconnected(), 5000);
        QVERIFY2(guard.seen.isEmpty(), qPrintable(guard.seen)); // no master-password prompt
        QVERIFY(vm.isUnlocked());
        // The password never shows up in the log or the ssh argv.
        QVERIFY(!ssh.args(2).contains(QStringLiteral("s3cret")));
        vm.lock();
    }

    // Review item 1: an import that replaces a session can't redirect its
    // stored password to another host, even if "use stored password" is
    // ticked again afterwards; only an explicit re-bind sends it there.
    void importedRedirectNeverGetsStoredPassword()
    {
        FakeSsh ssh({QStringLiteral("clean"), QStringLiteral("clean"), QStringLiteral("clean"), QStringLiteral("clean")},
                    /*askpass=*/true);
        VaultManager &vm = VaultManager::instance();
        vm.setPath(tmp.filePath(QStringLiteral("vault-redirect/vault.bin")));
        vm.setAutoLockIntervalMsForTests(0);
        QVERIFY(vm.vault().create(SecureBuffer::fromQString(QStringLiteral("master pw"))));
        vm.noteUnlocked();
        const SessionConfig mine = sshSession(QStringLiteral("core-sw1"), false, /*stored=*/true);
        QVERIFY(store.save(mine));
        QVERIFY(vaultbind::storeSecret(vm.vault(), mine, SecureBuffer::fromQString(QStringLiteral("s3cret"))));

        SessionConfig evil = mine;
        evil.host = QStringLiteral("evil.example");
        QString err;
        const auto r = importSessionsFromJson(store, sessionsToExportJson({evil}), SessionImportConflict::Overwrite, &err,
                                              ImportApproval::Approved);
        QCOMPARE(r.overwritten, 1);
        QVERIFY(!store.load(mine.name)->useStoredPassword);
        NoDialogs guard;
        {
            auto w = open(mine.name);
            w->startSession();
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("attempt 1 got [")), 10000);
            QVERIFY(!screen(w->terminal()).contains(QStringLiteral("s3cret")));
        }
        // Ticking it again doesn't help: the secret is bound to sw.example.
        SessionConfig reticked = *store.load(mine.name);
        reticked.useStoredPassword = true;
        QVERIFY(store.save(reticked));
        SessionWidget::setRebindAnswerForTests(0);
        {
            auto w = open(mine.name);
            w->startSession();
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("attempt 2 got [")), 10000);
            QVERIFY(!screen(w->terminal()).contains(QStringLiteral("s3cret")));
            QVERIFY(screen(w->terminal()).contains(QStringLiteral("not sent")));
        }
        QCOMPARE(vaultbind::check(vm.vault(), reticked), vaultbind::Status::Mismatch);
        // The user explicitly re-binds it to evil.example: now it is sent.
        SessionWidget::setRebindAnswerForTests(1);
        {
            auto w = open(mine.name);
            w->startSession();
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("attempt 3 got [s3cret]")), 10000);
        }
        QCOMPARE(vaultbind::check(vm.vault(), reticked), vaultbind::Status::Match);
        SessionWidget::setRebindAnswerForTests(-1);
        QVERIFY2(guard.seen.isEmpty(), qPrintable(guard.seen));

        // An unapproved import doesn't launch at all.
        SessionConfig pending = evil;
        pending.name = QStringLiteral("pending");
        importSessionsFromJson(store, sessionsToExportJson({pending}), SessionImportConflict::Overwrite, &err);
        QVERIFY(!store.load(pending.name)->approved);
        {
            auto w = open(pending.name);
            w->startSession();
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("hasn't been reviewed")), 5000);
        }
        QCOMPARE(ssh.count(), 3);
        vm.lock();
    }

    void serialUnplugAndReplugByIdPath()
    {
        if (!SocatPair::available()) {
            QSKIP("socat not installed");
        }
        // A fake /dev/serial/by-id: the adapter's stable name points at whatever
        // kernel name it has right now.
        QTemporaryDir byId;
        qputenv("ZTERMINAL_SERIAL_BY_ID_DIR", QFile::encodeName(byId.path()));
        const QString stable = byId.filePath(QStringLiteral("usb-FTDI_FT232R_USB_UART_A10K1234-if00-port0"));
        auto cable = std::make_unique<SocatPair>();
        QVERIFY(cable->start());
        QVERIFY(QFile::link(cable->a, stable));
        QCOMPARE(SerialBackend::byIdAlias(cable->a), stable);

        SessionConfig con;
        con.name = QStringLiteral("SG250 console");
        con.type = SessionConfig::Type::Serial;
        con.serialDevice = cable->a; // saved with the kernel name ...
        QVERIFY(store.save(con));
        auto w = open(con.name);
        QVERIFY(w->startLogging());
        const QString logPath = w->sessionLog()->path();
        w->startSession();
        QVERIFY(w->serial()->isOpen());
        QCOMPARE(w->serialWatchPath(), stable); // ... but the by-id name is watched
        {
            QSerialPort dev;
            dev.setPortName(cable->b);
            QVERIFY(dev.open(QIODevice::ReadWrite));
            dev.write("SG250#before\r\n");
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("SG250#before")), 5000);
        }

        // Unplug: socat goes away with its pty links (ResourceError / node gone).
        cable->stop();
        QTRY_VERIFY_WITH_TIMEOUT(w->isDisconnected(), 5000);
        QVERIFY(!w->serial()->isOpen());
        QVERIFY(w->isWaitingForDevice());
        QVERIFY2(w->bannerText().contains(QStringLiteral("Disconnected")), qPrintable(w->bannerText()));
        QVERIFY2(w->bannerText().contains(QStringLiteral("Waiting for")), qPrintable(w->bannerText()));
        QVERIFY(!child<QPushButton>(w.get(), "cancelReconnect")->isHidden());

        // Replug under a different kernel name (ttyUSB0 -> ttyUSB1); the by-id link follows.
        SocatPair other;
        QVERIFY(other.start());
        QFile::remove(stable);
        QVERIFY(QFile::link(other.a, stable));
        QTRY_VERIFY_WITH_TIMEOUT(w->serial()->isOpen(), 5000);
        QVERIFY(!w->isDisconnected());
        QVERIFY(w->banner()->isHidden());
        QCOMPARE(w->serial()->device(), stable);
        {
            QSerialPort dev;
            dev.setPortName(other.b);
            QVERIFY(dev.open(QIODevice::ReadWrite));
            dev.write("SG250#after\r\n");
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->terminal()).contains(QStringLiteral("SG250#after")), 5000);
        }
        QVERIFY(screen(w->terminal()).contains(QStringLiteral("SG250#before"))); // scrollback kept
        const QString log = QString::fromUtf8(readFile(logPath));
        QVERIFY2(log.contains(QStringLiteral("--- disconnected ")), qPrintable(log));
        QVERIFY2(log.contains(QStringLiteral("--- reconnected ")), qPrintable(log));
        QVERIFY(log.indexOf(QStringLiteral("SG250#before")) < log.indexOf(QStringLiteral("--- disconnected")));
        QVERIFY(log.indexOf(QStringLiteral("--- reconnected")) < log.indexOf(QStringLiteral("SG250#after")));
        qunsetenv("ZTERMINAL_SERIAL_BY_ID_DIR");
    }

    void serialSamePathComesBack()
    {
        if (!SocatPair::available()) {
            QSKIP("socat not installed");
        }
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig con;
        con.name = QStringLiteral("plain tty");
        con.type = SessionConfig::Type::Serial;
        con.serialDevice = cable.a;
        QVERIFY(store.save(con));
        auto w = open(con.name);
        w->startSession();
        QVERIFY(w->serial()->isOpen());
        QCOMPARE(w->serialWatchPath(), cable.a); // no by-id name: the saved path
        cable.stop();
        QTRY_VERIFY_WITH_TIMEOUT(w->isWaitingForDevice(), 5000);
        // Cancel stops waiting; Reconnect with the device still missing waits again.
        child<QPushButton>(w.get(), "cancelReconnect")->click();
        QVERIFY(!w->isWaitingForDevice());
        child<QPushButton>(w.get(), "reconnect")->click();
        QVERIFY(w->isWaitingForDevice());
        QVERIFY(cable.start()); // same path again
        QTRY_VERIFY_WITH_TIMEOUT(w->serial()->isOpen(), 5000);
        QVERIFY(!w->isDisconnected());
        QCOMPARE(w->serial()->device(), cable.a);
    }
};

QTEST_MAIN(TstReconnect)
#include "tst_reconnect.moc"
