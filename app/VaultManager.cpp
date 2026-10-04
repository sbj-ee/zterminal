#include "VaultManager.hpp"

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
    m_minutes = std::max(0, minutes);
    restartIdleTimer();
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
    const bool was = m_vault->isUnlocked();
    m_vault->lock(); // wipes + frees key and secrets
    m_idle->stop();
    if (was) {
        emit lockedChanged(false);
    }
}

void VaultManager::noteUnlocked()
{
    restartIdleTimer();
    emit lockedChanged(true);
}

bool VaultManager::createInteractive(QWidget *parent)
{
    CreateVaultDialog dlg(*m_vault, parent);
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    noteUnlocked();
    return true;
}

bool VaultManager::unlockInteractive(QWidget *parent, const QString &why)
{
    UnlockVaultDialog dlg(*m_vault, why, parent);
    if (dlg.exec() != QDialog::Accepted) {
        return false;
    }
    noteUnlocked();
    return true;
}

bool VaultManager::changePasswordInteractive(QWidget *parent)
{
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
