// Serial sessions in the UI (offscreen) against a socat pty pair: dialog
// fields, title, typing and output, Send Break, paced paste with Cancel,
// unplug banner + Reconnect, permission error banner, ad-hoc `zt serial`.
#include "MainWindow.hpp"
#include "SerialBackend.hpp"
#include "SessionDialog.hpp"
#include "SessionStore.hpp"
#include "SocatPair.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "WindowTitle.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSerialPort>
#include <QSettings>
#include <QSpinBox>
#include <QTest>
#include <QtGlobal>
#include <QTimer>

#include <functional>

#include <unistd.h>

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

QString screen(MainWindow &w)
{
    QString all;
    for (int r = 0; r < w.terminal()->totalLines(); ++r) {
        all += w.terminal()->lineText(r).trimmed() + QLatin1Char('\n');
    }
    return all;
}

// Run fn on the next modal dialog (e.g. the paste confirmation).
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

SessionConfig console(const QString &dev)
{
    SessionConfig s;
    s.name = QStringLiteral("SG250 console");
    s.type = SessionConfig::Type::Serial;
    s.serialDevice = dev;
    return s;
}

struct Router {
    QSerialPort port;
    QByteArray got;
    explicit Router(const QString &dev)
    {
        port.setPortName(dev);
        QObject::connect(&port, &QSerialPort::readyRead, [this]() { got += port.readAll(); });
    }
};
} // namespace

class TstSerialUi : public QObject
{
    Q_OBJECT
    SessionStore store;

private slots:
    void initTestCase()
    {
        if (!SocatPair::available()) {
            QSKIP("socat not installed");
        }
    }
    void init() { QDir(store.directory()).removeRecursively(); }

    void dialogSerialFields()
    {
        SessionDialog d(store, SessionConfig{});
        auto *type = child<QComboBox>(&d, "type");
        type->setCurrentIndex(type->findData(QStringLiteral("serial")));
        QVERIFY(child<QWidget>(&d, "serialGroup")->isEnabled());
        QVERIFY(!child<QWidget>(&d, "serialGroup")->isHidden());
        QVERIFY(child<QWidget>(&d, "sshGroup")->isHidden());
        // Defaults: 9600 8N1, no flow control, CR, no echo, no pacing, 300 ms break.
        QCOMPARE(child<QComboBox>(&d, "baudRate")->currentText(), QStringLiteral("9600"));
        QCOMPARE(child<QComboBox>(&d, "dataBits")->currentData().toInt(), 8);
        QCOMPARE(child<QComboBox>(&d, "parity")->currentData().toString(), QStringLiteral("none"));
        QCOMPARE(child<QComboBox>(&d, "stopBits")->currentData().toInt(), 1);
        QCOMPARE(child<QComboBox>(&d, "flowControl")->currentData().toString(), QStringLiteral("none"));
        QCOMPARE(child<QComboBox>(&d, "enterSends")->currentData().toString(), QStringLiteral("cr"));
        QVERIFY(!child<QCheckBox>(&d, "localEcho")->isChecked());
        QCOMPARE(child<QSpinBox>(&d, "breakMs")->value(), 300);
        auto *baud = child<QComboBox>(&d, "baudRate");
        QVERIFY(baud->findText(QStringLiteral("115200")) >= 0);
        QVERIFY(child<QComboBox>(&d, "serialDevice")->isEditable());

        // Missing / relative device is refused.
        child<QLineEdit>(&d, "name")->setText(QStringLiteral("sw"));
        QVERIFY(!d.saveCurrent());
        child<QComboBox>(&d, "serialDevice")->setCurrentText(QStringLiteral("ttyUSB0"));
        QVERIFY(!d.saveCurrent());
        QVERIFY(d.errorText().contains(QStringLiteral("absolute")));

        // Fill everything and save.
        child<QComboBox>(&d, "serialDevice")->setCurrentText(QStringLiteral("/dev/ttyACM0"));
        baud->setCurrentText(QStringLiteral("115200"));
        auto pick = [&d](const char *name, const QVariant &v) {
            auto *c = child<QComboBox>(&d, name);
            c->setCurrentIndex(c->findData(v));
        };
        pick("dataBits", 7);
        pick("parity", QStringLiteral("even"));
        pick("stopBits", 2);
        pick("flowControl", QStringLiteral("xonxoff"));
        pick("enterSends", QStringLiteral("crlf"));
        child<QCheckBox>(&d, "localEcho")->setChecked(true);
        child<QSpinBox>(&d, "charDelayMs")->setValue(5);
        child<QSpinBox>(&d, "lineDelayMs")->setValue(100);
        child<QSpinBox>(&d, "breakMs")->setValue(500);
        QVERIFY2(d.saveCurrent(), qPrintable(d.errorText()));
        const auto s = store.load(QStringLiteral("sw"));
        QVERIFY(s);
        QCOMPARE(s->type, SessionConfig::Type::Serial);
        QCOMPARE(s->serialDevice, QStringLiteral("/dev/ttyACM0"));
        QCOMPARE(s->baudRate, 115200);
        QCOMPARE(s->dataBits, 7);
        QCOMPARE(s->parity, QStringLiteral("even"));
        QCOMPARE(s->stopBits, 2);
        QCOMPARE(s->flowControl, QStringLiteral("xonxoff"));
        QCOMPARE(s->enterSends, QStringLiteral("crlf"));
        QVERIFY(s->localEcho);
        QCOMPARE(s->charDelayMs, 5);
        QCOMPARE(s->lineDelayMs, 100);
        QCOMPARE(s->breakMs, 500);
        // Load gives the same back.
        SessionDialog d2(store, *s);
        QVERIFY(d2.config() == *s);
    }

    void savedSerialSessionEndToEnd()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        QVERIFY(store.save(console(cable.a)));
        const QString name = QStringLiteral("SG250 console");
        MainWindow w(parseCommandLine({name}), {name});
        w.show();
        QCOMPARE(w.windowTitle(), QStringLiteral("zterminal " ZTERMINAL_EXPECTED_VERSION " \u2014 SG250 console"));
        QVERIFY(w.isSerialSession());
        QVERIFY(w.action(QStringLiteral("sendBreak"))->isEnabled());
        w.startSession();
        QVERIFY(w.serial()->isOpen());
        QTRY_VERIFY_WITH_TIMEOUT(screen(w).contains(QStringLiteral("connected to "))
                                         && screen(w).contains(QStringLiteral("at 9600 8N1")),
                                     5000);
        QVERIFY(w.serialBanner()->isHidden());

        Router router(cable.b);
        QVERIFY(router.port.open(QIODevice::ReadWrite));
        // Keys typed in the terminal reach the device; Enter is CR.
        QTest::keyClicks(w.view(), QStringLiteral("show ver"));
        QTest::keyClick(w.view(), Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(router.got, QByteArray("show ver\r"), 5000);
        // Device output appears in the terminal.
        router.port.write("\r\nCisco IOS Software, C2960\r\nswitch#");
        QTRY_VERIFY_WITH_TIMEOUT(screen(w).contains(QStringLiteral("switch#")), 5000);

        // Send Break holds the line, then releases it.
        w.action(QStringLiteral("sendBreak"))->trigger();
        QVERIFY(w.serial()->isBreakActive());
        QVERIFY(w.serial()->port()->isBreakEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!w.serial()->isBreakActive(), 3000);

        // Restart reconnects.
        w.action(QStringLiteral("restartSession"))->trigger();
        QVERIFY(w.serial()->isOpen());
    }

    void pacedPasteShowsBarAndCancels()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        SessionConfig s = console(cable.a);
        s.charDelayMs = 25;
        s.lineDelayMs = 100;
        QVERIFY(store.save(s));
        MainWindow w(parseCommandLine({s.name}), {s.name});
        w.show();
        w.startSession();
        Router router(cable.b);
        QVERIFY(router.port.open(QIODevice::ReadWrite));
        QString config;
        for (int i = 0; i < 40; ++i) {
            config += QStringLiteral("interface Gi0/%1\n description port %1\n").arg(i);
        }
        QApplication::clipboard()->setText(config);
        // The multi-line paste confirmation comes first and shows the pacing.
        QString notes, summary;
        whenModal([&](QWidget *m) {
            if (m->objectName() != QLatin1String("pasteConfirmDialog")) {
                return false;
            }
            summary = child<QLabel>(m, "pasteSummary")->text();
            notes = child<QLabel>(m, "pasteNotes")->text();
            child<QPushButton>(m, "pasteButton")->click();
            return true;
        });
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY2(summary.contains(QStringLiteral("80 lines")), qPrintable(summary));
        QVERIFY2(notes.contains(QStringLiteral("serial pacing (25 ms/char, 100 ms/line)")), qPrintable(notes));
        QVERIFY2(notes.contains(QStringLiteral("about ")), qPrintable(notes));
        QVERIFY(w.serial()->pending() > 1000);
        QVERIFY(!w.pasteBar()->isHidden());
        QVERIFY(w.action(QStringLiteral("cancelPaste"))->isEnabled());
        QTest::qWait(200);
        QVERIFY(router.got.size() > 0);
        QVERIFY(router.got.size() < 20);
        child<QPushButton>(w.pasteBar(), "cancelPaste")->click();
        QCOMPARE(w.serial()->pending(), 0);
        QVERIFY(w.pasteBar()->isHidden());
        QVERIFY(!w.action(QStringLiteral("cancelPaste"))->isEnabled());
        const int sent = router.got.size();
        QTest::qWait(200);
        QCOMPARE(router.got.size(), sent);
        // Pasted line ends were sent as CR (the Enter setting).
        QVERIFY(!router.got.contains('\n'));
    }

    void unplugShowsBannerAndReconnects()
    {
        auto cable = std::make_unique<SocatPair>();
        QVERIFY(cable->start());
        const QString dev = cable->a;
        const QString peer = cable->b;
        QVERIFY(store.save(console(dev)));
        MainWindow w(parseCommandLine({QStringLiteral("SG250 console")}), {});
        w.show();
        w.startSession();
        QVERIFY(w.serial()->isOpen());
        cable->stop(); // "unplug"
        QTRY_VERIFY_WITH_TIMEOUT(!w.serialBanner()->isHidden(), 5000);
        QVERIFY(w.serialBannerText().contains(QStringLiteral("Disconnected")));
        QVERIFY(screen(w).contains(QStringLiteral("disconnected")));
        QVERIFY(!w.serial()->isOpen());

        // "Plug it back in" on the same path, then Reconnect.
        SocatPair again;
        again.proc.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        again.proc.start(QStringLiteral("socat"), {QStringLiteral("pty,raw,echo=0,link=") + dev,
                                                    QStringLiteral("pty,raw,echo=0,link=") + peer});
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dev) && QFileInfo::exists(peer), 5000);
        child<QPushButton>(w.serialBanner(), "reconnect")->click();
        QVERIFY(w.serial()->isOpen());
        QVERIFY(w.serialBanner()->isHidden());
        Router router(peer);
        QVERIFY(router.port.open(QIODevice::ReadWrite));
        QTest::keyClicks(w.view(), QStringLiteral("x"));
        QTRY_COMPARE_WITH_TIMEOUT(router.got, QByteArray("x"), 5000);
    }

    void openErrorsShowHelpfulBanner()
    {
        // Missing device
        QVERIFY(store.save(console(QStringLiteral("/dev/ttyUSB-zterminal-missing"))));
        {
            MainWindow w(parseCommandLine({QStringLiteral("SG250 console")}), {});
            w.show();
            w.startSession();
            QVERIFY(!w.serialBanner()->isHidden());
            QVERIFY(w.serialBannerText().contains(QStringLiteral("was not found")));
        }
        if (::geteuid() == 0) {
            QSKIP("running as root: permissions are not enforced");
        }
        // Permission denied -> dialout explanation, in the banner and the terminal.
        SocatPair cable;
        QVERIFY(cable.start());
        QVERIFY(QFile::setPermissions(QFileInfo(cable.a).symLinkTarget(), QFileDevice::Permissions{}));
        QVERIFY(store.save(console(cable.a)));
        MainWindow w(parseCommandLine({QStringLiteral("SG250 console")}), {});
        w.show();
        w.startSession();
        QVERIFY(!w.serial()->isOpen());
#if defined(Q_OS_MACOS)
        QVERIFY(w.serialBannerText().contains(QStringLiteral("Permission denied")));
        QVERIFY(screen(w).contains(QStringLiteral("Permission denied")));
#else
        QVERIFY(w.serialBannerText().contains(QStringLiteral("dialout")));
        QVERIFY(w.serialBannerText().contains(QStringLiteral("sudo usermod -aG dialout $USER")));
        QVERIFY(screen(w).contains(QStringLiteral("sudo usermod -aG dialout $USER")));
#endif
    }

    void adHocSerialAndSave()
    {
        SocatPair cable;
        QVERIFY(cable.start());
        const LaunchRequest req = parseCommandLine({QStringLiteral("serial"), cable.a, QStringLiteral("115200")});
        QCOMPARE(req.kind, LaunchRequest::Kind::Serial);
        MainWindow w(req, {});
        w.show();
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("serial ") + cable.a));
        w.startSession();
        QVERIFY(w.serial()->isOpen());
        QCOMPARE(w.serial()->port()->baudRate(), 115200);
        QString err;
        QVERIFY2(w.saveCurrentSessionAs(QStringLiteral("bench"), &err), qPrintable(err));
        const auto s = store.load(QStringLiteral("bench"));
        QVERIFY(s && s->type == SessionConfig::Type::Serial && s->baudRate == 115200 && s->serialDevice == cable.a);
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("bench")));

        // Local windows have no Send Break.
        MainWindow local(LaunchRequest{}, {});
        QVERIFY(!local.action(QStringLiteral("sendBreak"))->isEnabled());
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    TstSerialUi t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_serialui.moc"
