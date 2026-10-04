#pragma once

#include "AppSettings.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QSpinBox;

namespace zterminal {

// Settings > Preferences (also Ctrl+Shift+, and the Ctrl+right-click menu):
// global defaults. OK and Apply hand the result to the window, which saves it;
// every open zterminal window then picks the change up (MainWindow watches the file).
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreferencesDialog(const AppSettings &current, QWidget *parent = nullptr);
    AppSettings result() const;

signals:
    // Apply pressed: settings to save and apply now, dialog stays open.
    void applied(const zterminal::AppSettings &settings);

private:
    AppSettings m_base;
    QComboBox *m_middle = nullptr;
    QCheckBox *m_copyToClipboard = nullptr;
    QFontComboBox *m_font = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QComboBox *m_scheme = nullptr;
    QSpinBox *m_scrollback = nullptr;
    QCheckBox *m_checkUpdates = nullptr;
    QString m_initialComboFamily; // what the font combo showed before any edit
};

} // namespace zterminal
