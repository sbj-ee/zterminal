#include "PreferencesDialog.hpp"

#include "ColorScheme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace zterminal {

PreferencesDialog::PreferencesDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
    , m_base(current)
{
    setWindowTitle(QStringLiteral("Preferences"));
    setObjectName(QStringLiteral("preferencesDialog"));

    auto *mouseBox = new QGroupBox(QStringLiteral("Mouse and clipboard"));
    // Explicit line breaks rather than word wrap: a wrapped QLabel's height is
    // computed for the wrong width and the text ends up clipped.
    auto *mouseOuter = new QVBoxLayout(mouseBox);
    auto *selectNote = new QLabel(QStringLiteral(
        "Selecting text copies it to PRIMARY. Right-click pastes CLIPBOARD.\n"
        "Ctrl+right-click opens the menu. Hold Shift to bypass mouse reporting."));
    mouseOuter->addWidget(selectNote);
    auto *mouseForm = new QFormLayout;
    mouseOuter->addLayout(mouseForm);
    m_copyToClipboard = new QCheckBox(QStringLiteral("Selecting also copies to CLIPBOARD"));
    m_copyToClipboard->setObjectName(QStringLiteral("copyToClipboard"));
    m_copyToClipboard->setChecked(current.mouse.copyOnSelectToClipboard);
    mouseForm->addRow(m_copyToClipboard);
    m_middle = new QComboBox;
    m_middle->setObjectName(QStringLiteral("middleClick"));
    m_middle->addItem(QStringLiteral("Paste PRIMARY"), MouseSettings::toString(MiddleClickAction::PastePrimary));
    m_middle->addItem(QStringLiteral("Paste CLIPBOARD"), MouseSettings::toString(MiddleClickAction::PasteClipboard));
    m_middle->addItem(QStringLiteral("Do nothing"), MouseSettings::toString(MiddleClickAction::Off));
    m_middle->setCurrentIndex(m_middle->findData(MouseSettings::toString(current.mouse.middleClick)));
    mouseForm->addRow(QStringLiteral("Middle-click:"), m_middle);

    auto *lookBox = new QGroupBox(QStringLiteral("Appearance"));
    auto *lookForm = new QFormLayout(lookBox);
    m_font = new QFontComboBox;
    m_font->setObjectName(QStringLiteral("fontFamily"));
    m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    m_font->setCurrentFont(current.font());
    m_initialComboFamily = m_font->currentFont().family();
    lookForm->addRow(QStringLiteral("Font:"), m_font);
    m_fontSize = new QSpinBox;
    m_fontSize->setObjectName(QStringLiteral("fontSize"));
    m_fontSize->setRange(6, 48);
    m_fontSize->setValue(current.fontSize);
    lookForm->addRow(QStringLiteral("Size:"), m_fontSize);
    m_scheme = new QComboBox;
    m_scheme->setObjectName(QStringLiteral("colorScheme"));
    for (const ColorScheme &s : ColorScheme::builtIn()) {
        m_scheme->addItem(s.name, s.id);
    }
    m_scheme->setCurrentIndex(std::max(0, m_scheme->findData(current.colorScheme)));
    lookForm->addRow(QStringLiteral("Color scheme:"), m_scheme);

    auto *termBox = new QGroupBox(QStringLiteral("Terminal"));
    auto *termForm = new QFormLayout(termBox);
    m_scrollback = new QSpinBox;
    m_scrollback->setObjectName(QStringLiteral("scrollbackLines"));
    m_scrollback->setRange(0, 1000000);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setValue(current.scrollbackLines);
    termForm->addRow(QStringLiteral("Scrollback lines:"), m_scrollback);
    m_vaultAutoLock = new QSpinBox;
    m_vaultAutoLock->setObjectName(QStringLiteral("vaultAutoLockMinutes"));
    m_vaultAutoLock->setRange(0, 24 * 60);
    m_vaultAutoLock->setSuffix(QStringLiteral(" min"));
    m_vaultAutoLock->setSpecialValueText(QStringLiteral("Never"));
    m_vaultAutoLock->setValue(current.vaultAutoLockMinutes);
    m_vaultAutoLock->setToolTip(QStringLiteral("Lock the password vault after this long without keyboard or mouse input"));
    termForm->addRow(QStringLiteral("Lock vault when idle:"), m_vaultAutoLock);

    auto *logBox = new QGroupBox(QStringLiteral("Session logs"));
    auto *logForm = new QFormLayout(logBox);
    auto *logRow = new QHBoxLayout;
    m_logDir = new QLineEdit(current.logDirectory);
    m_logDir->setObjectName(QStringLiteral("logDirectory"));
    m_logDir->setPlaceholderText(QStringLiteral("~/zterminal-logs"));
    auto *browseLog = new QPushButton(QStringLiteral("Browse\u2026"));
    browseLog->setAutoDefault(false);
    connect(browseLog, &QPushButton::clicked, this, [this]() {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Log folder"), QDir::homePath());
        if (!d.isEmpty()) {
            m_logDir->setText(d);
        }
    });
    logRow->addWidget(m_logDir, 1);
    logRow->addWidget(browseLog);
    logForm->addRow(QStringLiteral("Folder:"), logRow);
    m_logTimestamps = new QCheckBox(QStringLiteral("Timestamp every line (ISO 8601, local time)"));
    m_logTimestamps->setObjectName(QStringLiteral("logTimestamps"));
    m_logTimestamps->setChecked(current.logTimestamps);
    logForm->addRow(m_logTimestamps);

    auto *updBox = new QGroupBox(QStringLiteral("Updates"));
    auto *updLayout = new QVBoxLayout(updBox);
    m_checkUpdates = new QCheckBox(QStringLiteral("Check for updates at startup"));
    m_checkUpdates->setObjectName(QStringLiteral("checkForUpdatesOnStartup"));
    m_checkUpdates->setChecked(current.checkForUpdatesOnStartup);
    updLayout->addWidget(m_checkUpdates);
    auto *updNote = new QLabel(QStringLiteral(
        "<small>Your choice is saved now. The update checker itself arrives in a later release.</small>"));
    updNote->setWordWrap(true);
    updLayout->addWidget(updNote);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    buttons->setObjectName(QStringLiteral("buttons"));
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            [this]() { emit applied(result()); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(mouseBox);
    layout->addWidget(lookBox);
    layout->addWidget(termBox);
    layout->addWidget(logBox);
    layout->addWidget(updBox);
    layout->addWidget(buttons);
}

AppSettings PreferencesDialog::result() const
{
    AppSettings a = m_base;
    a.mouse.copyOnSelectToClipboard = m_copyToClipboard->isChecked();
    a.mouse.middleClick = MouseSettings::middleClickFromString(m_middle->currentData().toString());
    // The monospace-filtered combo may not list the stored family exactly; only
    // replace it when the user actually picked a different font.
    if (m_font->currentFont().family() != m_initialComboFamily) {
        a.fontFamily = m_font->currentFont().family();
    }
    a.fontSize = m_fontSize->value();
    a.colorScheme = m_scheme->currentData().toString();
    a.scrollbackLines = m_scrollback->value();
    a.vaultAutoLockMinutes = m_vaultAutoLock->value();
    a.logDirectory = m_logDir->text().trimmed();
    a.logTimestamps = m_logTimestamps->isChecked();
    a.checkForUpdatesOnStartup = m_checkUpdates->isChecked();
    return a;
}

} // namespace zterminal
