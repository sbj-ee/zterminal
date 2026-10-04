#pragma once

#include "AppSettings.hpp"
#include "CommandLine.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"

#include <functional>
#include <optional>

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
    QString sessionName() const { return m_saved ? m_saved->name : m_request.displayName(); }
    // The saved session this window runs (empty for local/ad-hoc windows).
    const std::optional<SessionConfig> &savedSession() const { return m_saved; }

    // What startSession() runs: program "" means the user's login shell.
    struct Launch {
        QString program;
        QStringList args;
        QString error;
    };
    Launch launchCommand() const;

    // File > New Session / Open Saved Session: opens the session dialog.
    void showSessionDialog(bool focusSaved);
    // Start `cfg` in a new window (by name when it matches the saved copy).
    bool openInNewWindow(const SessionConfig &cfg, QString *error = nullptr);
    // File > Save Session without the name prompt; the window then *is* that session.
    bool saveCurrentSessionAs(const QString &name, QString *error = nullptr);
    // Current window's session as a SessionConfig (nullopt if it can't be saved).
    std::optional<SessionConfig> currentSessionConfig(QString *why = nullptr) const;

    // How new windows are started (tests replace it). Default: QProcess::startDetached.
    using Launcher = std::function<bool(const QString &program, const QStringList &args)>;
    void setLauncher(Launcher l) { m_launcher = std::move(l); }
    QStringList relaunchArgs() const { return m_originalArgs; }
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
    void showAbout();
    void watchSettingsFile();
    void setMenuBarShown(bool shown);
    void showContextMenu(const QPoint &globalPos);

    void saveSessionInteractive();
    bool launch(const QStringList &args);

    LaunchRequest m_request;
    std::optional<SessionConfig> m_saved;
    SessionStore m_store;
    Launcher m_launcher;
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
