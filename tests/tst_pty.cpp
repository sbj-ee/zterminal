// PTY backend: output, exit codes, window size (TIOCSWINSZ), TERM/COLORTERM,
// home cwd + HOME, login shell for the default shell.
#include "Pty.hpp"

#include <QDir>
#include <QSignalSpy>
#include <QTest>

#include <csignal>

using namespace zterminal;

class TstPty : public QObject
{
    Q_OBJECT

    static QByteArray runUntilExit(Pty &pty, int *exitCode, int timeoutMs = 10000)
    {
        QByteArray out;
        QObject::connect(&pty, &Pty::dataReceived, [&out](const QByteArray &b) { out += b; });
        QSignalSpy finished(&pty, &Pty::finished);
        if (!finished.wait(timeoutMs) && finished.isEmpty()) {
            return out;
        }
        *exitCode = finished.first().at(0).toInt();
        return out;
    }

private slots:
    void outputAndEnvironment()
    {
        Pty pty;
        QVERIFY(pty.start(QStringLiteral("/bin/sh"),
                          {QStringLiteral("-c"), QStringLiteral("printf 'hello %s %s' \"$TERM\" \"$COLORTERM\"")},
                          24, 80));
        int code = -99;
        const QByteArray out = runUntilExit(pty, &code);
        QCOMPARE(code, 0);
        QVERIFY2(out.contains("hello xterm-256color truecolor"), out.constData());
        QVERIFY(!pty.isRunning());
    }

    void exitCode()
    {
        Pty pty;
        QVERIFY(pty.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 3")}, 24, 80));
        int code = -99;
        runUntilExit(pty, &code);
        QCOMPARE(code, 3);
    }

    void initialAndResizedWindowSize()
    {
        Pty pty;
        QVERIFY(pty.start(QStringLiteral("/bin/sh"),
                          {QStringLiteral("-c"), QStringLiteral("stty size; read x; stty size")}, 30, 100));
        QByteArray out;
        connect(&pty, &Pty::dataReceived, this, [&out](const QByteArray &b) { out += b; });
        QTRY_VERIFY_WITH_TIMEOUT(out.contains("30 100"), 10000);
        pty.resize(40, 120);
        pty.write("\n");
        QTRY_VERIFY_WITH_TIMEOUT(out.contains("40 120"), 10000);
    }

    void inputEcho()
    {
        Pty pty;
        QVERIFY(pty.start(QStringLiteral("/bin/cat"), {}, 24, 80));
        QByteArray out;
        connect(&pty, &Pty::dataReceived, this, [&out](const QByteArray &b) { out += b; });
        pty.write("ping\n");
        QTRY_VERIFY_WITH_TIMEOUT(out.contains("ping\r\nping"), 10000); // tty echo + cat
        pty.terminate();
        QVERIFY(!pty.isRunning());
    }

    void badProgramReports127()
    {
        Pty pty;
        QVERIFY(pty.start(QStringLiteral("/nonexistent/zterminal-test"), {}, 24, 80));
        int code = -99;
        const QByteArray out = runUntilExit(pty, &code);
        QCOMPARE(code, 127);
        QVERIFY(out.contains("failed to execute"));
    }

    // Writing VINTR (0x03) to the PTY master must deliver SIGINT to the
    // foreground process group — the kernel path Ctrl+C relies on once the
    // UI emits the byte (see AA_MacDontSwapCtrlAndMeta in main.cpp).
    void ctrlCByteDeliversSigint()
    {
        Pty pty;
        QVERIFY(pty.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")}, 24, 80));
        QVERIFY(pty.isRunning());
        QSignalSpy finished(&pty, &Pty::finished);
        QTest::qWait(100); // let sleep become the foreground group
        pty.write(QByteArray(1, '\x03'));
        QVERIFY2(finished.wait(5000), "sleep did not exit after VINTR (0x03)");
        QCOMPARE(finished.size(), 1);
        const int code = finished.first().at(0).toInt();
        const bool crashed = finished.first().at(1).toBool();
        // SIGINT → 128+SIGINT (130) with crashed=true, or a clean exit on some libc.
        QVERIFY2(crashed || code == 130 || code == 0,
                 qPrintable(QStringLiteral("unexpected exit code=%1 crashed=%2").arg(code).arg(crashed)));
        if (crashed) {
            QCOMPARE(code, 128 + SIGINT);
        }
        QVERIFY(!pty.isRunning());
    }

    // Finder/Dock launch leaves the app at cwd=/; the PTY child must still
    // start in $HOME with HOME set (the "/ $" prompt bug).
    void childStartsInHomeWithHomeSet()
    {
        const QString home = QDir::homePath();
        QVERIFY(!home.isEmpty());
        QVERIFY2(home != QLatin1String("/"), "test user home must not be /");
        const QString prev = QDir::currentPath();
        QVERIFY(QDir::setCurrent(QStringLiteral("/")));
        QCOMPARE(QDir::currentPath(), QStringLiteral("/"));

        Pty pty;
        QVERIFY(pty.start(QStringLiteral("/bin/sh"),
                          {QStringLiteral("-c"),
                           QStringLiteral("printf 'ZT_CWD=%s\nZT_HOME=%s\n' \"$PWD\" \"$HOME\"")},
                          24, 80));
        int code = -99;
        const QByteArray out = runUntilExit(pty, &code);
        QVERIFY(QDir::setCurrent(prev));
        QCOMPARE(code, 0);
        const QByteArray wantCwd = QByteArray("ZT_CWD=") + home.toLocal8Bit();
        const QByteArray wantHome = QByteArray("ZT_HOME=") + home.toLocal8Bit();
        QVERIFY2(out.contains(wantCwd), out.constData());
        QVERIFY2(out.contains(wantHome), out.constData());
    }

    // Empty program = login shell (argv0 "-zsh"/etc). Probe with a marker the
    // interactive shell prints, then exit. Tolerates motd/profile noise.
    void defaultShellIsLoginInHome()
    {
        const QString home = QDir::homePath();
        const QString prev = QDir::currentPath();
        QVERIFY(QDir::setCurrent(QStringLiteral("/")));

        Pty pty;
        QVERIFY(pty.start({}, {}, 24, 80));
        QByteArray out;
        connect(&pty, &Pty::dataReceived, this, [&out](const QByteArray &b) { out += b; });
        QSignalSpy finished(&pty, &Pty::finished);
        // bash: $- has l; zsh: [[ -o login ]]; either prints ZT_LOGIN=1.
        QByteArray cmd;
        cmd += "printf 'ZT_CWD=%s\nZT_HOME=%s\n' \"$PWD\" \"$HOME\"; ";
        cmd += "ZT_LOGIN=0; ";
        cmd += "case $- in *l*) ZT_LOGIN=1;; esac; ";
        cmd += "if eval '[[ -o login ]]' 2>/dev/null; then ZT_LOGIN=1; fi; ";
        cmd += "printf 'ZT_LOGIN=%s\n' \"$ZT_LOGIN\"; exit\n";
        pty.write(cmd);
        QVERIFY2(finished.wait(15000), "default shell did not exit");
        QVERIFY(QDir::setCurrent(prev));
        QVERIFY2(out.contains(QByteArray("ZT_CWD=") + home.toLocal8Bit()), out.constData());
        QVERIFY2(out.contains(QByteArray("ZT_HOME=") + home.toLocal8Bit()), out.constData());
        QVERIFY2(out.contains("ZT_LOGIN=1"), out.constData());
    }
};

QTEST_GUILESS_MAIN(TstPty)
#include "tst_pty.moc"
