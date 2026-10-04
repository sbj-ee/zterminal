// PTY backend: output, exit codes, window size (TIOCSWINSZ), TERM/COLORTERM.
#include "Pty.hpp"

#include <QSignalSpy>
#include <QTest>

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
};

QTEST_GUILESS_MAIN(TstPty)
#include "tst_pty.moc"
