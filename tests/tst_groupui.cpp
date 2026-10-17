// Session groups in the UI (1.6.0, offscreen): the Manage Groups dialog (add,
// remove, reorder, save), File > Open Group, opening a group as tabs in order
// with the right focus (the stagger timer is replaced by manual stepping),
// skipped members and their summary, ONE vault unlock for N stored-password
// members, the "needs input" tab marker, the Ctrl+Shift+P group picker,
// Reconnect All Disconnected, and the delete-session prompt that prunes groups.
// Fake `ssh` scripts on PATH stand in for real connections.
#include "AppSettings.hpp"
#include "AskpassServer.hpp"
#include "GroupDialog.hpp"
#include "GroupOpener.hpp"
#include "GroupPicker.hpp"
#include "GroupStore.hpp"
#include "MainWindow.hpp"
#include "Pty.hpp"
#include "SecureBuffer.hpp"
#include "SessionDialog.hpp"
#include "SessionStore.hpp"
#include "SessionWidget.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "Vault.hpp"
#include "VaultBindings.hpp"
#include "VaultDialogs.hpp"
#include "VaultManager.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

using namespace zterminal;

namespace {

const char *kMaster = "correct horse battery";
const char *kSecret = "Sw0rdfish-grp";

template <typename T>
T *child(QObject *o, const char *name)
{
    T *w = o->findChild<T *>(QString::fromLatin1(name));
    if (!w) {
        qFatal("missing child %s", name);
    }
    return w;
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

// Answer the next QMessageBox with `button`; records its text.
void answerBox(QMessageBox::StandardButton button, QString *text = nullptr)
{
    whenModal([button, text](QWidget *m) {
        auto *box = qobject_cast<QMessageBox *>(m);
        if (!box) {
            return false;
        }
        if (text) {
            *text = box->text();
        }
        box->button(button)->click();
        return true;
    });
}

// Counts the unlock dialogs that show up (answering each with `password`, or
// cancelling when empty) and records any other modal dialog.
struct DialogWatch {
    QTimer timer;
    int unlocks = 0;
    QStringList others;
    explicit DialogWatch(const QString &password)
    {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [this, password]() {
            QWidget *m = QApplication::activeModalWidget();
            if (!m) {
                return;
            }
            if (auto *d = qobject_cast<UnlockVaultDialog *>(m)) {
                ++unlocks;
                if (password.isEmpty()) {
                    d->reject();
                } else {
                    child<QLineEdit>(d, "masterPassword")->setText(password);
                    child<QPushButton>(d, "unlock")->click();
                }
                return;
            }
            others << m->windowTitle() + QLatin1Char('/') + QString::fromLatin1(m->metaObject()->className());
            m->close();
        });
        timer.start();
    }
};

QString screen(Terminal *t)
{
    QString all;
    for (int r = 0; r < t->totalLines(); ++r) {
        all += t->lineText(r).trimmed() + QLatin1Char('\n');
    }
    return all;
}

// A fake `ssh` on PATH running `body` (a /bin/sh script). $1 = -G (resolveTarget's
// `ssh -G`) exits 1 so the session fields are used. Every run drops a file
// "run.<pid>" in $S, the script's own directory, so runs can be counted.
struct FakeSsh {
    QTemporaryDir dir;
    QByteArray oldPath = qgetenv("PATH");
    explicit FakeSsh(const QByteArray &body)
    {
        QFile f(dir.filePath(QStringLiteral("ssh")));
        if (f.open(QIODevice::WriteOnly)) {
            f.write("#!/bin/sh\n[ \"$1\" = -G ] && exit 1\nS='" + QFile::encodeName(dir.path()) + "'\n: > \"$S/run.$$\"\n"
                    "for a; do h=$a; done\n" + body);
        }
        f.close();
        QFile::setPermissions(f.fileName(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv("PATH", QFile::encodeName(dir.path()) + ':' + oldPath);
    }
    ~FakeSsh() { qputenv("PATH", oldPath); }
    int runs() const { return int(QDir(dir.path()).entryList({QStringLiteral("run.*")}, QDir::Files).size()); }
};

} // namespace

class TstGroupUi : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    SessionStore store;
    GroupStore groups;
    int vaultN = 0;

    VaultManager &vm() { return VaultManager::instance(); }

    void local(const QStringList &names)
    {
        for (const QString &n : names) {
            SessionConfig s;
            s.name = n;
            QVERIFY(store.save(s));
        }
    }
    SessionConfig ssh(const QString &name, const QString &host, bool stored = false, bool autoReconnect = false)
    {
        SessionConfig s;
        s.name = name;
        s.type = SessionConfig::Type::Ssh;
        s.host = host;
        s.user = QStringLiteral("admin");
        s.useStoredPassword = stored;
        s.autoReconnect = autoReconnect;
        return s;
    }
    GroupConfig group(const QString &name, const QStringList &members, int focus = 0, int stagger = 0,
                      GroupConfig::OpenIn in = GroupConfig::OpenIn::NewWindow)
    {
        GroupConfig g;
        g.name = name;
        g.members = members;
        g.focusIndex = focus;
        g.staggerMs = stagger;
        g.openIn = in;
        return g;
    }
    static QStringList tabTexts(MainWindow *w)
    {
        QStringList t;
        for (int i = 0; i < w->tabCount(); ++i) {
            t << w->tabs()->tabText(i);
        }
        return t;
    }
    // A window the test didn't create itself (opened by a group).
    static MainWindow *otherWindow(const MainWindow *mine)
    {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (auto *m = qobject_cast<MainWindow *>(w); m && m != mine) {
                return m;
            }
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        Vault::setKdfOverrideForTests(1, 8192);
        AppSettings{}.save();
        SessionWidget::setReconnectStableMsForTests(400);
        // Release builds ignore $ZTERMINAL_ASKPASS (ZTERMINAL_DEV_OVERRIDES); CMake passes it to tests.
        AskpassServer::setHelperPathForTests(qEnvironmentVariable("ZTERMINAL_ASKPASS"));
    }
    void init()
    {
        QDir(store.directory()).removeRecursively();
        QDir(groups.directory()).removeRecursively();
        GroupOpener::setManualSteppingForTests(false);
        vm().setPath(tmp.filePath(QStringLiteral("vault%1/vault.bin").arg(++vaultN)));
        vm().setAutoLockIntervalMsForTests(0);
    }
    void cleanup()
    {
        // Windows a group opened (WA_DeleteOnClose, live shells): end them.
        QList<QPointer<QWidget>> tops;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            tops << w;
        }
        for (const QPointer<QWidget> &w : tops) {
            if (auto *m = qobject_cast<MainWindow *>(w.data())) {
                delete m;
            }
        }
        GroupOpener::setManualSteppingForTests(false);
    }

    // ---- Manage Groups ----

    void dialogAddRemoveReorderSave()
    {
        local({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        GroupDialog dlg(store, groups);
        QVERIFY(dlg.memberNames().isEmpty());
        QVERIFY(!child<QPushButton>(&dlg, "openGroup")->isEnabled()); // nothing to open yet
        dlg.addSessions({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("ghost")});
        auto *list = child<QListWidget>(&dlg, "memberList");
        QCOMPARE(dlg.memberNames(), (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("ghost")}));
        // A member that isn't a saved session shows as missing (and is still kept by name).
        QCOMPARE(list->item(3)->text(), QStringLiteral("ghost  (missing)"));
        QCOMPARE(list->item(3)->data(Qt::UserRole).toString(), QStringLiteral("ghost"));
        QCOMPARE(list->item(0)->text(), QStringLiteral("a"));

        // Move c up one.
        list->item(2)->setSelected(true);
        dlg.moveSelected(-1);
        QCOMPARE(dlg.memberNames(), (QStringList{QStringLiteral("a"), QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("ghost")}));
        // Already at the top: stays.
        list->clearSelection();
        list->item(0)->setSelected(true);
        dlg.moveSelected(-1);
        QCOMPARE(dlg.memberNames().first(), QStringLiteral("a"));
        // A drag and drop within the list is a model row move: a to the end.
        QVERIFY(list->model()->moveRow(QModelIndex(), 0, QModelIndex(), 4));
        QCOMPARE(dlg.memberNames(), (QStringList{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("ghost"), QStringLiteral("a")}));
        // Down moves a block of two.
        list->clearSelection();
        list->item(0)->setSelected(true);
        list->item(1)->setSelected(true);
        dlg.moveSelected(+1);
        QCOMPARE(dlg.memberNames(), (QStringList{QStringLiteral("ghost"), QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")}));
        // Remove the missing one.
        list->clearSelection();
        list->item(0)->setSelected(true);
        QVERIFY(dlg.removeSelectedMembers());
        QVERIFY(!dlg.removeSelectedMembers()); // nothing selected now
        QCOMPARE(dlg.memberNames(), (QStringList{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")}));

        // Settings, then Save.
        child<QLineEdit>(&dlg, "groupName")->setText(QStringLiteral("lab"));
        child<QComboBox>(&dlg, "openIn")->setCurrentIndex(1);
        child<QSpinBox>(&dlg, "stagger")->setValue(500);
        child<QSpinBox>(&dlg, "focusIndex")->setValue(3);
        QVERIFY(dlg.saveCurrent());
        const auto saved = groups.load(QStringLiteral("lab"));
        QVERIFY(saved.has_value());
        QCOMPARE(saved->members, (QStringList{QStringLiteral("c"), QStringLiteral("b"), QStringLiteral("a")}));
        QCOMPARE(saved->openIn, GroupConfig::OpenIn::ThisWindow);
        QCOMPARE(saved->staggerMs, 500);
        QCOMPARE(saved->focusIndex, 2); // shown 1-based, stored 0-based
        QCOMPARE(child<QListWidget>(&dlg, "groupList")->count(), 1);

        // New clears the editor; picking the group in the list brings it back.
        dlg.newGroup();
        QVERIFY(dlg.config().members.isEmpty());
        QVERIFY(dlg.config().name.isEmpty());
        child<QListWidget>(&dlg, "groupList")->setCurrentRow(0);
        QCOMPARE(dlg.config(), *saved);

        // Validation: a nameless group isn't saved.
        dlg.newGroup();
        dlg.addSessions({QStringLiteral("a")});
        QVERIFY(!dlg.saveCurrent());
        QVERIFY(!dlg.errorText().isEmpty());
        QCOMPARE(groups.names(), QStringList{QStringLiteral("lab")});

        // Delete the saved one.
        child<QListWidget>(&dlg, "groupList")->setCurrentRow(0);
        QVERIFY(dlg.deleteSelected());
        QVERIFY(!groups.contains(QStringLiteral("lab")));
        QCOMPARE(child<QListWidget>(&dlg, "groupList")->count(), 0);
    }

    void dialogOpenGroupAccepts()
    {
        local({QStringLiteral("a"), QStringLiteral("b")});
        GroupDialog dlg(store, groups);
        QVERIFY(!dlg.openGroup()); // empty
        QVERIFY(!dlg.errorText().isEmpty());
        dlg.addSessions({QStringLiteral("b"), QStringLiteral("a")});
        child<QSpinBox>(&dlg, "focusIndex")->setValue(2);
        QVERIFY(dlg.openGroup());
        QCOMPARE(dlg.result(), int(QDialog::Accepted));
        QCOMPARE(dlg.config().members, (QStringList{QStringLiteral("b"), QStringLiteral("a")}));
        QCOMPARE(dlg.config().focusIndex, 1);
    }

    // ---- opening ----

    void opensTabsInOrderWithRightFocus()
    {
        local({QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")});
        GroupOpener::setManualSteppingForTests(true);
        QString err;
        QPointer<MainWindow> w = GroupOpener::open(
            group(QStringLiteral("lab"), {QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")}, /*focus=*/2),
            nullptr, &err);
        QVERIFY2(w, qPrintable(err));
        auto *opener = w->findChild<GroupOpener *>();
        QVERIFY(opener);
        QSignalSpy done(opener, &GroupOpener::finished);
        // The window opens with the first member, started.
        QCOMPARE(w->tabCount(), 1);
        QCOMPARE(w->tabs()->tabText(0), QStringLiteral("s1"));
        QVERIFY(w->sessionAt(0)->pty()->isRunning());
        QCOMPARE(opener->focusPosition(), 2);
        // Background tabs don't take the focus...
        opener->step();
        QCOMPARE(w->tabCount(), 2);
        QCOMPARE(w->tabs()->currentIndex(), 0);
        QVERIFY(w->sessionAt(1)->pty()->isRunning());
        QVERIFY(!opener->isFinished());
        // ...the focus member does (as soon as it exists)...
        opener->step();
        QCOMPARE(w->tabs()->currentIndex(), 2);
        // ...and the ones after it don't take it back.
        opener->step();
        QCOMPARE(w->tabCount(), 4);
        QCOMPARE(w->tabs()->currentIndex(), 2);
        QVERIFY(opener->isFinished());
        QCOMPARE(done.count(), 1);
        QCOMPARE(done.first().first().toString(), QStringLiteral("Opened 4 of 4"));
        QCOMPARE(tabTexts(w), (QStringList{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3"), QStringLiteral("s4")}));
        for (int i = 0; i < 4; ++i) {
            QVERIFY(w->sessionAt(i)->pty()->isRunning());
        }
        opener->step(); // nothing left: harmless
        QCOMPARE(w->tabCount(), 4);
        // Nothing to report: no summary box.
        QVERIFY(!w->findChild<QMessageBox *>(QStringLiteral("groupSummary")));
    }

    void staggerTimerPacesTheTabs()
    {
        local({QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3")});
        QElapsedTimer clock;
        clock.start();
        QPointer<MainWindow> w = GroupOpener::open(
            group(QStringLiteral("slow"), {QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3")}, 0, /*stagger=*/80), nullptr);
        QVERIFY(w);
        QCOMPARE(w->tabCount(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(w->tabCount(), 3, 10000);
        QVERIFY2(clock.elapsed() >= 150, "two 80 ms gaps");
        QCOMPARE(tabTexts(w), (QStringList{QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3")}));
        QCOMPARE(w->tabs()->currentIndex(), 0);
    }

    void skipsMissingUnapprovedAndInvalidWithOneSummary()
    {
        local({QStringLiteral("ok1"), QStringLiteral("ok2")});
        SessionConfig imported;
        imported.name = QStringLiteral("imp");
        imported.approved = false;
        QVERIFY(store.save(imported));
        SessionConfig bad = ssh(QStringLiteral("badssh"), QStringLiteral("-oProxyCommand=evil"));
        QVERIFY(store.save(bad));
        SessionConfig badSerial;
        badSerial.name = QStringLiteral("badserial");
        badSerial.type = SessionConfig::Type::Serial; // no device
        QVERIFY(store.save(badSerial));

        GroupOpener::setManualSteppingForTests(true);
        const GroupConfig g = group(QStringLiteral("mixed"),
                                    {QStringLiteral("ok1"), QStringLiteral("ghost"), QStringLiteral("imp"), QStringLiteral("ok2"),
                                     QStringLiteral("badssh"), QStringLiteral("badserial")});
        const auto plan = GroupOpener::plan(g, store);
        QCOMPARE(plan.total, 6);
        QCOMPARE(plan.openable.size(), 2);
        QCOMPARE(plan.skipped.size(), 4);
        QVERIFY(!plan.needsVault);

        QPointer<MainWindow> w = GroupOpener::open(g, nullptr);
        QVERIFY(w);
        auto *opener = w->findChild<GroupOpener *>();
        QVERIFY(opener);
        QCOMPARE(w->tabs()->tabText(0), QStringLiteral("ok1")); // the first openable one, not "ghost"
        opener->step();
        QVERIFY(opener->isFinished());
        QCOMPARE(tabTexts(w), (QStringList{QStringLiteral("ok1"), QStringLiteral("ok2")}));
        auto *box = w->findChild<QMessageBox *>(QStringLiteral("groupSummary"));
        QVERIFY(box);
        const QString text = box->text();
        QVERIFY2(text.contains(QStringLiteral("Opened 2 of 6; skipped: ghost (missing), imp (imported, not reviewed yet), badssh (invalid:")),
                 qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("badserial (invalid:")));
        QVERIFY(!box->isModal()); // doesn't block the new tabs
    }

    void focusFallsBackWhenTheFocusMemberIsSkipped()
    {
        local({QStringLiteral("a"), QStringLiteral("b")});
        const auto g = group(QStringLiteral("f"), {QStringLiteral("a"), QStringLiteral("ghost"), QStringLiteral("b")}, /*focus=*/1);
        GroupOpener::setManualSteppingForTests(true);
        QPointer<MainWindow> w = GroupOpener::open(g, nullptr);
        QVERIFY(w);
        auto *opener = w->findChild<GroupOpener *>();
        QCOMPARE(opener->focusPosition(), 0); // "ghost" is skipped: the member before it
        opener->step();
        QCOMPARE(w->tabs()->currentIndex(), 0);
        // A focus index past the end means the last one.
        QPointer<MainWindow> w2 = GroupOpener::open(group(QStringLiteral("g"), {QStringLiteral("a"), QStringLiteral("b")}, 9), nullptr);
        QCOMPARE(w2->findChild<GroupOpener *>()->focusPosition(), 1);
    }

    void nothingOpenableOpensNothing()
    {
        local({QStringLiteral("a")});
        const int before = QApplication::topLevelWidgets().size();
        QString err;
        QVERIFY(!GroupOpener::open(group(QStringLiteral("g"), {QStringLiteral("ghost")}), nullptr, &err));
        QVERIFY2(err.contains(QStringLiteral("ghost (missing)")), qPrintable(err));
        QVERIFY(!GroupOpener::open(group(QStringLiteral("empty"), {}), nullptr, &err));
        QVERIFY(err.contains(QStringLiteral("no sessions")));
        QVERIFY(!GroupOpener::openByName(QStringLiteral("nope"), nullptr, &err));
        QVERIFY(err.contains(QStringLiteral("unknown group \"nope\"")));
        QCOMPARE(QApplication::topLevelWidgets().size(), before);
    }

    void openInThisWindowAppendsAndFileMenuListsGroups()
    {
        local({QStringLiteral("s1"), QStringLiteral("s2")});
        MainWindow w(LaunchRequest{}, {});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.startSession();
        QMenu *menu = w.openGroupMenu();
        QVERIFY(menu);
        emit menu->aboutToShow();
        QCOMPARE(menu->actions().size(), 1); // no groups yet
        QVERIFY(!menu->actions().first()->isEnabled());
        QVERIFY(groups.save(group(QStringLiteral("lab"), {QStringLiteral("s1"), QStringLiteral("s2")}, 1, 0, GroupConfig::OpenIn::ThisWindow)));
        QVERIFY(groups.save(group(QStringLiteral("edge"), {QStringLiteral("s2")})));
        emit menu->aboutToShow(); // rebuilt each time it opens
        QStringList names;
        for (QAction *a : menu->actions()) {
            names << a->objectName();
        }
        QCOMPARE(names, (QStringList{QStringLiteral("openGroup:edge"), QStringLiteral("openGroup:lab")}));
        QCOMPARE(menu->actions().at(1)->text(), QStringLiteral("lab  (2)"));
        QVERIFY(w.action(QStringLiteral("manageGroups")));

        GroupOpener::setManualSteppingForTests(true);
        menu->actions().at(1)->trigger(); // lab: This window
        auto *opener = w.findChild<GroupOpener *>();
        QVERIFY(opener);
        QCOMPARE(w.tabCount(), 2); // the first member started at once
        opener->step();
        QCOMPARE(w.tabCount(), 3);
        QCOMPARE(tabTexts(&w), (QStringList{QStringLiteral("local shell"), QStringLiteral("s1"), QStringLiteral("s2")}));
        QCOMPARE(w.tabs()->currentIndex(), 2); // focus member 2 (index 1) -> the second of the group
        QVERIFY(!otherWindow(&w));            // no new window
        // The same group again opens again (no de-duplication).
        w.openGroup(QStringLiteral("lab"));
        QCOMPARE(w.tabCount(), 4);
    }

    // ---- vault ----

    void oneVaultUnlockForManyStoredPasswordMembers()
    {
        FakeSsh fake("if [ -n \"$SSH_ASKPASS\" ]; then got=$(\"$SSH_ASKPASS\" \"admin@$h's password: \"); else got=none; fi\n"
                     "printf 'got [%s] for %s\\n' \"$got\" \"$h\"\nexec cat\n");
        const QStringList names = {QStringLiteral("sw1"), QStringLiteral("sw2"), QStringLiteral("sw3")};
        QVERIFY(vm().vault().create(SecureBuffer::fromQString(QString::fromUtf8(kMaster))));
        vm().noteUnlocked();
        for (const QString &n : names) {
            const SessionConfig s = ssh(n, n + QStringLiteral(".example"), /*stored=*/true);
            QVERIFY(store.save(s));
            QVERIFY(vm().vault().setSecret(vaultbind::sshSecretKey(n), SecureBuffer::fromQString(QString::fromUtf8(kSecret))));
            QVERIFY(vaultbind::rebind(vm().vault(), s));
        }
        vm().lock();
        QVERIFY(!vm().isUnlocked());

        const auto g = group(QStringLiteral("net"), names);
        QVERIFY(GroupOpener::plan(g, store).needsVault);
        DialogWatch watch{QString::fromUtf8(kMaster)};
        QPointer<MainWindow> w = GroupOpener::open(g, nullptr);
        QVERIFY(w);
        QTRY_COMPARE_WITH_TIMEOUT(w->tabCount(), 3, 10000);
        // Background tabs start with the window's terminal size, not a 1x9 default.
        for (int i = 1; i < 3; ++i) {
            QCOMPARE(w->sessionAt(i)->view()->gridRows(), w->sessionAt(0)->view()->gridRows());
            QCOMPARE(w->sessionAt(i)->view()->gridCols(), w->sessionAt(0)->view()->gridCols());
        }
        for (int i = 0; i < 3; ++i) {
            const QString host = names.at(i) + QStringLiteral(".example");
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->sessionAt(i)->terminal()).contains(QStringLiteral("got [%1] for %2").arg(QString::fromUtf8(kSecret), host)),
                                     15000);
        }
        QCOMPARE(watch.unlocks, 1);             // one prompt for the whole group
        QVERIFY2(watch.others.isEmpty(), qPrintable(watch.others.join(QLatin1Char(' ')))); // no re-bind or other dialogs
        QVERIFY(vm().isUnlocked());
        QCOMPARE(fake.runs(), 3);
    }

    void cancelledUnlockStillOpensEveryMember()
    {
        FakeSsh fake("if [ -n \"$SSH_ASKPASS\" ]; then got=asked; else got=none; fi\nprintf 'got [%s] for %s\\n' \"$got\" \"$h\"\nexec cat\n");
        const QStringList names = {QStringLiteral("sw1"), QStringLiteral("sw2")};
        QVERIFY(vm().vault().create(SecureBuffer::fromQString(QString::fromUtf8(kMaster))));
        vm().noteUnlocked();
        for (const QString &n : names) {
            QVERIFY(store.save(ssh(n, n + QStringLiteral(".example"), true)));
        }
        vm().lock();
        DialogWatch watch{QString()}; // Cancel
        QPointer<MainWindow> w = GroupOpener::open(group(QStringLiteral("net"), names), nullptr);
        QVERIFY(w);
        QTRY_COMPARE_WITH_TIMEOUT(w->tabCount(), 2, 10000);
        for (int i = 0; i < 2; ++i) {
            // ssh runs without the stored password (it asks in the tab) and says so, quietly.
            QTRY_VERIFY_WITH_TIMEOUT(screen(w->sessionAt(i)->terminal()).contains(QStringLiteral("got [none]")), 15000);
            QVERIFY(screen(w->sessionAt(i)->terminal()).contains(QStringLiteral("vault locked; ssh will ask for the password")));
        }
        QCOMPARE(watch.unlocks, 1);
        QVERIFY(watch.others.isEmpty());
        QVERIFY(!vm().isUnlocked());
        QCOMPARE(fake.runs(), 2);
    }

    // ---- needs input ----

    void needsInputMarkerAndStatusCount()
    {
        FakeSsh fake("case $h in\n"
                     " pw.example) printf \"admin@pw.example's password: \"; read x; echo accepted;;\n"
                     " yn.example) printf 'Are you sure you want to continue connecting (yes/no/[fingerprint])? '; read x; echo trusted;;\n"
                     " *) echo calm;;\n"
                     "esac\nexec cat\n");
        QVERIFY(store.save(ssh(QStringLiteral("quiet"), QStringLiteral("quiet.example"))));
        QVERIFY(store.save(ssh(QStringLiteral("pw"), QStringLiteral("pw.example"))));
        QVERIFY(store.save(ssh(QStringLiteral("yn"), QStringLiteral("yn.example"))));
        MainWindow w(parseCommandLine({QStringLiteral("quiet")}), {QStringLiteral("quiet")});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.startSession();
        for (const QString &n : {QStringLiteral("pw"), QStringLiteral("yn")}) {
            w.addTab(parseCommandLine({n}), {n}, true, /*activate=*/false);
        }
        QVERIFY(!w.needsInputIndicator()->isVisible() || w.needsInputCount() > 0);
        QTRY_COMPARE_WITH_TIMEOUT(w.needsInputCount(), 2, 10000);
        QCOMPARE(w.tabs()->tabText(0), QStringLiteral("quiet"));
        QCOMPARE(w.tabs()->tabText(1), QStringLiteral("? pw"));
        QCOMPARE(w.tabs()->tabText(2), QStringLiteral("? yn"));
        QVERIFY(w.sessionAt(1)->needsInput());
        QVERIFY(!w.sessionAt(0)->needsInput());
        QVERIFY(w.needsInputIndicator()->isVisible());
        QCOMPARE(w.needsInputIndicator()->text(), QStringLiteral("? 2 tabs waiting for input"));
        // Answer one: its marker goes, the count drops.
        w.sessionAt(1)->pty()->write("hunter2\r");
        QTRY_COMPARE_WITH_TIMEOUT(w.tabs()->tabText(1), QStringLiteral("pw"), 10000);
        QCOMPARE(w.needsInputIndicator()->text(), QStringLiteral("? 1 tab waiting for input"));
        w.sessionAt(2)->pty()->write("yes\r");
        QTRY_COMPARE_WITH_TIMEOUT(w.tabs()->tabText(2), QStringLiteral("yn"), 10000);
        QVERIFY(!w.needsInputIndicator()->isVisible());
        QCOMPARE(w.needsInputCount(), 0);
        // Closing a waiting tab updates the count too.
        w.addTab(parseCommandLine({QStringLiteral("pw")}), {QStringLiteral("pw")}, true, false);
        QTRY_COMPARE_WITH_TIMEOUT(w.needsInputCount(), 1, 10000);
        QVERIFY(w.closeTab(3, /*force=*/true));
        QCOMPARE(w.needsInputCount(), 0);
        QVERIFY(!w.needsInputIndicator()->isVisible());
    }

    // ---- Ctrl+Shift+P ----

    void pickerFiltersAndChooses()
    {
        for (const QString &n : {QStringLiteral("lab"), QStringLiteral("labtwo"), QStringLiteral("edge")}) {
            QVERIFY(groups.save(group(n, {QStringLiteral("x"), QStringLiteral("y")})));
        }
        GroupPicker p(groups);
        p.show();
        QCOMPARE(p.visibleGroups(), (QStringList{QStringLiteral("edge"), QStringLiteral("lab"), QStringLiteral("labtwo")}));
        auto *filter = child<QLineEdit>(&p, "groupFilter");
        QVERIFY(filter->hasFocus() || filter->isVisible());
        filter->setText(QStringLiteral("LAB"));
        QCOMPARE(p.visibleGroups(), (QStringList{QStringLiteral("lab"), QStringLiteral("labtwo")}));
        QCOMPARE(child<QListWidget>(&p, "groupPickerList")->item(0)->text(), QStringLiteral("lab  (2 sessions)"));
        QTest::keyClick(filter, Qt::Key_Down);
        QTest::keyClick(filter, Qt::Key_Return);
        QCOMPARE(p.result(), int(QDialog::Accepted));
        QCOMPARE(p.chosen(), QStringLiteral("labtwo"));
        // Enter with nothing matching chooses nothing.
        GroupPicker none(groups);
        none.show();
        child<QLineEdit>(&none, "groupFilter")->setText(QStringLiteral("zzz"));
        QVERIFY(none.visibleGroups().isEmpty());
        QTest::keyClick(child<QLineEdit>(&none, "groupFilter"), Qt::Key_Return);
        QVERIFY(none.chosen().isEmpty());
        none.reject();
    }

    void shortcutOpensThePickerAndTheChosenGroup()
    {
        local({QStringLiteral("s1"), QStringLiteral("s2")});
        QVERIFY(groups.save(group(QStringLiteral("lab"), {QStringLiteral("s1")})));
        QVERIFY(groups.save(group(QStringLiteral("edge"), {QStringLiteral("s2"), QStringLiteral("s1")})));
        MainWindow w(LaunchRequest{}, {});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QAction *a = w.action(QStringLiteral("openGroupByName"));
        QVERIFY(a);
        QCOMPARE(a->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+P")));
        GroupOpener::setManualSteppingForTests(true);
        whenModal([](QWidget *m) {
            auto *p = qobject_cast<GroupPicker *>(m);
            if (!p) {
                return false;
            }
            auto *filter = child<QLineEdit>(p, "groupFilter");
            filter->setText(QStringLiteral("edg"));
            QTest::keyClick(filter, Qt::Key_Return);
            return true;
        });
        a->trigger();
        MainWindow *opened = otherWindow(&w);
        QVERIFY(opened);
        QCOMPARE(opened->tabs()->tabText(0), QStringLiteral("s2")); // edge's first member
    }

    // ---- Reconnect All ----

    void reconnectAllDisconnected()
    {
        // The first run for a host drops like a lost link; later runs stay up.
        FakeSsh fake("if [ ! -e \"$S/seen.$h\" ]; then : > \"$S/seen.$h\"; echo \"first $h\"; sleep 0.3\n"
                     "  echo 'Timeout, server not responding.' >&2; exit 255; fi\n"
                     "echo \"again $h\"; exec cat\n");
        QVERIFY(store.save(ssh(QStringLiteral("d1"), QStringLiteral("h1.example"))));
        QVERIFY(store.save(ssh(QStringLiteral("d2"), QStringLiteral("h2.example"))));
        MainWindow w(parseCommandLine({QStringLiteral("d1")}), {QStringLiteral("d1")});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.startSession();
        w.addTab(parseCommandLine({QStringLiteral("d2")}), {QStringLiteral("d2")});
        QCOMPARE(w.reconnectAllDisconnected(), 0); // nothing dropped yet
        QTRY_VERIFY_WITH_TIMEOUT(w.sessionAt(0)->isDisconnected() && w.sessionAt(1)->isDisconnected(), 15000);
        QVERIFY(w.action(QStringLiteral("reconnectAll")));
        QCOMPARE(w.reconnectAllDisconnected(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!w.sessionAt(0)->isDisconnected() && !w.sessionAt(1)->isDisconnected(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(screen(w.sessionAt(0)->terminal()).contains(QStringLiteral("again h1.example")), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(screen(w.sessionAt(1)->terminal()).contains(QStringLiteral("again h2.example")), 10000);
        QCOMPARE(w.reconnectAllDisconnected(), 0);
    }

    // ---- deleting a session that groups use ----

    void deletingASessionOffersToPruneGroups()
    {
        local({QStringLiteral("x"), QStringLiteral("y")});
        auto pickY = [](SessionDialog &dlg) {
            auto *list = child<QListWidget>(&dlg, "sessionList");
            list->setCurrentItem(list->findItems(QStringLiteral("y"), Qt::MatchExactly).first());
        };
        auto reseed = [this]() {
            QVERIFY(groups.save(group(QStringLiteral("lab"), {QStringLiteral("x"), QStringLiteral("y")})));
            QVERIFY(groups.save(group(QStringLiteral("edge"), {QStringLiteral("y")})));
            QVERIFY(groups.save(group(QStringLiteral("other"), {QStringLiteral("x")})));
        };
        reseed();
        {
            // Cancel: nothing is deleted.
            SessionDialog dlg(store, SessionConfig{});
            pickY(dlg);
            QString text;
            answerBox(QMessageBox::Cancel, &text);
            QVERIFY(!dlg.deleteSelected());
            QVERIFY2(text.contains(QStringLiteral("Used in groups: edge, lab")), qPrintable(text));
            QVERIFY(store.contains(QStringLiteral("y")));
            QCOMPARE(groups.load(QStringLiteral("edge"))->members, QStringList{QStringLiteral("y")});
        }
        {
            // No: the session goes, the groups keep the (now dangling) name.
            SessionDialog dlg(store, SessionConfig{});
            pickY(dlg);
            answerBox(QMessageBox::No);
            QVERIFY(dlg.deleteSelected());
            QVERIFY(!store.contains(QStringLiteral("y")));
            QCOMPARE(groups.load(QStringLiteral("edge"))->members, QStringList{QStringLiteral("y")});
        }
        local({QStringLiteral("y")});
        {
            // Yes: pruned from every group that listed it.
            SessionDialog dlg(store, SessionConfig{});
            pickY(dlg);
            answerBox(QMessageBox::Yes);
            QVERIFY(dlg.deleteSelected());
            QVERIFY(!store.contains(QStringLiteral("y")));
            QCOMPARE(groups.load(QStringLiteral("lab"))->members, QStringList{QStringLiteral("x")});
            QVERIFY(groups.load(QStringLiteral("edge"))->members.isEmpty());
            QCOMPARE(groups.load(QStringLiteral("other"))->members, QStringList{QStringLiteral("x")});
        }
        {
            // A session no group uses is deleted without a question.
            SessionDialog dlg(store, SessionConfig{});
            auto *list = child<QListWidget>(&dlg, "sessionList");
            list->setCurrentItem(list->findItems(QStringLiteral("x"), Qt::MatchExactly).first());
            QVERIFY(groups.removeSessionRef(QStringLiteral("x")).size() == 2); // lab and other
            QVERIFY(dlg.deleteSelected());
            QVERIFY(!store.contains(QStringLiteral("x")));
        }
    }
};

QTEST_MAIN(TstGroupUi)
#include "tst_groupui.moc"
