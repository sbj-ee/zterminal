#include "SessionWidget.hpp"

#include "FindBar.hpp"

#include "AskpassServer.hpp"
#include "PasteConfirmDialog.hpp"
#include "PasteGuard.hpp"
#include "Reconnect.hpp"
#include "Pty.hpp"
#include "SecureBuffer.hpp"
#include "SerialBackend.hpp"
#include "SessionLog.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "Vault.hpp"
#include "VaultManager.hpp"
#include "ColorScheme.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <sodium.h>

#include <algorithm>

namespace zterminal {

namespace {
int s_stableMsValue = 5000;
} // namespace

SessionWidget::SessionWidget(const LaunchRequest &request, const QStringList &originalArgs,
                             const AppSettings &settings, QWidget *parent)
    : QWidget(parent)
    , m_request(request)
    , m_originalArgs(originalArgs)
    , m_settings(settings)
    , m_log(std::make_unique<SessionLog>())
{
    setObjectName(QStringLiteral("sessionWidget"));
    if (m_request.kind == LaunchRequest::Kind::SavedSession) {
        m_saved = m_store.load(m_request.sessionName);
    }
    m_term = new Terminal(24, 80, this);
    m_pty = new Pty(this);
    m_serial = new SerialBackend(this);
    m_view = new TerminalView(m_term, this);

    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    m_banner = new QWidget;
    m_banner->setObjectName(QStringLiteral("sessionBanner"));
    m_banner->setAutoFillBackground(true);
    m_banner->setStyleSheet(QStringLiteral("#sessionBanner { background: #7b1d1d; } #sessionBanner QLabel { color: white; }"));
    auto *bannerRow = new QHBoxLayout(m_banner);
    bannerRow->setContentsMargins(8, 4, 8, 4);
    m_bannerText = new QLabel;
    m_bannerText->setObjectName(QStringLiteral("bannerText"));
    m_bannerText->setWordWrap(true);
    m_bannerText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bannerText->setTextFormat(Qt::RichText);
    auto *reconnect = new QPushButton(QStringLiteral("&Reconnect"));
    reconnect->setObjectName(QStringLiteral("reconnect"));
    reconnect->setFocusPolicy(Qt::NoFocus);
    connect(reconnect, &QPushButton::clicked, this, &SessionWidget::reconnectNow);
    m_reconnectButton = reconnect;
    m_cancelReconnectButton = new QPushButton(QStringLiteral("Cancel"));
    m_cancelReconnectButton->setObjectName(QStringLiteral("cancelReconnect"));
    m_cancelReconnectButton->setFocusPolicy(Qt::NoFocus);
    m_cancelReconnectButton->hide();
    connect(m_cancelReconnectButton, &QPushButton::clicked, this, &SessionWidget::cancelReconnect);
    bannerRow->addWidget(m_bannerText, 1);
    bannerRow->addWidget(reconnect, 0, Qt::AlignTop);
    bannerRow->addWidget(m_cancelReconnectButton, 0, Qt::AlignTop);
    m_banner->hide();
    m_pasteBar = new QWidget;
    m_pasteBar->setObjectName(QStringLiteral("pasteBar"));
    m_pasteBar->setAutoFillBackground(true);
    m_pasteBar->setStyleSheet(QStringLiteral("#pasteBar { background: #5c4a00; } #pasteBar QLabel { color: white; }"));
    auto *pasteRow = new QHBoxLayout(m_pasteBar);
    pasteRow->setContentsMargins(8, 2, 8, 2);
    m_pasteText = new QLabel;
    auto *cancelPaste = new QPushButton(QStringLiteral("Cancel"));
    cancelPaste->setObjectName(QStringLiteral("cancelPaste"));
    cancelPaste->setFocusPolicy(Qt::NoFocus);
    connect(cancelPaste, &QPushButton::clicked, m_serial, &SerialBackend::cancelPending);
    pasteRow->addWidget(m_pasteText, 1);
    pasteRow->addWidget(cancelPaste);
    m_pasteBar->hide();
    col->addWidget(m_banner);
    col->addWidget(m_pasteBar);
    col->addWidget(m_view, 1);
    m_findBar = new FindBar(m_term, m_view, this);
    col->addWidget(m_findBar);
    connect(m_findBar, &FindBar::closed, this, [this]() { m_view->setFocus(); });
    setFocusProxy(m_view);

    // Keys/pastes go to whichever backend this tab runs.
    connect(m_term, &Terminal::output, this, [this](const QByteArray &d) {
        if (m_serial->isOpen()) {
            m_serial->write(d);
        } else {
            m_pty->write(d);
        }
    });
    connect(m_serial, &SerialBackend::dataReceived, m_term, &Terminal::feed);
    connect(m_serial, &SerialBackend::dataReceived, this, &SessionWidget::onSerialData);
    connect(m_serial, &SerialBackend::dataReceived, this, [this](const QByteArray &d) { logOutput(d, false); });
    connect(m_serial, &SerialBackend::disconnected, this, &SessionWidget::onSerialDisconnected);
    connect(m_serial, &SerialBackend::pendingChanged, this, &SessionWidget::updatePasteBar);
    connect(m_pty, &Pty::dataReceived, m_term, &Terminal::feed);
    connect(m_pty, &Pty::dataReceived, this, [this](const QByteArray &d) { logOutput(d, true); });
    connect(m_pty, &Pty::finished, this, &SessionWidget::onSessionFinished);
    connect(m_pty, &Pty::dataReceived, this, [this](const QByteArray &d) {
        m_ptyTail += d;
        if (m_ptyTail.size() > 4096) {
            m_ptyTail.remove(0, m_ptyTail.size() - 4096);
        }
    });
    m_reconnect = new ReconnectScheduler(this);
    connect(m_reconnect, &ReconnectScheduler::countdown, this, [this]() { updateDisconnectBanner(); });
    connect(m_reconnect, &ReconnectScheduler::attemptDue, this, &SessionWidget::onAttemptDue);
    m_stable = new QTimer(this);
    m_stable->setSingleShot(true);
    connect(m_stable, &QTimer::timeout, this, [this]() {
        if (m_pty->isRunning()) {
            markReconnected();
        }
    });
    m_deviceWatch = new QTimer(this);
    m_deviceWatch->setInterval(500);
    connect(m_deviceWatch, &QTimer::timeout, this, &SessionWidget::checkDeviceBack);
    connect(m_view, &TerminalView::gridSizeChanged, m_pty, &Pty::resize);
    connect(m_view, &TerminalView::gridSizeChanged, this, [this](int, int cols) { m_log->setColumns(cols); });
    m_view->setPasteGuard([this](const QString &t) { return confirmPaste(t); });
    connect(m_term, &Terminal::titleChanged, this, &SessionWidget::titleChanged);
    connect(m_term, &Terminal::bell, this, []() { QApplication::beep(); });
    connect(m_view, &TerminalView::contextMenuRequested, this, &SessionWidget::contextMenuRequested);
    // Each tab pauses its own log while a vault dialog is open.
    connect(&VaultManager::instance(), &VaultManager::dialogOpenChanged, this, [this](bool open) {
        open ? m_log->suspend(QStringLiteral("vault dialog open")) : m_log->resume(QStringLiteral("vault dialog open"));
    });
    applySettings(m_settings);
}

SessionWidget::~SessionWidget()
{
    shutdown();
}

void SessionWidget::shutdown()
{
    m_reconnect->cancel();
    m_deviceWatch->stop();
    m_stable->stop();
    m_log->stop();
    m_serial->close();
    m_pty->terminate();
}

bool SessionWidget::isLive() const
{
    return m_pty->isRunning() || m_serial->isOpen();
}

bool SessionWidget::isLogging() const
{
    return m_log->isActive();
}

void SessionWidget::setSavedSession(const SessionConfig &cfg)
{
    m_saved = cfg;
    applySettings(m_settings);
    emit titleChanged();
}

void SessionWidget::applySettings(const AppSettings &s)
{
    m_settings = s;
    // Preferences, with this saved session's font/scheme overrides on top.
    AppSettings eff = m_settings;
    if (m_saved) {
        if (!m_saved->fontFamily.isEmpty()) {
            eff.fontFamily = m_saved->fontFamily;
        }
        if (m_saved->fontSize > 0) {
            eff.fontSize = m_saved->fontSize;
        }
        if (!m_saved->colorScheme.isEmpty()) {
            eff.colorScheme = m_saved->colorScheme;
        }
    }
    m_view->setTerminalFont(eff.font());
    m_view->setMouseSettings(m_settings.mouse);
    m_view->setTrimCopiedWhitespace(m_settings.trimCopiedWhitespace);
    m_term->setScrollbackLimit(m_settings.scrollbackLines);
    m_term->setColorScheme(ColorScheme::byId(eff.colorScheme));
}

QString SessionWidget::bannerText() const
{
    return m_banner->isHidden() ? QString() : m_bannerText->text();
}

void SessionWidget::showBanner(const QString &text)
{
    m_bannerText->setText(text);
    m_banner->show();
}

void SessionWidget::hideBanner()
{
    m_banner->hide();
}

bool SessionWidget::saveAs(const QString &name, QString *error)
{
    auto cfg = currentSessionConfig(error);
    if (!cfg) {
        return false;
    }
    cfg->name = name.trimmed();
    if (!m_store.save(*cfg, error)) {
        return false;
    }
    m_saved = cfg;
    m_request = LaunchRequest{};
    m_request.kind = LaunchRequest::Kind::SavedSession;
    m_request.sessionName = cfg->name;
    m_originalArgs = {cfg->name}; // Duplicate now opens the saved session
    emit titleChanged();
    return true;
}

bool SessionWidget::startLogging(QString *error)
{
    if (m_log->isActive()) {
        return true;
    }
    m_log->setColumns(m_view->gridCols());
    if (!m_log->start(m_settings.effectiveLogDirectory(), sessionName(), m_settings.logTimestamps)) {
        if (error) {
            *error = m_log->errorString();
        }
        return false;
    }
    if (VaultManager::instance().isDialogOpen()) {
        m_log->suspend(QStringLiteral("vault dialog open"));
    }
    if (m_loginWait) {
        m_log->suspend(QStringLiteral("Send Stored Login"));
    }
    emit loggingChanged(true);
    return true;
}

void SessionWidget::stopLogging()
{
    m_log->stop();
    emit loggingChanged(false);
}

void SessionWidget::logOutput(const QByteArray &d, bool fromPty)
{
    if (!m_log->isActive()) {
        return;
    }
    if (fromPty) {
        // Canonical + no-echo on our PTY = someone is typing a secret (sudo,
        // passwd, ssh's own prompt, zterminal-askpass's manual fallback):
        // keep whatever is printed meanwhile out of the log.
        static const QString why = QStringLiteral("password prompt (terminal echo off)");
        if (m_pty->isSecretInputMode()) {
            m_log->suspend(why);
            return;
        }
        m_log->resume(why);
    }
    m_log->feed(d);
}

SessionWidget::Launch SessionWidget::launchCommand() const
{
    Launch l;
    switch (m_request.kind) {
    case LaunchRequest::Kind::Ssh:
    case LaunchRequest::Kind::Command:
        l.program = m_request.program;
        l.args = m_request.args;
        break;
    case LaunchRequest::Kind::Serial: {
        SessionConfig s = *currentSessionConfig();
        if (const QString e = validateSerial(s); !e.isEmpty()) {
            l.error = e;
        } else {
            l.serial = s;
        }
        break;
    }
    case LaunchRequest::Kind::SavedSession:
        if (!m_saved) {
            l.error = m_store.unknownSessionMessage(m_request.sessionName);
            break;
        }
        switch (m_saved->type) {
        case SessionConfig::Type::Ssh: {
            const SshCommand c = buildSshCommand(*m_saved);
            if (!c.ok()) {
                l.error = QStringLiteral("session \"%1\": %2").arg(m_saved->name, c.error);
            } else {
                l.program = c.program;
                l.args = c.args;
            }
            break;
        }
        case SessionConfig::Type::Serial:
            if (const QString e = validateSerial(*m_saved); !e.isEmpty()) {
                l.error = QStringLiteral("session \"%1\": %2").arg(m_saved->name, e);
            } else {
                l.serial = *m_saved;
            }
            break;
        case SessionConfig::Type::LocalShell:
            break;
        }
        break;
    default:
        break; // local shell
    }
    return l;
}

void SessionWidget::startSession()
{
    if (m_saved && m_saved->autoLog && !m_log->isActive()) {
        QString err;
        if (!startLogging(&err)) {
            m_term->feed(QStringLiteral("\x1b[31mzterminal: logging not started: %1\x1b[0m\r\n").arg(err).toUtf8());
        }
    }
    const Launch l = launchCommand();
    if (!l.error.isEmpty()) {
        m_term->feed(QStringLiteral("\x1b[31mzterminal: %1\x1b[0m\r\n").arg(l.error).toUtf8());
        return;
    }
    if (l.serial) {
        const SessionConfig &s = *l.serial;
        if (!m_serial->open(s)) {
            m_term->feed(QStringLiteral("\x1b[31mzterminal: %1\x1b[0m\r\n")
                             .arg(QString(m_serial->errorString()).replace(QLatin1Char('\n'), QStringLiteral("\r\n")))
                             .toUtf8());
            showBanner(m_serial->errorString());
            return;
        }
        hideBanner();
        // Watch the stable by-id name when there is one (ttyUSB0 may come back as ttyUSB1).
        const QString alias = SerialBackend::byIdAlias(s.serialDevice);
        m_serialWatchPath = alias.isEmpty() ? s.serialDevice : alias;
        const QString line = QStringLiteral("%1 at %2 %3%4%5%6")
                                 .arg(s.serialDevice)
                                 .arg(s.baudRate)
                                 .arg(s.dataBits)
                                 .arg(s.parity.left(1).toUpper())
                                 .arg(s.stopBits)
                                 .arg(s.flowControl == QLatin1String("none") ? QString() : QStringLiteral(", ") + s.flowControl);
        m_term->feed(QStringLiteral("\x1b[2m[connected to %1]\x1b[0m\r\n")
                         .arg(line)
                         .toUtf8());
        if (m_disconnected) {
            markReconnected();
        }
        return;
    }
    const QSize grid(m_view->gridCols(), m_view->gridRows());
    const QStringList env = prepareStoredPasswordFor(!m_autoAttempt);
    m_ptyTail.clear();
    m_userStop = false;
    if (!m_pty->start(l.program, l.args, grid.height(), grid.width(), env)) {
        m_term->feed(QStringLiteral("\x1b[31mzterminal: could not start %1: %2\x1b[0m\r\n")
                         .arg(l.program.isEmpty() ? Pty::defaultShell() : l.program, m_pty->errorString())
                         .toUtf8());
        delete m_askpass;
        m_askpass = nullptr;
        return;
    }
    if (m_askpass) {
        m_askpass->setAllowedAncestor(m_pty->pid());
    }
    if (m_disconnected) {
        m_stable->start(s_stableMsValue);
    }
}

QStringList SessionWidget::prepareStoredPassword()
{
    return prepareStoredPasswordFor(true);
}

QStringList SessionWidget::prepareStoredPasswordFor(bool mayPrompt)
{
    delete m_askpass; // a restart gets a fresh one-shot server
    m_askpass = nullptr;
    if (!m_saved || m_saved->type != SessionConfig::Type::Ssh || !m_saved->useStoredPassword) {
        return {};
    }
    auto note = [this](const QString &m) {
        m_term->feed(QStringLiteral("\x1b[2m[zterminal: %1]\x1b[0m\r\n").arg(m).toUtf8());
    };
    const QString helper = AskpassServer::findHelper();
    if (helper.isEmpty()) {
        note(QStringLiteral("zterminal-askpass not found; ssh will ask for the password"));
        return {};
    }
    VaultManager &vm = VaultManager::instance();
    if (!vm.exists()) {
        note(QStringLiteral("no password vault yet; ssh will ask for the password"));
        return {};
    }
    // The vault is shared by every tab and window: while it is unlocked this
    // asks nothing (a reconnect reuses the stored password silently). An
    // automatic retry never pops up the unlock dialog; ssh asks in the tab.
    if (!vm.isUnlocked() && !mayPrompt) {
        note(QStringLiteral("vault locked; ssh will ask for the password (Reconnect asks to unlock)"));
        return {};
    }
    if (!vm.ensureUnlocked(this, QStringLiteral("Session \"%1\" uses a stored password.").arg(m_saved->name.toHtmlEscaped()))) {
        note(QStringLiteral("vault locked; ssh will ask for the password"));
        return {};
    }
    if (!vm.refresh()) {
        note(QStringLiteral("vault locked (%1); ssh will ask for the password").arg(vm.vault().lastError()));
        return {};
    }
    vm.touch(); // using a stored password counts as activity
    const SecureBuffer *secret =
        vm.vault().secret(Vault::secretKeyFor(QStringLiteral("ssh-password"), m_saved->name));
    if (!secret) {
        note(QStringLiteral("no stored password for this session; ssh will ask for it"));
        return {};
    }
    m_askpass = new AskpassServer(secret->clone(), this);
    QString err;
    if (!m_askpass->listen(&err)) {
        delete m_askpass;
        m_askpass = nullptr;
        note(QStringLiteral("can't offer the stored password (%1); ssh will ask for it").arg(err));
        return {};
    }
    connect(m_askpass, &AskpassServer::served, this, [note]() {
        note(QStringLiteral("stored password sent once; if it is rejected, ssh asks here"));
    });
    return m_askpass->sshEnvironment(helper);
}

namespace {
// The device is *currently* asking for a password: the last line received
// (no newline after it yet) mentions "password" and ends with ':'.
bool endsWithPasswordPrompt(const QByteArray &tail)
{
    const QByteArray t = tail.trimmed();
    const qsizetype nl = std::max(t.lastIndexOf('\n'), t.lastIndexOf('\r'));
    const QByteArray last = t.mid(nl + 1).toLower();
    return last.contains("assword") && last.endsWith(':');
}
} // namespace

void SessionWidget::onSerialData(const QByteArray &d)
{
    m_serialTail.append(d);
    if (m_serialTail.size() > 512) {
        m_serialTail.remove(0, m_serialTail.size() - 512);
    }
    if (m_loginWait && endsWithPasswordPrompt(m_serialTail)) {
        finishLogin(true);
    }
}

bool SessionWidget::sendStoredLogin()
{
    if (!m_saved || m_saved->type != SessionConfig::Type::Serial || !m_serial->isOpen() || m_loginWait) {
        return false;
    }
    VaultManager &vm = VaultManager::instance();
    if (!vm.exists()) {
        QMessageBox::information(this, QStringLiteral("Send Stored Login"),
                                 QStringLiteral("There is no password vault yet. Store a login password in File > New Session (Save)."));
        return false;
    }
    if (!vm.ensureUnlocked(this, QStringLiteral("Send Stored Login for \"%1\".").arg(m_saved->name.toHtmlEscaped()))) {
        return false;
    }
    if (!vm.refresh()) {
        QMessageBox::warning(this, QStringLiteral("Send Stored Login"), vm.vault().lastError());
        return false;
    }
    vm.touch();
    if (!vm.vault().secret(Vault::secretKeyFor(QStringLiteral("serial-password"), m_saved->name))) {
        QMessageBox::information(this, QStringLiteral("Send Stored Login"),
                                 QStringLiteral("No login password is stored for \"%1\".").arg(m_saved->name));
        return false;
    }
    m_log->suspend(QStringLiteral("Send Stored Login"));
    m_loginWait = new QTimer(this);
    m_loginWait->setSingleShot(true);
    m_loginWait->setInterval(10000);
    connect(m_loginWait, &QTimer::timeout, this, [this]() { finishLogin(false); });
    const bool promptShowing = endsWithPasswordPrompt(m_serialTail);
    m_serialTail.clear();
    if (promptShowing) {
        finishLogin(true);
        return true;
    }
    if (!m_saved->loginUser.isEmpty()) {
        m_serial->write(m_saved->loginUser.toUtf8() + '\r'); // write() maps CR to the session's Enter
    }
    m_loginWait->start();
    return true;
}

void SessionWidget::finishLogin(bool sendPassword)
{
    delete m_loginWait;
    m_loginWait = nullptr;
    // Resume a moment after the password went out, so a device that echoes it
    // (or '*'s) back isn't logged either.
    QTimer::singleShot(sendPassword ? 1500 : 0, this, [this]() { m_log->resume(QStringLiteral("Send Stored Login")); });
    if (!sendPassword) {
        m_term->feed(QByteArrayLiteral("\r\n\x1b[2m[zterminal: no password prompt within 10 s; stored password NOT sent]\x1b[0m\r\n"));
        return;
    }
    VaultManager &vm = VaultManager::instance();
    const SecureBuffer *pw = vm.isUnlocked() && m_saved
        ? vm.vault().secret(Vault::secretKeyFor(QStringLiteral("serial-password"), m_saved->name))
        : nullptr;
    if (!pw) {
        return;
    }
    vm.touch();
    // QSerialPort keeps its own write buffer (not locked memory); wipe our copy.
    QByteArray bytes(reinterpret_cast<const char *>(pw->data()), qsizetype(pw->size()));
    bytes += '\r';
    m_serial->write(bytes, /*echo=*/false);
    sodium_memzero(bytes.data(), std::size_t(bytes.size()));
    m_serialTail.clear(); // that prompt has been answered
}

void SessionWidget::onSessionFinished(int exitCode, bool crashed)
{
    m_stable->stop();
    const bool userStop = m_userStop;
    m_userStop = false;
    if (isSshSession() && !userStop) {
        const SshExit x = SshExit::classify(exitCode, crashed, m_ptyTail);
        if (x.isDrop()) {
            if (!m_disconnected) {
                markDisconnected(x.reason);
            } else {
                m_term->feed(QStringLiteral("\x1b[2m[reconnect attempt %1 failed: %2]\x1b[0m\r\n")
                                 .arg(std::max(1, m_reconnect->attempt()))
                                 .arg(x.reason)
                                 .toUtf8());
            }
            if (autoReconnectEnabled()) {
                m_reconnect->scheduleNext();
            }
            updateDisconnectBanner();
            return;
        }
        if (m_disconnected && x.kind == SshExit::Kind::Refused) {
            // Retrying can't fix this (password changed, host key, ...): stop.
            m_reconnect->cancel();
            m_term->feed(QStringLiteral("\x1b[2m[reconnect failed: %1]\x1b[0m\r\n").arg(x.reason).toUtf8());
            updateDisconnectBanner(QStringLiteral("Reconnect failed: %1").arg(x.reason.toHtmlEscaped()));
            return;
        }
        if (x.kind == SshExit::Kind::Clean) {
            // exit/logout: never reconnect.
            m_reconnect->cancel();
            if (m_disconnected) {
                m_disconnected = false;
                hideBanner();
            }
        }
    }
    const QString msg = crashed
        ? QStringLiteral("\r\n\x1b[7m[process terminated (%1). Session > Restart Session (Ctrl+Shift+R) restarts it.]\x1b[0m\r\n")
              .arg(exitCode)
        : QStringLiteral("\r\n\x1b[7m[process exited with code %1. Session > Restart Session (Ctrl+Shift+R) restarts it.]\x1b[0m\r\n")
              .arg(exitCode);
    m_term->feed(msg.toUtf8());
}

bool SessionWidget::confirmPaste(const QString &text)
{
    if (!m_settings.confirmMultilinePaste || m_skipPasteConfirm) {
        return true;
    }
    const PasteInfo info = PasteInfo::analyze(text);
    if (!info.needsConfirm()) {
        return true;
    }
    PasteConfirmDialog::Context ctx;
    ctx.bracketedPaste = m_term->bracketedPasteEnabled();
    if (m_serial->isOpen() && m_serial->pacingEnabled()) {
        ctx.serialPaced = true;
        ctx.charDelayMs = m_serial->config().charDelayMs;
        ctx.lineDelayMs = m_serial->config().lineDelayMs;
        ctx.pacedMs = PasteInfo::pacedMilliseconds(Terminal::preparePasteBytes(text), ctx.charDelayMs,
                                                   ctx.lineDelayMs);
    }
    PasteConfirmDialog dlg(info, ctx, this);
    const bool ok = dlg.exec() == QDialog::Accepted;
    if (ok && dlg.dontAskAgain()) {
        m_skipPasteConfirm = true;
    }
    m_view->setFocus();
    return ok;
}

bool SessionWidget::isSerialSession() const
{
    return m_request.kind == LaunchRequest::Kind::Serial
        || (m_saved && m_saved->type == SessionConfig::Type::Serial);
}

void SessionWidget::onSerialDisconnected(const QString &reason)
{
    updatePasteBar(0);
    markDisconnected(reason);
    // Reopen by itself as soon as the device node is back.
    m_deviceWatch->start();
    updateDisconnectBanner();
}

bool SessionWidget::isWaitingForDevice() const
{
    return m_deviceWatch->isActive();
}

void SessionWidget::checkDeviceBack()
{
    if (m_serial->isOpen()) {
        m_deviceWatch->stop();
        return;
    }
    if (m_serialWatchPath.isEmpty() || !QFileInfo::exists(m_serialWatchPath)) {
        return;
    }
    const Launch l = launchCommand();
    if (!l.serial) {
        m_deviceWatch->stop();
        return;
    }
    SessionConfig cfg = *l.serial;
    cfg.serialDevice = m_serialWatchPath;
    // The node can appear a moment before udev makes it accessible: keep trying.
    if (!m_serial->open(cfg)) {
        return;
    }
    m_deviceWatch->stop();
    m_term->feed(QStringLiteral("\x1b[2m[connected to %1 again]\x1b[0m\r\n").arg(m_serialWatchPath).toUtf8());
    markReconnected();
}

bool SessionWidget::isSshSession() const
{
    return m_request.kind == LaunchRequest::Kind::Ssh || (m_saved && m_saved->type == SessionConfig::Type::Ssh);
}

bool SessionWidget::autoReconnectEnabled() const
{
    return m_saved && m_saved->autoReconnect;
}

void SessionWidget::setReconnectStableMsForTests(int ms)
{
    s_stableMsValue = ms;
}

void SessionWidget::markDisconnected(const QString &reason)
{
    m_disconnected = true;
    m_droppedAt = QDateTime::currentDateTime();
    m_dropReason = reason;
    m_log->marker(QStringLiteral("disconnected"), reason);
    m_term->feed(QStringLiteral("\r\n\x1b[41;97m[disconnected %1: %2]\x1b[0m\r\n")
                     .arg(m_droppedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), reason)
                     .toUtf8());
}

void SessionWidget::markReconnected()
{
    m_stable->stop();
    m_deviceWatch->stop();
    const int attempt = m_reconnect->attempt();
    m_reconnect->reset();
    m_autoAttempt = false;
    if (!m_disconnected) {
        return;
    }
    m_disconnected = false;
    ++m_reconnects;
    const QDateTime now = QDateTime::currentDateTime();
    m_log->marker(QStringLiteral("reconnected"),
                  attempt > 0 ? QStringLiteral("attempt %1").arg(attempt) : QString());
    m_term->feed(QStringLiteral("\x1b[2m[reconnected %1]\x1b[0m\r\n")
                     .arg(now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                     .toUtf8());
    hideBanner();
}

void SessionWidget::updateDisconnectBanner(const QString &note)
{
    if (!m_disconnected) {
        return;
    }
    QString status;
    bool cancellable = false;
    if (!note.isEmpty()) {
        status = note;
    } else if (m_reconnect->isWaiting()) {
        status = QStringLiteral("Reconnecting in %1 s (attempt %2)\u2026")
                     .arg(m_reconnect->secondsLeft())
                     .arg(m_reconnect->attempt());
        cancellable = true;
    } else if (m_pty->isRunning()) {
        status = m_reconnect->attempt() > 0
            ? QStringLiteral("Reconnecting (attempt %1)\u2026").arg(m_reconnect->attempt())
            : QStringLiteral("Reconnecting\u2026");
    } else if (isWaitingForDevice()) {
        status = QStringLiteral("Waiting for %1 to come back; it reopens by itself.").arg(m_serialWatchPath.toHtmlEscaped());
        cancellable = true;
    } else {
        status = QStringLiteral("Press Reconnect to connect again.");
    }
    const QString when = m_droppedAt.date() == QDate::currentDate()
        ? m_droppedAt.toString(QStringLiteral("HH:mm:ss"))
        : m_droppedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    showBanner(QStringLiteral("<b>Disconnected</b> at %1 \u2014 %2<br>%3")
                   .arg(when, m_dropReason.toHtmlEscaped(), status));
    m_reconnectButton->setText(cancellable ? QStringLiteral("&Reconnect Now") : QStringLiteral("&Reconnect"));
    m_reconnectButton->setEnabled(!m_pty->isRunning());
    m_cancelReconnectButton->setVisible(cancellable);
}

void SessionWidget::onAttemptDue(int attempt)
{
    if (!m_disconnected || m_pty->isRunning() || m_serial->isOpen()) {
        return;
    }
    m_term->feed(QStringLiteral("\x1b[2m[reconnecting, attempt %1\u2026]\x1b[0m\r\n").arg(attempt).toUtf8());
    // Leave whatever full-screen state the dropped session left behind
    // (alternate screen, mouse modes, bracketed paste); the scrollback stays.
    resetModesForReconnect();
    m_autoAttempt = true;
    startSession();
    m_autoAttempt = false;
    updateDisconnectBanner();
}

void SessionWidget::resetModesForReconnect()
{
    // Only leave the alternate screen if the dropped program was in it:
    // ?1049l also restores the saved cursor, which would overwrite the screen.
    if (m_term->altScreen()) {
        m_term->feed(QByteArrayLiteral("\x1b[?1049l"));
    }
    m_term->feed(QByteArrayLiteral("\x1b[0m\x1b[?25h\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?2004l"));
}

void SessionWidget::reconnectNow()
{
    if (m_pty->isRunning()) {
        return;
    }
    m_reconnect->cancel();
    if (isSerialSession()) {
        m_deviceWatch->stop();
        m_serial->close();
        startSession();
        if (m_disconnected && !m_serial->isOpen() && !m_serialWatchPath.isEmpty()) {
            m_deviceWatch->start(); // still gone: keep waiting
            updateDisconnectBanner();
        }
        return;
    }
    if (m_disconnected) {
        resetModesForReconnect();
    }
    startSession();
    updateDisconnectBanner();
}

void SessionWidget::cancelReconnect()
{
    m_reconnect->cancel();
    m_deviceWatch->stop();
    updateDisconnectBanner(QStringLiteral("Automatic reconnect cancelled. Press Reconnect to try again."));
}

void SessionWidget::updatePasteBar(qint64 remaining)
{
    const bool show = remaining > 0 && m_serial->pacingEnabled();
    m_pasteBar->setVisible(show);
    emit pendingChanged(remaining);
    if (show) {
        m_pasteText->setText(QStringLiteral("Sending paste slowly (%1 ms/char, %2 ms/line)\u2026 %3 bytes left")
                                 .arg(m_serial->config().charDelayMs)
                                 .arg(m_serial->config().lineDelayMs)
                                 .arg(remaining));
    }
}

void SessionWidget::restartSession()
{
    m_reconnect->cancel();
    if (isSerialSession()) {
        m_deviceWatch->stop();
        // Reconnect: close and reopen the port.
        m_serial->close();
        m_term->feed(QByteArrayLiteral("\r\n"));
        startSession();
        return;
    }
    if (m_pty->isRunning()) {
        const auto ret = QMessageBox::question(this, QStringLiteral("Restart Session"),
                                               QStringLiteral("The session is still running. Terminate it and restart?"));
        if (ret != QMessageBox::Yes) {
            return;
        }
        m_userStop = true;
        m_pty->terminate();
    }
    m_term->feed(QByteArrayLiteral("\r\n"));
    startSession();
}

std::optional<SessionConfig> SessionWidget::currentSessionConfig(QString *why) const
{
    if (m_saved) {
        return m_saved;
    }
    switch (m_request.kind) {
    case LaunchRequest::Kind::LocalShell:
        return SessionConfig{};
    case LaunchRequest::Kind::Ssh:
        return sessionFromSshArgs(m_request.args, why);
    case LaunchRequest::Kind::Serial: {
        SessionConfig s;
        s.type = SessionConfig::Type::Serial;
        s.serialDevice = m_request.program;
        s.baudRate = m_request.baudRate;
        return s;
    }
    default:
        if (why) {
            *why = QStringLiteral("only local shell, SSH and serial sessions can be saved");
        }
        return std::nullopt;
    }
}

} // namespace zterminal

namespace zterminal {

void SessionWidget::openFind()
{
    m_findBar->open();
}

} // namespace zterminal
