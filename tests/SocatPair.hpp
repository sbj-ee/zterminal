#pragma once
// A virtual serial cable: `socat pty,link=A pty,link=B`. Each end is a real
// tty, so QSerialPort can open it like /dev/ttyUSB0.
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

struct SocatPair {
    QTemporaryDir dir;
    QProcess proc;
    QString a, b;

    static bool available() { return !QStandardPaths::findExecutable(QStringLiteral("socat")).isEmpty(); }

    bool start()
    {
        a = dir.filePath(QStringLiteral("ttyA"));
        b = dir.filePath(QStringLiteral("ttyB"));
        proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        proc.start(QStringLiteral("socat"),
                   {QStringLiteral("pty,raw,echo=0,link=") + a, QStringLiteral("pty,raw,echo=0,link=") + b});
        if (!proc.waitForStarted(5000)) {
            return false;
        }
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 5000) {
            if (QFileInfo::exists(a) && QFileInfo::exists(b)) {
                return true;
            }
            QTest::qWait(20);
        }
        return false;
    }
    // "Unplug": stop this socat (our own child, by its PID only).
    void stop()
    {
        if (proc.state() != QProcess::NotRunning) {
            proc.terminate();
            if (!proc.waitForFinished(3000)) {
                proc.kill();
                proc.waitForFinished(3000);
            }
        }
    }
    ~SocatPair() { stop(); }
};
