// Settings must actually work (0.1.1): QSettings round-trip, Preferences
// reachable without the menu bar, dialog -> settings, live apply to this
// window and to other open windows, persistence across windows.
#include "AppSettings.hpp"
#include "ColorScheme.hpp"
#include "MainWindow.hpp"
#include "PreferencesDialog.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontComboBox>
#include <QLabel>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

// CMake points XDG_CONFIG_HOME at a per-test directory, never the real ~/.config.

using namespace zterminal;

namespace {

AppSettings nonDefaults()
{
    AppSettings a;
    a.fontFamily = QStringLiteral("Some Mono");
    a.fontSize = 17;
    a.colorScheme = ColorScheme::builtIn().last().id;
    a.scrollbackLines = 4321;
    a.checkForUpdatesOnStartup = false;
    a.mouse.middleClick = MiddleClickAction::Off;
    a.mouse.copyOnSelectToClipboard = false;
    a.mouse.wordDelimiters = QStringLiteral(" ;");
    return a;
}

QString otherMonospaceFamily(const QString &notThis)
{
    for (const QString &f : QFontDatabase::families()) {
        if (QFontDatabase::isFixedPitch(f) && f != notThis && QFontDatabase::isSmoothlyScalable(f)) {
            return f;
        }
    }
    return {};
}

// Runs `fn` on the modal Preferences dialog once it is open (exec() blocks).
void whenPreferencesOpen(const std::function<void(PreferencesDialog *)> &fn)
{
    auto *timer = new QTimer;
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, [timer, fn]() {
        if (auto *d = qobject_cast<PreferencesDialog *>(QApplication::activeModalWidget())) {
            timer->stop();
            timer->deleteLater();
            fn(d);
        }
    });
    timer->start();
}

} // namespace

class TstSettings : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        QFile::remove(AppSettings::filePath());
    }

    void defaultsWhenNothingStored()
    {
        QTemporaryDir dir;
        const QSettings s(dir.filePath(QStringLiteral("z.ini")), QSettings::IniFormat);
        const AppSettings a = AppSettings::load(s);
        QCOMPARE(a.fontSize, AppSettings::kDefaultFontSize);
        QCOMPARE(a.colorScheme, QStringLiteral("xterm"));
        QCOMPARE(a.scrollbackLines, 100000);
        QVERIFY(a.checkForUpdatesOnStartup);
        QVERIFY(a.mouse.copyOnSelectToClipboard);
        QCOMPARE(a.mouse.middleClick, MiddleClickAction::PastePrimary);
        QVERIFY(!a.fontFamily.isEmpty());
    }

    void roundTripThroughQSettingsInTempDir()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("zterminal.ini"));
        const AppSettings in = nonDefaults();
        {
            QSettings s(path, QSettings::IniFormat);
            in.save(s);
            s.sync();
            QCOMPARE(s.status(), QSettings::NoError);
        }
        QVERIFY(QFile::exists(path));
        const QSettings s(path, QSettings::IniFormat);
        const AppSettings out = AppSettings::load(s);
        QCOMPARE(out.fontFamily, in.fontFamily);
        QCOMPARE(out.fontSize, in.fontSize);
        QCOMPARE(out.colorScheme, in.colorScheme);
        QCOMPARE(out.scrollbackLines, in.scrollbackLines);
        QCOMPARE(out.checkForUpdatesOnStartup, false);
        QCOMPARE(out.mouse.middleClick, MiddleClickAction::Off);
        QCOMPARE(out.mouse.copyOnSelectToClipboard, false);
        QCOMPARE(out.mouse.wordDelimiters, in.mouse.wordDelimiters);
        QVERIFY(out == in);
        QCOMPARE(s.value(QStringLiteral("updates/checkOnStartup")).toBool(), false);
    }

    void defaultFileIsUnderXdgConfigHome()
    {
        // main() uses IniFormat + "zterminal"/"zterminal": $XDG_CONFIG_HOME/zterminal/zterminal.ini
        const QString expected = qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/zterminal/zterminal.ini");
        QCOMPARE(AppSettings::filePath(), expected);
        nonDefaults().save();
        QVERIFY(QFile::exists(expected));
        QVERIFY(AppSettings::load() == nonDefaults());
    }

    void dialogReflectsAndReturnsEveryControl()
    {
        AppSettings base;
        base.fontFamily = AppSettings::defaultFontFamily();
        PreferencesDialog d(base);
        auto *copy = d.findChild<QCheckBox *>(QStringLiteral("copyToClipboard"));
        auto *middle = d.findChild<QComboBox *>(QStringLiteral("middleClick"));
        auto *font = d.findChild<QFontComboBox *>(QStringLiteral("fontFamily"));
        auto *size = d.findChild<QSpinBox *>(QStringLiteral("fontSize"));
        auto *scheme = d.findChild<QComboBox *>(QStringLiteral("colorScheme"));
        auto *scroll = d.findChild<QSpinBox *>(QStringLiteral("scrollbackLines"));
        auto *upd = d.findChild<QCheckBox *>(QStringLiteral("checkForUpdatesOnStartup"));
        for (QWidget *w : std::initializer_list<QWidget *>{copy, middle, font, size, scheme, scroll, upd}) {
            QVERIFY(w);
            QVERIFY(w->isEnabled()); // nothing stubbed or disabled
        }
        QVERIFY(scheme->count() >= 2);

        // Unchanged dialog returns the same settings (font family is not rewritten).
        QVERIFY(d.result() == base);

        copy->setChecked(false);
        middle->setCurrentIndex(middle->findData(QStringLiteral("paste-clipboard")));
        size->setValue(19);
        scheme->setCurrentIndex(scheme->count() - 1);
        scroll->setValue(2500);
        upd->setChecked(false);
        const QString other = otherMonospaceFamily(font->currentFont().family());
        if (!other.isEmpty()) {
            font->setCurrentFont(QFont(other));
        }
        const AppSettings r = d.result();
        QCOMPARE(r.mouse.copyOnSelectToClipboard, false);
        QCOMPARE(r.mouse.middleClick, MiddleClickAction::PasteClipboard);
        QCOMPARE(r.fontSize, 19);
        QCOMPARE(r.colorScheme, ColorScheme::builtIn().last().id);
        QCOMPARE(r.scrollbackLines, 2500);
        // Unlimited: the spin box is disabled and a memory warning shows.
        auto *unlimited = d.findChild<QCheckBox *>(QStringLiteral("unlimitedScrollback"));
        auto *warning = d.findChild<QLabel *>(QStringLiteral("unlimitedScrollbackWarning"));
        QVERIFY(unlimited && warning);
        QVERIFY(!unlimited->isChecked());
        QVERIFY(warning->isHidden());
        unlimited->setChecked(true);
        QVERIFY(!scroll->isEnabled());
        QVERIFY(!warning->isHidden());
        QVERIFY(warning->text().contains(QStringLiteral("memory")));
        QCOMPARE(d.result().scrollbackLines, AppSettings::kUnlimitedScrollback);
        unlimited->setChecked(false);
        QCOMPARE(d.result().scrollbackLines, 2500);
        QCOMPARE(r.checkForUpdatesOnStartup, false);
        if (!other.isEmpty()) {
            QCOMPARE(r.fontFamily, font->currentFont().family());
        }

        // Apply emits the settings without closing.
        AppSettings applied;
        connect(&d, &PreferencesDialog::applied, this, [&applied](const AppSettings &a) { applied = a; });
        d.findChild<QDialogButtonBox *>(QStringLiteral("buttons"))->button(QDialogButtonBox::Apply)->click();
        QVERIFY(applied == r);
    }

    void brandSchemesAreOfferedEverywhere()
    {
        // Boilermakers / Badgers / Packers (shared with zmail): Preferences
        // combo box and View > Color Scheme; picking one applies and persists.
        AppSettings base;
        base.fontFamily = AppSettings::defaultFontFamily();
        PreferencesDialog d(base);
        auto *scheme = d.findChild<QComboBox *>(QStringLiteral("colorScheme"));
        QVERIFY(scheme);
        LaunchRequest req;
        MainWindow w(req, {});
        w.show();
        for (const char *id : {"boilermakers", "badgers", "packers"}) {
            const QString sid = QString::fromLatin1(id);
            QVERIFY2(scheme->findData(sid) >= 0, id);
            QAction *a = w.action(QStringLiteral("scheme:") + sid);
            QVERIFY2(a, id);
            a->trigger();
            QCOMPARE(w.terminal()->colorScheme().id, sid);
            QCOMPARE(AppSettings::load().colorScheme, sid);
        }
        QCOMPARE(scheme->itemText(scheme->findData(QStringLiteral("badgers"))), QStringLiteral("Badgers"));
    }

    void applyingUpdatesTheWidgetsLive()
    {
        LaunchRequest req;
        MainWindow w(req, {});
        w.show();
        AppSettings s = w.settings();
        s.fontSize = 21;
        const QString other = otherMonospaceFamily(w.view()->terminalFont().family());
        if (!other.isEmpty()) {
            s.fontFamily = other;
        }
        const ColorScheme target = ColorScheme::builtIn().last();
        QVERIFY(target.id != w.terminal()->colorScheme().id);
        s.colorScheme = target.id;
        s.scrollbackLines = 777;
        s.mouse.middleClick = MiddleClickAction::Off;
        s.mouse.copyOnSelectToClipboard = false;
        const QSize cellBefore = w.view()->cellSize();
        w.setSettings(s);

        QCOMPARE(w.view()->terminalFont().pointSize(), 21);
        if (!other.isEmpty()) {
            QCOMPARE(w.view()->terminalFont().family(), other);
        }
        QVERIFY(w.view()->cellSize() != cellBefore);
        QCOMPARE(w.terminal()->colorScheme().id, target.id);
        QVERIFY(w.action(QStringLiteral("scheme:") + target.id)->isChecked());
        QCOMPARE(w.terminal()->scrollbackLimit(), 777);
        QCOMPARE(w.view()->mouseSettings().middleClick, MiddleClickAction::Off);
        QCOMPARE(w.view()->mouseSettings().copyOnSelectToClipboard, false);
        // ...and persisted.
        QVERIFY(AppSettings::load() == s);

        // View > Color Scheme menu applies and persists too.
        const ColorScheme first = ColorScheme::builtIn().first();
        w.action(QStringLiteral("scheme:") + first.id)->trigger();
        QCOMPARE(w.terminal()->colorScheme().id, first.id);
        QCOMPARE(AppSettings::load().colorScheme, first.id);
    }

    void otherOpenWindowsFollowTheSettingsFile()
    {
        LaunchRequest req;
        MainWindow a(req, {});
        MainWindow b(req, {});
        a.show();
        b.show();
        AppSettings s = a.settings();
        s.fontSize = 23;
        s.colorScheme = ColorScheme::builtIn().last().id;
        a.setSettings(s);
        QTRY_COMPARE_WITH_TIMEOUT(b.view()->terminalFont().pointSize(), 23, 5000);
        QCOMPARE(b.terminal()->colorScheme().id, s.colorScheme);

        // A change written by another process (simulated: write the file directly).
        AppSettings t = s;
        t.scrollbackLines = 1234;
        t.save();
        QTRY_COMPARE_WITH_TIMEOUT(a.terminal()->scrollbackLimit(), 1234, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(b.terminal()->scrollbackLimit(), 1234, 5000);
        // b saving later must not resurrect stale values.
        AppSettings u = b.settings();
        u.fontSize = 12;
        b.setSettings(u);
        QCOMPARE(AppSettings::load().scrollbackLines, 1234);
    }

    void preferencesReachableWithoutMenuBar()
    {
        LaunchRequest req;
        MainWindow w(req, {});
        w.show();
        QAction *prefs = w.action(QStringLiteral("preferences"));
        QVERIFY(prefs && prefs->isEnabled());
        QVERIFY(prefs->shortcuts().contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Comma)));
        QVERIFY(w.contextMenu()->actions().contains(prefs));
        QVERIFY(w.contextMenu()->actions().contains(w.action(QStringLiteral("showMenuBar"))));
        QAction *change = w.action(QStringLiteral("changeSettings"));
        QVERIFY(change && change->isEnabled());

        // The shortcut is an app shortcut, not sent to the session.
        QKeyEvent ev(QEvent::ShortcutOverride, Qt::Key_Comma, Qt::ControlModifier | Qt::ShiftModifier);
        ev.ignore();
        QApplication::sendEvent(w.view(), &ev);
        QVERIFY(!ev.isAccepted());

        // Session > Change Settings opens Preferences; OK saves and applies.
        bool opened = false;
        whenPreferencesOpen([&opened](PreferencesDialog *d) {
            opened = true;
            d->findChild<QSpinBox *>(QStringLiteral("fontSize"))->setValue(15);
            d->accept();
        });
        change->trigger();
        QVERIFY(opened);
        QCOMPARE(w.view()->terminalFont().pointSize(), 15);
        QCOMPARE(AppSettings::load().fontSize, 15);

        // Cancel changes nothing.
        whenPreferencesOpen([](PreferencesDialog *d) {
            d->findChild<QSpinBox *>(QStringLiteral("fontSize"))->setValue(30);
            d->reject();
        });
        prefs->trigger();
        QCOMPARE(w.view()->terminalFont().pointSize(), 15);
        QCOMPARE(AppSettings::load().fontSize, 15);
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    TstSettings t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_settings.moc"
