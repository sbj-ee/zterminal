// PTY backend: output, exit codes, window size (TIOCSWINSZ), TERM/COLORTERM.
#include "Pty.hpp"

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
};

QTEST_GUILESS_MAIN(TstPty)
#include "tst_pty.moc"
