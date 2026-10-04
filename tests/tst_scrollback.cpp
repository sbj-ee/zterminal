// Compact, chunked, compressed scrollback (docs/PLAN.md §4.16).
#include "Scrollback.hpp"
#include "Terminal.hpp"

#include <QSignalSpy>
#include <QTest>

#include <cstring>

using namespace zterminal;

namespace {

bool sameCell(const Cell &a, const Cell &b)
{
    return a.text == b.text && a.width == b.width && a.fg == b.fg && a.bg == b.bg && a.defaultBg == b.defaultBg
        && a.bold == b.bold && a.italic == b.italic && a.underline == b.underline && a.strike == b.strike
        && a.conceal == b.conceal;
}

VTermScreenCell blankCell()
{
    VTermScreenCell c;
    std::memset(&c, 0, sizeof c);
    c.width = 1;
    vterm_color_indexed(&c.fg, 7);
    vterm_color_indexed(&c.bg, 0);
    c.fg.type |= VTERM_COLOR_DEFAULT_FG;
    c.bg.type |= VTERM_COLOR_DEFAULT_BG;
    return c;
}

bool sameRaw(const VTermScreenCell &a, const VTermScreenCell &b)
{
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL; ++i) {
        if (a.chars[i] != b.chars[i]) {
            return false;
        }
        if (!a.chars[i]) {
            break;
        }
    }
    return a.width == b.width && std::memcmp(&a.attrs, &b.attrs, sizeof a.attrs) == 0
        && std::memcmp(&a.fg, &b.fg, sizeof a.fg) == 0 && std::memcmp(&a.bg, &b.bg, sizeof a.bg) == 0;
}

QByteArray numberedLines(int from, int to)
{
    QByteArray b;
    for (int i = from; i < to; ++i) {
        b += "\x1b[3" + QByteArray::number(i % 8) + "mline " + QByteArray::number(i) + " \x1b[1mbold\x1b[0m tail\r\n";
    }
    return b;
}

} // namespace

class TstScrollback : public QObject
{
    Q_OBJECT
private slots:
    void encodeDecodeRoundTrip()
    {
        std::vector<VTermScreenCell> in(12, blankCell());
        in[0].chars[0] = 'A';
        in[1].chars[0] = ' '; // a printed space is not a blank
        in[2].chars[0] = 0x4E2D; // wide CJK
        in[2].width = 2;
        in[3].chars[0] = static_cast<uint32_t>(-1); // libvterm's wide right half
        in[3].width = 1;
        in[4].chars[0] = 'e';
        in[4].chars[1] = 0x0301; // combining acute
        in[5].chars[0] = 0x1F600; // astral
        in[6].chars[0] = 'x';
        in[6].attrs.bold = 1;
        in[6].attrs.underline = 2;
        in[6].attrs.italic = 1;
        in[6].attrs.reverse = 1;
        vterm_color_rgb(&in[6].fg, 1, 2, 3);
        vterm_color_indexed(&in[6].bg, 200);
        in[7].chars[0] = 'y';
        in[7].attrs.strike = 1;
        in[7].attrs.conceal = 1;
        // 8..11 blank, default style: not stored.
        const QByteArray enc = Scrollback::encode(in.data(), int(in.size()));
        QVERIFY(enc.size() < 120);
        std::vector<VTermScreenCell> out;
        Scrollback::decode(enc.constData(), enc.size(), &out);
        QCOMPARE(out.size(), in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            QVERIFY2(sameRaw(in[i], out[i]), qPrintable(QStringLiteral("cell %1").arg(i)));
        }
        std::vector<int> cols;
        const QString text = Scrollback::decodeText(enc.constData(), enc.size(), &cols);
        QCOMPARE(text, QString::fromUtf8("A \u4E2De\u0301\U0001F600xy"));
        QCOMPARE(cols, (std::vector<int>{0, 1, 2, 4, 4, 5, 5, 6, 7}));
    }

    void plainLineIsSmall()
    {
        std::vector<VTermScreenCell> in(200, blankCell());
        for (int i = 0; i < 80; ++i) {
            in[std::size_t(i)].chars[0] = uint32_t('a' + i % 26);
        }
        const QByteArray enc = Scrollback::encode(in.data(), int(in.size()));
        // header + one run + one byte per cell, trailing blanks dropped
        QCOMPARE(enc.size(), qsizetype(6 + 14 + 80));
    }

    void historyMatchesScreen()
    {
        // What a line looked like on screen is exactly what history returns.
        Terminal t(4, 40);
        t.feed("\x1b[1;31mred\x1b[0m \x1b[48;2;10;20;30mtrue\x1b[0m \x1b[4;3mul\x1b[0m \xe4\xb8\xad\xe6\x96\x87 e\xcc\x81 \x1b[7mrev\x1b[0m");
        std::vector<Cell> before;
        for (int c = 0; c < 40; ++c) {
            before.push_back(t.cell(0, c));
        }
        const QString textBefore = t.lineText(0);
        t.feed("\r\n\r\n\r\n\r\n");
        QCOMPARE(t.scrollbackLines(), 1);
        for (int c = 0; c < 40; ++c) {
            QVERIFY2(sameCell(before[std::size_t(c)], t.cell(0, c)), qPrintable(QStringLiteral("col %1").arg(c)));
        }
        QCOMPARE(t.lineText(0), textBefore);
    }

    void chunkingAndCompression()
    {
        Terminal t(24, 80);
        t.setScrollbackLimit(Terminal::kUnlimitedScrollback);
        t.feed(numberedLines(0, 5000));
        QCOMPARE(t.scrollbackLines(), 5000 - 23);
        QVERIFY(t.history().compressedChunks() >= 5000 / Scrollback::kLinesPerChunk - Scrollback::kRawChunks - 1);
        // Random access into compressed chunks, newest and oldest.
        for (int i : {0, 1, 511, 512, 513, 2000, 4000, t.scrollbackLines() - 1}) {
            QCOMPARE(t.lineText(i), QStringLiteral("line %1 bold tail").arg(i));
            QCOMPARE(t.cell(i, 0).fg, t.colorScheme().ansi[i % 8]);
            QVERIFY(t.cell(i, QStringLiteral("line %1 ").arg(i).size()).bold);
        }
        // Far below VTermScreenCell storage (~40 bytes per cell = 16 MB here),
        // including the raw newest chunks and the decompressed read cache.
        QVERIFY2(t.scrollbackMemoryBytes() < 5000 * 200, qPrintable(QString::number(t.scrollbackMemoryBytes())));
    }

    void limitDropsOldestAcrossChunks()
    {
        Terminal t(10, 60);
        t.setScrollbackLimit(1000);
        QSignalSpy spy(&t, &Terminal::scrolledIntoHistory);
        t.feed(numberedLines(0, 3000));
        QCOMPARE(t.scrollbackLines(), 1000);
        int dropped = 0;
        for (const auto &args : spy) {
            dropped += args.at(1).toInt();
        }
        QCOMPARE(dropped, 3000 - 9 - 1000);
        // Oldest kept line is the one right after the dropped ones.
        QCOMPARE(t.lineText(0), QStringLiteral("line %1 bold tail").arg(dropped));
        QCOMPARE(t.lineText(999), QStringLiteral("line %1 bold tail").arg(dropped + 999));
        // Lowering the limit trims immediately.
        t.setScrollbackLimit(100);
        QCOMPARE(t.scrollbackLines(), 100);
        QCOMPARE(t.lineText(99), QStringLiteral("line %1 bold tail").arg(dropped + 999));
        t.setScrollbackLimit(0);
        QCOMPARE(t.scrollbackLines(), 0);
        t.feed("more\r\n");
        QCOMPARE(t.scrollbackLines(), 0);
    }

    void unlimitedKeepsEverything()
    {
        Terminal t(5, 40);
        t.setScrollbackLimit(Terminal::kUnlimitedScrollback);
        QCOMPARE(t.scrollbackLimit(), Terminal::kUnlimitedScrollback);
        t.feed(numberedLines(0, 20000));
        QCOMPARE(t.scrollbackLines(), 20000 - 4);
        QCOMPARE(t.lineText(0), QStringLiteral("line 0 bold tail"));
    }

    void popBackOnResizeAcrossChunks()
    {
        Terminal t(10, 40);
        t.feed(numberedLines(0, 600)); // > one chunk
        const int sb = t.scrollbackLines();
        QCOMPARE(sb, 600 - 9);
        // Taller window: libvterm pulls lines back from history.
        t.resize(200, 40);
        QCOMPARE(t.scrollbackLines(), sb - 190);
        QCOMPARE(t.lineText(t.scrollbackLines() - 1), QStringLiteral("line %1 bold tail").arg(sb - 191));
        QCOMPARE(t.lineText(t.scrollbackLines()), QStringLiteral("line %1 bold tail").arg(sb - 190));
        QCOMPARE(t.cell(t.scrollbackLines(), 0).fg, t.colorScheme().ansi[(sb - 190) % 8]);
        t.resize(10, 40);
        QCOMPARE(t.scrollbackLines(), sb);
        QCOMPARE(t.lineText(sb - 1), QStringLiteral("line %1 bold tail").arg(sb - 1));
    }

    void clearFreesMemory()
    {
        Terminal t(5, 40);
        t.feed(numberedLines(0, 3000));
        QVERIFY(t.scrollbackMemoryBytes() > 10000);
        t.clearScrollback();
        QCOMPARE(t.scrollbackLines(), 0);
        QVERIFY(t.scrollbackMemoryBytes() < 4096);
        t.feed(numberedLines(0, 10)); // the 4 lines left on screen scroll up first
        QCOMPARE(t.scrollbackLines(), 10);
        QCOMPARE(t.lineText(0), QStringLiteral("line 2996 bold tail"));
        QCOMPARE(t.lineText(4), QStringLiteral("line 0 bold tail"));
    }

    void searchTextColumns()
    {
        Terminal t(3, 20);
        t.feed("a\xe4\xb8\xad" "b  c\r\n\r\n\r\n");
        std::vector<int> cols;
        QCOMPARE(t.searchText(0, &cols), QString::fromUtf8("a\u4E2Db  c"));
        QCOMPARE(cols, (std::vector<int>{0, 1, 3, 4, 5, 6}));
        t.feed("a\xe4\xb8\xad" "b");
        cols.clear();
        const int screenLine = t.scrollbackLines() + t.cursorPos().y();
        QCOMPARE(t.searchText(screenLine, &cols), QString::fromUtf8("a\u4E2Db"));
        QCOMPARE(cols, (std::vector<int>{0, 1, 3}));
    }
};

QTEST_GUILESS_MAIN(TstScrollback)
#include "tst_scrollback.moc"
