// Find (docs/PLAN.md §4.17): TerminalSearch over scrollback + screen, the
// per-tab find bar (Ctrl+Shift+F, Enter / Shift+Enter, Esc, case / regex /
// wrap, match count) and the highlights painted by TerminalView.
#include "AppSettings.hpp"
#include "FindBar.hpp"
#include "MainWindow.hpp"
#include "SessionWidget.hpp"
#include "Terminal.hpp"
#include "TerminalSearch.hpp"
#include "TerminalView.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

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

QByteArray lines(int from, int to, const char *word = "needle")
{
    QByteArray b;
    for (int i = from; i < to; ++i) {
        b += "row " + QByteArray::number(i) + ((i % 10 == 0) ? QByteArray(" ") + word : QByteArray(" hay")) + "\r\n";
    }
    return b;
}

// A Terminal + view + find bar, as SessionWidget lays them out.
struct Harness {
    QWidget top;
    Terminal term{10, 60};
    TerminalView *view;
    FindBar *bar;
    Harness()
    {
        auto *col = new QVBoxLayout(&top);
        view = new TerminalView(&term, &top);
        bar = new FindBar(&term, view, &top);
        col->addWidget(view, 1);
        col->addWidget(bar);
        top.resize(700, 400);
        top.show();
    }
};

QSize gridOf(const TerminalView *v) { return v->cellSize(); }

QRgb pixelAt(TerminalView *v, int line, int col)
{
    const QImage img = v->viewport()->grab().toImage();
    const int row = line - v->firstVisibleLine();
    const QSize cell = gridOf(v);
    const qreal dpr = img.devicePixelRatio();
    return img.pixel(int((2 + col * cell.width() + 1) * dpr), int((2 + row * cell.height() + 1) * dpr));
}

} // namespace

class TstFind : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() { AppSettings{}.save(); }

    void searchEngine()
    {
        Terminal t(5, 40);
        t.feed("Alpha beta ALPHA\r\nfoo.bar foo-bar\r\n\xe4\xb8\xad" "alpha\r\n\r\n\r\n\r\n\r\n\r\n");
        t.feed("alpha on screen");
        QVERIFY(t.scrollbackLines() >= 3);
        auto m = TerminalSearch::findAll(t, QStringLiteral("alpha"), {});
        QCOMPARE(m.size(), std::size_t(4)); // case-insensitive, history + screen
        QCOMPARE(m[0], (FindMatch{0, 0, 5}));
        QCOMPARE(m[1], (FindMatch{0, 11, 5}));
        QCOMPARE(m[2], (FindMatch{2, 2, 5})); // after a wide character
        QCOMPARE(m[3].line, t.scrollbackLines() + t.cursorPos().y());
        FindOptions cs;
        cs.caseSensitive = true;
        QCOMPARE(TerminalSearch::findAll(t, QStringLiteral("ALPHA"), cs).size(), std::size_t(1));
        // Plain text: regex characters are literal.
        QCOMPARE(TerminalSearch::findAll(t, QStringLiteral("foo.bar"), {}).size(), std::size_t(1));
        FindOptions re;
        re.regex = true;
        QCOMPARE(TerminalSearch::findAll(t, QStringLiteral("foo.bar"), re).size(), std::size_t(2));
        QCOMPARE(TerminalSearch::findAll(t, QStringLiteral("^alpha"), re).size(), std::size_t(2));
        // A wide character spans two columns.
        auto wide = TerminalSearch::findAll(t, QString::fromUtf8("\u4E2Da"), {});
        QCOMPARE(wide.size(), std::size_t(1));
        QCOMPARE(wide[0], (FindMatch{2, 0, 3}));
        // Empty matches are skipped; invalid patterns report an error.
        QVERIFY(TerminalSearch::findAll(t, QStringLiteral("x*"), re).empty());
        QString err;
        QVERIFY(TerminalSearch::findAll(t, QStringLiteral("(unclosed"), re, &err).empty());
        QVERIFY(!err.isEmpty());
        QVERIFY(TerminalSearch::findAll(t, QString(), {}).empty());
    }

    void matchesAcrossSoftWraps()
    {
        Terminal t(6, 20);
        // 20 columns: "changed state" is cut by the wrap after "...changed sta".
        t.feed("Interface X changed state to up\r\n");
        t.feed("01234567890123456789"); // exactly fills a line, then a new line starts
        t.feed("tail\r\n");
        t.feed("short changed\r\nstate (hard newline)\r\n");
        QCOMPARE(t.searchText(0), QStringLiteral("Interface X changed "));
        auto m = TerminalSearch::findAll(t, QStringLiteral("changed state"), {});
        QCOMPARE(m.size(), std::size_t(2)); // the hard newline doesn't join
        QCOMPARE(m[0], (FindMatch{0, 12, 8, false}));
        QCOMPARE(m[1], (FindMatch{1, 0, 5, true}));
        // A full line that is followed by more text joins too (exact-width output).
        auto j = TerminalSearch::findAll(t, QStringLiteral("89tail"), {});
        QCOMPARE(j.size(), std::size_t(2));
        QCOMPARE(j[0], (FindMatch{2, 18, 2, false}));
        QCOMPARE(j[1], (FindMatch{3, 0, 4, true}));
        // Regex across the wrap.
        FindOptions re;
        re.regex = true;
        QCOMPARE(TerminalSearch::findAll(t, QStringLiteral("X ch\\w+ st"), re).size(), std::size_t(2));
    }

    void barNavigationAndCount()
    {
        Harness h;
        h.term.feed(lines(0, 200));
        h.bar->open();
        QVERIFY(h.bar->isVisible());
        auto *text = child<QLineEdit>(h.bar, "findText");
        QTRY_VERIFY(text->hasFocus());
        QTest::keyClicks(text, QStringLiteral("NEEDLE"));
        QTRY_COMPARE(h.bar->matchCount(), 20); // debounced
        auto *count = child<QLabel>(h.bar, "findCount");
        QCOMPARE(count->text(), QStringLiteral("1 of 20"));
        // Current = newest (row 190), and the view highlights all matches.
        QCOMPARE(h.bar->matches()[std::size_t(h.bar->currentIndex())].line, 190);
        QCOMPARE(h.view->findMatches().size(), std::size_t(20));
        QCOMPARE(h.view->currentFindMatch(), 19);

        // Enter = next (older), Shift+Enter = previous (newer).
        QTest::keyClick(text, Qt::Key_Return);
        QCOMPARE(count->text(), QStringLiteral("2 of 20"));
        QCOMPARE(h.bar->matches()[std::size_t(h.bar->currentIndex())].line, 180);
        QTest::keyClick(text, Qt::Key_Enter);
        QCOMPARE(h.bar->currentNumber(), 3);
        QTest::keyClick(text, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(h.bar->currentNumber(), 2);
        // The view scrolled to the current match.
        const int line = h.bar->matches()[std::size_t(h.bar->currentIndex())].line;
        QVERIFY(line >= h.view->firstVisibleLine() && line < h.view->firstVisibleLine() + h.term.rows());

        // Wrap (default on): previous from the newest goes to the oldest.
        QTest::keyClick(text, Qt::Key_Return, Qt::ShiftModifier);
        QTest::keyClick(text, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(h.bar->currentNumber(), 20);
        QVERIFY(count->text().startsWith(QStringLiteral("20 of 20")));
        QCOMPARE(h.view->firstVisibleLine(), 0); // scrolled to the oldest match
        // Wrap off: next from the oldest stays there.
        child<QCheckBox>(h.bar, "findWrap")->setChecked(false);
        QTest::keyClick(text, Qt::Key_Return);
        QCOMPARE(h.bar->currentNumber(), 20);
        QVERIFY2(count->text().contains(QStringLiteral("top reached")), qPrintable(count->text()));
        QTest::keyClick(text, Qt::Key_Return, Qt::ShiftModifier);
        QCOMPARE(h.bar->currentNumber(), 19);
        // Buttons do the same.
        child<QToolButton>(h.bar, "findNext")->click();
        QCOMPARE(h.bar->currentNumber(), 20);
        child<QToolButton>(h.bar, "findPrevious")->click();
        QCOMPARE(h.bar->currentNumber(), 19);

        // Esc closes and drops the highlights.
        QSignalSpy closed(h.bar, &FindBar::closed);
        QTest::keyClick(text, Qt::Key_Escape);
        QVERIFY(!h.bar->isVisible());
        QCOMPARE(closed.count(), 1);
        QVERIFY(h.view->findMatches().empty());
        // Reopening keeps the text, selected, and searches again.
        h.bar->open();
        QCOMPARE(text->selectedText(), QStringLiteral("NEEDLE"));
        QCOMPARE(h.bar->matchCount(), 20);
    }

    void options()
    {
        Harness h;
        h.term.feed("Error error ERROR err0r\r\nwarn: disk 93% full\r\n");
        h.bar->open();
        h.bar->setText(QStringLiteral("error"));
        h.bar->search();
        QCOMPARE(h.bar->matchCount(), 3);
        child<QCheckBox>(h.bar, "findCase")->setChecked(true);
        QCOMPARE(h.bar->matchCount(), 1);
        child<QCheckBox>(h.bar, "findCase")->setChecked(false);
        child<QCheckBox>(h.bar, "findRegex")->setChecked(true);
        h.bar->setText(QStringLiteral("err[o0]r"));
        h.bar->search();
        QCOMPARE(h.bar->matchCount(), 4);
        h.bar->setText(QStringLiteral("\\d+%"));
        h.bar->search();
        QCOMPARE(h.bar->matchCount(), 1);
        QCOMPARE(h.bar->matches()[0], (FindMatch{1, 11, 3}));
        // Invalid regex: message, no matches, no crash on Enter.
        h.bar->setText(QStringLiteral("err[or"));
        h.bar->search();
        QCOMPARE(h.bar->matchCount(), 0);
        QCOMPARE(h.bar->countText(), QStringLiteral("Invalid regex"));
        QVERIFY(!h.bar->errorText().isEmpty());
        QTest::keyClick(child<QLineEdit>(h.bar, "findText"), Qt::Key_Return);
        h.bar->setText(QStringLiteral("nothing-here"));
        h.bar->search();
        QCOMPARE(h.bar->countText(), QStringLiteral("No matches"));
        QVERIFY(!child<QToolButton>(h.bar, "findNext")->isEnabled());
    }

    void highlightsArePainted()
    {
        Harness h;
        h.term.feed("plain text needle and needle\r\nnothing\r\n");
        h.bar->open();
        h.bar->setText(QStringLiteral("needle"));
        h.bar->search();
        QCOMPARE(h.bar->matchCount(), 2);
        // Current (newest = second on the line) orange, the other yellow.
        QTRY_COMPARE(pixelAt(h.view, 0, 22), TerminalView::kFindCurrentBackground);
        QCOMPARE(pixelAt(h.view, 0, 11), TerminalView::kFindMatchBackground);
        QCOMPARE(pixelAt(h.view, 0, 2), h.term.colorScheme().background);
        h.bar->findNext();
        QCOMPARE(pixelAt(h.view, 0, 11), TerminalView::kFindCurrentBackground);
        QCOMPARE(pixelAt(h.view, 0, 22), TerminalView::kFindMatchBackground);
        h.bar->close();
        QCOMPARE(pixelAt(h.view, 0, 11), h.term.colorScheme().background);
    }

    void followsOutputAndDroppedLines()
    {
        Harness h;
        h.term.setScrollbackLimit(100);
        h.term.feed(lines(0, 50));
        h.bar->open();
        h.bar->setText(QStringLiteral("needle"));
        h.bar->search();
        QCOMPARE(h.bar->matchCount(), 5);
        h.bar->findNext(); // row 30
        const FindMatch cur = h.bar->matches()[std::size_t(h.bar->currentIndex())];
        QCOMPARE(cur.line, 30);
        // More output: old lines are dropped (limit 100) and indices shift; the
        // bar re-searches once output pauses and keeps the current match.
        h.term.feed(lines(50, 160));
        const int firstKept = 161 - (100 + h.term.rows()); // 160 rows + the empty cursor line
        QVERIFY(firstKept > 30);
        const int firstNeedle = (firstKept + 9) / 10 * 10;
        QTRY_COMPARE(h.bar->matchCount(), (159 - firstNeedle) / 10 + 1);
        const FindMatch now = h.bar->matches()[std::size_t(h.bar->currentIndex())];
        // Row 30 was dropped: the current match moved to the oldest one left.
        QCOMPARE(h.bar->currentIndex(), 0);
        QCOMPARE(h.term.lineText(now.line), QStringLiteral("row %1 needle").arg(firstNeedle));
        // Clearing the scrollback re-searches the screen only.
        h.term.clearScrollback();
        QTRY_VERIFY(h.bar->matchCount() <= 1);
    }

    void perTabFromMainWindow()
    {
        auto shArgs = [](const QString &script) {
            return QStringList{QStringLiteral("-e"), QStringLiteral("sh"), QStringLiteral("-c"), script};
        };
        MainWindow w(parseCommandLine(shArgs(QStringLiteral("echo TAB-A apple apple; exec cat"))), {});
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        w.startSession();
        SessionWidget *a = w.currentSession();
        SessionWidget *b = w.addTab(parseCommandLine(shArgs(QStringLiteral("echo TAB-B banana; exec cat"))), {});
        QTRY_VERIFY_WITH_TIMEOUT(a->terminal()->searchText(0).contains(QStringLiteral("TAB-A")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(b->terminal()->searchText(0).contains(QStringLiteral("TAB-B")), 5000);
        QCOMPARE(w.currentSession(), b);
        QVERIFY(w.action(QStringLiteral("find"))->isEnabled());
        QCOMPARE(w.action(QStringLiteral("find"))->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+F")));

        // Ctrl+Shift+F from the terminal is an app shortcut, not session input.
        b->view()->setFocus();
        QTRY_VERIFY(b->view()->hasFocus());
        QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        over.ignore();
        QApplication::sendEvent(b->view(), &over);
        QVERIFY(!over.isAccepted());
        w.action(QStringLiteral("find"))->trigger();
        QVERIFY(b->findBar()->isVisible());
        QVERIFY(!a->findBar()->isVisible());
        QTRY_VERIFY(child<QLineEdit>(b->findBar(), "findText")->hasFocus());
        b->findBar()->setText(QStringLiteral("banana"));
        b->findBar()->search();
        QCOMPARE(b->findBar()->matchCount(), 1);

        // The other tab has its own bar, text and matches.
        w.tabs()->setCurrentWidget(a);
        w.action(QStringLiteral("find"))->trigger();
        QVERIFY(a->findBar()->isVisible());
        QVERIFY(a->findBar()->text().isEmpty());
        a->findBar()->setText(QStringLiteral("apple"));
        a->findBar()->search();
        QCOMPARE(a->findBar()->matchCount(), 2);
        QCOMPARE(b->findBar()->matchCount(), 1);
        QVERIFY(!b->findBar()->isHidden()); // still open in its (background) tab

        // Esc closes this tab's bar and gives the keyboard back to the terminal.
        QTest::keyClick(child<QLineEdit>(a->findBar(), "findText"), Qt::Key_Escape);
        QVERIFY(!a->findBar()->isVisible());
        QTRY_VERIFY(a->view()->hasFocus());
        QVERIFY(!b->findBar()->isHidden());
    }
};

QTEST_MAIN(TstFind)
#include "tst_find.moc"
