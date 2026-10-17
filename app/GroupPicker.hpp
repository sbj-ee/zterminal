#pragma once

#include "GroupStore.hpp"

#include <QDialog>

class QLineEdit;
class QListWidget;

namespace zterminal {

// Ctrl+Shift+P: a small "open group" launcher. Type to filter, Up/Down to
// move, Enter (or a double click) to choose.
class GroupPicker : public QDialog
{
    Q_OBJECT
public:
    explicit GroupPicker(const GroupStore &groups, QWidget *parent = nullptr);

    // The chosen group's name; empty if cancelled.
    QString chosen() const { return m_chosen; }
    // Names currently shown (after filtering).
    QStringList visibleGroups() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyFilter(const QString &text);
    void choose();

    GroupStore m_groups;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QString m_chosen;
};

} // namespace zterminal
