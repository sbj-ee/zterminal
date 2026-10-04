#pragma once

#include "AppSettings.hpp"
#include "CommandLine.hpp"

#include <QMainWindow>

class QAction;
class QActionGroup;
class QFileSystemWatcher;
class QTimer;
class QMenu;

namespace zterminal {

class Pty;
class Terminal;
class TerminalView;

// One window = one session (approved: no tabs). Owns the PTY, the emulator and
// the view, plus the classic menu bar (docs/PLAN.md §3).
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const LaunchRequest &request, const QStringList &originalArgs,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    void startSession();
    TerminalView *view() const { return m_view; }
    Terminal *terminal() const { return m_term; }
    Pty *pty() const { return m_pty; }
    QString sessionName() const { return m_request.displayName(); }
    // Every QAction by objectName (used by tests and the context menu).
    QAction *action(const QString &name) const;
    QMenu *contextMenu() const { return m_contextMenu; }

    const AppSettings &settings() const { return m_settings; }
    // Save as the new global defaults and apply to this window now; other open
    // windows (and other zterminal processes) follow via the settings-file watcher.
    void setSettings(const AppSettings &s);
    // Re-read the settings file and apply it if it changed.
    void reloadSettings();
    // Open Settings > Preferences (modal).
    void showPreferences();

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildMenus();
    QAction *addAct(QMenu *menu, const QString &name, const QString &text,
                    const QKeySequence &shortcut = {}, bool enabled = true,
                    const QString &plannedNote = {});
    void applySettings();
    void updateTitle();
    void setFontSize(int points);
    void onSessionFinished(int exitCode, bool crashed);
    void restartSession();
    void duplicateSession();
    void newSession();
    void showAbout();
    void watchSettingsFile();
    void setMenuBarShown(bool shown);
    void showContextMenu(const QPoint &globalPos);

    LaunchRequest m_request;
    QStringList m_originalArgs;
    AppSettings m_settings;
    Terminal *m_term = nullptr;
    Pty *m_pty = nullptr;
    TerminalView *m_view = nullptr;
    QMenu *m_contextMenu = nullptr;
    QActionGroup *m_schemeGroup = nullptr;
    QFileSystemWatcher *m_settingsWatcher = nullptr;
    QTimer *m_reloadTimer = nullptr;
    QList<QAction *> m_actions;
};

} // namespace zterminal
