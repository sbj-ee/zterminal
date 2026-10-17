#pragma once

#include "AppSettings.hpp"
#include "CommandLine.hpp"
#include "GroupStore.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"
#include "SessionWidget.hpp"

#include <functional>
#include <optional>

#include <QMainWindow>

class QAction;
class QActionGroup;
class QFileSystemWatcher;
class QLabel;
class QTabWidget;
class QTimer;
class QMenu;

namespace zterminal {

class AskpassServer;
class ThemeEditorDialog;
class Pty;
class SessionLog;
class SerialBackend;
class Terminal;
class TerminalView;

// A window with one or more tabs; each tab is an independent SessionWidget
// (local shell, SSH or serial). Owns the classic menu bar (docs/PLAN.md §3);
// menu actions act on the current tab. No split panes.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const LaunchRequest &request, const QStringList &originalArgs,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    // ---- tabs ----
    QTabWidget *tabs() const { return m_tabs; }
    int tabCount() const;
    SessionWidget *currentSession() const;
    SessionWidget *sessionAt(int index) const;
    // Adds a tab for `request` (args = what Duplicate re-opens), makes it current
    // (unless activate is false: a background tab, as a group opens its members)
    // and, with start, starts its session.
    SessionWidget *addTab(const LaunchRequest &request, const QStringList &args, bool start = true,
                          bool activate = true,
                          SessionWidget::StartMode mode = SessionWidget::StartMode::Interactive);
    // Closes a tab; a live session asks first (unless force). The window closes
    // with its last tab. False if the user kept it.
    bool closeTab(int index, bool force = false);
    void nextTab();
    void previousTab();
    // Tabs whose process is running / serial port is open.
    int liveTabCount() const;

    // ---- the current tab (kept for callers and tests) ----
    void startSession();
    TerminalView *view() const;
    Terminal *terminal() const;
    Pty *pty() const;
    SerialBackend *serial() const;
    bool isSerialSession() const;
    QWidget *sessionBanner() const;
    QWidget *serialBanner() const { return sessionBanner(); }
    QString serialBannerText() const;
    QWidget *pasteBar() const;
    QString sessionName() const;
    const std::optional<SessionConfig> &savedSession() const;
    using Launch = SessionWidget::Launch;
    Launch launchCommand() const;

    // File > New Session / Open Saved Session: opens the session dialog; the
    // chosen session opens in a new tab.
    void showSessionDialog(bool focusSaved);
    // File > Export / Import Sessions (JSON; no vault secrets).
    void exportSessionsInteractive();
    void importSessionsInteractive();
    bool openInNewTab(const SessionConfig &cfg, QString *error = nullptr);
    bool saveCurrentSessionAs(const QString &name, QString *error = nullptr);
    std::optional<SessionConfig> currentSessionConfig(QString *why = nullptr) const;

    // ---- groups (docs/PLAN.md §4.20) ----
    // File > Open Group > NAME: opens a saved group (GroupOpener). The tabs go
    // to a new window, or to this one when the group says so. nullptr (and
    // *error) when nothing can be opened.
    MainWindow *openGroup(const QString &name, QString *error = nullptr);
    // File > Manage Groups...: the group dialog; Open there opens the group.
    void showGroupDialog();
    // Ctrl+Shift+P: filter box + list of groups; Enter opens the chosen one.
    void showGroupPicker();
    QMenu *openGroupMenu() const { return m_groupMenu; }
    // Session > Reconnect All Disconnected: one vault unlock, then every
    // dropped tab reconnects (unattended). Returns how many were reconnected.
    int reconnectAllDisconnected();
    // Tabs whose last output line is a password / yes-no prompt.
    int needsInputCount() const;
    QLabel *needsInputIndicator() const { return m_inputLabel; }

    // A new window for `args` (zterminal's command line) in this process, so it
    // shares the running app's unlocked vault. nullptr (and *error) if invalid.
    static MainWindow *openWindow(const QStringList &args, QString *error = nullptr);
    // How File > New Window opens a window (tests replace it). Default:
    // openWindow() in this process (until 0.7.0: a new zterminal process).
    using Launcher = std::function<bool(const QString &program, const QStringList &args)>;
    void setLauncher(Launcher l) { m_launcher = std::move(l); }
    QAction *action(const QString &name) const;
    QMenu *contextMenu() const { return m_contextMenu; }

    const AppSettings &settings() const { return m_settings; }
    void setSettings(const AppSettings &s);
    void reloadSettings();
    void showPreferences();
    // Settings > Theme Editor (non-modal; returned for tests).
    ThemeEditorDialog *showThemeEditor();

    AskpassServer *askpassServer() const;
    bool sendStoredLogin();
    bool loginPending() const;
    bool startLogging(QString *error = nullptr);
    void stopLogging();
    SessionLog *sessionLog() const;
    // Window-level "● REC" marker: shows the *current* tab's logging state
    // (other logging tabs carry "● " in their tab text).
    QLabel *recIndicator() const { return m_recLabel; }
    // Status-bar vault marker: a padlock (closed = locked, open = unlocked)
    // and, while unlocked, how long it stays so: "\u221E" when the idle
    // auto-lock is off, else the timeout. Both hidden while there is no vault.
    QLabel *vaultIndicatorIcon() const { return m_vaultIcon; }
    QLabel *vaultIndicatorText() const { return m_vaultText; }
    bool confirmPaste(const QString &text);
    bool pasteConfirmSkipped() const;

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void buildMenus();
    QAction *addAct(QMenu *menu, const QString &name, const QString &text,
                    const QKeySequence &shortcut = {}, bool enabled = true,
                    const QString &plannedNote = {});
    void applySettings();
    void updateTitle();
    void updateTabText(SessionWidget *s);
    void onCurrentTabChanged();
    void updateSessionActions();
    void setFontSize(int points);
    void setSchemeFor(const QString &id);
    void rebuildSchemeMenu();
    void rebuildGroupMenu();
    void updateNeedsInput();
    void updateSavedEverywhere(const SessionConfig &cfg);
    void duplicateSession();
    void showAbout();
    void watchSettingsFile();
    void setMenuBarShown(bool shown);
    void saveSessionInteractive();
    void updateVaultActions();
    void updateVaultIndicator();
    void updateLoggingUi();

    SessionStore m_store;
    GroupStore m_groups;
    Launcher m_launcher;
    AppSettings m_settings;
    QTabWidget *m_tabs = nullptr;
    QMenu *m_contextMenu = nullptr;
    QActionGroup *m_schemeGroup = nullptr;
    QMenu *m_schemeMenu = nullptr;
    QMenu *m_groupMenu = nullptr;
    QLabel *m_inputLabel = nullptr;
    QAction *m_themeEditorAction = nullptr;
    QFileSystemWatcher *m_settingsWatcher = nullptr;
    QTimer *m_reloadTimer = nullptr;
    QList<QAction *> m_actions;
    QSet<int> m_reservedKeys;
    QLabel *m_recLabel = nullptr;
    QLabel *m_vaultIcon = nullptr;
    QLabel *m_vaultText = nullptr;
    bool m_closingConfirmed = false;
};

} // namespace zterminal
