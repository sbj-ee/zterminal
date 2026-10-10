#include "VaultManager.hpp"

#include "SessionStore.hpp"
#include "VaultBindings.hpp"
#include "VaultDialogs.hpp"

#include <QApplication>
#include <QEvent>
#include <QTimer>

namespace zterminal {

VaultManager &VaultManager::instance()
{
    static VaultManager *m = new VaultManager; // lives until exit; lock() on aboutToQuit
    return *m;
}

VaultManager::VaultManager()
    : m_vault(std::make_unique<Vault>())
{
    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    connect(m_idle, &QTimer::timeout, this, &VaultManager::lock);
    if (qApp) {
        qApp->installEventFilter(this);
        connect(qApp, &QCoreApplication::aboutToQuit, this, &VaultManager::lock);
    }
}

void VaultManager::setPath(const QString &path)
{
    lock();
    m_vault = std::make_unique<Vault>(path);
}

void VaultManager::setAutoLockMinutes(int minutes)
{
    minutes = std::max(0, minutes);
    // Every window applies the preferences when it opens and whenever the
    // settings file changes; re-applying the same value must not push the
    // idle deadline back (only real input and use of the vault do that).
    if (minutes == m_minutes && (m_idle->isActive() || !m_vault->isUnlocked())) {
        return;
    }
    const bool changed = minutes != m_minutes;
    m_minutes = minutes;
    restartIdleTimer();
    if (changed) {
        emit autoLockMinutesChanged(m_minutes);
    }
}

void VaultManager::setAutoLockIntervalMsForTests(int ms)
{
    m_testIntervalMs = ms;
    restartIdleTimer();
}

bool VaultManager::autoLockArmed() const
{
    return m_idle->isActive();
}

void VaultManager::restartIdleTimer()
{
    const int ms = m_testIntervalMs > 0 ? m_testIntervalMs : m_minutes * 60 * 1000;
    if (!m_vault->isUnlocked() || ms <= 0) {
        m_idle->stop();
        return;
    }
    m_idle->start(ms);
}

bool VaultManager::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
        if (m_vault->isUnlocked() && m_idle->isActive()) {
            restartIdleTimer();
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void VaultManager::lock()
{
    // m_announced: the Vault may already have locked itself (a failed
    // refresh); the windows still need to hear about it.
    const bool was = m_vault->isUnlocked() || m_announced;
    m_vault->lock(); // wipes + frees key and secrets
    m_idle->stop();
    m_announced = false;
    if (was) {
        emit lockedChanged(false);
    }
}

void VaultManager::touch()
{
    if (m_vault->isUnlocked() && m_idle->isActive()) {
        restartIdleTimer();
    }
}

bool VaultManager::refresh()
{
    if (!m_vault->isUnlocked()) {
        return false;
    }
    if (m_vault->refresh()) {
        return true;
    }
    if (!m_vault->isUnlocked()) { // Vault::refresh locked it: tell everyone
        lock();
    }
    return false;
}

void VaultManager::noteUnlocked()
{
    if (m_vault->isUnlocked()) {
        // Deletions queued while locked, orphaned secrets, and binding of
        // secrets stored before bindings existed (VaultBindings.hpp).
        m_lastReconcile = vaultbind::reconcile(*m_vault, SessionStore());
        if (!m_lastReconcile.ok && !m_vault->isUnlocked()) {
            lock();
            return;
        }
    }
    m_announced = m_vault->isUnlocked();
    restartIdleTimer();
    emit lockedChanged(true);
}

struct DialogScope {
    explicit DialogScope(VaultManager &m)
        : vm(m)
    {
        if (vm.m_dialogs++ == 0) {
            emit vm.dialogOpenChanged(true);
        }
    }
    ~DialogScope()
    {
        if (--vm.m_dialogs == 0) {
            emit vm.dialogOpenChanged(false);
        }
    }
    VaultManager &vm;
};

bool VaultManager::createInteractive(QWidget *parent)
{
    DialogScope scope(*this);
    CreateVaultDialog dlg(*m_vault, parent);
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    noteUnlocked();
    return true;
}

bool VaultManager::unlockInteractive(QWidget *parent, const QString &why)
{
    if (m_vault->isUnlocked()) {
        return true; // already open app-wide: nothing to ask
    }
    DialogScope scope(*this);
    // Anchor to the top-level window (not a SessionWidget): ApplicationModal
    // unlock is shared process-wide and is visible to tests via allWidgets().
    QWidget *anchor = parent ? parent->window() : nullptr;
    UnlockVaultDialog dlg(*m_vault, why, anchor);
    // Another window unlocked meanwhile (e.g. two sessions starting at once):
    // this prompt is no longer needed.
    const QMetaObject::Connection c = connect(this, &VaultManager::lockedChanged, &dlg, [&dlg](bool unlocked) {
        if (unlocked) {
            dlg.accept();
        }
    });
    const int r = dlg.exec();
    disconnect(c);
    if (r != QDialog::Accepted || !m_vault->isUnlocked()) {
        return false;
    }
    noteUnlocked();
    return true;
}

bool VaultManager::changePasswordInteractive(QWidget *parent)
{
    DialogScope scope(*this);
    ChangeMasterPasswordDialog dlg(*m_vault, parent);
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    noteUnlocked();
    return true;
}

bool VaultManager::ensureUnlocked(QWidget *parent, const QString &why, bool allowCreate)
{
    if (m_vault->isUnlocked()) {
        return true;
    }
    if (!m_vault->exists()) {
        return allowCreate && createInteractive(parent);
    }
    return unlockInteractive(parent, why);
}

} // namespace zterminal
