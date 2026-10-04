// Selection model: char/word/line selection, text extraction across
// scrollback, select-all and history trimming.
#include "MouseSettings.hpp"
#include "Selection.hpp"
#include "Terminal.hpp"

#include <QTest>

using namespace zterminal;

class TstSelection : public QObject
{
    Q_OBJECT
    const QString delims = MouseSettings().wordDelimiters;

private slots:
    void charSelectionSingleLine()
    {
        Terminal t(5, 30);
        t.feed("hello brave world");
        Selection s;
        s.begin({0, 6}, Selection::Mode::Char, t, delims);
        QVERIFY(s.isEmpty()); // a click alone selects nothing
        s.extend({0, 11}, t, delims);
        QCOMPARE(s.text(t), QStringLiteral("brave"));
        QVERIFY(s.contains(0, 6));
        QVERIFY(!s.contains(0, 11));
    }

    void charSelectionBackwardsAndMultiLine()
    {
        Terminal t(5, 30);
        t.feed("first line   \r\nsecond line\r\nthird");
        Selection s;
        s.begin({2, 3}, Selection::Mode::Char, t, delims);
        s.extend({0, 6}, t, delims); // drag upwards
        QCOMPARE(s.text(t), QStringLiteral("line\nsecond line\nthi"));
    }

    void doubleClickWord()
    {
        Terminal t(5, 60);
        t.feed("ssh admin@10.0.0.1 -p 22 /etc/ssh/sshd_config");
        Selection s;
        s.begin({0, 8}, Selection::Mode::Word, t, delims);
        QCOMPARE(s.text(t), QStringLiteral("admin@10.0.0.1"));
        s.begin({0, 30}, Selection::Mode::Word, t, delims);
        QCOMPARE(s.text(t), QStringLiteral("/etc/ssh/sshd_config"));
        s.begin({0, 1}, Selection::Mode::Word, t, delims);
        s.extend({0, 15}, t, delims); // drag extends by words
        QCOMPARE(s.text(t), QStringLiteral("ssh admin@10.0.0.1"));
    }

    void wordStopsAtDelimiters()
    {
        Terminal t(5, 40);
        t.feed("echo \"quoted\" (paren)");
        Selection s;
        s.begin({0, 8}, Selection::Mode::Word, t, delims);
        QCOMPARE(s.text(t), QStringLiteral("quoted"));
        s.begin({0, 16}, Selection::Mode::Word, t, delims);
        QCOMPARE(s.text(t), QStringLiteral("paren"));
    }

    void tripleClickLine()
    {
        Terminal t(5, 30);
        t.feed("one\r\n  two words  \r\nthree");
        Selection s;
        s.begin({1, 5}, Selection::Mode::Line, t, delims);
        QCOMPARE(s.text(t), QStringLiteral("  two words"));
        s.extend({2, 0}, t, delims);
        QCOMPARE(s.text(t), QStringLiteral("  two words\nthree"));
    }

    void acrossScrollback()
    {
        Terminal t(3, 20);
        for (int i = 0; i < 8; ++i) {
            t.feed(QStringLiteral("row%1\r\n").arg(i).toUtf8());
        }
        QVERIFY(t.scrollbackLines() >= 5);
        Selection s;
        s.begin({1, 0}, Selection::Mode::Line, t, delims);
        s.extend({t.scrollbackLines() + 1, 0}, t, delims);
        const QStringList lines = s.text(t).split(QLatin1Char('\n'));
        QCOMPARE(lines.first(), QStringLiteral("row1"));
        QCOMPARE(lines.last(), QStringLiteral("row7"));
    }

    void selectAllAndClear()
    {
        Terminal t(3, 20);
        t.feed("a\r\nb\r\nc");
        Selection s;
        s.selectAll(t);
        QCOMPARE(s.text(t), QStringLiteral("a\nb\nc"));
        s.clear();
        QVERIFY(s.isEmpty());
        QVERIFY(s.text(t).isEmpty());
    }

    void linesDroppedShiftsOrClears()
    {
        Terminal t(3, 20);
        Selection s;
        s.begin({5, 0}, Selection::Mode::Line, t, delims);
        s.linesDropped(2);
        QCOMPARE(s.start().line, 3);
        s.linesDropped(10);
        QVERIFY(s.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TstSelection)
#include "tst_selection.moc"
