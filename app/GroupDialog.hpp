#pragma once

#include "Group.hpp"
#include "GroupStore.hpp"
#include "SessionStore.hpp"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace zterminal {

// File > Manage Groups: the saved groups on the left, the selected group's
// settings and ordered member list on the right. A member is a saved-session
// NAME (a reference): one that no longer exists shows red as "(missing)" and
// is skipped when the group opens. Open Group accepts the dialog; the caller
// opens config() (what is in the editor, saved or not).
class GroupDialog : public QDialog
{
    Q_OBJECT
public:
    GroupDialog(const SessionStore &sessions, const GroupStore &groups, QWidget *parent = nullptr);

    // The editor's state.
    GroupConfig config() const;
    void setConfig(const GroupConfig &g);
    QString errorText() const;

    // Button actions (public for tests). Each returns false and shows the
    // reason in the dialog on failure.
    bool saveCurrent();
    bool deleteSelected();
    bool openGroup();
    void newGroup();
    // Appends these sessions (Add Session... picks them from the saved ones).
    void addSessions(const QStringList &names);
    // Removes the selected members / moves them by `delta` rows (-1 up, +1 down).
    bool removeSelectedMembers();
    void moveSelected(int delta);
    QStringList memberNames() const;

private:
    void refreshList(const QString &select = {});
    void setMembers(const QStringList &names);
    void addMemberItem(const QString &name);
    void updateEnabled();
    void setError(const QString &e);
    void pickSessions();

    SessionStore m_sessions;
    GroupStore m_groups;
    QListWidget *m_list = nullptr;
    QPushButton *m_new = nullptr;
    QPushButton *m_delete = nullptr;
    QLineEdit *m_name = nullptr;
    QComboBox *m_openIn = nullptr;
    QSpinBox *m_stagger = nullptr;
    QSpinBox *m_focus = nullptr;
    QListWidget *m_members = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_remove = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_save = nullptr;
    QLabel *m_error = nullptr;
};

} // namespace zterminal
