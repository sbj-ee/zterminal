#include "MainWindow.hpp"

#include "ColorScheme.hpp"
#include "PreferencesDialog.hpp"
#include "Pty.hpp"
#include "SerialBackend.hpp"
#include "SessionDialog.hpp"
#include "SessionLog.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "VaultManager.hpp"
#include "WindowTitle.hpp"
#include "UpdateManager.hpp"
#include "version.hpp"

#include <vterm.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSet>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

#include <algorithm>

namespace zterminal {

namespace {
const char *kPlannedProperty = "zterminalPlanned";
const std::optional<SessionConfig> kNoSession;
} // namespace

MainWindow::MainWindow(const LaunchRequest &request, const QStringList &originalArgs, QWidget *parent)
    : QMainWindow(parent)
    , m_settings(AppSettings::load())
{
    // New windows open in this process, so they share its one unlocked vault
    // (until 0.7.0 File > New Window started a new process with its own,
    // locked vault and asked for the master password again).
    m_launcher = [](const QString &, const QStringList &args) { return openWindow(args) != nullptr; };
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("tabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    // One tab looks exactly like the pre-tabs window; the bar appears with the second.
    m_tabs->setTabBarAutoHide(true);
    m_tabs->tabBar()->setExpanding(false);
    setCentralWidget(m_tabs);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);

    // Always draw the menu bar inside the window. Without this, Qt hands the
    // menus to a global-menu service whenever com.canonical.AppMenu.Registrar
    // is on the session bus (e.g. the Fildem GNOME extension), hides the
    // in-window bar, and on GNOME nothing may show the exported menus.
    menuBar()->setNativeMenuBar(false);

    m_recLabel = new QLabel(QStringLiteral("\u25CF REC"));
    m_recLabel->setObjectName(QStringLiteral("recIndicator"));
    m_recLabel->setStyleSheet(QStringLiteral("QLabel { color: white; background: #c0392b; font-weight: bold; "
                                             "padding: 1px 8px; border-radius: 3px; margin: 2px 6px; }"));
    m_recLabel->hide();

    buildMenus();
    menuBar()->setCornerWidget(m_recLabel, Qt::TopRightCorner);
    connect(&VaultManager::instance(), &VaultManager::lockedChanged, this, &MainWindow::updateVaultActions);
    updateVaultActions();
    VaultManager::instance().setAutoLockMinutes(m_settings.vaultAutoLockMinutes);
    watchSettingsFile();

    addTab(request, originalArgs, /*start=*/false); // the caller starts it (after show())
    resize(view()->sizeHint() + QSize(0, menuBar()->sizeHint().height()));
}

MainWindow::~MainWindow()
{
    // Tabs (children) shut their sessions down in their destructors.
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
    connect(addAct(file, QStringLiteral("exportSessions"), QStringLiteral("&Export Sessions\u2026")),
            &QAction::triggered, this, &MainWindow::exportSessionsInteractive);
    connect(addAct(file, QStringLiteral("importSessions"), QStringLiteral("&Import Sessions\u2026")),
            &QAction::triggered, this, &MainWindow::importSessionsInteractive);
    file->addSeparator();
    // Tabs (Ctrl+Shift variants and Ctrl+PgUp/PgDn: plain Ctrl+letter keys stay with the session).
    connect(addAct(file, QStringLiteral("newTab"), QStringLiteral("New &Tab"), QKS(QStringLiteral("Ctrl+Shift+T"))),
            &QAction::triggered, this, [this]() { addTab(LaunchRequest{}, {}); });
    connect(addAct(file, QStringLiteral("closeTab"), QStringLiteral("Close T&ab"), QKS(QStringLiteral("Ctrl+Shift+W"))),
            &QAction::triggered, this, [this]() { closeTab(m_tabs->currentIndex()); });
    QAction *next = addAct(file, QStringLiteral("nextTab"), QStringLiteral("Ne&xt Tab"));
    // Shift+] is reported as '}' on US layouts, so register both forms.
    next->setShortcuts({QKS(Qt::CTRL | Qt::Key_PageDown), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight),
                        QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceRight)});
    connect(next, &QAction::triggered, this, &MainWindow::nextTab);
    QAction *prev = addAct(file, QStringLiteral("previousTab"), QStringLiteral("Pre&vious Tab"));
    prev->setShortcuts({QKS(Qt::CTRL | Qt::Key_PageUp), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft),
                        QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceLeft)});
    connect(prev, &QAction::triggered, this, &MainWindow::previousTab);
    file->addSeparator();
    connect(addAct(file, QStringLiteral("newWindow"), QStringLiteral("New &Window")), &QAction::triggered, this,
            [this]() { m_launcher(QApplication::applicationFilePath(), {}); });
    connect(addAct(file, QStringLiteral("close"), QStringLiteral("&Close Window")),
            &QAction::triggered, this, &QWidget::close);
    connect(addAct(file, QStringLiteral("quit"), QStringLiteral("&Quit"), QKS(QStringLiteral("Ctrl+Shift+Q"))),
            &QAction::triggered, qApp, &QApplication::closeAllWindows);

    // Edit
    QMenu *edit = menuBar()->addMenu(QStringLiteral("&Edit"));
    connect(addAct(edit, QStringLiteral("copy"), QStringLiteral("&Copy"), QKS(QStringLiteral("Ctrl+Shift+C"))),
            &QAction::triggered, this, [this]() { view()->copySelection(); });
    connect(addAct(edit, QStringLiteral("paste"), QStringLiteral("&Paste"), QKS(QStringLiteral("Ctrl+Shift+V"))),
            &QAction::triggered, this, [this]() { view()->pasteClipboard(); });
    connect(addAct(edit, QStringLiteral("pastePrimary"), QStringLiteral("Paste P&rimary"),
                   QKS(QStringLiteral("Shift+Insert"))),
            &QAction::triggered, this, [this]() { view()->pastePrimary(); });
    edit->addSeparator();
    connect(addAct(edit, QStringLiteral("selectAll"), QStringLiteral("Select &All"),
                   QKS(QStringLiteral("Ctrl+Shift+A"))),
            &QAction::triggered, this, [this]() { view()->selectAll(); });
    edit->addSeparator();
    connect(addAct(edit, QStringLiteral("find"), QStringLiteral("&Find\u2026"), QKS(QStringLiteral("Ctrl+Shift+F"))),
            &QAction::triggered, this, [this]() {
                if (SessionWidget *s = currentSession()) {
                    s->openFind();
                }
            });

    // View
    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    // Shift changes the key Qt reports (= -> +, - -> _, 0 -> ), so register both forms.
    QAction *larger = addAct(view, QStringLiteral("fontLarger"), QStringLiteral("Font Size &Up"));
    larger->setShortcuts({QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Plus), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal)});
    connect(larger, &QAction::triggered, this, [this]() { setFontSize(this->view()->terminalFont().pointSize() + 1); });
    QAction *smaller = addAct(view, QStringLiteral("fontSmaller"), QStringLiteral("Font Size &Down"));
    smaller->setShortcuts({QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Underscore), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Minus)});
    connect(smaller, &QAction::triggered, this, [this]() { setFontSize(this->view()->terminalFont().pointSize() - 1); });
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
    connect(m_schemeGroup, &QActionGroup::triggered, this, [this](QAction *a) { setSchemeFor(a->data().toString()); });
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
            &QAction::triggered, this, [this]() { currentSession()->restartSession(); });
    QAction *brk = addAct(session, QStringLiteral("sendBreak"), QStringLiteral("Send &Break"), {}, false);
    connect(brk, &QAction::triggered, this, [this]() {
        SessionWidget *s = currentSession();
        if (!s->serial()->sendBreak() && !s->serial()->isOpen()) {
            s->showBanner(QStringLiteral("Not connected: Send Break needs an open serial port."));
        }
    });
    QAction *login = addAct(session, QStringLiteral("sendStoredLogin"), QStringLiteral("Send Stored &Login"), {}, false);
    connect(login, &QAction::triggered, this, &MainWindow::sendStoredLogin);
    QAction *cancelPasteAct = addAct(session, QStringLiteral("cancelPaste"), QStringLiteral("Cancel &Paste"), {}, false);
    connect(cancelPasteAct, &QAction::triggered, this, [this]() { serial()->cancelPending(); });
    session->addSeparator();
    QAction *logAct = addAct(session, QStringLiteral("toggleLogging"), QStringLiteral("Start &Logging"),
                             QKS(QStringLiteral("Ctrl+Shift+G")));
    connect(logAct, &QAction::triggered, this, [this]() {
        if (currentSession()->isLogging()) {
            stopLogging();
            return;
        }
        QString err;
        if (!startLogging(&err)) {
            QMessageBox::warning(this, QStringLiteral("Start Logging"), err);
        }
    });
    session->addSeparator();
    connect(addAct(session, QStringLiteral("clearScrollback"), QStringLiteral("&Clear Scrollback"),
                   QKS(QStringLiteral("Ctrl+Shift+K"))),
            &QAction::triggered, this, [this]() { terminal()->clearScrollback(); });
    connect(addAct(session, QStringLiteral("resetTerminal"), QStringLiteral("Reset &Terminal")),
            &QAction::triggered, this, [this]() { terminal()->reset(); });
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
    settings->addSeparator();
    QMenu *vaultMenu = settings->addMenu(QStringLiteral("Password &Vault"));
    connect(addAct(vaultMenu, QStringLiteral("unlockVault"), QStringLiteral("&Unlock Vault\u2026")),
            &QAction::triggered, this, [this]() {
                // Already unlocked (in any window or tab): no prompt.
                VaultManager::instance().ensureUnlocked(this, {}, /*allowCreate=*/true);
            });
    connect(addAct(vaultMenu, QStringLiteral("lockVault"), QStringLiteral("&Lock Vault"), QKS(QStringLiteral("Ctrl+Shift+L"))),
            &QAction::triggered, &VaultManager::instance(), &VaultManager::lock);
    connect(addAct(vaultMenu, QStringLiteral("changeMasterPassword"), QStringLiteral("&Change Master Password\u2026")),
            &QAction::triggered, this, [this]() { VaultManager::instance().changePasswordInteractive(this); });

    // Help
    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    connect(addAct(help, QStringLiteral("about"), QStringLiteral("&About zterminal")),
            &QAction::triggered, this, &MainWindow::showAbout);
    connect(addAct(help, QStringLiteral("checkForUpdates"), QStringLiteral("Check for &Updates\u2026")),
            &QAction::triggered, this, [this]() { UpdateManager::instance().checkNow(this); });

    // Ctrl+right-click menu (no context menu on plain right-click: that pastes).
    m_contextMenu = new QMenu(this);
    for (const char *n : {"copy", "paste", "selectAll", "find"}) {
        m_contextMenu->addAction(action(QString::fromLatin1(n)));
    }
    m_contextMenu->addSeparator();
    m_contextMenu->addAction(showMenu);
    m_contextMenu->addAction(full);
    m_contextMenu->addSeparator();
    for (const char *n : {"newTab", "closeTab", "duplicateSession", "restartSession", "clearScrollback", "resetTerminal"}) {
        m_contextMenu->addAction(action(QString::fromLatin1(n)));
    }
    m_contextMenu->addSeparator();
    m_contextMenu->addAction(prefs); // reachable even when the menu bar is hidden

    // Only these key combinations are taken from the session.
    for (QAction *a : m_actions) {
        for (const QKeySequence &ks : a->shortcuts()) {
            if (!ks.isEmpty()) {
                m_reservedKeys.insert(ks[0].toCombined());
            }
        }
    }
}

// ---- tabs ----

int MainWindow::tabCount() const
{
    return m_tabs->count();
}

SessionWidget *MainWindow::sessionAt(int index) const
{
    return qobject_cast<SessionWidget *>(m_tabs->widget(index));
}

SessionWidget *MainWindow::currentSession() const
{
    return qobject_cast<SessionWidget *>(m_tabs->currentWidget());
}

SessionWidget *MainWindow::addTab(const LaunchRequest &request, const QStringList &args, bool start)
{
    auto *s = new SessionWidget(request, args, m_settings, m_tabs);
    s->view()->setAppShortcuts(m_reservedKeys);
    connect(s, &SessionWidget::titleChanged, this, [this, s]() {
        updateTabText(s);
        if (s == currentSession()) {
            updateTitle();
        }
    });
    connect(s, &SessionWidget::loggingChanged, this, [this, s]() {
        updateTabText(s);
        if (s == currentSession()) {
            updateLoggingUi();
        }
    });
    connect(s, &SessionWidget::pendingChanged, this, [this, s](qint64 remaining) {
        if (s == currentSession()) {
            action(QStringLiteral("cancelPaste"))->setEnabled(remaining > 0);
        }
    });
    connect(s, &SessionWidget::contextMenuRequested, this, [this](const QPoint &p) { m_contextMenu->popup(p); });
    const int i = m_tabs->addTab(s, s->sessionName());
    updateTabText(s);
    m_tabs->setCurrentIndex(i);
    onCurrentTabChanged();
    if (start) {
        s->startSession();
    }
    s->view()->setFocus();
    return s;
}

int MainWindow::liveTabCount() const
{
    int n = 0;
    for (int i = 0; i < m_tabs->count(); ++i) {
        n += sessionAt(i)->isLive() ? 1 : 0;
    }
    return n;
}

bool MainWindow::closeTab(int index, bool force)
{
    SessionWidget *s = sessionAt(index);
    if (!s) {
        return false;
    }
    if (!force && s->isLive()) {
        m_tabs->setCurrentIndex(index);
        const auto ret = QMessageBox::question(
            this, QStringLiteral("Close Tab"),
            QStringLiteral("\"%1\" is still running. Close the tab and end its session?").arg(s->sessionName().toHtmlEscaped()),
            QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
        if (ret != QMessageBox::Close) {
            return false;
        }
    }
    if (m_tabs->count() == 1) {
        // The last tab: end it and close the window (no second question).
        s->shutdown();
        m_closingConfirmed = true;
        close();
        return true;
    }
    m_tabs->removeTab(index);
    s->shutdown();
    s->deleteLater();
    return true;
}

void MainWindow::nextTab()
{
    if (m_tabs->count() > 1) {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
    }
}

void MainWindow::previousTab()
{
    if (m_tabs->count() > 1) {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    }
}

void MainWindow::updateTabText(SessionWidget *s)
{
    const int i = m_tabs->indexOf(s);
    if (i < 0) {
        return;
    }
    // The tab title is the session name; "● " marks a tab that is logging.
    m_tabs->setTabText(i, (s->isLogging() ? QStringLiteral("\u25CF ") : QString()) + s->sessionName());
    m_tabs->setTabToolTip(i, makeWindowTitle(s->sessionName(), s->terminal()->title())
                                 + (s->isLogging() ? QStringLiteral(" [REC]") : QString()));
}

void MainWindow::onCurrentTabChanged()
{
    SessionWidget *s = currentSession();
    if (!s) {
        return;
    }
    updateSessionActions();
    updateLoggingUi();
    for (QAction *a : m_schemeGroup->actions()) {
        a->setChecked(a->data().toString() == s->terminal()->colorScheme().id);
    }
    s->view()->setFocus();
}

void MainWindow::updateSessionActions()
{
    SessionWidget *s = currentSession();
    const bool serial = s->isSerialSession();
    QAction *brk = action(QStringLiteral("sendBreak"));
    brk->setEnabled(serial);
    brk->setToolTip(serial ? QStringLiteral("Holds the serial line in break (duration set per session; default 300 ms)")
                           : QStringLiteral("Serial sessions only"));
    QAction *login = action(QStringLiteral("sendStoredLogin"));
    const bool canLogin = serial && s->savedSession();
    login->setEnabled(canLogin);
    login->setToolTip(canLogin ? QStringLiteral("Sends the login user, then the vault password when the device asks for it")
                               : QStringLiteral("Saved serial sessions only"));
    action(QStringLiteral("cancelPaste"))->setEnabled(s->serial()->pending() > 0);
}

// ---- current-tab forwarding ----

void MainWindow::startSession() { currentSession()->startSession(); }
TerminalView *MainWindow::view() const { return currentSession()->view(); }
Terminal *MainWindow::terminal() const { return currentSession()->terminal(); }
Pty *MainWindow::pty() const { return currentSession()->pty(); }
SerialBackend *MainWindow::serial() const { return currentSession()->serial(); }
bool MainWindow::isSerialSession() const { return currentSession()->isSerialSession(); }
QWidget *MainWindow::sessionBanner() const { return currentSession()->banner(); }
QString MainWindow::serialBannerText() const { return currentSession()->bannerText(); }
QWidget *MainWindow::pasteBar() const { return currentSession()->pasteBar(); }
QString MainWindow::sessionName() const { return currentSession()->sessionName(); }
MainWindow::Launch MainWindow::launchCommand() const { return currentSession()->launchCommand(); }
AskpassServer *MainWindow::askpassServer() const { return currentSession()->askpassServer(); }
bool MainWindow::sendStoredLogin() { return currentSession()->sendStoredLogin(); }
bool MainWindow::loginPending() const { return currentSession()->loginPending(); }
bool MainWindow::startLogging(QString *error) { return currentSession()->startLogging(error); }
void MainWindow::stopLogging() { currentSession()->stopLogging(); }
SessionLog *MainWindow::sessionLog() const { return currentSession()->sessionLog(); }
bool MainWindow::confirmPaste(const QString &text) { return currentSession()->confirmPaste(text); }
bool MainWindow::pasteConfirmSkipped() const { return currentSession()->pasteConfirmSkipped(); }

const std::optional<SessionConfig> &MainWindow::savedSession() const
{
    SessionWidget *s = currentSession();
    return s ? s->savedSession() : kNoSession;
}

std::optional<SessionConfig> MainWindow::currentSessionConfig(QString *why) const
{
    return currentSession()->currentSessionConfig(why);
}

bool MainWindow::saveCurrentSessionAs(const QString &name, QString *error)
{
    return currentSession()->saveAs(name, error); // titleChanged updates tab + window
}

// ---- window ----

void MainWindow::applySettings()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        sessionAt(i)->applySettings(m_settings);
    }
    VaultManager::instance().setAutoLockMinutes(m_settings.vaultAutoLockMinutes);
    if (SessionWidget *s = currentSession()) {
        for (QAction *a : m_schemeGroup->actions()) {
            a->setChecked(a->data().toString() == s->terminal()->colorScheme().id);
        }
    }
}

void MainWindow::updateTitle()
{
    SessionWidget *s = currentSession();
    if (!s) {
        return;
    }
    // Version + the active tab's session (+ program title, + [REC] while it logs).
    const QString t = makeWindowTitle(s->sessionName(), s->terminal()->title());
    setWindowTitle(s->isLogging() ? t + QStringLiteral(" [REC]") : t);
}

void MainWindow::updateLoggingUi()
{
    SessionWidget *s = currentSession();
    const bool on = s && s->isLogging();
    m_recLabel->setVisible(on);
    m_recLabel->setToolTip(on ? QStringLiteral("Logging to %1").arg(s->sessionLog()->path()) : QString());
    if (QAction *a = action(QStringLiteral("toggleLogging"))) {
        a->setText(on ? QStringLiteral("Stop &Logging") : QStringLiteral("Start &Logging"));
        a->setStatusTip(on ? s->sessionLog()->path() : QString());
    }
    updateTitle();
}

void MainWindow::updateSavedEverywhere(const SessionConfig &cfg)
{
    m_store.save(cfg);
    for (int i = 0; i < m_tabs->count(); ++i) {
        SessionWidget *s = sessionAt(i);
        if (s->savedSession() && s->savedSession()->name == cfg.name) {
            s->setSavedSession(cfg);
        }
    }
    applySettings();
}

void MainWindow::setFontSize(int points)
{
    const auto &saved = savedSession();
    if (saved && saved->fontSize > 0) {
        // This session overrides the size: zoom changes (and saves) the override.
        SessionConfig cfg = *saved;
        cfg.fontSize = std::clamp(points, 6, 48);
        updateSavedEverywhere(cfg);
        return;
    }
    AppSettings s = m_settings;
    s.fontSize = std::clamp(points, 6, 48);
    setSettings(s);
}

void MainWindow::setSchemeFor(const QString &id)
{
    const auto &saved = savedSession();
    if (saved && !saved->colorScheme.isEmpty()) {
        // This session overrides the scheme: change (and save) the override.
        SessionConfig cfg = *saved;
        cfg.colorScheme = id;
        updateSavedEverywhere(cfg);
        return;
    }
    AppSettings s = m_settings;
    s.colorScheme = id;
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
    // Changes made in one window reach the others (in this process and in
    // separately started zterminals) through the settings file. QSettings replaces the file atomically,
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

void MainWindow::updateVaultActions()
{
    VaultManager &vm = VaultManager::instance();
    const bool open = vm.isUnlocked();
    if (QAction *a = action(QStringLiteral("unlockVault"))) {
        a->setText(vm.exists() ? QStringLiteral("&Unlock Vault\u2026") : QStringLiteral("&Create Vault\u2026"));
        a->setEnabled(!open);
    }
    if (QAction *a = action(QStringLiteral("lockVault"))) {
        a->setEnabled(open);
    }
    if (QAction *a = action(QStringLiteral("changeMasterPassword"))) {
        a->setEnabled(vm.exists());
    }
}

MainWindow *MainWindow::openWindow(const QStringList &args, QString *error)
{
    const LaunchRequest r = parseCommandLine(args);
    QString why;
    switch (r.kind) {
    case LaunchRequest::Kind::Error:
        why = r.error;
        break;
    case LaunchRequest::Kind::Version:
    case LaunchRequest::Kind::Help:
    case LaunchRequest::Kind::ListSessions:
    case LaunchRequest::Kind::CheckSession:
        why = QStringLiteral("not a session");
        break;
    case LaunchRequest::Kind::SavedSession:
        if (const SessionStore store; !store.contains(r.sessionName)) {
            why = store.unknownSessionMessage(r.sessionName);
        }
        break;
    default:
        break;
    }
    if (!why.isEmpty()) {
        if (error) {
            *error = why;
        }
        return nullptr;
    }
    auto *w = new MainWindow(r, args);
    w->setAttribute(Qt::WA_DeleteOnClose);
    w->show();
    // Now, while its only tab is the current one (as main() does). A deferred
    // start would hit whatever tab is current by then: a tab opened meanwhile
    // would be started twice and lose its one-shot askpass socket.
    w->startSession();
    return w;
}

void MainWindow::duplicateSession()
{
    SessionWidget *s = currentSession();
    addTab(s->request(), s->originalArgs());
}

void MainWindow::showSessionDialog(bool focusSaved)
{
    // New Session starts blank; Open Saved Session starts on this tab's saved
    // session (if any) with the list focused.
    SessionConfig initial;
    if (focusSaved && savedSession()) {
        initial = *savedSession();
    }
    SessionDialog dlg(m_store, initial, this);
    if (focusSaved) {
        dlg.findChild<QWidget *>(QStringLiteral("sessionList"))->setFocus();
    }
    if (dlg.exec() == QDialog::Accepted) {
        QString err;
        if (!openInNewTab(dlg.config(), &err)) {
            QMessageBox::warning(this, QStringLiteral("Open Session"), err);
        }
    }
}


void MainWindow::exportSessionsInteractive()
{
    // Reuse the dialog's export flow (file picker + status); no need to Open.
    SessionDialog dlg(m_store, SessionConfig{}, this);
    dlg.exportSessions();
}

void MainWindow::importSessionsInteractive()
{
    SessionDialog dlg(m_store, SessionConfig{}, this);
    dlg.importSessions();
}

bool MainWindow::openInNewTab(const SessionConfig &cfg, QString *error)
{
    auto fail = [error](const QString &m) {
        if (error) {
            *error = m;
        }
        return false;
    };
    // The same arguments `zterminal ARGS` would take; Duplicate re-opens them.
    QStringList args;
    if (const auto stored = cfg.name.isEmpty() ? std::nullopt : m_store.load(cfg.name); stored && *stored == cfg) {
        // A saved session that matches what is in the dialog opens by name, so
        // the tab's title, Duplicate and overrides all refer to it.
        args = {cfg.name};
    } else {
        switch (cfg.type) {
        case SessionConfig::Type::LocalShell:
            break;
        case SessionConfig::Type::Ssh: {
            const SshCommand c = buildSshCommand(cfg);
            if (!c.ok()) {
                return fail(c.error);
            }
            args = QStringList{QStringLiteral("ssh")} + c.args;
            break;
        }
        case SessionConfig::Type::Serial:
            if (const QString e = validateSerial(cfg); !e.isEmpty()) {
                return fail(e);
            }
            // Unsaved: device and baud only (8N1, no flow control); save the
            // session to keep the other line settings.
            args = {QStringLiteral("serial"), cfg.serialDevice, QString::number(cfg.baudRate)};
            break;
        }
    }
    const LaunchRequest req = parseCommandLine(args);
    if (req.kind == LaunchRequest::Kind::Error) {
        return fail(req.error);
    }
    addTab(req, args);
    return true;
}

void MainWindow::saveSessionInteractive()
{
    QString why;
    if (!currentSessionConfig(&why)) {
        QMessageBox::warning(this, QStringLiteral("Save Session"), QStringLiteral("This tab can't be saved: %1.").arg(why));
        return;
    }
    const auto &saved = savedSession();
    QString suggestion = saved ? saved->name : QString();
    if (suggestion.isEmpty() && currentSession()->request().kind == LaunchRequest::Kind::Ssh) {
        suggestion = currentSession()->request().displayName().mid(4); // "ssh user@host" -> "user@host"
    }
    for (;;) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Save Session"),
                                                   QStringLiteral("Save this tab's session as:"),
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
        if (m_store.contains(name) && (!saved || saved->name != name)
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
                       "<p>A PuTTY-like terminal emulator for Linux and Apple Silicon.</p>"
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
    const int live = liveTabCount();
    if (!m_closingConfirmed && live > 0) {
        const QString what = live == 1 ? QStringLiteral("1 tab has a running session")
                                       : QStringLiteral("%1 tabs have running sessions").arg(live);
        const auto ret = QMessageBox::question(this, QStringLiteral("Close Window"),
                                               QStringLiteral("%1. Close the window and end them?").arg(what),
                                               QMessageBox::Close | QMessageBox::Cancel, QMessageBox::Cancel);
        if (ret != QMessageBox::Close) {
            e->ignore();
            return;
        }
    }
    m_closingConfirmed = false;
    for (int i = 0; i < m_tabs->count(); ++i) {
        sessionAt(i)->shutdown();
    }
    e->accept();
}

} // namespace zterminal
