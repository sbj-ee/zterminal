#pragma once

#include "Vault.hpp"

#include <QObject>

#include <memory>

class QTimer;
class QWidget;

namespace zterminal {

// The process-wide password vault (each zterminal window is its own process,
// so each window unlocks - and auto-locks - on its own).
//
// Auto-lock: after `autoLockMinutes` without keyboard, mouse-button or wheel
// input in this process the vault is locked, which wipes and frees the key
// and every decrypted secret. 0 disables it.
class VaultManager : public QObject
{
    Q_OBJECT
public:
    static VaultManager &instance();
    // Tests: point the singleton at another file (locks first).
    void setPath(const QString &path);

    Vault &vault() { return *m_vault; }
    bool exists() const { return m_vault->exists(); }
    bool isUnlocked() const { return m_vault->isUnlocked(); }

    void setAutoLockMinutes(int minutes);
    int autoLockMinutes() const { return m_minutes; }
    // Tests: the idle interval in milliseconds (overrides minutes).
    void setAutoLockIntervalMsForTests(int ms);
    bool autoLockArmed() const;

    // Unlocked already: true. Otherwise shows the unlock dialog (or, if there
    // is no vault and allowCreate, the create dialog). False if cancelled.
    bool ensureUnlocked(QWidget *parent, const QString &why = {}, bool allowCreate = false);
    bool createInteractive(QWidget *parent);
    bool unlockInteractive(QWidget *parent, const QString &why = {});
    bool changePasswordInteractive(QWidget *parent);
    void lock();

    // Called by the dialogs after a successful create/unlock/change.
    void noteUnlocked();

    bool isDialogOpen() const { return m_dialogs > 0; }

signals:
    void lockedChanged(bool unlocked);
    // A create/unlock/change dialog opened (true) or closed (false); session
    // logging pauses while one is up.
    void dialogOpenChanged(bool open);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    VaultManager();
    void restartIdleTimer();

    std::unique_ptr<Vault> m_vault;
    QTimer *m_idle = nullptr;
    int m_minutes = 15;
    int m_testIntervalMs = 0;
    int m_dialogs = 0;
    friend struct DialogScope;
};

} // namespace zterminal
