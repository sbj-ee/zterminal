#include "MainWindow.hpp"

#include "ColorScheme.hpp"
#include "PreferencesDialog.hpp"
#include "Pty.hpp"
#include "Terminal.hpp"
#include "TerminalView.hpp"
#include "WindowTitle.hpp"
#include "version.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QSet>

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
    m_term = new Terminal(24, 80, this);
    m_pty = new Pty(this);
    m_view = new TerminalView(m_term, this);
    setCentralWidget(m_view);

    connect(m_term, &Terminal::output, m_pty, &Pty::write);
    connect(m_pty, &Pty::dataReceived, m_term, &Terminal::feed);
    connect(m_pty, &Pty::finished, this, &MainWindow::onSessionFinished);
    connect(m_view, &TerminalView::gridSizeChanged, m_pty, &Pty::resize);
    connect(m_term, &Terminal::titleChanged, this, &MainWindow::updateTitle);
    connect(m_term, &Terminal::bell, this, []() { QApplication::beep(); });
    connect(m_view, &TerminalView::contextMenuRequested, this, &MainWindow::showContextMenu);

    buildMenus();
    applySettings();
    updateTitle();
    resize(m_view->sizeHint() + QSize(0, menuBar()->sizeHint().height()));
    m_view->setFocus();
}

MainWindow::~MainWindow()
{
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
    connect(addAct(file, QStringLiteral("newSession"), QStringLiteral("&New Session"),
                   QKS(QStringLiteral("Ctrl+Shift+N"))),
            &QAction::triggered, this, &MainWindow::newSession);
    addAct(file, QStringLiteral("openSavedSession"), QStringLiteral("&Open Saved Session\u2026"),
           QKS(QStringLiteral("Ctrl+Shift+O")), false, later);
    addAct(file, QStringLiteral("saveSession"), QStringLiteral("&Save Session"),
           QKS(QStringLiteral("Ctrl+Shift+S")), false, later);
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
    connect(larger, &QAction::triggered, this, [this]() { setFontSize(m_settings.fontSize + 1); });
    QAction *smaller = addAct(view, QStringLiteral("fontSmaller"), QStringLiteral("Font Size &Down"));
    smaller->setShortcuts({QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Underscore), QKS(Qt::CTRL | Qt::SHIFT | Qt::Key_Minus)});
    connect(smaller, &QAction::triggered, this, [this]() { setFontSize(m_settings.fontSize - 1); });
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
        m_settings.colorScheme = a->data().toString();
        m_settings.save();
        applySettings();
    });
    view->addSeparator();
    QAction *full = addAct(view, QStringLiteral("fullScreen"), QStringLiteral("&Full Screen"), QKS(Qt::Key_F11));
    full->setCheckable(true);
    connect(full, &QAction::toggled, this, [this](bool on) {
        on ? showFullScreen() : showNormal();
    });
    QAction *showMenu = addAct(view, QStringLiteral("showMenuBar"), QStringLiteral("Show &Menu Bar"),
                               QKS(QStringLiteral("Ctrl+Shift+M")));
    showMenu->setCheckable(true);
    connect(showMenu, &QAction::toggled, this, &MainWindow::setMenuBarShown);

    // Session
    QMenu *session = menuBar()->addMenu(QStringLiteral("&Session"));
    connect(addAct(session, QStringLiteral("duplicateSession"), QStringLiteral("&Duplicate Session"),
                   QKS(QStringLiteral("Ctrl+Shift+D"))),
            &QAction::triggered, this, &MainWindow::duplicateSession);
    connect(addAct(session, QStringLiteral("restartSession"), QStringLiteral("&Restart Session"),
                   QKS(QStringLiteral("Ctrl+Shift+R"))),
            &QAction::triggered, this, &MainWindow::restartSession);
    addAct(session, QStringLiteral("sendBreak"), QStringLiteral("Send &Break"), {}, false,
           QStringLiteral("Serial sessions only (planned)"));
    session->addSeparator();
    connect(addAct(session, QStringLiteral("clearScrollback"), QStringLiteral("&Clear Scrollback"),
                   QKS(QStringLiteral("Ctrl+Shift+K"))),
            &QAction::triggered, m_term, &Terminal::clearScrollback);
    connect(addAct(session, QStringLiteral("resetTerminal"), QStringLiteral("Reset &Terminal")),
            &QAction::triggered, m_term, &Terminal::reset);
    session->addSeparator();
    addAct(session, QStringLiteral("changeSettings"), QStringLiteral("Change &Settings\u2026"), {}, false, later);

    // Settings
    QMenu *settings = menuBar()->addMenu(QStringLiteral("Se&ttings"));
    connect(addAct(settings, QStringLiteral("preferences"), QStringLiteral("&Preferences\u2026")),
            &QAction::triggered, this, &MainWindow::showPreferences);

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
    for (const char *n : {"duplicateSession", "restartSession", "clearScrollback", "resetTerminal", "changeSettings"}) {
        m_contextMenu->addAction(action(QString::fromLatin1(n)));
    }

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
    m_view->setTerminalFont(m_settings.font());
    m_view->setMouseSettings(m_settings.mouse);
    m_term->setScrollbackLimit(m_settings.scrollbackLines);
    m_term->setColorScheme(ColorScheme::byId(m_settings.colorScheme));
    for (QAction *a : m_schemeGroup->actions()) {
        a->setChecked(a->data().toString() == m_term->colorScheme().id);
    }
    if (QAction *a = action(QStringLiteral("showMenuBar"))) {
        a->setChecked(m_settings.menuBarVisible);
    }
    menuBar()->setVisible(m_settings.menuBarVisible);
}

void MainWindow::updateTitle()
{
    setWindowTitle(makeWindowTitle(sessionName(), m_term->title()));
}

void MainWindow::setFontSize(int points)
{
    m_settings.fontSize = std::clamp(points, 6, 48);
    m_settings.save();
    m_view->setTerminalFont(m_settings.font());
}

void MainWindow::setMenuBarShown(bool shown)
{
    menuBar()->setVisible(shown);
    if (m_settings.menuBarVisible != shown) {
        m_settings.menuBarVisible = shown;
        m_settings.save();
    }
}

void MainWindow::showContextMenu(const QPoint &globalPos)
{
    m_contextMenu->popup(globalPos);
}

void MainWindow::startSession()
{
    QString program;
    QStringList args;
    switch (m_request.kind) {
    case LaunchRequest::Kind::Ssh:
    case LaunchRequest::Kind::Command:
        program = m_request.program;
        args = m_request.args;
        break;
    case LaunchRequest::Kind::SavedSession:
        m_term->feed(QByteArrayLiteral("\x1b[33mzterminal: saved sessions are not implemented yet "
                                       "(planned); opening a local shell.\x1b[0m\r\n"));
        break;
    default:
        break; // local shell
    }
    const QSize grid(m_view->gridCols(), m_view->gridRows());
    if (!m_pty->start(program, args, grid.height(), grid.width())) {
        m_term->feed(QStringLiteral("\x1b[31mzterminal: could not start %1: %2\x1b[0m\r\n")
                         .arg(program.isEmpty() ? Pty::defaultShell() : program, m_pty->errorString())
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

void MainWindow::restartSession()
{
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

void MainWindow::newSession()
{
    // Until saved sessions land, New Session opens a local shell in a new window.
    QProcess::startDetached(QApplication::applicationFilePath(), {});
}

void MainWindow::duplicateSession()
{
    QProcess::startDetached(QApplication::applicationFilePath(), m_originalArgs);
}

void MainWindow::showPreferences()
{
    PreferencesDialog dlg(m_settings, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_settings = dlg.result();
        m_settings.save();
        applySettings();
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
    m_pty->terminate();
    e->accept();
}

} // namespace zterminal
