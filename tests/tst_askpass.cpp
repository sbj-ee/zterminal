// AskpassServer + zterminal-askpass: one-shot delivery to a descendant over a
// private Unix socket, rejection of other processes, prompts that aren't
// password prompts never touch the socket, and the no-tty fallback.
#include "AskpassServer.hpp"
#include "SecureBuffer.hpp"
#include "askpass_policy.h"

#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace zterminal;

namespace {

const char *kHelper = ZTERMINAL_ASKPASS_BIN;

struct HelperRun {
    int exitCode = -1;
    QByteArray out;
};

// Runs the helper in its own session (setsid: no controlling tty, so a manual
// fallback can't block on the developer's terminal) while the event loop runs.
HelperRun runHelper(const QString &prompt, const QString &socketPath, const QStringList &extraEnv = {})
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char *v : {"ZTERMINAL_ASKPASS_SOCKET", "ZTERMINAL_ASKPASS_TARGET", "ZTERMINAL_ASKPASS_HOSTKEYALIAS",
                          "ZTERMINAL_ASKPASS_VIA_JUMP"}) {
        env.remove(QString::fromLatin1(v));
    }
    if (!socketPath.isEmpty()) {
        env.insert(QStringLiteral("ZTERMINAL_ASKPASS_SOCKET"), socketPath);
    }
    for (const QString &kv : extraEnv) {
        const qsizetype eq = kv.indexOf(QLatin1Char('='));
        env.insert(kv.left(eq), kv.mid(eq + 1));
    }
    p.setProcessEnvironment(env);
    p.setChildProcessModifier([]() { ::setsid(); });
    p.start(QString::fromLatin1(kHelper), {prompt});
    HelperRun r;
    if (!p.waitForStarted(5000)) {
        return r;
    }
    for (int i = 0; i < 1000 && p.state() != QProcess::NotRunning; ++i) {
        QTest::qWait(10); // keep the event loop (the server) running
    }
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
    r.out = p.readAllStandardOutput();
    return r;
}

SecureBuffer secret(const char *s)
{
    return SecureBuffer::fromQString(QString::fromUtf8(s));
}

} // namespace

class TstAskpass : public QObject
{
    Q_OBJECT
private slots:
    void helperExists() { QVERIFY2(QFileInfo(QString::fromLatin1(kHelper)).isExecutable(), kHelper); }

    void deliversOnceToDescendant()
    {
        AskpassServer srv(secret("pa$$ wörd\"'`;"));
        QString err;
        QVERIFY2(srv.listen(&err), qPrintable(err));
        const QString path = srv.socketPath();
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(QFileInfo(path).absolutePath()).constData(), &st), 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0700)); // private directory
        srv.setAllowedAncestor(QCoreApplication::applicationPid()); // the helper is our child
        QSignalSpy served(&srv, &AskpassServer::served);
        const QStringList target{QStringLiteral("ZTERMINAL_ASKPASS_TARGET=stevebj@core-sw1")};
        const HelperRun r = runHelper(QStringLiteral("stevebj@core-sw1's password: "), path, target);
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, QByteArray("pa$$ w\xc3\xb6rd\"'`;\n"));
        QCOMPARE(served.count(), 1);
        // One shot: socket and directory are gone, the server is done.
        QVERIFY(!srv.isActive());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
        // A second password prompt (the stored one was wrong) gets nothing from
        // zterminal: the helper falls back to /dev/tty; with no tty it cancels.
        const HelperRun again = runHelper(QStringLiteral("stevebj@core-sw1's password: "), path, target);
        QCOMPARE(again.exitCode, 1);
        QVERIFY(again.out.isEmpty());
    }

    void environmentForSsh()
    {
        AskpassServer srv(secret("UNIQUE-SECRET-42"));
        QVERIFY(srv.listen());
        const QStringList env = srv.sshEnvironment(QStringLiteral("/usr/libexec/zterminal/zterminal-askpass"));
        QCOMPARE(env, (QStringList{QStringLiteral("SSH_ASKPASS=/usr/libexec/zterminal/zterminal-askpass"),
                                   QStringLiteral("SSH_ASKPASS_REQUIRE=force"),
                                   QStringLiteral("ZTERMINAL_ASKPASS_SOCKET=") + srv.socketPath()}));
        for (const QString &e : env) {
            QVERIFY(!e.contains(QStringLiteral("UNIQUE-SECRET"))); // only the socket *path* is passed
        }
        // With a target: who the password is for goes along (not secret).
        AskpassTarget t;
        t.user = QStringLiteral("stevebj");
        t.host = QStringLiteral("[fe80::1]");
        t.hostKeyAlias = QStringLiteral("core-sw1");
        t.viaJump = true;
        srv.setTarget(t);
        const QStringList env2 = srv.sshEnvironment(QStringLiteral("/h"));
        QVERIFY(env2.contains(QStringLiteral("ZTERMINAL_ASKPASS_TARGET=stevebj@fe80::1")));
        QVERIFY(env2.contains(QStringLiteral("ZTERMINAL_ASKPASS_HOSTKEYALIAS=core-sw1")));
        QVERIFY(env2.contains(QStringLiteral("ZTERMINAL_ASKPASS_VIA_JUMP=1")));
    }

    // Item 3: the stored password only answers the target's own prompt.
    void promptPolicy_data()
    {
        QTest::addColumn<QString>("prompt");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("alias");
        QTest::addColumn<bool>("viaJump");
        QTest::addColumn<bool>("answer");
        const QString t = QStringLiteral("stevebj@core-sw1.example");
        // ssh's own password-auth prompt.
        QTest::newRow("target pw") << "stevebj@core-sw1.example's password: " << t << "" << true << true;
        QTest::newRow("target pw, host case") << "stevebj@CORE-SW1.example's password: " << t << "" << true << true;
        QTest::newRow("user case matters") << "Stevebj@core-sw1.example's password: " << t << "" << false << false;
        QTest::newRow("jump host pw") << "jumpuser@bastion.example's password: " << t << "" << true << false;
        QTest::newRow("same user, jump host") << "stevebj@bastion.example's password: " << t << "" << true << false;
        QTest::newRow("same host, other user") << "root@core-sw1.example's password: " << t << "" << false << false;
        QTest::newRow("host prefix only") << "stevebj@core-sw1.example.evil's password: " << t << "" << false << false;
        QTest::newRow("ipv6 brackets") << "admin@[fe80::1]'s password: " << QStringLiteral("admin@fe80::1") << ""
                                       << false << true;
        // Keyboard-interactive, labelled by ssh >= 8.4; only the first label counts.
        QTest::newRow("kbd target") << "(stevebj@core-sw1.example) Password: " << t << "" << true << true;
        QTest::newRow("kbd jump") << "(jumpuser@bastion.example) Password: " << t << "" << true << false;
        QTest::newRow("kbd jump spoofing target")
            << "(jumpuser@bastion.example) (stevebj@core-sw1.example) Password: " << t << "" << true << false;
        QTest::newRow("kbd jump spoofing pw label")
            << "(jumpuser@bastion.example) stevebj@core-sw1.example's password: " << t << "" << true << false;
        QTest::newRow("kbd alias") << "(stevebj@core-sw1) Password: " << t << "core-sw1" << true << true;
        QTest::newRow("kbd alias no match") << "(stevebj@core-sw2) Password: " << t << "core-sw1" << true << false;
        // Bare prompts: only without a jump host/proxy.
        QTest::newRow("bare, direct") << "Password: " << t << "" << false << true;
        QTest::newRow("bare, via jump") << "Password: " << t << "" << true << false;
        QTest::newRow("bare lower, direct") << "password: " << t << "" << false << true;
        QTest::newRow("bare with spaces in label") << "Enter the password for stevebj@x's password: " << t << ""
                                                   << true << false;
        // Not a password prompt at all.
        QTest::newRow("passphrase") << "Enter passphrase for key '/k': " << t << "" << false << false;
        QTest::newRow("hostkey") << "Are you sure you want to continue connecting (yes/no)? " << t << "" << false
                                 << false;
        // Unknown target: labelled prompts are refused.
        QTest::newRow("no target, labelled") << "stevebj@core-sw1.example's password: " << "" << "" << false << false;
        // ssh truncates long labels to %.30s@%.128s.
        const QString longUser = QString(40, QLatin1Char('u'));
        const QString longHost = QString(140, QLatin1Char('h')) + QStringLiteral(".example");
        QTest::newRow("truncated label") << longUser.left(30) + QLatin1Char('@') + longHost.left(128) + "'s password: "
                                         << longUser + QLatin1Char('@') + longHost << "" << true << true;
        QTest::newRow("truncated wrong")
            << longUser.left(30) + QLatin1Char('@') + QString(128, QLatin1Char('x')) + "'s password: "
            << longUser + QLatin1Char('@') + longHost << "" << true << false;
    }
    void promptPolicy()
    {
        QFETCH(QString, prompt);
        QFETCH(QString, target);
        QFETCH(QString, alias);
        QFETCH(bool, viaJump);
        QFETCH(bool, answer);
        const QByteArray p = prompt.toUtf8(), t = target.toUtf8(), a = alias.toUtf8();
        QCOMPARE(zt_askpass_may_answer(p.constData(), t.constData(), a.constData(), viaJump ? 1 : 0) != 0, answer);
    }

    // End to end: a -J connection; the jump host's prompt gets nothing (the
    // helper asks the tty, there is none), the target's prompt gets the password.
    void jumpHostPromptNeverGetsThePassword()
    {
        AskpassServer srv(secret("target-only"));
        QVERIFY(srv.listen());
        srv.setAllowedAncestor(QCoreApplication::applicationPid());
        QSignalSpy served(&srv, &AskpassServer::served);
        const QStringList env{QStringLiteral("ZTERMINAL_ASKPASS_TARGET=stevebj@core-sw1.example"),
                              QStringLiteral("ZTERMINAL_ASKPASS_VIA_JUMP=1")};
        for (const char *prompt : {"jumpuser@bastion.example's password: ", "(jumpuser@bastion.example) Password: ",
                                   "Password: "}) {
            const HelperRun r = runHelper(QString::fromLatin1(prompt), srv.socketPath(), env);
            QCOMPARE(r.exitCode, 1);
            QVERIFY(!r.out.contains("target-only"));
        }
        QCOMPARE(served.count(), 0);
        QVERIFY(srv.isActive());
        const HelperRun ok = runHelper(QStringLiteral("stevebj@core-sw1.example's password: "), srv.socketPath(), env);
        QCOMPARE(ok.exitCode, 0);
        QCOMPARE(ok.out, QByteArray("target-only\n"));
        QCOMPARE(served.count(), 1);
    }

    void parsesSshConfigDump()
    {
        AskpassTarget fb;
        fb.user = QStringLiteral("stevebj");
        fb.host = QStringLiteral("sw1");
        const QByteArray dump = "user admin\nhostname 10.0.0.1\nport 22\nhostkeyalias sw1-key\n"
                                "proxyjump jump@bastion\nproxycommand none\n";
        const AskpassTarget t = AskpassServer::parseSshConfigDump(dump, fb);
        QCOMPARE(t.user, QStringLiteral("admin"));
        QCOMPARE(t.host, QStringLiteral("10.0.0.1"));
        QCOMPARE(t.hostKeyAlias, QStringLiteral("sw1-key"));
        QVERIFY(t.viaJump);
        const AskpassTarget d = AskpassServer::parseSshConfigDump("user u\nhostname h\nproxyjump none\n", fb);
        QVERIFY(!d.viaJump);
        QVERIFY(d.hostKeyAlias.isEmpty());
        // Garbage: the fallback.
        const AskpassTarget g = AskpassServer::parseSshConfigDump("ssh: unknown option -- G\n", fb);
        QCOMPARE(g.host, fb.host);
        QCOMPARE(g.user, fb.user);
    }

    void resolveTargetFallsBack()
    {
        AskpassTarget fb;
        fb.user = QStringLiteral("stevebj");
        fb.host = QStringLiteral("sw1");
        fb.viaJump = true;
        // A program that doesn't exist, and one that fails.
        const AskpassTarget a = AskpassServer::resolveTarget(QStringLiteral("/nonexistent/ssh"), {}, fb, 1000);
        QCOMPARE(a.host, fb.host);
        const AskpassTarget b = AskpassServer::resolveTarget(QStringLiteral("false"), {}, fb, 1000);
        QCOMPARE(b.host, fb.host);
        QVERIFY(b.viaJump);
    }

    void resolveTargetWithRealSsh()
    {
        const QString ssh = QStandardPaths::findExecutable(QStringLiteral("ssh"));
        if (ssh.isEmpty()) {
            QSKIP("no ssh installed");
        }
        AskpassTarget fb;
        fb.host = QStringLiteral("fallback.invalid");
        // -F /dev/null: no user config. ssh -G only prints, never connects.
        const AskpassTarget t = AskpassServer::resolveTarget(
            ssh,
            {QStringLiteral("-F"), QStringLiteral("/dev/null"), QStringLiteral("-l"), QStringLiteral("admin"),
             QStringLiteral("-J"), QStringLiteral("jump.invalid"), QStringLiteral("--"), QStringLiteral("Core-SW1.invalid")},
            fb);
        QCOMPARE(t.user, QStringLiteral("admin"));
        QCOMPARE(t.host.toLower(), QStringLiteral("core-sw1.invalid"));
        QVERIFY(t.viaJump);
    }

    void rejectsProcessesOutsideTheSession()
    {
        AskpassServer srv(secret("topsecret"));
        QVERIFY(srv.listen());
        // Allowed: an unrelated process (a sibling `sleep`), so our helper child is not a descendant.
        QProcess sibling;
        sibling.start(QStringLiteral("sleep"), {QStringLiteral("30")});
        QVERIFY(sibling.waitForStarted());
        srv.setAllowedAncestor(sibling.processId());
        QSignalSpy rejected(&srv, &AskpassServer::rejected);
        QSignalSpy served(&srv, &AskpassServer::served);
        const HelperRun r = runHelper(QStringLiteral("Password: "), srv.socketPath());
        QCOMPARE(r.exitCode, 1); // got nothing, no tty to ask on
        QVERIFY(!r.out.contains("topsecret"));
        QCOMPARE(served.count(), 0);
        QVERIFY(rejected.count() >= 1);
        QVERIFY(rejected.first().first().toString().contains(QStringLiteral("not part of this ssh session")));
        QVERIFY(srv.isActive()); // still waiting for the real helper
        sibling.kill(); // our own child, by handle
        sibling.waitForFinished();
        // No ancestor set at all: also rejected.
        AskpassServer none(secret("topsecret"));
        QVERIFY(none.listen());
        QCOMPARE(runHelper(QStringLiteral("Password: "), none.socketPath()).exitCode, 1);
        QVERIFY(none.isActive());
    }

    void nonPasswordPromptsNeverUseTheSocket()
    {
        AskpassServer srv(secret("topsecret"));
        QVERIFY(srv.listen());
        srv.setAllowedAncestor(QCoreApplication::applicationPid());
        QSignalSpy served(&srv, &AskpassServer::served);
        for (const char *prompt : {"Are you sure you want to continue connecting (yes/no/[fingerprint])? ",
                                   "Enter passphrase for key '/home/u/.ssh/id_ed25519': "}) {
            const HelperRun r = runHelper(QString::fromLatin1(prompt), srv.socketPath());
            QCOMPARE(r.exitCode, 1);
            QVERIFY(r.out.isEmpty());
        }
        QCOMPARE(served.count(), 0);
        QVERIFY(srv.isActive());
    }

    void expires()
    {
        AskpassServer srv(secret("x"));
        QVERIFY(srv.listen());
        QSignalSpy expired(&srv, &AskpassServer::expired);
        srv.setTimeoutMs(50);
        QTRY_COMPARE_WITH_TIMEOUT(expired.count(), 1, 3000);
        QVERIFY(!srv.isActive());
        QVERIFY(!QFileInfo::exists(srv.socketPath()));
    }

    void descendantCheck()
    {
        const qint64 me = QCoreApplication::applicationPid();
        QVERIFY(AskpassServer::isDescendant(me, me));
        QVERIFY(AskpassServer::isDescendant(me, getppid()));
        QVERIFY(!AskpassServer::isDescendant(getppid(), me));
        QVERIFY(!AskpassServer::isDescendant(me, -1));
        QVERIFY(!AskpassServer::isDescendant(me, 1)); // init is never an accepted ancestor
    }

    void findHelperHonoursTestOverride()
    {
        AskpassServer::setHelperPathForTests(QString::fromLatin1(kHelper));
        QCOMPARE(AskpassServer::findHelper(), QString::fromLatin1(kHelper));
        AskpassServer::setHelperPathForTests(QStringLiteral("/nonexistent/zterminal-askpass"));
        QVERIFY(AskpassServer::findHelper().isEmpty());
        AskpassServer::setHelperPathForTests(QString());
    }

    // Item 9: $ZTERMINAL_ASKPASS is a development override only.
    void environmentOverrideOnlyInDevBuilds()
    {
        qputenv("ZTERMINAL_ASKPASS", "/nonexistent/zterminal-askpass");
#if defined(ZTERMINAL_DEV_OVERRIDES)
        QVERIFY(AskpassServer::findHelper().isEmpty()); // honoured (and not executable)
#else
        QVERIFY(AskpassServer::findHelper() != QStringLiteral("/nonexistent/zterminal-askpass")); // ignored
#endif
        qunsetenv("ZTERMINAL_ASKPASS");
    }
};

QTEST_GUILESS_MAIN(TstAskpass)
#include "tst_askpass.moc"
