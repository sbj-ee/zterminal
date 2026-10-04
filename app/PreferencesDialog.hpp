#pragma once

#include "AppSettings.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QSpinBox;

namespace zterminal {

// Settings > Preferences: global defaults (v0.1 subset: mouse, appearance, scrollback).
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreferencesDialog(const AppSettings &current, QWidget *parent = nullptr);
    AppSettings result() const;

private:
    AppSettings m_base;
    QComboBox *m_middle = nullptr;
    QCheckBox *m_copyToClipboard = nullptr;
    QFontComboBox *m_font = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QComboBox *m_scheme = nullptr;
    QSpinBox *m_scrollback = nullptr;
};

} // namespace zterminal
