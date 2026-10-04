// Regression test for "no menu bar" on GNOME with a global-menu extension
// (zterminal 0.1.0 on Pop!_OS with Fildem). When a process owns
// com.canonical.AppMenu.Registrar on the session bus, Qt's xcb/generic theme
// exports QMenuBar menus over D-Bus and hides the in-window bar. zterminal
// must keep its menu bar in the window regardless.
//
// Needs a real X server and a private session bus, so ctest runs it as
//   dbus-run-session -- xvfb-run -a tst_globalmenu   (QT_QPA_PLATFORM=xcb)
#include "MainWindow.hpp"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QMainWindow>
#include <QMenuBar>
#include <QTest>

using namespace zterminal;

class TstGlobalMenu : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            QSKIP("no session bus (run under dbus-run-session)");
        }
        // Pretend to be a global-menu service (like Fildem's appmenu daemon).
        QVERIFY(bus.registerService(QStringLiteral("com.canonical.AppMenu.Registrar")));
        QVERIFY(bus.interface()->isServiceRegistered(QStringLiteral("com.canonical.AppMenu.Registrar")));
    }

    void plainQtWindowLosesItsMenuBar()
    {
        // Control: proves this environment reproduces the bug for a default QMenuBar.
        QMainWindow w;
        w.menuBar()->addMenu(QStringLiteral("&File"));
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        if (!w.menuBar()->isNativeMenuBar()) {
            QSKIP("this Qt platform theme does not use the D-Bus global menu; cannot reproduce");
        }
        QVERIFY(!w.menuBar()->isVisible());
    }

    void zterminalKeepsMenuBarInWindow()
    {
        LaunchRequest req;
        MainWindow w(req, {});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(!w.menuBar()->isNativeMenuBar());
        QVERIFY(w.menuBar()->isVisible());
        QVERIFY(w.menuBar()->height() > 0);
        QCOMPARE(w.menuBar()->actions().size(), 6);
    }
};

QTEST_MAIN(TstGlobalMenu)
#include "tst_globalmenu.moc"
