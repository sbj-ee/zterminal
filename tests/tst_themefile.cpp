// Custom themes (*.ztheme.json, shared with zmail): JSON round trip,
// validation, derivation of missing roles / terminal colours, cross-app
// files (tests/data/themes, identical in sbj-ee/zmail), and the Theme Editor.
#include "AppSettings.hpp"
#include "BrandThemes.h"
#include "ColorScheme.hpp"
#include "MainWindow.hpp"
#include "PreferencesDialog.hpp"
#include "Terminal.hpp"
#include "ThemeEditorDialog.hpp"
#include "ThemeFile.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QComboBox>
#include <functional>
#include <QToolButton>
#include <QPushButton>
#include <QColorDialog>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <vterm.h>

using namespace zterminal;
using namespace sbj::theme;
using TermColors = sbj::theme::Terminal;

namespace {
QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QString fixture(const char *name)
{
    return QStringLiteral(ZTERMINAL_TEST_DATA "/themes/") + QLatin1String(name);
}
void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}
// The theme zterminal's editor makes in tests/data/themes/zterminal-made.ztheme.json:
// Badgers duplicated, renamed, one ANSI colour, cursor style and font changed.
Theme zterminalMade()
{
    Theme t = ColorScheme::byId(QStringLiteral("badgers")).toThemeFile();
    t.name = QStringLiteral("Badgers Night");
    t.basedOn = QStringLiteral("badgers");
    t.terminal->ansi[4] = 0x7AB0F0;
    t.terminal->cursorShape = QStringLiteral("block");
    t.terminal->cursorBlink = false;
    t.fonts.terminal = QStringLiteral("DejaVu Sans Mono");
    t.fonts.terminalSize = 12;
    return t;
}
void checkReadableTerminal(const TermColors &t, std::uint32_t bg, const char *what)
{
    for (int i = 1; i < 16; ++i) {
        QVERIFY2(contrast(t.ansi[size_t(i)], bg) >= 4.5, qPrintable(QStringLiteral("%1 ansi %2").arg(QLatin1String(what)).arg(i)));
        for (int j = 0; j < i; ++j) {
            QVERIFY2(t.ansi[size_t(i)] != t.ansi[size_t(j)], what);
        }
    }
}
} // namespace

class TstThemeFile : public QObject
{
    Q_OBJECT
    QTemporaryDir m_config;

private slots:
    void initTestCase()
    {
        QVERIFY(m_config.isValid());
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(m_config.path()));
        if (qEnvironmentVariableIsSet("ZTHEME_WRITE_FIXTURES")) { // maintainers: regenerate the fixture of this app
            writeFile(fixture("zterminal-made.ztheme.json"), serialize(zterminalMade()));
        }
    }

    void roundTrip()
    {
        for (const sbj::brand::Theme &b : sbj::brand::kThemes) {
            const Theme t = fromBrand(b);
            QString err;
            const std::optional<Theme> back = parse(serialize(t), &err);
            QVERIFY2(back, qPrintable(err));
            QVERIFY(*back == t);
            QCOMPARE(serialize(*back), serialize(t));
        }
        Theme full = zterminalMade();
        full.fonts.ui = QStringLiteral("Noto Sans");
        full.fonts.uiSize = 10;
        full.ui.rowStripes = 30;
        const std::optional<Theme> back = parse(serialize(full));
        QVERIFY(back && *back == full);
        // Files on disk too.
        const QString path = m_config.filePath(QStringLiteral("rt") + QLatin1String(kSuffix));
        QVERIFY(save(full, path));
        QVERIFY(load(path) == full);
    }

    void zterminalWritesItsFixture()
    {
        // The cross-app fixture is exactly what this code writes...
        QCOMPARE(serialize(zterminalMade()), readFile(fixture("zterminal-made.ztheme.json")));
        // ...and zterminal reads it back with its cursor style and font.
        const std::optional<Theme> t = load(fixture("zterminal-made.ztheme.json"));
        QVERIFY(t);
        const ColorScheme s = ColorScheme::fromThemeFile(*t, QStringLiteral("custom:x"));
        QCOMPARE(s.ansi[4], qRgb(0x7A, 0xB0, 0xF0));
        QCOMPARE(s.cursorShape, int(VTERM_PROP_CURSORSHAPE_BLOCK));
        QCOMPARE(s.cursorBlink, 0);
    }

    void readsAThemeMadeByZmail()
    {
        // Written by zmail's editor code (zmail tst_themefile checks that):
        // palette + fonts.ui + ui.rowStripes, no terminal block.
        const std::optional<Theme> t = load(fixture("zmail-made.ztheme.json"));
        QVERIFY(t);
        QVERIFY(!t->terminal);
        QVERIFY(!t->fonts.ui.isEmpty());
        QVERIFY(t->ui.rowStripes.has_value());
        const ColorScheme s = ColorScheme::fromThemeFile(*t, QStringLiteral("zmail:x"));
        QCOMPARE(s.background & 0xFFFFFF, t->roles[Background]);
        QCOMPARE(s.foreground & 0xFFFFFF, t->roles[Foreground]);
        // zterminal derives the ANSI colours, cursor and selection from the roles.
        const TermColors d = deriveTerminal(t->roles);
        for (size_t i = 0; i < 16; ++i) {
            QCOMPARE(s.ansi[i] & 0xFFFFFF, d.ansi[i]);
        }
        QCOMPARE(s.cursor & 0xFFFFFF, t->roles[Accent]);
        QCOMPARE(s.selectionBackground & 0xFFFFFF, t->roles[Selection]);
        QCOMPARE(s.cursorShape, -1);
        checkReadableTerminal(d, t->roles[Background], "zmail-made");
    }

    void missingRolesAndTerminalAreDerived_data()
    {
        QTest::addColumn<QString>("bg");
        QTest::addColumn<QString>("fg");
        QTest::newRow("dark") << "#1E1E2E" << "#CDD6F4";
        QTest::newRow("light") << "#FFFFFF" << "#202020";
        QTest::newRow("sepia") << "#F4ECD8" << "#5B4636";
        QTest::newRow("green") << "#203731" << "#FFB612";
        QTest::newRow("black") << "#000000" << "#FFFFFF";
    }
    void missingRolesAndTerminalAreDerived()
    {
        QFETCH(QString, bg);
        QFETCH(QString, fg);
        const QByteArray json = QStringLiteral("{\"format\":\"ztheme\",\"version\":1,\"name\":\"Minimal\","
                                           "\"palette\":{\"background\":\"%1\",\"foreground\":\"%2\"}}").arg(bg, fg).toUtf8();
        QString err;
        const std::optional<Theme> t = parse(json, &err);
        QVERIFY2(t, qPrintable(err));
        const Roles &r = t->roles;
        QCOMPARE(hex(r[Background]), bg);
        QVERIFY(contrast(r[Muted], r[Background]) >= 4.5);
        QVERIFY(contrast(r[Link], r[Background]) >= 4.5);
        QVERIFY(contrast(r[SelectionText], r[Selection]) >= 4.5);
        QVERIFY(contrast(r[ChromeText], r[Chrome]) >= 4.5);
        QVERIFY(contrast(r[HeaderText], r[Header]) >= 4.5);
        checkReadableTerminal(terminalOf(*t), r[Background], qPrintable(bg));
        // A partial terminal block keeps what it has and derives the rest.
        const std::optional<Theme> p = parse(QStringLiteral("{\"format\":\"ztheme\",\"version\":1,\"name\":\"P\","
                                                            "\"palette\":{\"background\":\"%1\",\"foreground\":\"%2\"},\"terminal\":{\"cursor\":\"#FF0000\"}}").arg(bg, fg).toUtf8());
        QVERIFY(p && p->terminal);
        QCOMPARE(p->terminal->cursor, 0xFF0000u);
        QVERIFY(p->terminal->ansi == deriveTerminal(p->roles).ansi);
    }

    void tolerantWhereItShouldBe()
    {
        const std::optional<Theme> t = parse("{\"format\":\"ztheme\",\"version\":1,\"name\":\"  Short hex  \",\"future\":{\"x\":1},"
                                             "\"palette\":{\"background\":\"#000\",\"foreground\":\"#fFf\",\"sparkle\":\"#123456\"}}");
        QVERIFY(t);
        QCOMPARE(t->name, QStringLiteral("Short hex"));
        QCOMPARE(t->roles[Background], 0x000000u);
        QCOMPARE(t->roles[Foreground], 0xFFFFFFu);
        QCOMPARE(fileStem(QStringLiteral("  Boiler--Up!  2026 ")), QStringLiteral("boiler-up-2026"));
        QCOMPARE(fileStem(QStringLiteral("!!!")), QStringLiteral("theme"));
    }

    void badFilesAreRejected_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("why");
        const QByteArray ok = "\"format\":\"ztheme\",\"version\":1,\"name\":\"X\"";
        const QByteArray pal = "\"palette\":{\"background\":\"#000000\",\"foreground\":\"#FFFFFF\"}";
        QTest::newRow("not json") << QByteArray("{nope") << "JSON";
        QTest::newRow("array") << QByteArray("[1,2]") << "object";
        QTest::newRow("format") << QByteArray("{\"format\":\"zthemes\",\"version\":1,\"name\":\"X\"," + pal + "}") << "format";
        QTest::newRow("no version") << QByteArray("{\"format\":\"ztheme\",\"name\":\"X\"," + pal + "}") << "version";
        QTest::newRow("version string") << QByteArray("{\"format\":\"ztheme\",\"version\":\"1\",\"name\":\"X\"," + pal + "}") << "version";
        QTest::newRow("newer version") << QByteArray("{\"format\":\"ztheme\",\"version\":2,\"name\":\"X\"," + pal + "}") << "newer";
        QTest::newRow("no name") << QByteArray("{\"format\":\"ztheme\",\"version\":1," + pal + "}") << "name";
        QTest::newRow("long name") << QByteArray("{\"format\":\"ztheme\",\"version\":1,\"name\":\"" + QByteArray(65, 'a') + "\"," + pal + "}") << "name";
        QTest::newRow("no palette") << QByteArray("{" + ok + "}") << "palette";
        QTest::newRow("no foreground") << QByteArray("{" + ok + ",\"palette\":{\"background\":\"#000000\"}}") << "foreground";
        QTest::newRow("colour name") << QByteArray("{" + ok + ",\"palette\":{\"background\":\"black\",\"foreground\":\"#FFFFFF\"}}") << "palette.background";
        QTest::newRow("short colour") << QByteArray("{" + ok + ",\"palette\":{\"background\":\"#00000\",\"foreground\":\"#FFFFFF\"}}") << "palette.background";
        QTest::newRow("15 ansi") << QByteArray("{" + ok + "," + pal + ",\"terminal\":{\"ansi\":[\"#000000\",\"#000001\",\"#000002\",\"#000003\",\"#000004\",\"#000005\",\"#000006\",\"#000007\",\"#000008\",\"#000009\",\"#00000A\",\"#00000B\",\"#00000C\",\"#00000D\",\"#00000E\"]}}") << "16";
        QTest::newRow("bad ansi") << QByteArray("{" + ok + "," + pal + ",\"terminal\":{\"ansi\":[\"#000000\",\"#000001\",\"#000002\",\"#000003\",\"#000004\",\"#000005\",\"#000006\",\"#000007\",\"#000008\",\"#000009\",\"#00000A\",\"#00000B\",\"#00000C\",\"#00000D\",\"#00000E\",7]}}") << "ansi[15]";
        QTest::newRow("cursor shape") << QByteArray("{" + ok + "," + pal + ",\"terminal\":{\"cursorShape\":\"beam\"}}") << "cursorShape";
        QTest::newRow("cursor blink") << QByteArray("{" + ok + "," + pal + ",\"terminal\":{\"cursorBlink\":\"yes\"}}") << "cursorBlink";
        QTest::newRow("terminal not object") << QByteArray("{" + ok + "," + pal + ",\"terminal\":[]}") << "terminal";
        QTest::newRow("font size small") << QByteArray("{" + ok + "," + pal + ",\"fonts\":{\"terminalSize\":3}}") << "terminalSize";
        QTest::newRow("font size fraction") << QByteArray("{" + ok + "," + pal + ",\"fonts\":{\"uiSize\":10.5}}") << "uiSize";
        QTest::newRow("stripes") << QByteArray("{" + ok + "," + pal + ",\"ui\":{\"rowStripes\":101}}") << "rowStripes";
        QTest::newRow("huge") << QByteArray("{" + ok + "," + pal + ",\"pad\":\"" + QByteArray(kMaxFileBytes, 'x') + "\"}") << "large";
    }
    void badFilesAreRejected()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, why);
        QString err;
        QVERIFY(!parse(json, &err));
        QVERIFY2(err.contains(why, Qt::CaseInsensitive), qPrintable(err));
    }

    void editorDuplicatesEditsSavesAndApplies()
    {
        LaunchRequest req;
        MainWindow w(req, {});
        w.show();
        ThemeEditorDialog *ed = w.showThemeEditor();
        QVERIFY(ed->select(QStringLiteral("packers")));
        QVERIFY(!ed->currentEditable()); // built-in: read-only
        QVERIFY(!ed->saveCurrent());
        QVERIFY(ed->duplicateCurrent());
        const QString id = ed->currentId();
        QCOMPARE(id, QStringLiteral("custom:packers-copy"));
        QVERIFY(ed->currentEditable());
        QCOMPARE(ed->theme().name, QStringLiteral("Packers copy"));
        QVERIFY(QFile::exists(ColorScheme::userThemesDir() + QStringLiteral("/packers-copy.ztheme.json")));
        // Listed with the built-ins: View > Color Scheme and Preferences.
        QVERIFY(w.action(QStringLiteral("scheme:") + id));
        {
            PreferencesDialog prefs(w.settings());
            QVERIFY(prefs.findChild<QComboBox *>(QStringLiteral("colorScheme"))->findData(id) >= 0);
        }

        // Live preview, then save.
        Theme t = ed->theme();
        t.roles[Background] = 0x102030;
        t.terminal->cursorShape = QStringLiteral("underline");
        ed->setTheme(t);
        QVERIFY(ed->isDirty());
        QCOMPARE(ed->previewTerminal()->colorScheme().background, qRgb(0x10, 0x20, 0x30));
        QCOMPARE(ed->previewTerminal()->defaultCursorShape(), int(VTERM_PROP_CURSORSHAPE_UNDERLINE));
        QVERIFY(ed->saveCurrent());
        QCOMPARE(load(ColorScheme::userThemesDir() + QStringLiteral("/packers-copy.ztheme.json"))->roles[Background], 0x102030u);

        // Apply: Preferences use it; the tab gets its colours and cursor style.
        ed->applyCurrent();
        QCOMPARE(w.settings().colorScheme, id);
        QCOMPARE(AppSettings::load().colorScheme, id);
        QCOMPARE(w.terminal()->colorScheme().background, qRgb(0x10, 0x20, 0x30));
        QCOMPARE(w.terminal()->defaultCursorShape(), int(VTERM_PROP_CURSORSHAPE_UNDERLINE));

        // Rename moves the file and keeps Preferences pointing at it.
        QVERIFY(ed->renameCurrent(QStringLiteral("Lambeau Night")));
        QCOMPARE(ed->currentId(), QStringLiteral("custom:lambeau-night"));
        QVERIFY(!QFile::exists(ColorScheme::userThemesDir() + QStringLiteral("/packers-copy.ztheme.json")));
        QCOMPARE(w.settings().colorScheme, QStringLiteral("custom:lambeau-night"));

        // Export (also works for a built-in) and delete.
        const QString out = m_config.filePath(QStringLiteral("export.ztheme.json"));
        QVERIFY(ed->exportCurrent(out));
        QCOMPARE(load(out)->name, QStringLiteral("Lambeau Night"));
        QVERIFY(ed->select(QStringLiteral("xterm")));
        QVERIFY(ed->exportCurrent(out));
        QCOMPARE(load(out)->terminal->ansi[1], 0xCD0000u);
        QVERIFY(ed->select(QStringLiteral("custom:lambeau-night")));
        QVERIFY(ed->deleteCurrent());
        QVERIFY(!QFile::exists(ColorScheme::userThemesDir() + QStringLiteral("/lambeau-night.ztheme.json")));
        QVERIFY(!w.action(QStringLiteral("scheme:custom:lambeau-night")));
        ed->close();
    }

    // Duplicate a read-only theme with the button, then change a colour with
    // its swatch button: the picker must be Qt's own dialog, opened on the
    // editor without blocking it (the native one could come up behind the
    // editor on Wayland and leave it looking frozen), and the choice must land.
    void colourButtonOpensANonBlockingPickerOnTheEditor()
    {
        ThemeEditorDialog ed(AppSettings::load());
        ed.show();
        QVERIFY(ed.select(QStringLiteral("putty")));
        QVERIFY(!ed.currentEditable());
        QPushButton *dup = nullptr;
        for (QPushButton *b : ed.findChildren<QPushButton *>()) {
            if (b->text().remove(QLatin1Char('&')) == QLatin1String("Duplicate")) {
                dup = b;
            }
        }
        QVERIFY(dup);
        dup->click();
        QVERIFY(ed.currentEditable());
        QCOMPARE(ed.currentId(), QStringLiteral("custom:putty-copy"));
        const QString file = ColorScheme::userThemesDir() + QStringLiteral("/putty-copy.ztheme.json");
        QVERIFY(QFile::exists(file));

        // One palette role, one ANSI colour: both kinds of swatch.
        const struct { const char *button; std::function<std::uint32_t()> value; } swatches[] = {
            {"role:accent", [&ed] { return ed.theme().roles[Accent]; }},
            {"ansi:1", [&ed] { return ed.theme().terminal->ansi[1]; }},
        };
        std::uint32_t pick = 0x12ab34;
        for (const auto &s : swatches) {
            auto *swatch = ed.findChild<QToolButton *>(QLatin1String(s.button));
            QVERIFY2(swatch, s.button);
            QVERIFY(swatch->isEnabled());
            swatch->click(); // returns: nothing blocks here
            QColorDialog *picker = nullptr;
            for (QColorDialog *d : ed.findChildren<QColorDialog *>(QStringLiteral("colourPicker"))) {
                if (d->isVisible()) {
                    picker = d;
                }
            }
            QVERIFY2(picker, s.button);
            QVERIFY(picker->testOption(QColorDialog::DontUseNativeDialog));
            QCOMPARE(picker->parentWidget(), &ed);
            QCOMPARE(picker->windowModality(), Qt::WindowModal);
            QCOMPARE(std::uint32_t(picker->currentColor().rgb() & 0xFFFFFF), s.value());

            picker->setCurrentColor(QColor::fromRgb(pick));
            picker->accept();
            QCOMPARE(s.value(), pick);
            QVERIFY(ed.isDirty());
            ++pick;
        }
        QVERIFY(ed.saveCurrent());
        QCOMPARE(load(file)->roles[Accent], 0x12ab34u);
        QCOMPARE(load(file)->terminal->ansi[1], 0x12ab35u);

        // Cancelling changes nothing.
        ed.findChild<QToolButton *>(QStringLiteral("role:accent"))->click();
        QColorDialog *picker = nullptr;
        for (QColorDialog *d : ed.findChildren<QColorDialog *>(QStringLiteral("colourPicker"))) {
            if (d->isVisible()) {
                picker = d;
            }
        }
        QVERIFY(picker);
        picker->setCurrentColor(QColor(Qt::red));
        picker->reject();
        QCOMPARE(ed.theme().roles[Accent], 0x12ab34u);
        QVERIFY(!ed.isDirty());
        QVERIFY(ed.deleteCurrent());
        QVERIFY(!QFile::exists(file));
    }

    void editorImportsAndListsZmailThemes()
    {
        ThemeEditorDialog ed(AppSettings::load());
        // A bad file is refused with a reason; nothing is added.
        const QString bad = m_config.filePath(QStringLiteral("bad.ztheme.json"));
        writeFile(bad, "{\"format\":\"ztheme\",\"version\":9,\"name\":\"Future\"}");
        QString err;
        QVERIFY(!ed.importFile(bad, &err));
        QVERIFY(err.contains(QStringLiteral("newer")));
        // zmail's file imports as an editable copy, terminal colours derived.
        QVERIFY2(ed.importFile(fixture("zmail-made.ztheme.json"), &err), qPrintable(err));
        QVERIFY(ed.currentId().startsWith(QStringLiteral("custom:")));
        QVERIFY(ed.currentEditable());
        const Theme imported = ed.theme();
        QVERIFY(!imported.terminal);
        QCOMPARE(ed.previewTerminal()->colorScheme().ansi[1] & 0xFFFFFF, deriveTerminal(imported.roles).ansi[1]);

        // Themes in zmail's own dir are listed read-only.
        writeFile(ColorScheme::zmailThemesDir() + QStringLiteral("/lake.ztheme.json"), readFile(fixture("zmail-made.ztheme.json")));
        const ColorScheme z = ColorScheme::byId(QStringLiteral("zmail:lake"));
        QCOMPARE(z.id, QStringLiteral("zmail:lake"));
        QVERIFY(z.name.endsWith(QStringLiteral("(zmail)")));
        QVERIFY(!z.editable);
        bool listed = false;
        for (const ColorScheme &s : ColorScheme::all()) {
            listed |= s.id == z.id;
        }
        QVERIFY(listed);
        ThemeEditorDialog ed2(AppSettings::load());
        QVERIFY(ed2.select(z.id));
        QVERIFY(!ed2.currentEditable());
        QVERIFY(!ed2.renameCurrent(QStringLiteral("Mine")));
        QVERIFY(!ed2.deleteCurrent());
        QVERIFY(QFile::exists(z.path));
        // A missing custom theme falls back to the default scheme.
        QCOMPARE(ColorScheme::byId(QStringLiteral("custom:gone")).id, QStringLiteral("xterm"));
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("zterminal"));
    QApplication::setApplicationName(QStringLiteral("zterminal"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    TstThemeFile t;
    return QTest::qExec(&t, argc, argv);
}
#include "tst_themefile.moc"
