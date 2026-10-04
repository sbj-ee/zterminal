// Safe copy and paste in the UI (offscreen): Preferences toggles (on by
// default), plain-text trimmed copy, and the multi-line paste confirmation
// (line count, preview, cancel, "don't ask again", bracketed-paste note).
// Serial pacing with the confirmation is covered in tst_serialui.
#include "AppSettings.hpp"
#include "CommandLine.hpp"
#include "MainWindow.hpp"
#include "PreferencesDialog.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
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

struct Seen {
    bool shown = false;
    QString summary, notes, preview;
};

// Answer the next paste confirmation: accept or cancel, optionally ticking
// "Don't ask again". Records what it showed.
void answerPaste(Seen *seen, bool accept, bool dontAsk = false)
{
    auto *timer = new QTimer;
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, [=]() {
        QWidget *m = QApplication::activeModalWidget();
        if (!m || m->objectName() != QLatin1String("pasteConfirmDialog")) {
            return;
        }
        seen->shown = true;
        seen->summary = child<QLabel>(m, "pasteSummary")->text();
        if (auto *n = m->findChild<QLabel *>(QStringLiteral("pasteNotes"))) {
            seen->notes = n->text();
        }
        seen->preview = child<QPlainTextEdit>(m, "pastePreview")->toPlainText();
        child<QCheckBox>(m, "dontAskAgain")->setChecked(dontAsk);
        if (accept) {
            child<QPushButton>(m, "pasteButton")->click();
        } else {
            QMetaObject::invokeMethod(m, "reject");
        }
        timer->stop();
        timer->deleteLater();
    });
    timer->start();
}

LaunchRequest idle()
{
    return parseCommandLine({QStringLiteral("-e"), QStringLiteral("sleep"), QStringLiteral("60")});
}

QByteArray sentBy(QSignalSpy &spy)
{
    QByteArray all;
    for (const auto &a : spy) {
        all += a.at(0).toByteArray();
    }
    spy.clear();
    return all;
}

} // namespace

class TstSafePaste : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        AppSettings{}.save(); // defaults for every test
    }

    void defaultsAndPreferences()
    {
        AppSettings d;
        QVERIFY(d.trimCopiedWhitespace);
        QVERIFY(d.confirmMultilinePaste);
        PreferencesDialog dlg(d);
        auto *trim = child<QCheckBox>(&dlg, "trimCopiedWhitespace");
        auto *confirm = child<QCheckBox>(&dlg, "confirmMultilinePaste");
        QVERIFY(trim->isChecked());
        QVERIFY(confirm->isChecked());
        trim->setChecked(false);
        confirm->setChecked(false);
        const AppSettings r = dlg.result();
        QVERIFY(!r.trimCopiedWhitespace);
        QVERIFY(!r.confirmMultilinePaste);
        QVERIFY(r != d);
        QTemporaryDir tmp;
        QSettings ini(tmp.filePath(QStringLiteral("z.ini")), QSettings::IniFormat);
        r.save(ini);
        QCOMPARE(ini.value(QStringLiteral("clipboard/trimTrailingWhitespace")).toBool(), false);
        QCOMPARE(ini.value(QStringLiteral("clipboard/confirmMultilinePaste")).toBool(), false);
        QVERIFY(AppSettings::load(ini) == r);
    }

    void copyIsPlainAndTrimmed()
    {
        MainWindow w(idle(), {});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.terminal()->feed("\x1b[1;31mred   \x1b[0m\r\n  ok\t\r\n");
        w.action(QStringLiteral("selectAll"))->trigger();
        w.action(QStringLiteral("copy"))->trigger();
        const QMimeData *md = QApplication::clipboard()->mimeData(QClipboard::Clipboard);
        QVERIFY(md);
        QVERIFY(!md->hasHtml());
        QVERIFY(QApplication::clipboard()->text().startsWith(QStringLiteral("red\n  ok\n")));

        AppSettings s;
        s.trimCopiedWhitespace = false;
        w.setSettings(s);
        w.action(QStringLiteral("copy"))->trigger();
        QVERIFY2(QApplication::clipboard()->text().startsWith(QStringLiteral("red   \n  ok")),
                 qPrintable(QApplication::clipboard()->text()));
    }

    void singleLineNeedsNoConfirmation()
    {
        MainWindow w(idle(), {});
        w.show();
        QSignalSpy out(w.terminal(), &Terminal::output);
        QApplication::clipboard()->setText(QStringLiteral("show vlan brief"));
        w.action(QStringLiteral("paste"))->trigger();
        QCOMPARE(sentBy(out), QByteArray("show vlan brief"));
    }

    void multiLineAsksAndCancelSendsNothing()
    {
        MainWindow w(idle(), {});
        w.show();
        QSignalSpy out(w.terminal(), &Terminal::output);
        QApplication::clipboard()->setText(QStringLiteral("conf t\ninterface Gi1/0/1\n shutdown\nend\n"));
        Seen seen;
        answerPaste(&seen, false);
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(seen.shown);
        QVERIFY2(seen.summary.contains(QStringLiteral("Paste 4 lines")), qPrintable(seen.summary));
        QCOMPARE(seen.preview, QStringLiteral("conf t\ninterface Gi1/0/1\n shutdown\nend"));
        QVERIFY(seen.notes.contains(QStringLiteral("like pressing Enter")));
        QVERIFY(sentBy(out).isEmpty());

        // Accept: sent, with CR line ends.
        Seen again;
        answerPaste(&again, true);
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(again.shown);
        QCOMPARE(sentBy(out), QByteArray("conf t\rinterface Gi1/0/1\r shutdown\rend\r"));
        QVERIFY(!w.pasteConfirmSkipped());
    }

    void trailingNewlineCountsAsMultiLine()
    {
        MainWindow w(idle(), {});
        w.show();
        QApplication::clipboard()->setText(QStringLiteral("reload\n"));
        Seen seen;
        answerPaste(&seen, false);
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(seen.shown);
        QVERIFY(seen.summary.contains(QStringLiteral("Paste 1 line ")));
    }

    void dontAskAgainLastsForThisWindow()
    {
        MainWindow w(idle(), {});
        w.show();
        QSignalSpy out(w.terminal(), &Terminal::output);
        QApplication::clipboard()->setText(QStringLiteral("a\nb"));
        Seen seen;
        answerPaste(&seen, true, true);
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(seen.shown);
        QVERIFY(w.pasteConfirmSkipped());
        QCOMPARE(sentBy(out), QByteArray("a\rb"));
        // No dialog now (one would block: the test would time out).
        w.action(QStringLiteral("paste"))->trigger();
        QCOMPARE(sentBy(out), QByteArray("a\rb"));

        // A new window (session) asks again.
        MainWindow other(idle(), {});
        other.show();
        Seen next;
        answerPaste(&next, false);
        other.action(QStringLiteral("paste"))->trigger();
        QVERIFY(next.shown);
    }

    void cancelWithDontAskDoesNotDisable()
    {
        MainWindow w(idle(), {});
        w.show();
        QApplication::clipboard()->setText(QStringLiteral("a\nb"));
        Seen seen;
        answerPaste(&seen, false, true);
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(seen.shown);
        QVERIFY(!w.pasteConfirmSkipped());
    }

    void settingOffPastesDirectly()
    {
        MainWindow w(idle(), {});
        w.show();
        AppSettings s;
        s.confirmMultilinePaste = false;
        w.setSettings(s);
        QSignalSpy out(w.terminal(), &Terminal::output);
        QApplication::clipboard()->setText(QStringLiteral("x\ny\n"));
        w.action(QStringLiteral("paste"))->trigger();
        QCOMPARE(sentBy(out), QByteArray("x\ry\r"));
    }

    void bracketedPasteIsNotedAndUsed()
    {
        MainWindow w(idle(), {});
        w.show();
        w.terminal()->feed("\x1b[?2004h"); // as bash/zsh/vim do
        QSignalSpy out(w.terminal(), &Terminal::output);
        QApplication::clipboard()->setText(QStringLiteral("echo 1\necho 2\n"));
        Seen seen;
        answerPaste(&seen, true);
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(seen.shown);
        QVERIFY2(seen.notes.contains(QStringLiteral("bracketed paste")), qPrintable(seen.notes));
        QCOMPARE(sentBy(out), QByteArray("\x1b[200~echo 1\recho 2\r\x1b[201~"));
    }

    void controlCharactersAreVisibleInPreview()
    {
        MainWindow w(idle(), {});
        w.show();
        QApplication::clipboard()->setText(QStringLiteral("ls\x1b[8m secret\nrm -rf /tmp/x\n"));
        Seen seen;
        answerPaste(&seen, false);
        w.action(QStringLiteral("paste"))->trigger();
        QVERIFY(seen.shown);
        QVERIFY(seen.preview.startsWith(QStringLiteral("ls\u241b[8m secret")));
        QVERIFY(seen.notes.contains(QStringLiteral("1 control character")));
    }
};

QTEST_MAIN(TstSafePaste)
#include "tst_safepaste.moc"
