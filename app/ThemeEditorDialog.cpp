#include "ThemeEditorDialog.hpp"

#include "Terminal.hpp"
#include "TerminalView.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace zterminal {

using namespace sbj::theme;
using TermColors = sbj::theme::Terminal; // zterminal::Terminal is the emulator

namespace {
QIcon swatchIcon(std::uint32_t rgb)
{
    QPixmap pm(28, 16);
    pm.fill(QColor::fromRgb(rgb));
    QPainter p(&pm);
    p.setPen(QColor(0x80, 0x80, 0x80));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}

QByteArray previewText()
{
    QByteArray b = "\033[1;32muser@host\033[0m:\033[1;34m~/src\033[0m$ ls --color\r\n"
                   "\033[1;34mdocs\033[0m  \033[1;32mbuild.sh\033[0m  \033[1;36mlink\033[0m  \033[1;31mold.tar.gz\033[0m  README.md\r\n";
    for (int i = 0; i < 16; ++i) {
        b += "\033[48;5;" + QByteArray::number(i) + "m" + (i >= 7 ? "\033[30m" : "") + " " +
             QByteArray::number(i).rightJustified(2) + " \033[0m";
        if (i == 7) {
            b += "\r\n";
        }
    }
    b += "\r\n\033[31mred \033[32mgreen \033[33myellow \033[34mblue \033[35mmagenta \033[36mcyan\033[0m "
         "\033[7m selected \033[0m\r\n"
         "\033[33mwarning:\033[0m unused variable   \033[1;31merror:\033[0m expected ';'\r\n"
         "\033[1;32muser@host\033[0m:\033[1;34m~/src\033[0m$ ";
    return b;
}
} // namespace

ThemeEditorDialog::ThemeEditorDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
    , m_settings(current)
{
    setWindowTitle(QStringLiteral("Theme Editor"));
    setObjectName(QStringLiteral("themeEditor"));

    // Left: themes and what to do with them.
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("themeList"));
    m_list->setMinimumWidth(190);
    auto *dup = new QPushButton(QStringLiteral("D&uplicate"));
    m_rename = new QPushButton(QStringLiteral("&Rename\u2026"));
    m_delete = new QPushButton(QStringLiteral("&Delete"));
    auto *imp = new QPushButton(QStringLiteral("&Import\u2026"));
    auto *exp = new QPushButton(QStringLiteral("E&xport\u2026"));
    auto *listButtons = new QGridLayout;
    listButtons->addWidget(dup, 0, 0);
    listButtons->addWidget(m_rename, 0, 1);
    listButtons->addWidget(m_delete, 1, 0);
    listButtons->addWidget(imp, 2, 0);
    listButtons->addWidget(exp, 2, 1);
    auto *left = new QVBoxLayout;
    left->addWidget(m_list, 1);
    left->addLayout(listButtons);
    auto *note = new QLabel(QStringLiteral("Built-in and zmail themes (italic) are\nread-only: duplicate one to edit it."));
    note->setForegroundRole(QPalette::PlaceholderText);
    left->addWidget(note);

    // Right: the shared palette roles, terminal colours, cursor and font.
    auto *paletteBox = new QGroupBox(QStringLiteral("Palette (shared with zmail)"));
    auto *pg = new QGridLayout(paletteBox);
    for (int r = 0; r < RoleCount; ++r) {
        QToolButton *b = colourButton(QStringLiteral("role:") + QLatin1String(roleKey(r)),
                                      [this, r]() { return m_theme.roles[size_t(r)]; },
                                      [this, r](std::uint32_t c) {
                                          m_theme.roles[size_t(r)] = c;
                                      });
        pg->addWidget(new QLabel(QLatin1String(roleKey(r))), r / 3, (r % 3) * 2);
        pg->addWidget(b, r / 3, (r % 3) * 2 + 1);
    }
    auto *termBox = new QGroupBox(QStringLiteral("Terminal"));
    auto *tg = new QGridLayout(termBox);
    for (int i = 0; i < 16; ++i) {
        QToolButton *b = colourButton(QStringLiteral("ansi:%1").arg(i), [this, i]() { return terminalOf(m_theme).ansi[size_t(i)]; },
                                      [this, i](std::uint32_t c) { editTerminal([&](TermColors &t) { t.ansi[size_t(i)] = c; }); });
        b->setToolTip(QStringLiteral("ANSI colour %1").arg(i));
        tg->addWidget(b, i / 8, i % 8);
    }
    auto *cursorRow = new QHBoxLayout;
    struct T { const char *name; const char *label; std::uint32_t TermColors::*field; };
    for (const T &t : {T{"cursor", "Cursor", &TermColors::cursor}, T{"selection", "Selection", &TermColors::selection},
                       T{"selectionText", "Selected text", &TermColors::selectionText}}) {
        auto field = t.field;
        cursorRow->addWidget(new QLabel(QLatin1String(t.label)));
        cursorRow->addWidget(colourButton(QStringLiteral("term:") + QLatin1String(t.name),
                                          [this, field]() { return terminalOf(m_theme).*field; },
                                          [this, field](std::uint32_t c) { editTerminal([&](TermColors &x) { x.*field = c; }); }));
    }
    cursorRow->addStretch();
    tg->addLayout(cursorRow, 2, 0, 1, 8);
    m_cursorShape = new QComboBox;
    m_cursorShape->setObjectName(QStringLiteral("cursorShape"));
    m_cursorShape->addItem(QStringLiteral("Default (bar)"), QString());
    m_cursorShape->addItem(QStringLiteral("Block"), QStringLiteral("block"));
    m_cursorShape->addItem(QStringLiteral("Underline"), QStringLiteral("underline"));
    m_cursorShape->addItem(QStringLiteral("Bar"), QStringLiteral("bar"));
    m_cursorBlink = new QComboBox;
    m_cursorBlink->setObjectName(QStringLiteral("cursorBlink"));
    m_cursorBlink->addItem(QStringLiteral("Default (blink)"), -1);
    m_cursorBlink->addItem(QStringLiteral("Blink"), 1);
    m_cursorBlink->addItem(QStringLiteral("Steady"), 0);
    m_derive = new QPushButton(QStringLiteral("Derive from &palette"));
    m_derive->setToolTip(QStringLiteral("Drop the terminal colours: zterminal derives them from the palette"));
    auto *styleRow = new QHBoxLayout;
    styleRow->addWidget(new QLabel(QStringLiteral("Cursor style:")));
    styleRow->addWidget(m_cursorShape);
    styleRow->addWidget(m_cursorBlink);
    styleRow->addStretch();
    styleRow->addWidget(m_derive);
    tg->addLayout(styleRow, 3, 0, 1, 8);

    m_setFont = new QCheckBox(QStringLiteral("Theme sets the terminal font:"));
    m_setFont->setObjectName(QStringLiteral("setFont"));
    m_font = new QFontComboBox;
    m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    m_fontSize = new QSpinBox;
    m_fontSize->setRange(kMinFontSize, kMaxFontSize);
    auto *fontRow = new QHBoxLayout;
    fontRow->addWidget(m_setFont);
    fontRow->addWidget(m_font, 1);
    fontRow->addWidget(m_fontSize);

    m_term = new Terminal(8, 64, this);
    m_view = new TerminalView(m_term);
    m_view->setObjectName(QStringLiteral("themePreview"));
    m_view->setFocusPolicy(Qt::NoFocus);
    m_view->setMinimumHeight(150);
    m_term->feed(previewText());

    auto *right = new QVBoxLayout;
    right->addWidget(paletteBox);
    right->addWidget(termBox);
    right->addLayout(fontRow);
    right->addWidget(new QLabel(QStringLiteral("Preview:")));
    right->addWidget(m_view, 1);

    auto *body = new QHBoxLayout;
    body->addLayout(left);
    body->addLayout(right, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_save = buttons->addButton(QStringLiteral("&Save"), QDialogButtonBox::ActionRole);
    QPushButton *apply = buttons->addButton(QStringLiteral("&Apply"), QDialogButtonBox::ApplyRole);
    apply->setToolTip(QStringLiteral("Save, then use this theme for every tab (Preferences)"));
    auto *outer = new QVBoxLayout(this);
    outer->addLayout(body, 1);
    outer->addWidget(buttons);

    m_editors = {paletteBox, termBox, m_setFont, m_font, m_fontSize};

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_loading || row == m_row) {
            return;
        }
        if (m_dirty && QMessageBox::question(this, windowTitle(), QStringLiteral("Discard unsaved changes to \u201c%1\u201d?")
                                                                      .arg(m_theme.name)) != QMessageBox::Yes) {
            const QSignalBlocker block(m_list);
            m_list->setCurrentRow(m_row);
            return;
        }
        m_row = row;
        m_dirty = false;
        m_theme = row >= 0 ? m_schemes.at(row).toThemeFile() : Theme{};
        showTheme();
    });
    for (QComboBox *c : {m_cursorShape, m_cursorBlink}) {
        connect(c, &QComboBox::currentIndexChanged, this, &ThemeEditorDialog::fieldsChanged);
    }
    connect(m_setFont, &QCheckBox::toggled, this, &ThemeEditorDialog::fieldsChanged);
    connect(m_font, &QFontComboBox::currentFontChanged, this, &ThemeEditorDialog::fieldsChanged);
    connect(m_fontSize, &QSpinBox::valueChanged, this, &ThemeEditorDialog::fieldsChanged);
    connect(m_derive, &QPushButton::clicked, this, [this]() {
        m_theme.terminal.reset();
        m_dirty = true;
        showTheme();
    });
    connect(dup, &QPushButton::clicked, this, [this]() { duplicateCurrent(); });
    connect(m_rename, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString n = QInputDialog::getText(this, QStringLiteral("Rename Theme"), QStringLiteral("Name:"),
                                                QLineEdit::Normal, m_theme.name, &ok);
        QString err;
        if (ok && !renameCurrent(n, &err)) {
            QMessageBox::warning(this, QStringLiteral("Rename Theme"), err);
        }
    });
    connect(m_delete, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("Delete Theme"),
                                  QStringLiteral("Delete the theme \u201c%1\u201d?").arg(m_theme.name)) == QMessageBox::Yes) {
            deleteCurrent();
        }
    });
    connect(imp, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Theme"), QString(),
                                                          QStringLiteral("Themes (*.ztheme.json *.json)"));
        QString err;
        if (!path.isEmpty() && !importFile(path, &err)) {
            QMessageBox::warning(this, QStringLiteral("Import Theme"), err);
        }
    });
    connect(exp, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Theme"),
                                                          fileStem(m_theme.name) + QLatin1String(kSuffix),
                                                          QStringLiteral("Themes (*.ztheme.json)"));
        QString err;
        if (!path.isEmpty() && !exportCurrent(path, &err)) {
            QMessageBox::warning(this, QStringLiteral("Export Theme"), err);
        }
    });
    connect(m_save, &QPushButton::clicked, this, [this]() {
        QString err;
        if (!saveCurrent(&err)) {
            QMessageBox::warning(this, QStringLiteral("Save Theme"), err);
        }
    });
    connect(apply, &QPushButton::clicked, this, &ThemeEditorDialog::applyCurrent);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    reloadList(current.colorScheme);
    resize(900, 720);
}

ThemeEditorDialog::~ThemeEditorDialog()
{
    delete m_view; // before the terminal it draws
    m_view = nullptr;
}

QToolButton *ThemeEditorDialog::colourButton(const QString &objectName, std::function<std::uint32_t()> get,
                                             std::function<void(std::uint32_t)> set)
{
    auto *b = new QToolButton;
    b->setObjectName(objectName);
    b->setIconSize(QSize(28, 16));
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(b, &QToolButton::clicked, this, [this, get, set]() {
        const QColor c = QColorDialog::getColor(QColor::fromRgb(get()), this, QStringLiteral("Choose Colour"));
        if (c.isValid()) {
            set(c.rgb() & 0xFFFFFF);
            m_dirty = true;
            showTheme();
        }
    });
    m_colourButtons.append({b, get});
    return b;
}

void ThemeEditorDialog::editTerminal(const std::function<void(TermColors &)> &change)
{
    TermColors t = terminalOf(m_theme); // the first edit pins the derived colours
    change(t);
    m_theme.terminal = t;
}

void ThemeEditorDialog::reloadList(const QString &selectId)
{
    m_loading = true;
    m_schemes = ColorScheme::all();
    m_list->clear();
    int sel = 0;
    for (int i = 0; i < m_schemes.size(); ++i) {
        const ColorScheme &s = m_schemes.at(i);
        auto *item = new QListWidgetItem(s.name, m_list);
        if (!s.editable) { // read-only: italic
            QFont f = item->font();
            f.setItalic(true);
            item->setFont(f);
            item->setToolTip(QStringLiteral("Read-only: duplicate it to edit"));
        }
        item->setData(Qt::UserRole, s.id);
        item->setIcon(swatchIcon(s.background & 0xFFFFFF));
        if (s.id == selectId) {
            sel = i;
        }
    }
    m_loading = false;
    m_row = -1;
    m_dirty = false;
    m_list->setCurrentRow(sel);
}

bool ThemeEditorDialog::select(const QString &id)
{
    for (int i = 0; i < m_schemes.size(); ++i) {
        if (m_schemes.at(i).id == id) {
            m_dirty = false;
            m_list->setCurrentRow(i);
            return true;
        }
    }
    return false;
}

QString ThemeEditorDialog::currentId() const
{
    return m_row >= 0 ? m_schemes.at(m_row).id : QString();
}

bool ThemeEditorDialog::currentEditable() const
{
    return m_row >= 0 && m_schemes.at(m_row).editable;
}

void ThemeEditorDialog::setTheme(const Theme &t)
{
    m_theme = t;
    m_dirty = true;
    showTheme();
}

void ThemeEditorDialog::showTheme()
{
    m_loading = true;
    for (auto &[button, get] : m_colourButtons) {
        button->setIcon(swatchIcon(get()));
        button->setToolTip(hex(get()));
    }
    const TermColors term = terminalOf(m_theme);
    m_cursorShape->setCurrentIndex(std::max(0, m_cursorShape->findData(term.cursorShape)));
    m_cursorBlink->setCurrentIndex(m_cursorBlink->findData(term.cursorBlink ? int(*term.cursorBlink) : -1));
    m_setFont->setChecked(!m_theme.fonts.terminal.isEmpty());
    m_font->setCurrentFont(QFont(m_theme.fonts.terminal.isEmpty() ? m_settings.font().family() : m_theme.fonts.terminal));
    m_fontSize->setValue(m_theme.fonts.terminalSize > 0 ? m_theme.fonts.terminalSize : m_settings.fontSize);
    m_loading = false;
    updatePreview();
    updateButtons();
}

void ThemeEditorDialog::fieldsChanged()
{
    if (m_loading) {
        return;
    }
    const QString shape = m_cursorShape->currentData().toString();
    const int blink = m_cursorBlink->currentData().toInt();
    const TermColors now = terminalOf(m_theme);
    if (shape != now.cursorShape || (blink < 0 ? now.cursorBlink.has_value() : now.cursorBlink != (blink == 1))) {
        editTerminal([&](TermColors &t) {
            t.cursorShape = shape;
            t.cursorBlink = blink < 0 ? std::nullopt : std::optional<bool>(blink == 1);
        });
    }
    m_font->setEnabled(m_setFont->isChecked() && currentEditable());
    m_fontSize->setEnabled(m_setFont->isChecked() && currentEditable());
    m_theme.fonts.terminal = m_setFont->isChecked() ? m_font->currentFont().family() : QString();
    m_theme.fonts.terminalSize = m_setFont->isChecked() ? m_fontSize->value() : 0;
    m_dirty = true;
    updatePreview();
    updateButtons();
}

void ThemeEditorDialog::updatePreview()
{
    const ColorScheme s = ColorScheme::fromThemeFile(m_theme, currentId());
    m_term->setColorScheme(s);
    m_term->setDefaultCursorStyle(s.cursorShape >= 0 ? s.cursorShape : Terminal::kDefaultCursorShape,
                                  s.cursorBlink >= 0 ? s.cursorBlink != 0 : Terminal::kDefaultCursorBlink);
    QFont f = m_settings.font();
    if (!m_theme.fonts.terminal.isEmpty()) {
        f.setFamily(m_theme.fonts.terminal);
    }
    if (m_theme.fonts.terminalSize > 0) {
        f.setPointSize(m_theme.fonts.terminalSize);
    }
    m_view->setTerminalFont(f);
    m_view->viewport()->update();
}

void ThemeEditorDialog::updateButtons()
{
    const bool editable = currentEditable();
    for (QWidget *w : m_editors) {
        w->setEnabled(editable);
    }
    m_font->setEnabled(editable && m_setFont->isChecked());
    m_fontSize->setEnabled(editable && m_setFont->isChecked());
    m_cursorShape->setEnabled(editable);
    m_cursorBlink->setEnabled(editable);
    m_derive->setEnabled(editable && m_theme.terminal.has_value());
    m_rename->setEnabled(editable);
    m_delete->setEnabled(editable);
    m_save->setEnabled(editable && m_dirty);
}

bool ThemeEditorDialog::duplicateCurrent(const QString &name)
{
    if (m_row < 0) {
        return false;
    }
    Theme t = m_theme;
    t.basedOn = currentId();
    t.name = name.trimmed().isEmpty() ? (m_theme.name + QStringLiteral(" copy")).left(kMaxNameLength) : name.trimmed();
    const QString path = uniquePath(ColorScheme::userThemesDir(), t.name);
    if (!save(t, path)) {
        return false;
    }
    reloadList(QStringLiteral("custom:") + QFileInfo(path).fileName().chopped(qsizetype(qstrlen(kSuffix))));
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::saveCurrent(QString *error)
{
    if (!currentEditable()) {
        if (error) {
            *error = QStringLiteral("Built-in and zmail themes are read-only; duplicate it first.");
        }
        return false;
    }
    if (!save(m_theme, m_schemes.at(m_row).path, error)) {
        return false;
    }
    const QString id = currentId();
    reloadList(id);
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::renameCurrent(const QString &name, QString *error)
{
    const QString n = name.trimmed();
    if (!currentEditable() || n.isEmpty() || n.size() > kMaxNameLength) {
        if (error) {
            *error = QStringLiteral("Enter a name of 1 to %1 characters.").arg(kMaxNameLength);
        }
        return false;
    }
    const QString oldPath = m_schemes.at(m_row).path, oldId = currentId();
    m_theme.name = n;
    QString path = oldPath;
    if (fileStem(n) != QFileInfo(oldPath).fileName().chopped(qsizetype(qstrlen(kSuffix)))) {
        path = uniquePath(ColorScheme::userThemesDir(), n);
    }
    if (!save(m_theme, path, error)) {
        return false;
    }
    if (path != oldPath) {
        QFile::remove(oldPath);
    }
    const QString id = QStringLiteral("custom:") + QFileInfo(path).fileName().chopped(qsizetype(qstrlen(kSuffix)));
    reloadList(id);
    if (m_settings.colorScheme == oldId && id != oldId) { // keep Preferences pointing at it
        m_settings.colorScheme = id;
        emit applied(m_settings);
    }
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::deleteCurrent()
{
    if (!currentEditable() || !QFile::remove(m_schemes.at(m_row).path)) {
        return false;
    }
    reloadList(m_theme.basedOn); // back to the theme it came from (if it's still there)
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::importFile(const QString &path, QString *error)
{
    const std::optional<Theme> t = load(path, error);
    if (!t) {
        return false;
    }
    const QString dest = uniquePath(ColorScheme::userThemesDir(), t->name);
    if (!save(*t, dest, error)) {
        return false;
    }
    reloadList(QStringLiteral("custom:") + QFileInfo(dest).fileName().chopped(qsizetype(qstrlen(kSuffix))));
    emit themesChanged();
    return true;
}

bool ThemeEditorDialog::exportCurrent(const QString &path, QString *error)
{
    return save(m_theme, path, error);
}

void ThemeEditorDialog::applyCurrent()
{
    if (m_dirty && currentEditable() && !saveCurrent()) {
        return;
    }
    AppSettings s = m_settings;
    s.colorScheme = currentId();
    if (!m_theme.fonts.terminal.isEmpty()) {
        s.fontFamily = m_theme.fonts.terminal;
    }
    if (m_theme.fonts.terminalSize > 0) {
        s.fontSize = m_theme.fonts.terminalSize;
    }
    m_settings = s;
    emit applied(s);
}

} // namespace zterminal
