#pragma once

#include "AppSettings.hpp"
#include "CommandLine.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"

#include <functional>
#include <memory>
#include <optional>

#include <QMainWindow>

class QAction;
class QActionGroup;
class QFileSystemWatcher;
class QLabel;
class QPushButton;
class QWidget;
class QTimer;
class QMenu;

namespace zterminal {

class AskpassServer;
class Pty;
class SessionLog;
class SerialBackend;
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
    SerialBackend *serial() const { return m_serial; }
    bool isSerialSession() const;
    // The red "disconnected / could not open" bar with Reconnect (tests).
    QWidget *serialBanner() const { return m_banner; }
    QString serialBannerText() const;
    // "Sending paste... [Cancel]" bar shown while paced output is queued.
    QWidget *pasteBar() const { return m_pasteBar; }
    QString sessionName() const { return m_saved ? m_saved->name : m_request.displayName(); }
    // The saved session this window runs (empty for local/ad-hoc windows).
    const std::optional<SessionConfig> &savedSession() const { return m_saved; }

    // What startSession() runs: program "" means the user's login shell.
    struct Launch {
        QString program;
        QStringList args;
        QString error;
        std::optional<SessionConfig> serial; // set: open this serial port instead of a PTY
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

    // Vault-backed SSH password for this session's next start (tests).
    AskpassServer *askpassServer() const { return m_askpass; }
    // Session > Send Stored Login (serial): user + Enter, then the password once
    // a "password" prompt arrives (never typed blind). False if not started.
    bool sendStoredLogin();
    bool loginPending() const { return m_loginWait != nullptr; }

    // Session > Start/Stop Logging (Ctrl+Shift+G); also automatic for saved
    // sessions with "Log this session automatically".
    bool startLogging(QString *error = nullptr);
    void stopLogging();
    SessionLog *sessionLog() const { return m_log.get(); }
    QLabel *recIndicator() const { return m_recLabel; }
    // Multi-line paste confirmation: true = go ahead. "Don't ask again" lasts
    // for this window's session.
    bool confirmPaste(const QString &text);
    bool pasteConfirmSkipped() const { return m_skipPasteConfirm; }

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    bool m_skipPasteConfirm = false;
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
    void showSerialBanner(const QString &text);
    void onSerialDisconnected(const QString &reason);
    void updatePasteBar(qint64 remaining);
    bool launch(const QStringList &args);
    QStringList prepareStoredPassword();
    void updateVaultActions();
    void onSerialData(const QByteArray &d);
    void finishLogin(bool sendPassword);
    void logOutput(const QByteArray &d, bool fromPty);
    void updateLoggingUi();

    LaunchRequest m_request;
    std::optional<SessionConfig> m_saved;
    SessionStore m_store;
    Launcher m_launcher;
    QStringList m_originalArgs;
    AppSettings m_settings;
    Terminal *m_term = nullptr;
    Pty *m_pty = nullptr;
    SerialBackend *m_serial = nullptr;
    QWidget *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    QWidget *m_pasteBar = nullptr;
    QLabel *m_pasteText = nullptr;
    TerminalView *m_view = nullptr;
    QMenu *m_contextMenu = nullptr;
    QActionGroup *m_schemeGroup = nullptr;
    QFileSystemWatcher *m_settingsWatcher = nullptr;
    QTimer *m_reloadTimer = nullptr;
    QList<QAction *> m_actions;
    AskpassServer *m_askpass = nullptr;
    QByteArray m_serialTail; // last bytes received (prompt detection for Send Stored Login)
    QTimer *m_loginWait = nullptr;
    std::unique_ptr<SessionLog> m_log;
    QLabel *m_recLabel = nullptr;
};

} // namespace zterminal
