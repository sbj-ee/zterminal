// Session dialog and saved-session windows (offscreen): Load/Save/Delete,
// Serial greyed out, Open, File menu actions, title, Duplicate/Restart, and a
// saved SSH session reaching (a fake) ssh with exactly the built argv.
#include "MainWindow.hpp"
#include "Pty.hpp"
#include "SessionDialog.hpp"
#include "SessionStore.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "WindowTitle.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

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

SessionConfig ssh(const QString &name, const QString &host)
{
    SessionConfig s;
    s.name = name;
    s.type = SessionConfig::Type::Ssh;
    s.host = host;
    s.user = QStringLiteral("admin");
    s.port = 2222;
    return s;
}

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

} // namespace

class TstSessionUi : public QObject
{
    Q_OBJECT

    SessionStore store; // $XDG_CONFIG_HOME/zterminal/sessions (per-test dir from CMake)

private slots:
    void init()
    {
        QDir(store.directory()).removeRecursively();
    }

    void dialogLoadSaveDelete()
    {
        QVERIFY(store.save(ssh(QStringLiteral("core-sw1"), QStringLiteral("10.0.0.1"))));
        SessionDialog d(store, SessionConfig{});
        auto *list = child<QListWidget>(&d, "sessionList");
        QCOMPARE(list->count(), 1);

        // Serial is listed but greyed out.
        auto *type = child<QComboBox>(&d, "type");
        auto *model = qobject_cast<QStandardItemModel *>(type->model());
        QVERIFY(model);
        QCOMPARE(type->count(), 3);
        QVERIFY(!model->item(2)->isEnabled());
        QVERIFY(model->item(0)->isEnabled() && model->item(1)->isEnabled());

        // SSH fields only for SSH.
        QVERIFY(!child<QWidget>(&d, "sshGroup")->isEnabled());

        // Load
        list->setCurrentRow(0);
        QCOMPARE(child<QLineEdit>(&d, "name")->text(), QStringLiteral("core-sw1"));
        child<QPushButton>(&d, "load")->click();
        QCOMPARE(child<QLineEdit>(&d, "host")->text(), QStringLiteral("10.0.0.1"));
        QCOMPARE(child<QSpinBox>(&d, "port")->value(), 2222);
        QVERIFY(child<QWidget>(&d, "sshGroup")->isEnabled());
        QVERIFY(d.config() == *store.load(QStringLiteral("core-sw1")));
        QCOMPARE(child<QLineEdit>(&d, "commandPreview")->text(), QStringLiteral("ssh -p 2222 -l admin -- 10.0.0.1"));

        // Save under a new name with changes.
        child<QLineEdit>(&d, "name")->setText(QStringLiteral("edge-rtr"));
        child<QLineEdit>(&d, "host")->setText(QStringLiteral("edge.example.net"));
        child<QLineEdit>(&d, "jumpHost")->setText(QStringLiteral("sbj@vertex"));
        child<QPushButton>(&d, "save")->click();
        QCOMPARE(store.names(), (QStringList{QStringLiteral("core-sw1"), QStringLiteral("edge-rtr")}));
        QCOMPARE(store.load(QStringLiteral("edge-rtr"))->jumpHost, QStringLiteral("sbj@vertex"));
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->currentItem()->text(), QStringLiteral("edge-rtr"));

        // Invalid input is refused with a reason, nothing written.
        child<QLineEdit>(&d, "name")->setText(QStringLiteral("evil"));
        child<QLineEdit>(&d, "host")->setText(QStringLiteral("-oProxyCommand=touch /tmp/x"));
        QVERIFY(!d.saveCurrent());
        QVERIFY(d.errorText().contains(QStringLiteral("Host")));
        QVERIFY(!store.contains(QStringLiteral("evil")));
        QVERIFY(!d.openSession());
        QCOMPARE(d.result(), int(QDialog::Rejected));
        child<QLineEdit>(&d, "name")->setText(QStringLiteral("-bad"));
        QVERIFY(!d.saveCurrent());

        // Delete
        list->setCurrentRow(0);
        child<QPushButton>(&d, "delete")->click();
        QCOMPARE(store.names(), QStringList{QStringLiteral("edge-rtr")});
        QCOMPARE(list->count(), 1);
    }

    void overridesRoundTripThroughDialog()
    {
        SessionConfig s = ssh(QStringLiteral("look"), QStringLiteral("h"));
        s.fontSize = 15;
        s.colorScheme = QStringLiteral("solarized-dark");
        SessionDialog d(store, s);
        QVERIFY(d.config() == s);
        QVERIFY(d.saveCurrent());
        QVERIFY(*store.load(QStringLiteral("look")) == s);
    }

    void savedSessionWindowTitleAndArgv()
    {
        QVERIFY(store.save(ssh(QStringLiteral("core-sw1"), QStringLiteral("10.0.0.1"))));
        const LaunchRequest req = parseCommandLine({QStringLiteral("core-sw1")});
        MainWindow w(req, {QStringLiteral("core-sw1")});
        QVERIFY(w.savedSession());
        QCOMPARE(w.windowTitle(), QStringLiteral("zterminal " ZTERMINAL_EXPECTED_VERSION " \u2014 core-sw1"));
        const MainWindow::Launch l = w.launchCommand();
        QVERIFY(l.error.isEmpty());
        QCOMPARE(l.program, QStringLiteral("ssh"));
        QCOMPARE(l.args, (QStringList{QStringLiteral("-p"), QStringLiteral("2222"), QStringLiteral("-l"),
                                      QStringLiteral("admin"), QStringLiteral("--"), QStringLiteral("10.0.0.1")}));

        // Duplicate re-opens the saved session by name.
        QStringList launched;
        w.setLauncher([&launched](const QString &, const QStringList &args) {
            launched = args;
            return true;
        });
        w.action(QStringLiteral("duplicateSession"))->trigger();
        QCOMPARE(launched, QStringList{QStringLiteral("core-sw1")});
    }

    void tamperedSessionFileIsRefusedAtRunTime()
    {
        SessionConfig s = ssh(QStringLiteral("tampered"), QStringLiteral("ok"));
        QVERIFY(store.save(s));
        {
            QSettings f(store.filePathFor(s.name), QSettings::IniFormat);
            f.setValue(QStringLiteral("ssh/host"), QStringLiteral("-oProxyCommand=touch /tmp/zt-pwned"));
        }
        MainWindow w(parseCommandLine({s.name}), {s.name});
        const MainWindow::Launch l = w.launchCommand();
        QVERIFY(!l.error.isEmpty());
        QVERIFY(l.program.isEmpty());
    }

    void savedSshSessionRunsSystemSshWithBuiltArgv()
    {
        // A fake `ssh` first on PATH records its argv; nothing goes through a shell.
        QTemporaryDir bin;
        const QString fake = bin.filePath(QStringLiteral("ssh"));
        {
            QFile f(fake);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\nprintf 'FAKESSH'; for a in \"$@\"; do printf '[%s]' \"$a\"; done; printf '\\n'\n");
        }
        QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", bin.path().toLocal8Bit() + ':' + oldPath);

        SessionConfig s = ssh(QStringLiteral("lab"), QStringLiteral("lab.example.net"));
        s.extraArgs = QStringLiteral("-o \"SetEnv A=$(id)\"");
        QVERIFY(store.save(s));
        MainWindow w(parseCommandLine({s.name}), {s.name});
        w.show();
        w.startSession();
        const QString expected = QStringLiteral("FAKESSH[-o][SetEnv A=$(id)][-p][2222][-l][admin][--][lab.example.net]");
        auto screen = [&w]() {
            QString all;
            for (int r = 0; r < w.terminal()->totalLines(); ++r) {
                all += w.terminal()->lineText(r).trimmed();
            }
            return all;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(screen().contains(expected), qPrintable(screen()), 10000);

        // Restart runs it again.
        QTRY_VERIFY_WITH_TIMEOUT(!w.pty()->isRunning(), 10000);
        w.action(QStringLiteral("restartSession"))->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(screen().count(expected), 2, 10000);
        qputenv("PATH", oldPath);
    }

    void fileMenuActionsWork()
    {
        LaunchRequest req; // local shell
        MainWindow w(req, {});
        for (const char *n : {"newSession", "openSavedSession", "saveSession", "duplicateSession", "restartSession"}) {
            QVERIFY2(w.action(QString::fromLatin1(n)) && w.action(QString::fromLatin1(n))->isEnabled(), n);
        }
        QStringList launched;
        w.setLauncher([&launched](const QString &, const QStringList &args) {
            launched = args;
            return true;
        });

        // New Session -> dialog -> SSH -> Open: a new window with the built argv.
        whenModal([](QWidget *m) {
            auto *d = qobject_cast<SessionDialog *>(m);
            if (!d) {
                return false;
            }
            auto *type = child<QComboBox>(d, "type");
            type->setCurrentIndex(type->findData(QStringLiteral("ssh")));
            child<QLineEdit>(d, "host")->setText(QStringLiteral("sw9"));
            child<QPushButton>(d, "open")->click();
            return true;
        });
        w.action(QStringLiteral("newSession"))->trigger();
        QCOMPARE(launched, (QStringList{QStringLiteral("ssh"), QStringLiteral("--"), QStringLiteral("sw9")}));

        // Open Saved Session -> pick one -> opens by name.
        QVERIFY(store.save(ssh(QStringLiteral("core-sw1"), QStringLiteral("10.0.0.1"))));
        launched.clear();
        whenModal([](QWidget *m) {
            auto *d = qobject_cast<SessionDialog *>(m);
            if (!d) {
                return false;
            }
            auto *list = child<QListWidget>(d, "sessionList");
            list->setCurrentRow(0);
            d->loadSelected();
            d->openSession();
            return true;
        });
        w.action(QStringLiteral("openSavedSession"))->trigger();
        QCOMPARE(launched, QStringList{QStringLiteral("core-sw1")});

        // Save Session: this local window becomes saved session "my shell".
        QString err;
        QVERIFY2(w.saveCurrentSessionAs(QStringLiteral("my shell"), &err), qPrintable(err));
        QCOMPARE(store.load(QStringLiteral("my shell"))->type, SessionConfig::Type::LocalShell);
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("my shell")));
        w.action(QStringLiteral("duplicateSession"))->trigger();
        QCOMPARE(launched, QStringList{QStringLiteral("my shell")});
    }

    void adHocSshWindowCanBeSaved()
    {
        const LaunchRequest req = parseCommandLine({QStringLiteral("ssh"), QStringLiteral("-p"), QStringLiteral("2200"),
                                                    QStringLiteral("sbj@dragon")});
        MainWindow w(req, {});
        QString err;
        QVERIFY2(w.saveCurrentSessionAs(QStringLiteral("dragon"), &err), qPrintable(err));
        const auto s = store.load(QStringLiteral("dragon"));
        QVERIFY(s);
        QCOMPARE(s->host, QStringLiteral("dragon"));
        QCOMPARE(s->user, QStringLiteral("sbj"));
        QCOMPARE(s->port, 2200);
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("dragon")));

        MainWindow cmd(parseCommandLine({QStringLiteral("-e"), QStringLiteral("htop")}), {});
        QVERIFY(!cmd.saveCurrentSessionAs(QStringLiteral("htop"), &err));
    }

    void perSessionOverridesApply()
    {
        SessionConfig s;
        s.name = QStringLiteral("big");
        s.fontSize = 19;
        s.colorScheme = QStringLiteral("solarized-dark");
        QVERIFY(store.save(s));
        MainWindow w(parseCommandLine({s.name}), {s.name});
        QCOMPARE(w.view()->terminalFont().pointSize(), 19);
        QCOMPARE(w.terminal()->colorScheme().id, QStringLiteral("solarized-dark"));
        // Zoom changes the override, not the global default.
        const int globalSize = AppSettings::load().fontSize;
        w.action(QStringLiteral("fontLarger"))->trigger();
        QCOMPARE(w.view()->terminalFont().pointSize(), 20);
        QCOMPARE(store.load(s.name)->fontSize, 20);
        QCOMPARE(AppSettings::load().fontSize, globalSize);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    TstSessionUi t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_sessionui.moc"
