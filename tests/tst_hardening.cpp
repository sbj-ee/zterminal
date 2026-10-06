// Process hardening (release builds): no core dumps, not dumpable on Linux,
// and everything zterminal needs afterwards still works: /proc/self/exe
// (New Window), a PTY child, and the askpass helper reaching our socket
// (its peer check reads the helper's process ancestry, not ours).
// Own executable: the hardening can't be undone for the rest of the process.
#include "AskpassServer.hpp"
#include "ProcessHardening.hpp"
#include "Pty.hpp"
#include "SecureBuffer.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>

#include <sys/resource.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

using namespace zterminal;

class TstHardening : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QString why;
        QVERIFY2(applyProcessHardening(&why), qPrintable(why));
    }

    void noCoreDumps()
    {
        rlimit rl{};
        QCOMPARE(::getrlimit(RLIMIT_CORE, &rl), 0);
        QCOMPARE(rl.rlim_cur, rlim_t(0));
        QCOMPARE(rl.rlim_max, rlim_t(0)); // hard limit too: can't be raised again
        const rlimit raise{RLIM_INFINITY, RLIM_INFINITY};
        QVERIFY(::setrlimit(RLIMIT_CORE, &raise) != 0);
    }

    void notDumpable()
    {
#if defined(__linux__)
        QCOMPARE(::prctl(PR_GET_DUMPABLE, 0, 0, 0, 0), 0);
        // New Window re-executes ourselves via applicationFilePath().
        QVERIFY(QFileInfo(QCoreApplication::applicationFilePath()).isExecutable());
        QVERIFY(!QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget().isEmpty());
#else
        QSKIP("PR_SET_DUMPABLE is Linux-only");
#endif
    }

    void ptyChildStillWorks()
    {
        Pty pty;
        QSignalSpy data(&pty, &Pty::dataReceived);
        QVERIFY(pty.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("echo hardened-ok")}, 24, 80));
        QByteArray got;
        QTRY_VERIFY_WITH_TIMEOUT(([&]() {
                                     for (const auto &args : data) {
                                         got += args.first().toByteArray();
                                     }
                                     data.clear();
                                     return got.contains("hardened-ok");
                                 }()),
                                 5000);
    }

    void askpassHelperStillDelivers()
    {
        AskpassServer srv(SecureBuffer::fromQString(QStringLiteral("still-works")));
        QVERIFY(srv.listen());
        srv.setAllowedAncestor(QCoreApplication::applicationPid());
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("ZTERMINAL_ASKPASS_SOCKET"), srv.socketPath());
        env.insert(QStringLiteral("ZTERMINAL_ASKPASS_TARGET"), QStringLiteral("u@h"));
        p.setProcessEnvironment(env);
        p.setChildProcessModifier([]() { ::setsid(); });
        p.start(QString::fromLatin1(ZTERMINAL_ASKPASS_BIN), {QStringLiteral("u@h's password: ")});
        QVERIFY(p.waitForStarted(5000));
        for (int i = 0; i < 500 && p.state() != QProcess::NotRunning; ++i) {
            QTest::qWait(10);
        }
        QCOMPARE(p.exitCode(), 0);
        QCOMPARE(p.readAllStandardOutput(), QByteArray("still-works\n"));
    }
};

QTEST_GUILESS_MAIN(TstHardening)
#include "tst_hardening.moc"
