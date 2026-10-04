#include "MainWindow.hpp"

#include "ColorScheme.hpp"
#include "PreferencesDialog.hpp"
#include "SessionDialog.hpp"
#include "Pty.hpp"
#include "SerialBackend.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "WindowTitle.hpp"
#include "version.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace zterminal {

namespace {
const char *kPlannedProperty = "zterminalPlanned";
}

MainWindow::MainWindow(const LaunchRequest &request, const QStringList &originalArgs, QWidget *parent)
    : QMainWindow(parent)
    , m_request(request)
    , m_originalArgs(originalArgs)
    , m_settings(AppSettings::load())
{
    m_launcher = [](const QString &program, const QStringList &args) {
        return QProcess::startDetached(program, args);
    };
    if (m_request.kind == LaunchRequest::Kind::SavedSession) {
        m_saved = m_store.load(m_request.sessionName);
    }
    m_term = new Terminal(24, 80, this);
    m_pty = new Pty(this);
    m_serial = new SerialBackend(this);
    m_view = new TerminalView(m_term, this);

    // Central area: [serial banner] [paste bar] terminal.
    auto *central = new QWidget(this);
    auto *col = new QVBoxLayout(central);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    m_banner = new QWidget;
    m_banner->setObjectName(QStringLiteral("serialBanner"));
    m_banner->setAutoFillBackground(true);
    m_banner->setStyleSheet(QStringLiteral("#serialBanner { background: #7b1d1d; } #serialBanner QLabel { color: white; }"));
    auto *bannerRow = new QHBoxLayout(m_banner);
    bannerRow->setContentsMargins(8, 4, 8, 4);
    m_bannerText = new QLabel;
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
    setCentralWidget(central);

    // Always draw the menu bar inside the window. Without this, Qt hands the
    // menus to a global-menu service whenever com.canonical.AppMenu.Registrar
    // is on the session bus (e.g. the Fildem GNOME extension), hides the
    // in-window bar, and on GNOME nothing may show the exported menus.
    menuBar()->setNativeMenuBar(false);

    // Keys/pastes go to whichever backend this window runs.
    connect(m_term, &Terminal::output, this, [this](const QByteArray &d) {
        if (m_serial->isOpen()) {
            m_serial->write(d);
        } else {
            m_pty->write(d);
        }
    });
    connect(m_serial, &SerialBackend::dataReceived, m_term, &Terminal::feed);
    connect(m_serial, &SerialBackend::disconnected, this, &MainWindow::onSerialDisconnected);
    connect(m_serial, &SerialBackend::pendingChanged, this, &MainWindow::updatePasteBar);
    connect(m_pty, &Pty::dataReceived, m_term, &Terminal::feed);
    connect(m_pty, &Pty::finished, this, &MainWindow::onSessionFinished);
    connect(m_view, &TerminalView::gridSizeChanged, m_pty, &Pty::resize);
    connect(m_term, &Terminal::titleChanged, this, &MainWindow::updateTitle);
    connect(m_term, &Terminal::bell, this, []() { QApplication::beep(); });
    connect(m_view, &TerminalView::contextMenuRequested, this, &MainWindow::showContextMenu);

    buildMenus();
    applySettings();
    watchSettingsFile();
    updateTitle();
    resize(m_view->sizeHint() + QSize(0, menuBar()->sizeHint().height()));
    m_view->setFocus();
}

MainWindow::~MainWindow()
{
    m_serial->close();
    m_pty->terminate();
}

QAction *MainWindow::addAct(QMenu *menu, const QString &name, const QString &text,
                            const QKeySequence &shortcut, bool enabled, const QString &plannedNote)
{
    auto *a = new QAction(text, this);
    a->setObjectName(name);
    if (!shortcut.isEmpty()) {
        a->setShortcut(shortcut);
        a->setShortcutContext(Qt::WindowShortcut);
    }
    a->setEnabled(enabled);
    if (!plannedNote.isEmpty()) {
        a->setProperty(kPlannedProperty, true);
        a->setToolTip(plannedNote);
        a->setStatusTip(plannedNote);
    }
    menu->addAction(a);
    addAction(a); // shortcuts keep working while the menu bar is hidden
    m_actions << a;
    return a;
}

QAction *MainWindow::action(const QString &name) const
{
    for (QAction *a : m_actions) {
        if (a->objectName() == name) {
            return a;
        }
    }
    return nullptr;
}

void MainWindow::buildMenus()
{
    using QKS = QKeySequence;
    const QString later = QStringLiteral("Planned for a later release (see docs/PLAN.md)");

    // File
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    connect(addAct(file, QStringLiteral("newSession"), QStringLiteral("&New Session\u2026"),
                   QKS(QStringLiteral("Ctrl+Shift+N"))),
            &QAction::triggered, this, [this]() { showSessionDialog(false); });
    connect(addAct(file, QStringLiteral("openSavedSession"), QStringLiteral("&Open Saved Session\u2026"),
                   QKS(QStringLiteral("Ctrl+Shift+O"))),
            &QAction::triggered, this, [this]() { showSessionDialog(true); });
    connect(addAct(file, QStringLiteral("saveSession"), QStringLiteral("&Save Session\u2026"),
                   QKS(QStringLiteral("Ctrl+Shift+S"))),
            &QAction::triggered, this, &MainWindow::saveSessionInteractive);
    file->addSeparator();
    connect(addAct(file, QStringLiteral("close"), QStringLiteral("&Close"), QKS(QStringLiteral("Ctrl+Shift+W"))),
            &QAction::triggered, this, &QWidget::close);
    connect(addAct(file, QStringLiteral("quit"), QStringLiteral("&Quit"), QKS(QStringLiteral("Ctrl+Shift+Q"))),
            &QAction::triggered, qApp, &QApplication::closeAllWindows);

    // Edit
    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    connect(addAct(edit, QStringLiteral("copy"), QStringLiteral("&Copy"), QKS(QStringLiteral("Ctrl+Shift+C"))),
            &QAction::triggered, m_view, &TerminalView::copySelection);
    connect(addAct(edit, QStringLiteral("paste"), QStringLiteral("&Paste"), QKS(QStringLiteral("Ctrl+Shift+V"))),
            &QAction::triggered, m_view, &TerminalView::pasteClipboard);
    connect(addAct(edit, QStringLiteral("pastePrimary"), QStringLiteral("Paste P&rimary"),
                   QKS(QStringLiteral("Shift+Insert"))),
            &QAction::triggered, m_view, &TerminalView::pastePrimary);
    edit->addSeparator();
    connect(addAct(edit, QStringLiteral("selectAll"), QStringLiteral("Select &All"),
                   QKS(QStringLiteral("Ctrl+Shift+A"))),
            &QAction::triggered, m_view, &TerminalView::selectAll);
    edit->addSeparator();
    addAct(edit, QStringLiteral("find"), QStringLiteral("&Find\u2026"), QKS(QStringLiteral("Ctrl+Shift+F")), false, later);

    // View
    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    // Shift changes the key Qt reports (= -> +, - -> _, 0 -> ), so register both forms.
    QAction *larger = addAct(view, QStringLiteral("fontLarger"), QStringLiteral("Font Size &Up"));
    larger->setShortcuts({QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal)});
    connect(larger, &QAction::triggered, this, [this]() { setFontSize(m_view->terminalFont().pointSize() + 1); });
    QAction *smaller = addAct(view, QStringLiteral("fontSmaller"), QStringLiteral("Font Size &Down"));
    smaller->setShortcuts({QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Underscore), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Minus)});
    connect(smaller, &QAction::triggered, this, [this]() { setFontSize(m_view->terminalFont().pointSize() - 1); });
    QAction *resetFont = addAct(view, QStringLiteral("fontReset"), QStringLiteral("&Reset Font Size"));
    resetFont->setShortcuts({QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_ParenRight), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_0)});
    connect(resetFont, &QAction::triggered, this, [this]() { setFontSize(AppSettings::kDefaultFontSize); });
    view->addSeparator();
    QMenu *schemes = view->addMenu(QStringLiteral("Color &Scheme"));
    m_schemeGroup = new QActionGroup(this);
    for (const ColorScheme &s : ColorScheme::builtIn()) {
        QAction *a = schemes->addAction(s.name);
        a->setObjectName(QStringLiteral("scheme:") + s.id);
        a->setCheckable(true);
        a->setData(s.id);
        m_schemeGroup->addAction(a);
        m_actions << a;
    }
    connect(m_schemeGroup, &QActionGroup::triggered, this, [this](QAction *a) {
        if (m_saved && !m_saved->colorScheme.isEmpty()) {
            // This session overrides the scheme: change (and save) the override.
            m_saved->colorScheme = a->data().toString();
            m_store.save(*m_saved);
            applySettings();
            return;
        }
        AppSettings s = m_settings;
        s.colorScheme = a->data().toString();
        setSettings(s);
    });
    view->addSeparator();
    QAction *full = addAct(view, QStringLiteral("fullScreen"), QStringLiteral("&Full Screen"), QKS(Qt::Key_F11));
    full->setCheckable(true);
    connect(full, &QAction::toggled, this, [this](bool on) {
        // Full screen changes only the window state; the menu bar keeps whatever
        // visibility the user chose (shown by default).
        on ? showFullScreen() : showNormal();
    });
    QAction *showMenu = addAct(view, QStringLiteral("showMenuBar"), QStringLiteral("Show &Menu Bar"),
                               QKS(QStringLiteral("Ctrl+Shift+M")));
    showMenu->setCheckable(true);
    showMenu->setChecked(true);
    connect(showMenu, &QAction::toggled, this, &MainWindow::setMenuBarShown);

    // Session
    QMenu *session = menuBar()->addMenu(QStringLiteral("&Session"));
    connect(addAct(session, QStringLiteral("duplicateSession"), QStringLiteral("&Duplicate Session"),
                   QKS(QStringLiteral("Ctrl+Shift+D"))),
            &QAction::triggered, this, &MainWindow::duplicateSession);
    connect(addAct(session, QStringLiteral("restartSession"), QStringLiteral("&Restart Session"),
                   QKS(QStringLiteral("Ctrl+Shift+R"))),
            &QAction::triggered, this, &MainWindow::restartSession);
    QAction *brk = addAct(session, QStringLiteral("sendBreak"), QStringLiteral("Send &Break"), {}, isSerialSession());
    brk->setToolTip(QStringLiteral("Holds the serial line in break (duration set per session; default 300 ms)"));
    if (!isSerialSession()) {
        brk->setToolTip(QStringLiteral("Serial sessions only"));
    }
    connect(brk, &QAction::triggered, this, [this]() {
        if (!m_serial->sendBreak() && !m_serial->isOpen()) {
            showSerialBanner(QStringLiteral("Not connected: Send Break needs an open serial port."));
        }
    });
    QAction *cancelPasteAct = addAct(session, QStringLiteral("cancelPaste"), QStringLiteral("Cancel &Paste"), {}, false);
    connect(cancelPasteAct, &QAction::triggered, m_serial, &SerialBackend::cancelPending);
    session->addSeparator();
    connect(addAct(session, QStringLiteral("clearScrollback"), QStringLiteral("&Clear Scrollback"),
                   QKS(QStringLiteral("Ctrl+Shift+K"))),
            &QAction::triggered, m_term, &Terminal::clearScrollback);
    connect(addAct(session, QStringLiteral("resetTerminal"), QStringLiteral("Reset &Terminal")),
            &QAction::triggered, m_term, &Terminal::reset);
    session->addSeparator();
    // Per-session settings are planned (M6); until then this opens Preferences.
    QAction *changeSettings = addAct(session, QStringLiteral("changeSettings"), QStringLiteral("Change &Settings\u2026"));
    changeSettings->setToolTip(QStringLiteral("Opens Preferences (per-session settings are planned)"));
    changeSettings->setStatusTip(changeSettings->toolTip());
    connect(changeSettings, &QAction::triggered, this, &MainWindow::showPreferences);

    // Settings
    QMenu *settings = menuBar()->addMenu(QStringLiteral("Se&ttings"));
    QAction *prefs = addAct(settings, QStringLiteral("preferences"), QStringLiteral("&Preferences\u2026"));
    // Ctrl+Shift+, (Qt reports Shift+comma as '<' on US layouts, so register both).
    prefs->setShortcuts({QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Comma), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Less)});
    connect(prefs, &QAction::triggered, this, &MainWindow::showPreferences);

    // Help
    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    connect(addAct(help, QStringLiteral("about"), QStringLiteral("&About zterminal")),
            &QAction::triggered, this, &MainWindow::showAbout);
    addAct(help, QStringLiteral("checkForUpdates"), QStringLiteral("Check for &Updates\u2026"), {}, false, later);

    // Ctrl+right-click menu (no context menu on plain right-click: that pastes).
    m_contextMenu = new QMenu(this);
    for (const char *n : {"copy", "paste", "selectAll", "find"}) {
        m_contextMenu->addAction(action(QString::fromLatin1(n)));
    }
    m_contextMenu->addSeparator();
    m_contextMenu->addAction(showMenu);
    m_contextMenu->addAction(full);
    m_contextMenu->addSeparator();
    for (const char *n : {"duplicateSession", "restartSession", "clearScrollback", "resetTerminal"}) {
        m_contextMenu->addAction(action(QString::fromLatin1(n)));
    }
    m_contextMenu->addSeparator();
    m_contextMenu->addAction(prefs); // reachable even when the menu bar is hidden

    // Only these key combinations are taken from the session.
    QSet<int> reserved;
    for (QAction *a : m_actions) {
        for (const QKeySequence &ks : a->shortcuts()) {
            if (!ks.isEmpty()) {
                reserved.insert(ks[0].toCombined());
            }
        }
    }
    m_view->setAppShortcuts(reserved);
}

void MainWindow::applySettings()
{
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
    m_term->setScrollbackLimit(m_settings.scrollbackLines);
    m_term->setColorScheme(ColorScheme::byId(eff.colorScheme));
    for (QAction *a : m_schemeGroup->actions()) {
        a->setChecked(a->data().toString() == m_term->colorScheme().id);
    }
}

void MainWindow::updateTitle()
{
    setWindowTitle(makeWindowTitle(sessionName(), m_term->title()));
}

void MainWindow::setFontSize(int points)
{
    if (m_saved && m_saved->fontSize > 0) {
        // This session overrides the size: zoom changes (and saves) the override.
        m_saved->fontSize = std::clamp(points, 6, 48);
        m_store.save(*m_saved);
        applySettings();
        return;
    }
    AppSettings s = m_settings;
    s.fontSize = std::clamp(points, 6, 48);
    setSettings(s);
}

void MainWindow::setSettings(const AppSettings &s)
{
    m_settings = s;
    m_settings.save();
    applySettings();
    watchSettingsFile(); // the file may have just been created or replaced
}

void MainWindow::reloadSettings()
{
    watchSettingsFile();
    const AppSettings fresh = AppSettings::load();
    if (fresh != m_settings) {
        m_settings = fresh;
        applySettings();
    }
}

void MainWindow::watchSettingsFile()
{
    // Each window is its own process, so changes made in one window reach the
    // others through the settings file. QSettings replaces the file atomically,
    // so watch the directory too and re-add the file after every change.
    if (!m_settingsWatcher) {
        m_settingsWatcher = new QFileSystemWatcher(this);
        m_reloadTimer = new QTimer(this);
        m_reloadTimer->setSingleShot(true);
        m_reloadTimer->setInterval(150);
        connect(m_reloadTimer, &QTimer::timeout, this, &MainWindow::reloadSettings);
        auto kick = [this]() { m_reloadTimer->start(); };
        connect(m_settingsWatcher, &QFileSystemWatcher::fileChanged, this, kick);
        connect(m_settingsWatcher, &QFileSystemWatcher::directoryChanged, this, kick);
    }
    const QString file = AppSettings::filePath();
    const QString dir = QFileInfo(file).absolutePath();
    QDir().mkpath(dir);
    if (!m_settingsWatcher->directories().contains(dir)) {
        m_settingsWatcher->addPath(dir);
    }
    if (QFileInfo::exists(file) && !m_settingsWatcher->files().contains(file)) {
        m_settingsWatcher->addPath(file);
    }
}

void MainWindow::setMenuBarShown(bool shown)
{
    // Hiding lasts for this window only: every new window starts with the menu
    // bar visible, so a hidden bar can never be "stuck" across runs.
    menuBar()->setVisible(shown);
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    m_contextMenu->popup(globalPos);
}

MainWindow::Launch MainWindow::launchCommand() const
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

void MainWindow::startSession()
{
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
            showSerialBanner(m_serial->errorString());
            return;
        }
        m_banner->hide();
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
    if (!m_pty->start(l.program, l.args, grid.height(), grid.width())) {
        m_term->feed(QStringLiteral("\x1b[31mzterminal: could not start %1: %2\x1b[0m\r\n")
                         .arg(l.program.isEmpty() ? Pty::defaultShell() : l.program, m_pty->errorString())
                         .toUtf8());
    }
}

void MainWindow::onSessionFinished(int exitCode, bool crashed)
{
    const QString msg = crashed
        ? QStringLiteral("\r\n\x1b[7m[process terminated (%1). Session > Restart Session (Ctrl+Shift+R) restarts it.]\x1b[0m\r\n")
              .arg(exitCode)
        : QStringLiteral("\r\n\x1b[7m[process exited with code %1. Session > Restart Session (Ctrl+Shift+R) restarts it.]\x1b[0m\r\n")
              .arg(exitCode);
    m_term->feed(msg.toUtf8());
}

bool MainWindow::isSerialSession() const
{
    return m_request.kind == LaunchRequest::Kind::Serial
        || (m_saved && m_saved->type == SessionConfig::Type::Serial);
}

QString MainWindow::serialBannerText() const
{
    return m_banner->isHidden() ? QString() : m_bannerText->text();
}

void MainWindow::showSerialBanner(const QString &text)
{
    m_bannerText->setText(text);
    m_banner->show();
}

void MainWindow::onSerialDisconnected(const QString &reason)
{
    m_term->feed(QStringLiteral("\r\n\x1b[41;97m[%1]\x1b[0m\r\n").arg(reason).toUtf8());
    showSerialBanner(QStringLiteral("Disconnected: %1 Plug it back in and press Reconnect.").arg(reason));
    updatePasteBar(0);
}

void MainWindow::updatePasteBar(qint64 remaining)
{
    const bool show = remaining > 0 && m_serial->pacingEnabled();
    m_pasteBar->setVisible(show);
    if (QAction *a = action(QStringLiteral("cancelPaste"))) {
        a->setEnabled(remaining > 0);
    }
    if (show) {
        m_pasteText->setText(QStringLiteral("Sending paste slowly (%1 ms/char, %2 ms/line)\u2026 %3 bytes left")
                                 .arg(m_serial->config().charDelayMs)
                                 .arg(m_serial->config().lineDelayMs)
                                 .arg(remaining));
    }
}

void MainWindow::restartSession()
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

bool MainWindow::launch(const QStringList &args)
{
    return m_launcher(QApplication::applicationFilePath(), args);
}

void MainWindow::duplicateSession()
{
    launch(m_originalArgs);
}

void MainWindow::showSessionDialog(bool focusSaved)
{
    // New Session starts blank; Open Saved Session starts on this window's
    // saved session (if any) with the list focused.
    SessionConfig initial;
    if (focusSaved && m_saved) {
        initial = *m_saved;
    }
    SessionDialog dlg(m_store, initial, this);
    if (focusSaved) {
        dlg.findChild<QWidget *>(QStringLiteral("sessionList"))->setFocus();
    }
    if (dlg.exec() == QDialog::Accepted) {
        QString err;
        if (!openInNewWindow(dlg.config(), &err)) {
            QMessageBox::warning(this, QStringLiteral("Open Session"), err);
        }
    }
}

bool MainWindow::openInNewWindow(const SessionConfig &cfg, QString *error)
{
    auto fail = [error](const QString &m) {
        if (error) {
            *error = m;
        }
        return false;
    };
    // A saved session that matches what is in the dialog opens by name, so the
    // new window's title, Duplicate and overrides all refer to it.
    if (!cfg.name.isEmpty()) {
        if (const auto stored = m_store.load(cfg.name); stored && *stored == cfg) {
            return launch({cfg.name}) || fail(QStringLiteral("Could not start a new zterminal window."));
        }
    }
    switch (cfg.type) {
    case SessionConfig::Type::LocalShell:
        return launch({}) || fail(QStringLiteral("Could not start a new zterminal window."));
    case SessionConfig::Type::Ssh: {
        const SshCommand c = buildSshCommand(cfg);
        if (!c.ok()) {
            return fail(c.error);
        }
        return launch(QStringList{QStringLiteral("ssh")} + c.args)
            || fail(QStringLiteral("Could not start a new zterminal window."));
    }
    case SessionConfig::Type::Serial:
        if (const QString e = validateSerial(cfg); !e.isEmpty()) {
            return fail(e);
        }
        // Unsaved: device and baud travel on the command line (8N1, no flow
        // control); save the session to keep the other line settings.
        return launch({QStringLiteral("serial"), cfg.serialDevice, QString::number(cfg.baudRate)})
            || fail(QStringLiteral("Could not start a new zterminal window."));
    }
    return fail(QStringLiteral("Unknown session type."));
}

std::optional<SessionConfig> MainWindow::currentSessionConfig(QString *why) const
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

bool MainWindow::saveCurrentSessionAs(const QString &name, QString *error)
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
    updateTitle();
    return true;
}

void MainWindow::saveSessionInteractive()
{
    QString why;
    if (!currentSessionConfig(&why)) {
        QMessageBox::warning(this, QStringLiteral("Save Session"), QStringLiteral("This window can't be saved: %1.").arg(why));
        return;
    }
    QString suggestion = m_saved ? m_saved->name : QString();
    if (suggestion.isEmpty() && m_request.kind == LaunchRequest::Kind::Ssh) {
        suggestion = m_request.displayName().mid(4); // "ssh user@host" -> "user@host"
    }
    for (;;) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Save Session"),
                                                   QStringLiteral("Save this window's session as:"),
                                                   QLineEdit::Normal, suggestion, &ok)
                                 .trimmed();
        if (!ok) {
            return;
        }
        if (const QString e = validateSessionName(name); !e.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Save Session"), e);
            suggestion = name;
            continue;
        }
        if (m_store.contains(name) && (!m_saved || m_saved->name != name)
            && QMessageBox::question(this, QStringLiteral("Save Session"),
                                     QStringLiteral("Replace the saved session \"%1\"?").arg(name))
                != QMessageBox::Yes) {
            suggestion = name;
            continue;
        }
        QString err;
        if (!saveCurrentSessionAs(name, &err)) {
            QMessageBox::warning(this, QStringLiteral("Save Session"), err);
        }
        return;
    }
}

void MainWindow::showPreferences()
{
    PreferencesDialog dlg(m_settings, this);
    connect(&dlg, &PreferencesDialog::applied, this, &MainWindow::setSettings);
    if (dlg.exec() == QDialog::Accepted) {
        setSettings(dlg.result());
    }
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, QStringLiteral("About zterminal"),
        QStringLiteral("<h3>zterminal %1</h3>"
                       "<p>A PuTTY-like terminal emulator for Linux.</p>"
                       "<p>Qt %2 &middot; libvterm %3.%4</p>"
                       "<p>MIT License &middot; &copy; 2026 Stephen B. Johnson</p>"
                       "<p><a href=\"%5\">%5</a></p>")
            .arg(QString::fromLatin1(kVersionString), QString::fromLatin1(qVersion()))
            .arg(VTERM_VERSION_MAJOR)
            .arg(VTERM_VERSION_MINOR)
            .arg(QString::fromLatin1(kRepoUrl)));
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_serial->close();
    m_pty->terminate();
    e->accept();
}

} // namespace zterminal
