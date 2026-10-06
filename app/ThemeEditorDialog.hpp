#pragma once

#include "AppSettings.hpp"
#include "ColorScheme.hpp"
#include "ThemeFile.h"

#include <QDialog>
#include <array>
#include <functional>

class QCheckBox;
class QComboBox;
class QFontComboBox;
class QListWidget;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace zterminal {

class Terminal;
class TerminalView;

// Settings > Theme Editor: create and edit custom colour themes
// (*.ztheme.json in ColorScheme::userThemesDir(); format in docs/THEMES.md).
// Built-in schemes and zmail's themes are read-only: Duplicate them to edit.
// The buttons call the public functions below (tests drive those directly).
class ThemeEditorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ThemeEditorDialog(const AppSettings &current, QWidget *parent = nullptr);
    ~ThemeEditorDialog() override;

    bool select(const QString &id);
    QString currentId() const;
    bool currentEditable() const;
    const sbj::theme::Theme &theme() const { return m_theme; }
    // Edits (as the colour buttons and fields do): live preview, unsaved.
    void setTheme(const sbj::theme::Theme &t);
    bool isDirty() const { return m_dirty; }

    bool duplicateCurrent(const QString &name = {}); // new custom theme, selected
    bool renameCurrent(const QString &name, QString *error = nullptr);
    bool deleteCurrent();
    bool saveCurrent(QString *error = nullptr);
    bool importFile(const QString &path, QString *error = nullptr); // into the themes dir, selected
    bool exportCurrent(const QString &path, QString *error = nullptr);
    void applyCurrent(); // save, then use it in Preferences (and its font, if it sets one)

    Terminal *previewTerminal() const { return m_term; }
    TerminalView *previewView() const { return m_view; }

signals:
    void applied(const AppSettings &settings);
    void themesChanged();

private:
    void reloadList(const QString &selectId);
    void showTheme();     // m_theme -> widgets + preview
    void fieldsChanged(); // font / cursor widgets -> m_theme
    void updatePreview();
    void updateButtons();
    QToolButton *colourButton(const QString &objectName, std::function<std::uint32_t()> get,
                              std::function<void(std::uint32_t)> set);
    void editTerminal(const std::function<void(sbj::theme::Terminal &)> &change);

    AppSettings m_settings;
    QList<ColorScheme> m_schemes;
    int m_row = -1;
    sbj::theme::Theme m_theme;
    bool m_dirty = false;
    bool m_loading = false;

    QListWidget *m_list = nullptr;
    QList<std::pair<QToolButton *, std::function<std::uint32_t()>>> m_colourButtons;
    QComboBox *m_cursorShape = nullptr;
    QComboBox *m_cursorBlink = nullptr;
    QCheckBox *m_setFont = nullptr;
    QFontComboBox *m_font = nullptr;
    QSpinBox *m_fontSize = nullptr;
    QPushButton *m_rename = nullptr, *m_delete = nullptr, *m_save = nullptr, *m_derive = nullptr;
    QList<QWidget *> m_editors; // disabled for read-only themes
    Terminal *m_term = nullptr;
    TerminalView *m_view = nullptr;
};

} // namespace zterminal
