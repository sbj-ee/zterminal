#include "GroupOpener.hpp"

#include "GroupStore.hpp"
#include "MainWindow.hpp"
#include "Session.hpp"
#include "SessionWidget.hpp"
#include "VaultManager.hpp"

#include <QMessageBox>
#include <QTimer>

namespace zterminal {

namespace {
bool s_manual = false;

LaunchRequest requestFor(const QString &session)
{
    LaunchRequest r;
    r.kind = LaunchRequest::Kind::SavedSession;
    r.sessionName = session;
    return r;
}
} // namespace

void GroupOpener::setManualSteppingForTests(bool manual)
{
    s_manual = manual;
}

QString GroupOpener::Plan::summary() const
{
    QString s = QStringLiteral("Opened %1 of %2").arg(openable.size()).arg(total);
    if (!skipped.isEmpty()) {
        s += QStringLiteral("; skipped: ") + skipped.join(QStringLiteral(", "));
    }
    return s;
}

GroupOpener::Plan GroupOpener::plan(const GroupConfig &g, const SessionStore &sessions)
{
    Plan p;
    p.total = int(g.members.size());
    for (int i = 0; i < g.members.size(); ++i) {
        const QString &name = g.members.at(i);
        const auto cfg = sessions.load(name);
        QString why;
        if (!cfg) {
            why = QStringLiteral("missing");
        } else if (!cfg->approved) {
            why = QStringLiteral("imported, not reviewed yet");
        } else if (cfg->type == SessionConfig::Type::Ssh) {
            if (const SshCommand c = buildSshCommand(*cfg); !c.ok()) {
                why = QStringLiteral("invalid: ") + c.error;
            }
        } else if (cfg->type == SessionConfig::Type::Serial) {
            if (const QString e = validateSerial(*cfg); !e.isEmpty()) {
                why = QStringLiteral("invalid: ") + e;
            }
        }
        if (!why.isEmpty()) {
            p.skipped << QStringLiteral("%1 (%2)").arg(name, why);
            continue;
        }
        p.openable.append({name, i});
        p.needsVault = p.needsVault || (cfg->type == SessionConfig::Type::Ssh && cfg->useStoredPassword);
    }
    return p;
}

MainWindow *GroupOpener::openByName(const QString &name, MainWindow *current, QString *error)
{
    const GroupStore store;
    const auto g = store.load(name);
    if (!g) {
        if (error) {
            *error = store.unknownGroupMessage(name);
        }
        return nullptr;
    }
    return open(*g, current, error);
}

MainWindow *GroupOpener::open(const GroupConfig &g, MainWindow *current, QString *error)
{
    const Plan p = plan(g, SessionStore());
    if (p.openable.isEmpty()) {
        if (error) {
            *error = g.members.isEmpty()
                ? QStringLiteral("group \"%1\" has no sessions").arg(g.name)
                : QStringLiteral("nothing to open in group \"%1\"; skipped: %2").arg(g.name, p.skipped.join(QStringLiteral(", ")));
        }
        return nullptr;
    }
    // One unlock for the whole group, before the first tab. Cancelled or
    // failed: open anyway, ssh asks in each tab (no abort, no per-tab dialogs).
    VaultManager &vm = VaultManager::instance();
    if (p.needsVault && vm.exists() && !vm.isUnlocked()) {
        vm.ensureUnlocked(current, QStringLiteral("Group \"%1\" has sessions that use stored passwords.").arg(g.name.toHtmlEscaped()));
    }
    MainWindow *w = nullptr;
    int already = 0;
    if (g.openIn == GroupConfig::OpenIn::ThisWindow && current) {
        w = current;
    } else {
        const QString first = p.openable.front().name;
        w = new MainWindow(requestFor(first), {first});
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->show();
        w->currentSession()->startSession(SessionWidget::StartMode::Unattended);
        already = 1;
    }
    auto *o = new GroupOpener(g, p, w, already);
    o->begin();
    return w;
}

GroupOpener::GroupOpener(const GroupConfig &g, const Plan &p, MainWindow *window, int alreadyOpen)
    : QObject(window)
    , m_group(g)
    , m_plan(p)
    , m_window(window)
    , m_next(alreadyOpen)
    , m_manual(s_manual)
{
    setObjectName(QStringLiteral("groupOpener"));
    // The focus member: the last one whose list position is <= focusIndex
    // (a skipped focus member hands over to the one before it).
    for (int i = 0; i < m_plan.openable.size(); ++i) {
        if (m_plan.openable.at(i).index <= m_group.focusIndex) {
            m_focusPos = i;
        }
    }
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &GroupOpener::step);
}

void GroupOpener::begin()
{
    if (m_next == 0) {
        step(); // appending to a window: the first member starts now
    } else {
        next();
    }
}

void GroupOpener::next()
{
    if (m_next >= m_plan.openable.size()) {
        finish();
    } else if (!m_manual) {
        m_timer->start(m_group.staggerMs);
    }
}

void GroupOpener::step()
{
    if (m_finished || m_next >= m_plan.openable.size()) {
        return;
    }
    const int pos = m_next++;
    const QString name = m_plan.openable.at(pos).name;
    // Background tab, except the focus member.
    m_window->addTab(requestFor(name), {name}, /*start=*/true, /*activate=*/pos == m_focusPos,
                     SessionWidget::StartMode::Unattended);
    next();
}

void GroupOpener::finish()
{
    m_finished = true;
    const QString text = m_plan.summary();
    if (!m_plan.skipped.isEmpty()) {
        auto *box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Open Group"),
                                    QStringLiteral("Group \"%1\": %2").arg(m_group.name, text),
                                    QMessageBox::Ok, m_window);
        box->setObjectName(QStringLiteral("groupSummary"));
        box->setWindowModality(Qt::NonModal);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->show();
    }
    emit finished(text);
}

} // namespace zterminal
