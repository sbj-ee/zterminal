#pragma once

#include "Group.hpp"
#include "SessionStore.hpp"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

namespace zterminal {

class MainWindow;

// Opens a group (core/Group.hpp) as tabs: the first member starts at once, the
// rest follow `staggerMs` apart, each in the background so only the group's
// focus member takes the focus. Owned by the window the tabs go to (it dies
// with it, which ends the chain if that window is closed meanwhile).
//
//  1. Members that are missing, imported and not yet reviewed, or that don't
//     validate are skipped; one summary says which ("Opened 7 of 8; skipped:
//     build box (missing)").
//  2. If any member uses a stored password the vault is unlocked ONCE, up
//     front. Cancelling still opens everything; ssh asks in each tab.
//  3. Members start unattended (SessionWidget::StartMode): no unlock dialog and
//     no re-bind question per tab.
//  4. Tabs are independent: a member that fails to start shows its own banner,
//     the others carry on (nothing is rolled back).
class GroupOpener : public QObject
{
    Q_OBJECT
public:
    struct Member {
        QString name;
        int index = 0; // position in GroupConfig::members
    };
    struct Plan {
        QList<Member> openable;
        QStringList skipped; // "name (reason)"
        int total = 0;
        bool needsVault = false; // an openable member uses a stored password
        QString summary() const; // "Opened 7 of 8; skipped: x (missing)" / "Opened 8 of 8"
    };
    static Plan plan(const GroupConfig &g, const SessionStore &sessions);

    // Opens `g`. A new window by default (and always when `current` is null);
    // openIn=ThisWindow appends to `current`. Returns the window that got the
    // tabs, or nullptr (and *error) when no member can be opened.
    static MainWindow *open(const GroupConfig &g, MainWindow *current, QString *error = nullptr);
    static MainWindow *openByName(const QString &name, MainWindow *current, QString *error = nullptr);

    // Tests: don't start the stagger timer; the test calls step() instead.
    static void setManualSteppingForTests(bool manual);

    const Plan &plan() const { return m_plan; }
    bool isFinished() const { return m_finished; }
    int openedCount() const { return m_next; } // members started so far (new window: includes the first)
    int focusPosition() const { return m_focusPos; }
    // Starts the next member now (the stagger timer calls this).
    void step();

signals:
    void finished(const QString &summary);

private:
    GroupOpener(const GroupConfig &g, const Plan &p, MainWindow *window, int alreadyOpen);
    void begin();
    void next();
    void finish();

    GroupConfig m_group;
    Plan m_plan;
    MainWindow *m_window;
    QTimer *m_timer = nullptr;
    int m_next = 0;
    int m_focusPos = 0;
    bool m_manual = false;
    bool m_finished = false;
};

} // namespace zterminal
