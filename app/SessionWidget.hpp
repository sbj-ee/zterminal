#pragma once

#include "AppSettings.hpp"
#include "CommandLine.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"

#include <memory>
#include <optional>

#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace zterminal {

class AskpassServer;
class Pty;
class SerialBackend;
class SessionLog;
class Terminal;
class TerminalView;

// One tab = one independent session (docs/PLAN.md §4.15): its own emulator,
// view, PTY or serial port, log, askpass server, Send Stored Login state and
// paste-confirmation "don't ask again". Layout, top to bottom:
//   [session banner]  red bar with a message and Reconnect (serial errors now;
//                     the keepalive/reconnect work reuses it for "Disconnected")
//   [paste bar]       "Sending paste slowly... [Cancel]" while paced output is queued
//   terminal view
class SessionWidget : public QWidget
{
    Q_OBJECT
public:
    SessionWidget(const LaunchRequest &request, const QStringList &originalArgs, const AppSettings &settings,
                  QWidget *parent = nullptr);
    ~SessionWidget() override;

    void startSession();
    void restartSession();
    // Stop logging, close the port / end the process (closing the tab).
    void shutdown();
    // A process is running or a serial port is open: closing needs a confirmation.
    bool isLive() const;

    TerminalView *view() const { return m_view; }
    Terminal *terminal() const { return m_term; }
    Pty *pty() const { return m_pty; }
    SerialBackend *serial() const { return m_serial; }
    bool isSerialSession() const;
    QString sessionName() const { return m_saved ? m_saved->name : m_request.displayName(); }
    const std::optional<SessionConfig> &savedSession() const { return m_saved; }
    // The saved session changed on disk (font/scheme override): reload and apply.
    void setSavedSession(const SessionConfig &cfg);
    const LaunchRequest &request() const { return m_request; }
    QStringList originalArgs() const { return m_originalArgs; }

    // Banner (generic: message + Reconnect).
    QWidget *banner() const { return m_banner; }
    QString bannerText() const;
    void showBanner(const QString &text);
    void hideBanner();
    QWidget *pasteBar() const { return m_pasteBar; }

    struct Launch {
        QString program;
        QStringList args;
        QString error;
        std::optional<SessionConfig> serial; // set: open this serial port instead of a PTY
    };
    Launch launchCommand() const;
    std::optional<SessionConfig> currentSessionConfig(QString *why = nullptr) const;
    bool saveAs(const QString &name, QString *error = nullptr);

    void applySettings(const AppSettings &s);
    const AppSettings &settings() const { return m_settings; }

    AskpassServer *askpassServer() const { return m_askpass; }
    bool sendStoredLogin();
    bool loginPending() const { return m_loginWait != nullptr; }

    bool startLogging(QString *error = nullptr);
    void stopLogging();
    SessionLog *sessionLog() const { return m_log.get(); }
    bool isLogging() const;

    bool confirmPaste(const QString &text);
    bool pasteConfirmSkipped() const { return m_skipPasteConfirm; }

signals:
    void titleChanged();         // program title (OSC 0/2) or session name changed
    void loggingChanged(bool on);
    void pendingChanged(qint64 remaining);
    void contextMenuRequested(const QPoint &globalPos);

private:
    void onSessionFinished(int exitCode, bool crashed);
    void onSerialDisconnected(const QString &reason);
    void updatePasteBar(qint64 remaining);
    QStringList prepareStoredPassword();
    void onSerialData(const QByteArray &d);
    void finishLogin(bool sendPassword);
    void logOutput(const QByteArray &d, bool fromPty);

    LaunchRequest m_request;
    QStringList m_originalArgs;
    std::optional<SessionConfig> m_saved;
    SessionStore m_store;
    AppSettings m_settings;
    Terminal *m_term = nullptr;
    Pty *m_pty = nullptr;
    SerialBackend *m_serial = nullptr;
    QWidget *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    QWidget *m_pasteBar = nullptr;
    QLabel *m_pasteText = nullptr;
    TerminalView *m_view = nullptr;
    AskpassServer *m_askpass = nullptr;
    QByteArray m_serialTail; // last bytes received (prompt detection for Send Stored Login)
    QTimer *m_loginWait = nullptr;
    std::unique_ptr<SessionLog> m_log;
    bool m_skipPasteConfirm = false;
};

} // namespace zterminal
