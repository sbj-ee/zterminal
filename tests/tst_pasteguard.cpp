// Safe copy and paste, core parts: trailing-whitespace trimming of copied
// text, paste analysis (line count, preview, control characters, paced send
// time) and the bracketed-paste probe.
#include "MouseSettings.hpp"
#include "PasteGuard.hpp"
#include "Selection.hpp"
#include "Terminal.hpp"

#include <QSignalSpy>
#include <QTest>

using namespace zterminal;

class TstPasteGuard : public QObject
{
    Q_OBJECT
    const QString delims = MouseSettings().wordDelimiters;

private slots:
    void copyTrimsTrailingWhitespace()
    {
        Terminal t(5, 30);
        // Printed trailing spaces and a tab, then erased cells after "c".
        t.feed("a b   \r\n\tx\t\r\nc\x1b[K\r\nend  ");
        Selection s;
        s.selectAll(t);
        QCOMPARE(s.text(t).split(QLatin1Char('\n')).mid(0, 4),
                 (QStringList{QStringLiteral("a b"), QStringLiteral("        x"), QStringLiteral("c"), QStringLiteral("end")}));
        // Off: spaces the program printed stay; never-written cells don't.
        QCOMPARE(s.text(t, false).split(QLatin1Char('\n')).mid(0, 4),
                 (QStringList{QStringLiteral("a b   "), QStringLiteral("        x"), QStringLiteral("c"),
                              QStringLiteral("end  ")}));
    }

    void partialLastLineIsTrimmedToo()
    {
        Terminal t(5, 30);
        t.feed("one   \r\ntwo   three");
        Selection s;
        s.begin({0, 0}, Selection::Mode::Char, t, delims);
        s.extend({1, 6}, t, delims); // "two   "
        QCOMPARE(s.text(t), QStringLiteral("one\ntwo"));
        QCOMPARE(s.text(t, false), QStringLiteral("one   \ntwo   "));
    }

    void analyzeCountsLines()
    {
        PasteInfo one = PasteInfo::analyze(QStringLiteral("show vlan"));
        QCOMPARE(one.lines, 1);
        QVERIFY(!one.needsConfirm());

        PasteInfo enter = PasteInfo::analyze(QStringLiteral("reload\n"));
        QCOMPARE(enter.lines, 1);
        QVERIFY(enter.endsWithNewline);
        QVERIFY(enter.needsConfirm()); // would run on paste

        PasteInfo three = PasteInfo::analyze(QStringLiteral("conf t\r\nhostname sw1\rend"));
        QCOMPARE(three.lines, 3);
        QVERIFY(!three.endsWithNewline);
        QVERIFY(three.needsConfirm());
        QCOMPARE(three.preview, QStringLiteral("conf t\nhostname sw1\nend"));
        QCOMPARE(three.bytes, qsizetype(23)); // CRLF -> CR

        QVERIFY(!PasteInfo::analyze(QString()).needsConfirm());
    }

    void previewIsLimitedAndShowsControls()
    {
        QString big;
        for (int i = 0; i < 50; ++i) {
            big += QStringLiteral("line %1\n").arg(i);
        }
        PasteInfo info = PasteInfo::analyze(big, 12);
        QCOMPARE(info.lines, 50);
        QCOMPARE(info.previewLines, 12);
        QVERIFY(info.preview.endsWith(QStringLiteral("line 11")));

        PasteInfo ctl = PasteInfo::analyze(QStringLiteral("echo hi\x1b[2K\x07\n"));
        QCOMPARE(ctl.controlChars, 2);
        QCOMPARE(ctl.preview, QStringLiteral("echo hi\u241b[2K\u2407"));

        PasteInfo longLine = PasteInfo::analyze(QString(500, QLatin1Char('x')) + QStringLiteral("\nb"), 12, 100);
        QCOMPARE(longLine.preview.split(QLatin1Char('\n')).front().size(), 101); // 100 + "…"
    }

    void pacedTime()
    {
        QCOMPARE(PasteInfo::pacedMilliseconds("ab\rcd\r", 10, 100), qint64(10 + 10 + 100 + 10 + 10 + 100));
        QCOMPARE(PasteInfo::pacedMilliseconds("ab\rcd\r", 0, 250), qint64(500));
        QCOMPARE(PasteInfo::pacedMilliseconds("ab", 0, 0), qint64(0));
    }

    void bracketedPasteProbeSendsNothing()
    {
        Terminal t(5, 30);
        QSignalSpy out(&t, &Terminal::output);
        QVERIFY(!t.bracketedPasteEnabled());
        t.feed("\x1b[?2004h");
        QVERIFY(t.bracketedPasteEnabled());
        QCOMPARE(out.count(), 0);
        t.feed("\x1b[?2004l");
        QVERIFY(!t.bracketedPasteEnabled());
        // Paste still wraps only when on.
        t.feed("\x1b[?2004h");
        t.paste(QStringLiteral("a\nb"));
        QByteArray all;
        for (const auto &a : out) {
            all += a.at(0).toByteArray();
        }
        QCOMPARE(all, QByteArray("\x1b[200~a\rb\x1b[201~"));
    }
};

QTEST_GUILESS_MAIN(TstPasteGuard)
#include "tst_pasteguard.moc"
