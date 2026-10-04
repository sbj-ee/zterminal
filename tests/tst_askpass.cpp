// AskpassServer + zterminal-askpass: one-shot delivery to a descendant over a
// private Unix socket, rejection of other processes, prompts that aren't
// password prompts never touch the socket, and the no-tty fallback.
#include "AskpassServer.hpp"
#include "SecureBuffer.hpp"

#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
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
HelperRun runHelper(const QString &prompt, const QString &socketPath)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("ZTERMINAL_ASKPASS_SOCKET"));
    if (!socketPath.isEmpty()) {
        env.insert(QStringLiteral("ZTERMINAL_ASKPASS_SOCKET"), socketPath);
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
        const HelperRun r = runHelper(QStringLiteral("stevebj@core-sw1's password: "), path);
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.out, QByteArray("pa$$ w\xc3\xb6rd\"'`;\n"));
        QCOMPARE(served.count(), 1);
        // One shot: socket and directory are gone, the server is done.
        QVERIFY(!srv.isActive());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
        // A second password prompt (the stored one was wrong) gets nothing from
        // zterminal: the helper falls back to /dev/tty; with no tty it cancels.
        const HelperRun again = runHelper(QStringLiteral("stevebj@core-sw1's password: "), path);
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

    void findHelperHonoursOverride()
    {
        qputenv("ZTERMINAL_ASKPASS", kHelper);
        QCOMPARE(AskpassServer::findHelper(), QString::fromLatin1(kHelper));
        qputenv("ZTERMINAL_ASKPASS", "/nonexistent/zterminal-askpass");
        QVERIFY(AskpassServer::findHelper().isEmpty());
        qunsetenv("ZTERMINAL_ASKPASS");
    }
};

QTEST_GUILESS_MAIN(TstAskpass)
#include "tst_askpass.moc"
