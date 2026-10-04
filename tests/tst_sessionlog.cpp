// Session logging core: the escape-stripping line model (LogTextFilter) and
// the log file (SessionLog): names, 0700/0600, collisions, timestamps, pauses.
#include "LogTextFilter.hpp"
#include "SessionLog.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>

using namespace zterminal;

namespace {
QStringList lines(const QByteArray &b)
{
    return LogTextFilter::filterAll(b);
}
QByteArray readAll(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
mode_t modeOf(const QString &p)
{
    struct stat st {};
    ::stat(QFile::encodeName(p).constData(), &st);
    return st.st_mode & 0777;
}
const QDateTime kWhen(QDate(2026, 10, 3), QTime(20, 44, 12, 345));
} // namespace

class TstSessionLog : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;

private slots:
    void stripsColoursAndControlSequences()
    {
        QCOMPARE(lines("\x1b[1;31mred\x1b[0m plain\r\n"), QStringList{QStringLiteral("red plain")});
        QCOMPARE(lines("\x1b]0;my title\x07prompt$ \r\n"), QStringList{QStringLiteral("prompt$")});
        QCOMPARE(lines("\x1b]8;;https://x.example\x1b\\link\x1b]8;;\x1b\\\n"), QStringList{QStringLiteral("link")});
        QCOMPARE(lines("a\x1bP1$r0m\x1b\\b\x1b_apc\x1b\\c\n"), QStringList{QStringLiteral("abc")});
        QCOMPARE(lines("\x1b(B\x1b)0\x1b=\x1b>\x1b" "7x\x1b" "8\x0e\x0f\x07y\n"), QStringList{QStringLiteral("xy")});
        QCOMPARE(lines("\x1b[?25l\x1b[?2004hz\x1b[?2004l\x1b[>c\n"), QStringList{QStringLiteral("z")});
        // C1 CSI (U+009B, UTF-8 encoded) and a CSI aborted by ESC.
        QCOMPARE(lines("\xc2\x9b" "31mq\x1b[12\x1b[0mr\n"), QStringList{QStringLiteral("qr")});
        // No ESC or other control character ever survives.
        QByteArray noise;
        for (int i = 0; i < 4096; ++i) {
            noise += char((i * 7919) % 256);
        }
        for (const QString &l : lines(noise)) {
            for (const QChar c : l) {
                QVERIFY2(c.unicode() >= 0x20 && c.unicode() != 0x7F && !(c.unicode() >= 0x80 && c.unicode() < 0xA0),
                         qPrintable(QString::number(c.unicode(), 16)));
            }
        }
    }

    void overwritesAndLineEditing()
    {
        // Progress meter: CR overwrites, the final state is logged once.
        QCOMPARE(lines("progress  10%\rprogress  55%\rprogress 100%\r\n"), QStringList{QStringLiteral("progress 100%")});
        // Shorter overwrite keeps the tail unless erased.
        QCOMPARE(lines("abcdef\rXY\n"), QStringList{QStringLiteral("XYcdef")});
        QCOMPARE(lines("abcdef\rXY\x1b[K\n"), QStringList{QStringLiteral("XY")});
        // Backspace erase as shells do it, and BS + overtype.
        QCOMPARE(lines("$ lss\b \b -l\r\n"), QStringList{QStringLiteral("$ ls -l")});
        QCOMPARE(lines("ab\bX\n"), QStringList{QStringLiteral("aX")});
        // Cursor left/right/column, delete and insert characters.
        QCOMPARE(lines("hello\x1b[3Dy\n"), QStringList{QStringLiteral("heylo")});
        QCOMPARE(lines("a\x1b[3Cb\n"), QStringList{QStringLiteral("a   b")});
        QCOMPARE(lines("abcdef\x1b[3G\x1b[2P\n"), QStringList{QStringLiteral("abef")});
        QCOMPARE(lines("abef\x1b[3G\x1b[2@cd\n"), QStringList{QStringLiteral("abcdef")});
        QCOMPARE(lines("abcdef\x1b[4G\x1b[1K\n"), QStringList{QStringLiteral("    ef")}); // erases columns 1-4
        QCOMPARE(lines("abc\x1b[2Kxyz\n"), QStringList{QStringLiteral("   xyz")});
        // Tabs, blank lines, trailing spaces, LF without CR, a prompt with no newline.
        QCOMPARE(lines("a\tb\n\nc   \nd"), (QStringList{QStringLiteral("a       b"), QString(), QStringLiteral("c"), QStringLiteral("d")}));
        // A jump to another row ends the line.
        QCOMPARE(lines("top\x1b[5;1Hbottom\n"), (QStringList{QStringLiteral("top"), QStringLiteral("bottom")}));
    }

    void autowrappedLines()
    {
        // readline at the right margin of a 10-column terminal: " \r" wraps,
        // then CR returns to the start of the *second* row.
        QCOMPARE(LogTextFilter::filterAll("$ echo abc \rdefgh\r\n", 10), QStringList{QStringLiteral("$ echo abcdefgh")});
        // BS can't move back across the wrap; CSI A goes up a row.
        QCOMPARE(LogTextFilter::filterAll("0123456789ab\r\x1b[AX\n", 10), QStringList{QStringLiteral("X123456789ab")});
        // Unknown width: CR goes to column 0.
        QCOMPARE(LogTextFilter::filterAll("0123456789ab\rX\n"), QStringList{QStringLiteral("X123456789ab")});
    }

    void alternateScreenIsNotLogged()
    {
        QCOMPARE(lines("$ vim x\r\n\x1b[?1049h\x1b[Hvim screen ~\r\n~\r\n\x1b[?1049l$ done\r\n"),
                 (QStringList{QStringLiteral("$ vim x"), QStringLiteral("$ done")}));
        QCOMPARE(lines("a\x1b[?47hhidden\n\x1b[?47lb\n"), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    }

    void utf8AcrossChunks()
    {
        QStringList out;
        LogTextFilter f([&out](const QString &l) { out << l; });
        const QByteArray s = QStringLiteral("Zürich → 東京 ✓\n").toUtf8();
        for (char c : s) {
            f.feed(QByteArray(1, c)); // one byte at a time
        }
        QCOMPARE(out, QStringList{QStringLiteral("Zürich → 東京 ✓")});
    }

    void sanitizedFileNames()
    {
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral("core-sw1")), QStringLiteral("core-sw1"));
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral("core sw/1 (lab)")), QStringLiteral("core_sw_1_lab"));
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral("../../etc/passwd")), QStringLiteral("etc_passwd"));
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral("-rf *")), QStringLiteral("rf"));
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral(".hidden")), QStringLiteral("hidden"));
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral("ssh admin@10.0.0.1")), QStringLiteral("ssh_admin_10.0.0.1"));
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral("Zürich\n\t$(rm)")), QStringLiteral("Z_rich_rm"));
        QCOMPARE(SessionLog::sanitizeName(QStringLiteral("///")), QStringLiteral("session"));
        QCOMPARE(SessionLog::sanitizeName(QString(300, QLatin1Char('a'))).size(), 64);
        QCOMPARE(SessionLog::fileNameFor(QStringLiteral("SG250 console"), kWhen), QStringLiteral("SG250_console-20261003-204412.log"));
        QCOMPARE(SessionLog::defaultDirectory(), QDir::homePath() + QStringLiteral("/zterminal-logs"));
    }

    void pathAndPermissions()
    {
        const QString dir = tmp.filePath(QStringLiteral("a/b/logs"));
        SessionLog log;
        log.setClockForTests([] { return kWhen; });
        QVERIFY2(log.start(dir, QStringLiteral("core sw/1"), false), qPrintable(log.errorString()));
        QCOMPARE(log.path(), dir + QStringLiteral("/core_sw_1-20261003-204412.log"));
        QCOMPARE(modeOf(dir), mode_t(0700));
        QCOMPARE(modeOf(tmp.filePath(QStringLiteral("a/b"))), mode_t(0700)); // every directory we created
        QCOMPARE(modeOf(log.path()), mode_t(0600));
        // Same second again: a new file, never appended or overwritten.
        SessionLog second;
        second.setClockForTests([] { return kWhen; });
        QVERIFY(second.start(dir, QStringLiteral("core sw/1"), false));
        QCOMPARE(second.path(), dir + QStringLiteral("/core_sw_1-20261003-204412-2.log"));
        // A symlink planted at the name is not followed.
        QVERIFY(QFile::link(tmp.filePath(QStringLiteral("target")), dir + QStringLiteral("/evil-20261003-204412.log")));
        SessionLog third;
        third.setClockForTests([] { return kWhen; });
        QVERIFY(third.start(dir, QStringLiteral("evil"), false));
        QCOMPARE(third.path(), dir + QStringLiteral("/evil-20261003-204412-2.log"));
        QVERIFY(!QFileInfo::exists(tmp.filePath(QStringLiteral("target"))));
        // Unwritable location: a clear error.
        SessionLog bad;
        QVERIFY(!bad.start(QStringLiteral("/proc/zterminal-logs"), QStringLiteral("x"), false));
        QVERIFY(!bad.errorString().isEmpty());
        QVERIFY(!bad.isActive());
    }

    void contentAndTimestamps()
    {
        int tick = 0;
        SessionLog log;
        log.setClockForTests([&tick] { return kWhen.addSecs(tick++); });
        QVERIFY(log.start(tmp.filePath(QStringLiteral("ts")), QStringLiteral("lab"), true));
        log.feed("\x1b[32mline one\x1b[0m\r\nline ");
        log.feed("two\r\n$ ");
        log.stop();
        const QStringList got = QString::fromUtf8(readAll(log.path())).split(QLatin1Char('\n'));
        QCOMPARE(got.value(0), QStringLiteral("=== zterminal " ZTERMINAL_EXPECTED_VERSION " log of \"lab\" started 2026-10-03T20:44:12.345 ==="));
        QCOMPARE(got.value(1), QStringLiteral("2026-10-03T20:44:13.345 line one"));
        QCOMPARE(got.value(2), QStringLiteral("2026-10-03T20:44:14.345 line two"));
        QCOMPARE(got.value(3), QStringLiteral("2026-10-03T20:44:15.345 $"));
        QVERIFY(got.value(4).startsWith(QStringLiteral("=== log stopped 2026-10-03T20:44:16.345")));
        QVERIFY(!readAll(log.path()).contains('\x1b'));

        SessionLog plain;
        QVERIFY(plain.start(tmp.filePath(QStringLiteral("plain")), QStringLiteral("lab"), false));
        plain.feed("no stamp\n");
        plain.stop();
        QVERIFY(readAll(plain.path()).contains("\nno stamp\n"));
        // An empty line gets just the stamp (no trailing space).
        SessionLog blank;
        blank.setClockForTests([] { return kWhen; });
        QVERIFY(blank.start(tmp.filePath(QStringLiteral("blank")), QStringLiteral("b"), true));
        blank.feed("\r\n");
        blank.stop();
        QVERIFY(readAll(blank.path()).contains("\n2026-10-03T20:44:12.345\n"));
    }

    void pausesDropOutputAndLeaveMarkers()
    {
        SessionLog log;
        QVERIFY(log.start(tmp.filePath(QStringLiteral("pause")), QStringLiteral("p"), false));
        log.feed("visible 1\r\nPassword: ");
        log.suspend(QStringLiteral("vault dialog open"));
        log.suspend(QStringLiteral("Send Stored Login")); // nested
        log.suspend(QStringLiteral("vault dialog open")); // repeated: still one entry
        log.feed("SECRET-1\r\n");
        log.resume(QStringLiteral("vault dialog open"));
        log.feed("SECRET-2\r\n");
        QVERIFY(log.isSuspended());
        log.resume(QStringLiteral("Send Stored Login"));
        log.resume(QStringLiteral("not paused for this")); // harmless
        log.feed("visible 2\r\n");
        log.stop();
        const QByteArray text = readAll(log.path());
        QVERIFY(!text.contains("SECRET"));
        QVERIFY(text.contains("visible 1\nPassword:\n[zterminal: logging paused: vault dialog open]\n[zterminal: logging resumed]\nvisible 2\n"));
        QCOMPARE(text.count("logging paused"), 1);
    }
};

QTEST_GUILESS_MAIN(TstSessionLog)
#include "tst_sessionlog.moc"
