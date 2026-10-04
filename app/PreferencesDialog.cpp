#include "PreferencesDialog.hpp"

#include "ColorScheme.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace zterminal {

PreferencesDialog::PreferencesDialog(const AppSettings &current, QWidget *parent)
    : QDialog(parent)
    , m_base(current)
{
    setWindowTitle(QStringLiteral("Preferences"));

    auto *mouseBox = new QGroupBox(QStringLiteral("Mouse and clipboard"));
    auto *mouseForm = new QFormLayout(mouseBox);
    auto *selectNote = new QLabel(QStringLiteral(
        "Selecting text copies it to PRIMARY. Right-click pastes CLIPBOARD; "
        "Ctrl+right-click opens the menu; hold Shift to bypass mouse reporting."));
    selectNote->setWordWrap(true);
    mouseForm->addRow(selectNote);
    m_copyToClipboard = new QCheckBox(QStringLiteral("Selecting also copies to CLIPBOARD"));
    m_copyToClipboard->setChecked(current.mouse.copyOnSelectToClipboard);
    mouseForm->addRow(m_copyToClipboard);
    m_middle = new QComboBox;
    m_middle->addItem(QStringLiteral("Paste PRIMARY"), MouseSettings::toString(MiddleClickAction::PastePrimary));
    m_middle->addItem(QStringLiteral("Paste CLIPBOARD"), MouseSettings::toString(MiddleClickAction::PasteClipboard));
    m_middle->addItem(QStringLiteral("Do nothing"), MouseSettings::toString(MiddleClickAction::Off));
    m_middle->setCurrentIndex(m_middle->findData(MouseSettings::toString(current.mouse.middleClick)));
    mouseForm->addRow(QStringLiteral("Middle-click:"), m_middle);

    auto *lookBox = new QGroupBox(QStringLiteral("Appearance"));
    auto *lookForm = new QFormLayout(lookBox);
    m_font = new QFontComboBox;
    m_font->setFontFilters(QFontComboBox::MonospacedFonts);
    m_font->setCurrentFont(current.font());
    lookForm->addRow(QStringLiteral("Font:"), m_font);
    m_fontSize = new QSpinBox;
    m_fontSize->setRange(6, 48);
    m_fontSize->setValue(current.fontSize);
    lookForm->addRow(QStringLiteral("Size:"), m_fontSize);
    m_scheme = new QComboBox;
    for (const ColorScheme &s : ColorScheme::builtIn()) {
        m_scheme->addItem(s.name, s.id);
    }
    m_scheme->setCurrentIndex(std::max(0, m_scheme->findData(current.colorScheme)));
    lookForm->addRow(QStringLiteral("Color scheme:"), m_scheme);

    auto *termBox = new QGroupBox(QStringLiteral("Terminal"));
    auto *termForm = new QFormLayout(termBox);
    m_scrollback = new QSpinBox;
    m_scrollback->setRange(0, 1000000);
    m_scrollback->setSingleStep(1000);
    m_scrollback->setValue(current.scrollbackLines);
    termForm->addRow(QStringLiteral("Scrollback lines:"), m_scrollback);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(mouseBox);
    layout->addWidget(lookBox);
    layout->addWidget(termBox);
    layout->addWidget(buttons);
}

AppSettings PreferencesDialog::result() const
{
    AppSettings a = m_base;
    a.mouse.copyOnSelectToClipboard = m_copyToClipboard->isChecked();
    a.mouse.middleClick = MouseSettings::middleClickFromString(m_middle->currentData().toString());
    a.fontFamily = m_font->currentFont().family();
    a.fontSize = m_fontSize->value();
    a.colorScheme = m_scheme->currentData().toString();
    a.scrollbackLines = m_scrollback->value();
    return a;
}

} // namespace zterminal
