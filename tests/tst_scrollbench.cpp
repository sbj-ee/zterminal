// Benchmark (docs/PLAN.md §4.16/§4.17): a tab with a full 100,000-line
// scrollback of colourful ~80-column output. Prints memory, scroll-repaint and
// Find timings ("BENCH ..." lines) and fails only on generous bounds, so slow
// CI machines pass while a regression to per-cell storage or O(n) repaints
// does not.
#include "AppSettings.hpp"
#include "FindBar.hpp"
#include "Terminal.hpp"
#include "TerminalSearch.hpp"
#include "TerminalView.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QTest>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <iterator>

using namespace zterminal;

namespace {

constexpr int kLines = 100000;

// Resident set size in bytes (Linux), 0 if unknown.
qint64 rssBytes()
{
    QFile f(QStringLiteral("/proc/self/statm"));
    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QList<QByteArray> parts = f.readAll().split(' ');
    return parts.size() > 1 ? parts[1].toLongLong() * 4096 : 0;
}

// Log-like output: timestamps, coloured levels, bold keys, varied lengths.
QByteArray logLine(int i, QRandomGenerator &rng)
{
    static const char *levels[] = {"\x1b[32mINFO \x1b[0m", "\x1b[33mWARN \x1b[0m", "\x1b[1;31mERROR\x1b[0m",
                                   "\x1b[36mDEBUG\x1b[0m"};
    static const char *words[] = {"interface", "GigabitEthernet0/1", "changed", "state", "to", "up", "down",
                                  "neighbor", "10.0.0.1", "BGP", "session", "established", "route", "flap",
                                  "\x1b[38;5;208mpeer\x1b[0m", "\x1b[38;2;80;160;240mvlan\x1b[0m", "ok"};
    QByteArray b = QByteArray::number(1700000000 + i) + " " + levels[rng.bounded(4)] + " \x1b[1mid=" + QByteArray::number(i)
        + "\x1b[0m";
    const int n = 4 + int(rng.bounded(9));
    for (int k = 0; k < n; ++k) {
        b += ' ';
        b += words[rng.bounded(int(std::size(words)))];
    }
    if (i % 97 == 0) {
        b += " needle";
    }
    return b + "\r\n";
}

void report(const char *what, double value, const char *unit)
{
    std::printf("BENCH %-38s %10.2f %s\n", what, value, unit);
    std::fflush(stdout);
}

} // namespace

class TstScrollBench : public QObject
{
    Q_OBJECT
private slots:
    void fullScrollback()
    {
        AppSettings{}.save();
        QWidget top;
        auto *col = new QVBoxLayout(&top);
        Terminal term(24, 80);
        term.setScrollbackLimit(AppSettings{}.scrollbackLines);
        QCOMPARE(term.scrollbackLimit(), 100000);
        auto *view = new TerminalView(&term, &top);
        auto *bar = new FindBar(&term, view, &top);
        col->addWidget(view, 1);
        col->addWidget(bar);
        const int frame = 2 * view->frameWidth();
        view->setFixedSize(view->sizeHint() + QSize(frame, frame)); // exactly 80x24
        top.show();
        QVERIFY(QTest::qWaitForWindowExposed(&top));
        QCOMPARE(term.cols(), 80);

        QRandomGenerator rng(42);
        const qint64 rssBefore = rssBytes();
        QElapsedTimer t;
        t.start();
        QByteArray batch;
        for (int i = 0; i < kLines + 1000; ++i) {
            batch += logLine(i, rng);
            if (batch.size() > 64 * 1024) {
                term.feed(batch);
                batch.clear();
            }
        }
        term.feed(batch);
        const double feedMs = double(t.elapsed());
        QCOMPARE(term.scrollbackLines(), kLines);
        const double histMB = double(term.scrollbackMemoryBytes()) / (1024.0 * 1024.0);
        const double rssMB = double(rssBytes() - rssBefore) / (1024.0 * 1024.0);
        report("feed 101k lines", feedMs, "ms");
        report("scrollback store (100k lines)", histMB, "MiB");
        report("process RSS growth", rssMB, "MiB");
        report("compressed chunks", term.history().compressedChunks(), "");
        report("(VTermScreenCell storage would be)", double(kLines) * 80 * sizeof(VTermScreenCell) / (1024.0 * 1024.0),
               "MiB");

        // Scroll + repaint at random offsets across the whole buffer.
        QImage img(view->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        auto paintAt = [&](int firstLine) {
            view->scrollToLine(firstLine);
            QPainter p(&img);
            view->viewport()->render(&p);
        };
        paintAt(0); // warm up
        constexpr int kJumps = 200;
        std::vector<double> jumps;
        for (int i = 0; i < kJumps; ++i) {
            QElapsedTimer one;
            one.start();
            paintAt(int(rng.bounded(kLines)));
            jumps.push_back(double(one.nsecsElapsed()) / 1e6);
        }
        std::sort(jumps.begin(), jumps.end());
        double sum = 0;
        for (double v : jumps) {
            sum += v;
        }
        report("random jump + repaint, mean", sum / kJumps, "ms");
        report("random jump + repaint, p95", jumps[std::size_t(kJumps * 95 / 100)], "ms");

        // Smooth scrolling: one page at a time through compressed history.
        view->scrollToLine(10000);
        QScrollBar *sb = view->verticalScrollBar();
        constexpr int kPages = 300;
        QElapsedTimer pages;
        pages.start();
        for (int i = 0; i < kPages; ++i) {
            sb->setValue(sb->value() + term.rows());
            QPainter p(&img);
            view->viewport()->render(&p);
        }
        const double pageMs = double(pages.nsecsElapsed()) / 1e6 / kPages;
        report("page down + repaint, mean", pageMs, "ms");

        // Find across 100k lines + screen.
        QElapsedTimer f;
        f.start();
        auto plain = TerminalSearch::findAll(term, QStringLiteral("needle"), {});
        const double findMs = double(f.elapsed());
        QVERIFY(plain.size() > 500);
        report("find 'needle' (plain)", findMs, "ms");
        std::printf("BENCH   matches: %zu\n", plain.size());
        FindOptions re;
        re.regex = true;
        f.restart();
        auto rx = TerminalSearch::findAll(term, QStringLiteral("id=\\d+7 "), re);
        const double regexMs = double(f.elapsed());
        QVERIFY(rx.size() > 1000);
        report("find regex 'id=\\d+7 '", regexMs, "ms");
        f.restart();
        auto many = TerminalSearch::findAll(term, QStringLiteral("e"), {});
        const double manyMs = double(f.elapsed());
        report("find 'e' (very many matches)", manyMs, "ms");
        std::printf("BENCH   matches: %zu\n", many.size());

        // Through the find bar, with highlighting and jumping.
        bar->open();
        bar->setText(QStringLiteral("needle"));
        bar->search();
        report("find bar search + highlight", double(bar->lastSearchMs()), "ms");
        QElapsedTimer nx;
        nx.start();
        for (int i = 0; i < 50; ++i) {
            bar->findNext();
            QPainter p(&img);
            view->viewport()->render(&p);
        }
        report("find next + repaint, mean", double(nx.nsecsElapsed()) / 1e6 / 50, "ms");

        // Generous bounds (debug builds, slow shared CI runners).
        QVERIFY2(histMB < 40.0, qPrintable(QString::number(histMB)));
        QVERIFY2(sum / kJumps < 60.0, qPrintable(QString::number(sum / kJumps)));
        QVERIFY2(pageMs < 60.0, qPrintable(QString::number(pageMs)));
        QVERIFY2(findMs < 2000.0, qPrintable(QString::number(findMs)));
        QVERIFY2(regexMs < 4000.0, qPrintable(QString::number(regexMs)));
    }
};

QTEST_MAIN(TstScrollBench)
#include "tst_scrollbench.moc"
