// Serial sessions (0.3.0) against a socat pty pair: data both ways, Enter
// mapping and local echo, paste pacing timing and cancel, Send Break, unplug,
// error messages, and settings round-trip.
#include "SerialBackend.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"
#include "SocatPair.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QSerialPort>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <unistd.h>

using namespace zterminal;

namespace {

SessionConfig serialCfg(const QString &device)
{
    SessionConfig s;
    s.name = QStringLiteral("console");
    s.type = SessionConfig::Type::Serial;
    s.serialDevice = device;
    return s;
}

// The far end of the cable (the "router").
struct Peer {
    QSerialPort port;
    QByteArray got;
    QList<qint64> times; // ms since `clock` started, per received chunk byte
    QElapsedTimer clock;
    explicit Peer(const QString &dev)
    {
        port.setPortName(dev);
        port.setBaudRate(9600);
        clock.start();
        QObject::connect(&port, &QSerialPort::readyRead, [this]() {
            const QByteArray d = port.readAll();
            for (int i = 0; i < d.size(); ++i) {
                times << clock.elapsed();
            }
            got += d;
        });
    }
    bool open() { return port.open(QIODevice::ReadWrite); }
};

} // namespace

class TstSerial : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (!SocatPair::available()) {
            QSKIP("socat not installed");
        }
    }

    void settingsRoundTrip()
    {
        QTemporaryDir dir;
        const SessionStore store(dir.path());
        SessionConfig s = serialCfg(QStringLiteral("/dev/ttyUSB0"));
        s.baudRate = 115200;
        s.dataBits = 7;
        s.parity = QStringLiteral("even");
        s.stopBits = 2;
        s.flowControl = QStringLiteral("rtscts");
        s.localEcho = true;
        s.enterSends = QStringLiteral("crlf");
        s.charDelayMs = 5;
        s.lineDelayMs = 200;
        s.breakMs = 500;
        QVERIFY(store.save(s));
        const auto back = store.load(s.name);
        QVERIFY(back);
        QVERIFY(*back == s);

        // Defaults: 9600 8N1, no flow control, CR, no echo, no pacing, 300 ms break.
        const SessionConfig d;
        QCOMPARE(d.baudRate, 9600);
        QCOMPARE(d.dataBits, 8);
        QCOMPARE(d.parity, QStringLiteral("none"));
        QCOMPARE(d.stopBits, 1);
        QCOMPARE(d.flowControl, QStringLiteral("none"));
        QCOMPARE(d.enterSends, QStringLiteral("cr"));
        QVERIFY(!d.localEcho);
        QCOMPARE(d.charDelayMs, 0);
        QCOMPARE(d.lineDelayMs, 0);
        QCOMPARE(d.breakMs, 300);

        // A minimal hand-written file gets the defaults.
        {
            QSettings f(store.filePathFor(QStringLiteral("min")), QSettings::IniFormat);
            f.setValue(QStringLiteral("session/name"), QStringLiteral("min"));
            f.setValue(QStringLiteral("session/type"), QStringLiteral("serial"));
            f.setValue(QStringLiteral("serial/device"), QStringLiteral("/dev/ttyACM0"));
        }
        const auto min = store.load(QStringLiteral("min"));
        QVERIFY(min);
        QCOMPARE(min->baudRate, 9600);
        QCOMPARE(min->breakMs, 300);
        QVERIFY(validateSerial(*min).isEmpty());
    }

    void validation()
    {
        SessionConfig s = serialCfg(QString());
        QVERIFY(!validateSerial(s).isEmpty());
        s.serialDevice = QStringLiteral("ttyUSB0");
        QVERIFY(validateSerial(s).contains(QStringLiteral("absolute")));
        s.serialDevice = QStringLiteral("/dev/ttyUSB0");
        QVERIFY(validateSerial(s).isEmpty());
        s.baudRate = 0;
        QVERIFY(!validateSerial(s).isEmpty());
        s.baudRate = 9600;
        s.parity = QStringLiteral("bogus");
        QVERIFY(!validateSerial(s).isEmpty());
        s.parity = QStringLiteral("odd");
        s.dataBits = 9;
        QVERIFY(!validateSerial(s).isEmpty());
        QCOMPARE(enterSequence(QStringLiteral("cr")), QByteArray("\r"));
        QCOMPARE(enterSequence(QStringLiteral("crlf")), QByteArray("\r\n"));
        QCOMPARE(enterSequence(QStringLiteral("lf")), QByteArray("\n"));
    }

    void dataBothWays()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SerialBackend be;
        QVERIFY2(be.open(serialCfg(cable.a)), qPrintable(be.errorString()));
        QVERIFY(be.isOpen());
        QCOMPARE(be.port()->baudRate(), 9600);
        QCOMPARE(be.port()->dataBits(), QSerialPort::Data8);
        QCOMPARE(be.port()->parity(), QSerialPort::NoParity);
        QCOMPARE(be.port()->stopBits(), QSerialPort::OneStop);
        QCOMPARE(be.port()->flowControl(), QSerialPort::NoFlowControl);
        Peer router(cable.b);
        QVERIFY(router.open());

        be.write("show version\r");
        QTRY_COMPARE_WITH_TIMEOUT(router.got, QByteArray("show version\r"), 5000);

        QByteArray fromRouter;
        connect(&be, &SerialBackend::dataReceived, this, [&fromRouter](const QByteArray &d) { fromRouter += d; });
        router.port.write("\r\nRouter>");
        QTRY_COMPARE_WITH_TIMEOUT(fromRouter, QByteArray("\r\nRouter>"), 5000);
    }

    void enterMappingAndLocalEcho()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig cfg = serialCfg(cable.a);
        cfg.enterSends = QStringLiteral("crlf");
        cfg.localEcho = true;
        SerialBackend be;
        QVERIFY(be.open(cfg));
        Peer router(cable.b);
        QVERIFY(router.open());
        QByteArray echoed;
        connect(&be, &SerialBackend::dataReceived, this, [&echoed](const QByteArray &d) { echoed += d; });
        be.write("en\r");
        QTRY_COMPARE_WITH_TIMEOUT(router.got, QByteArray("en\r\n"), 5000);
        QCOMPARE(echoed, QByteArray("en\r\n"));
    }

    void pacingPerCharacter()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig cfg = serialCfg(cable.a);
        cfg.charDelayMs = 30;
        SerialBackend be;
        QVERIFY(be.open(cfg));
        QVERIFY(be.pacingEnabled());
        Peer router(cable.b);
        QVERIFY(router.open());
        router.clock.restart();
        be.write("abcdef");
        QCOMPARE(be.pending(), 5); // queued, sent asynchronously
        QTRY_COMPARE_WITH_TIMEOUT(router.got, QByteArray("abcdef"), 5000);
        QCOMPARE(be.pending(), 0);
        // 5 gaps of 30 ms (generous lower bound for timer jitter).
        const qint64 span = router.times.last() - router.times.first();
        QVERIFY2(span >= 5 * 30 - 15, qPrintable(QString::number(span)));
        for (int i = 1; i < router.times.size(); ++i) {
            QVERIFY2(router.times[i] - router.times[i - 1] >= 20, qPrintable(QString::number(router.times[i] - router.times[i - 1])));
        }
    }

    void pacingPerLine()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig cfg = serialCfg(cable.a);
        cfg.lineDelayMs = 150;
        SerialBackend be;
        QVERIFY(be.open(cfg));
        Peer router(cable.b);
        QVERIFY(router.open());
        router.clock.restart();
        be.write("interface Gi0/1\rdescription uplink\rno shutdown\r");
        QTRY_COMPARE_WITH_TIMEOUT(router.got, QByteArray("interface Gi0/1\rdescription uplink\rno shutdown\r"), 5000);
        const int l2 = router.got.indexOf("description");
        const int l3 = router.got.indexOf("no shutdown");
        // Each line arrives whole; the next one waits >= the line delay.
        QVERIFY2(router.times[l2] - router.times[l2 - 1] >= 135, qPrintable(QString::number(router.times[l2] - router.times[l2 - 1])));
        QVERIFY2(router.times[l3] - router.times[l3 - 1] >= 135, qPrintable(QString::number(router.times[l3] - router.times[l3 - 1])));
        QVERIFY(router.times[l2 - 2] - router.times[0] < 100); // no delay inside a line
    }

    void largePasteCanBeCancelled()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig cfg = serialCfg(cable.a);
        cfg.charDelayMs = 40;
        SerialBackend be;
        QVERIFY(be.open(cfg));
        Peer router(cable.b);
        QVERIFY(router.open());
        QSignalSpy pending(&be, &SerialBackend::pendingChanged);
        be.write(QByteArray(200, 'x'));
        QTest::qWait(150);
        QVERIFY(be.pending() > 150);
        be.cancelPending();
        QCOMPARE(be.pending(), 0);
        QCOMPARE(pending.last().first().toLongLong(), 0);
        QTest::qWait(250);
        QVERIFY2(router.got.size() < 10, qPrintable(QString::number(router.got.size())));
        // Still usable afterwards.
        router.got.clear();
        be.write("y");
        QTRY_COMPARE_WITH_TIMEOUT(router.got, QByteArray("y"), 5000);
    }

    void sendBreakHoldsTheLine()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig cfg = serialCfg(cable.a);
        SerialBackend be;
        QVERIFY(be.open(cfg));
        QSignalSpy spy(&be, &SerialBackend::breakChanged);
        QElapsedTimer t;
        t.start();
        QVERIFY(be.sendBreak());
        QVERIFY(be.isBreakActive());
        QVERIFY(be.port()->isBreakEnabled()); // setBreakEnabled(true) was called
        QVERIFY(!be.sendBreak());             // one at a time
        QTRY_VERIFY_WITH_TIMEOUT(!be.isBreakActive(), 3000);
        QVERIFY2(t.elapsed() >= 290, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!be.port()->isBreakEnabled());
        QCOMPARE(spy.size(), 2);
        QCOMPARE(spy.at(0).first().toBool(), true);
        QCOMPARE(spy.at(1).first().toBool(), false);

        // Configurable duration.
        cfg.breakMs = 80;
        QVERIFY(be.open(cfg));
        t.restart();
        QVERIFY(be.sendBreak());
        QTRY_VERIFY_WITH_TIMEOUT(!be.isBreakActive(), 3000);
        QVERIFY(t.elapsed() >= 75 && t.elapsed() < 280);
    }

    void unplugReportsDisconnected()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SerialBackend be;
        QVERIFY(be.open(serialCfg(cable.a)));
        QSignalSpy gone(&be, &SerialBackend::disconnected);
        cable.stop();
        QTRY_VERIFY_WITH_TIMEOUT(gone.size() == 1, 5000);
        QVERIFY(gone.first().first().toString().contains(QStringLiteral("disconnected")));
        QVERIFY(!be.isOpen());
    }

    void errorMessages()
    {
        SerialBackend be;
        // Not found
        QVERIFY(!be.open(serialCfg(QStringLiteral("/dev/ttyUSB-zterminal-test-missing"))));
        QVERIFY2(be.errorString().contains(QStringLiteral("was not found")), qPrintable(be.errorString()));
        // Invalid settings never reach the device
        QVERIFY(!be.open(serialCfg(QStringLiteral("relative"))));
        QVERIFY(be.errorString().contains(QStringLiteral("absolute")));
        // Permission denied -> the dialout explanation
        const QString perm = SerialBackend::describeError(QSerialPort::PermissionError, QStringLiteral("/dev/ttyUSB0"));
        QVERIFY(perm.contains(QStringLiteral("Permission denied opening /dev/ttyUSB0")));
        QVERIFY(perm.contains(QStringLiteral("dialout")));
        QVERIFY(perm.contains(QStringLiteral("sudo usermod -aG dialout $USER")));
        QVERIFY(perm.contains(QStringLiteral("log out and back in")));
        QVERIFY(SerialBackend::describeError(QSerialPort::OpenError, QStringLiteral("/dev/ttyS0")).contains(QStringLiteral("already open")));
        QVERIFY(SerialBackend::describeError(QSerialPort::ResourceError, QStringLiteral("/dev/ttyS0")).contains(QStringLiteral("unplugged")));

        // ...and for real: a device node we may not open.
        if (::geteuid() == 0) {
            QSKIP("running as root: permissions are not enforced");
        }
        SocatPair cable;
        QVERIFY(cable.start());
        const QString target = QFileInfo(cable.a).symLinkTarget();
        QVERIFY(!target.isEmpty());
        QVERIFY(QFile::setPermissions(target, QFileDevice::Permissions{}));
        QVERIFY(!be.open(serialCfg(cable.a)));
        QVERIFY2(be.errorString().contains(QStringLiteral("usermod -aG dialout")), qPrintable(be.errorString()));
    }
};

QTEST_GUILESS_MAIN(TstSerial)
#include "tst_serial.moc"
