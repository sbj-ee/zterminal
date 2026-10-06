// Session dialog and saved-session windows (offscreen): Load/Save/Delete,
// Serial greyed out, Open, File menu actions, title, Duplicate/Restart, and a
// saved SSH session reaching (a fake) ssh with exactly the built argv.
#include "MainWindow.hpp"
#include "Pty.hpp"
#include "SessionDialog.hpp"
#include "ImportReviewDialog.hpp"
#include "SessionExport.hpp"
#include "SessionStore.hpp"
#include "VaultBindings.hpp"
#include "VaultManager.hpp"
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
#include <QMessageBox>
#include <QTabWidget>
#include <QTreeWidget>
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

// A fake `ssh` first on PATH for the lifetime of the object (tabs started
// by Duplicate / Open must not reach the network).
struct FakeSsh {
    QTemporaryDir bin;
    QByteArray oldPath = qgetenv("PATH");
    FakeSsh()
    {
        QFile f(bin.filePath(QStringLiteral("ssh")));
        if (f.open(QIODevice::WriteOnly)) {
            f.write("#!/bin/sh\nprintf 'FAKESSH'; for a in \"$@\"; do printf '[%s]' \"$a\"; done; printf '\\n'\n");
        }
        f.close();
        QFile::setPermissions(f.fileName(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", bin.path().toLocal8Bit() + ':' + oldPath);
    }
    ~FakeSsh() { qputenv("PATH", oldPath); }
};

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

        // Local shell, SSH and (since 0.3.0) Serial are all selectable.
        auto *type = child<QComboBox>(&d, "type");
        auto *model = qobject_cast<QStandardItemModel *>(type->model());
        QVERIFY(model);
        QCOMPARE(type->count(), 3);
        QVERIFY(model->item(0)->isEnabled() && model->item(1)->isEnabled() && model->item(2)->isEnabled());

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
        QCOMPARE(child<QLineEdit>(&d, "commandPreview")->text(), QStringLiteral("ssh -o ServerAliveInterval=30 -o ServerAliveCountMax=3 -p 2222 -l admin -- 10.0.0.1"));

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
        QCOMPARE(l.args, (QStringList{QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"), QStringLiteral("-o"),
                                      QStringLiteral("ServerAliveCountMax=3"), QStringLiteral("-p"), QStringLiteral("2222"), QStringLiteral("-l"),
                                      QStringLiteral("admin"), QStringLiteral("--"), QStringLiteral("10.0.0.1")}));

        // Duplicate re-opens the saved session by name, in a new tab.
        FakeSsh fake;
        w.action(QStringLiteral("duplicateSession"))->trigger();
        QCOMPARE(w.tabCount(), 2);
        QCOMPARE(w.tabs()->currentIndex(), 1);
        QCOMPARE(w.currentSession()->originalArgs(), QStringList{QStringLiteral("core-sw1")});
        QCOMPARE(w.currentSession()->request().kind, LaunchRequest::Kind::SavedSession);
        QCOMPARE(w.windowTitle(), QStringLiteral("zterminal " ZTERMINAL_EXPECTED_VERSION " \u2014 core-sw1"));
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
        const QString expected = QStringLiteral("FAKESSH[-o][SetEnv A=$(id)][-o][ServerAliveInterval=30][-o][ServerAliveCountMax=3][-p][2222][-l][admin][--][lab.example.net]");
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
        for (const char *n : {"newSession", "openSavedSession", "saveSession", "exportSessions", "importSessions", "duplicateSession", "restartSession"}) {
            QVERIFY2(w.action(QString::fromLatin1(n)) && w.action(QString::fromLatin1(n))->isEnabled(), n);
        }
        FakeSsh fake;

        // New Session -> dialog -> SSH -> Open: a new tab running the built argv.
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
        QCOMPARE(w.tabCount(), 2);
        QCOMPARE(w.currentSession()->originalArgs(),
                 (QStringList{QStringLiteral("ssh"), QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"),
                              QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"), QStringLiteral("--"),
                              QStringLiteral("sw9")}));
        QCOMPARE(w.launchCommand().args, (QStringList{QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"),
                                                      QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"),
                                                      QStringLiteral("--"), QStringLiteral("sw9")}));
        QCOMPARE(w.tabs()->tabText(1), QStringLiteral("ssh sw9"));

        // Open Saved Session -> pick one -> opens by name in a third tab.
        QVERIFY(store.save(ssh(QStringLiteral("core-sw1"), QStringLiteral("10.0.0.1"))));
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
        QCOMPARE(w.tabCount(), 3);
        QCOMPARE(w.currentSession()->originalArgs(), QStringList{QStringLiteral("core-sw1")});
        QCOMPARE(w.tabs()->tabText(2), QStringLiteral("core-sw1"));

        // Save Session: the first (local) tab becomes saved session "my shell".
        w.tabs()->setCurrentIndex(0);
        QString err;
        QVERIFY2(w.saveCurrentSessionAs(QStringLiteral("my shell"), &err), qPrintable(err));
        QCOMPARE(store.load(QStringLiteral("my shell"))->type, SessionConfig::Type::LocalShell);
        QCOMPARE(w.windowTitle(), makeWindowTitle(QStringLiteral("my shell")));
        QCOMPARE(w.tabs()->tabText(0), QStringLiteral("my shell"));
        w.action(QStringLiteral("duplicateSession"))->trigger();
        QCOMPARE(w.tabCount(), 4);
        QCOMPARE(w.currentSession()->originalArgs(), QStringList{QStringLiteral("my shell")});
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
    // Review items 1 + 2: import shows everything for approval first, refuses
    // dangerous options, and never brings stored-password use along.
    void importNeedsReviewAndApproval()
    {
        SessionConfig mine = ssh(QStringLiteral("core-sw1"), QStringLiteral("10.0.0.1"));
        mine.useStoredPassword = true;
        QVERIFY(store.save(mine));
        SessionConfig replaced = ssh(QStringLiteral("core-sw1"), QStringLiteral("evil.example"));
        replaced.useStoredPassword = true;
        replaced.jumpHost = QStringLiteral("sbj@vertex");
        replaced.extraArgs = QStringLiteral("-C -o ServerAliveInterval=5");
        SessionConfig bad = ssh(QStringLiteral("bad"), QStringLiteral("bad.example"));
        bad.extraArgs = QStringLiteral("-oProxyCommand=\"sh -c 'touch /tmp/pwned'\"");
        SessionConfig fresh = ssh(QStringLiteral("fresh"), QStringLiteral("fresh.example"));
        const QByteArray json = sessionsToExportJson({replaced, bad, fresh});

        for (const bool approve : {false, true}) {
            QString treeText;
            int step = 0;
            whenModal([&](QWidget *w) {
                if (auto *box = qobject_cast<QMessageBox *>(w); box && step == 0) {
                    box->button(QMessageBox::Yes)->click(); // overwrite conflicts
                    step = 1;
                    return false;
                }
                if (w->objectName() == QLatin1String("importReview") && step == 1) {
                    auto *tree = child<QTreeWidget>(w, "importReviewList");
                    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                        QTreeWidgetItem *it = tree->topLevelItem(i);
                        treeText += it->text(0) + QLatin1Char('|') + it->text(1) + QLatin1Char('\n');
                        for (int c = 0; c < it->childCount(); ++c) {
                            treeText += QStringLiteral("  ") + it->child(c)->text(1) + QLatin1Char('\n');
                        }
                    }
                    child<QPushButton>(w, approve ? "approveImport" : "cancelImport")->click();
                    step = 2;
                    return !approve;
                }
                if (auto *box = qobject_cast<QMessageBox *>(w); box && step == 2) {
                    treeText += QStringLiteral("RESULT ") + box->text();
                    box->accept();
                    return true;
                }
                return false;
            });
            SessionDialog d(store, SessionConfig{});
            QCOMPARE(d.importSessionsFromBytes(json), approve);
            QVERIFY2(treeText.contains(QStringLiteral("core-sw1|REPLACES")), qPrintable(treeText));
            QVERIFY(treeText.contains(QStringLiteral("connects to admin@evil.example:2222")));
            QVERIFY(treeText.contains(QStringLiteral("jump host: sbj@vertex")));
            QVERIFY(treeText.contains(QStringLiteral("extra ssh options: -C -o ServerAliveInterval=5")));
            QVERIFY(treeText.contains(QStringLiteral("bad|REFUSED")));
            QVERIFY(treeText.contains(QStringLiteral("fresh|new")));
            if (!approve) {
                QCOMPARE(store.load(mine.name)->host, QStringLiteral("10.0.0.1")); // nothing written
                QVERIFY(!store.contains(QStringLiteral("fresh")));
                continue;
            }
            QVERIFY(treeText.contains(QStringLiteral("refused 1")));
            QVERIFY(!treeText.contains(QStringLiteral("Passwords are never imported; re-enter")));
            const SessionConfig now = *store.load(mine.name);
            QCOMPARE(now.host, QStringLiteral("evil.example"));
            QVERIFY(!now.useStoredPassword);
            QVERIFY(now.approved);
            QVERIFY(!store.contains(QStringLiteral("bad")));
            QVERIFY(store.contains(QStringLiteral("fresh")));
        }
    }

    void unapprovedSessionMustBeSavedBeforeOpen()
    {
        SessionConfig s = ssh(QStringLiteral("pending"), QStringLiteral("p.example"));
        s.approved = false;
        QVERIFY(store.save(s));
        SessionDialog d(store, SessionConfig{});
        auto *list = child<QListWidget>(&d, "sessionList");
        list->setCurrentRow(0);
        QVERIFY(d.loadSelected());
        QVERIFY(d.errorText().contains(QStringLiteral("not approved")));
        QVERIFY(!d.openSession());
        QVERIFY(d.errorText().contains(QStringLiteral("hasn't been approved")));
        QVERIFY(d.saveCurrent()); // the user reviewed it in the dialog
        QVERIFY(store.load(s.name)->approved);
        QVERIFY(d.openSession());
    }

    void deleteWhileLockedForgetsSecretAtNextUnlock()
    {
        Vault::setKdfOverrideForTests(1, 8192);
        QTemporaryDir vdir;
        VaultManager &vm = VaultManager::instance();
        vm.setPath(vdir.filePath(QStringLiteral("vault.bin")));
        vm.setAutoLockIntervalMsForTests(0);
        QVERIFY(vm.vault().create(SecureBuffer::fromQString(QStringLiteral("m"))));
        vm.noteUnlocked();
        SessionConfig s = ssh(QStringLiteral("doomed"), QStringLiteral("d.example"));
        s.useStoredPassword = true;
        QVERIFY(store.save(s));
        QVERIFY(vaultbind::storeSecret(vm.vault(), s, SecureBuffer::fromQString(QStringLiteral("pw"))));
        vm.lock();

        SessionDialog d(store, SessionConfig{});
        auto *list = child<QListWidget>(&d, "sessionList");
        list->setCurrentRow(0);
        QVERIFY(d.deleteSelected());
        QVERIFY(!store.contains(s.name));
        QVERIFY(!vaultbind::pendingDeletions(vm.vault()).isEmpty());

        QVERIFY(vm.vault().unlock(SecureBuffer::fromQString(QStringLiteral("m"))));
        vm.noteUnlocked();
        QVERIFY(vm.lastReconcile().deleted.contains(QStringLiteral("ssh-password/doomed")));
        QVERIFY(!vm.vault().secret(QStringLiteral("ssh-password/doomed")));
        QVERIFY(!vm.vault().secret(QStringLiteral("binding/ssh-password/doomed")));
        QVERIFY(vaultbind::pendingDeletions(vm.vault()).isEmpty());
        vm.lock();
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
