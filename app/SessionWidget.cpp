#include "SessionWidget.hpp"

#include "AskpassServer.hpp"
#include "PasteConfirmDialog.hpp"
#include "PasteGuard.hpp"
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
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <sodium.h>

#include <algorithm>

namespace zterminal {

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
    auto *reconnect = new QPushButton(QStringLiteral("&Reconnect"));
    reconnect->setObjectName(QStringLiteral("reconnect"));
    reconnect->setFocusPolicy(Qt::NoFocus);
    connect(reconnect, &QPushButton::clicked, this, [this]() { startSession(); });
    bannerRow->addWidget(m_bannerText, 1);
    bannerRow->addWidget(reconnect, 0, Qt::AlignTop);
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
        return;
    }
    const QSize grid(m_view->gridCols(), m_view->gridRows());
    const QStringList env = prepareStoredPassword();
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
}

QStringList SessionWidget::prepareStoredPassword()
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
    m_term->feed(QStringLiteral("\r\n\x1b[41;97m[%1]\x1b[0m\r\n").arg(reason).toUtf8());
    showBanner(QStringLiteral("Disconnected: %1 Plug it back in and press Reconnect.").arg(reason));
    updatePasteBar(0);
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
    if (isSerialSession()) {
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
