#pragma once

#include "Vault.hpp"
#include "VaultBindings.hpp"

#include <QObject>

#include <memory>

class QTimer;
class QWidget;

namespace zterminal {

// The one app-wide password vault and key holder. Every window opened from
// inside zterminal (File > New Session / Open Saved Session, the session
// dialog's Open, Session > Duplicate) runs in this same process (since 0.6.1),
// so a single unlock serves all of them until Lock Vault, the idle auto-lock
// (off by default) or exit.
// The key never leaves this process: ssh's askpass helper asks it over a
// one-shot 0700 socket (AskpassServer); nothing is passed in argv or env.
//
// Auto-lock: after `autoLockMinutes` without keyboard, mouse-button or wheel
// input in any window, or use of a stored password, the vault is locked,
// which wipes and frees the key and every decrypted secret. 0 (the default)
// disables it: the vault then stays unlocked for the rest of the run.
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
    // A stored password was just used: counts as activity for the idle timer.
    void touch();
    // Re-read the file with the in-memory key (Vault::refresh). If that had
    // to lock (password changed elsewhere, unreadable file) the lock is
    // announced like any other, so every window's menu and timer follow.
    bool refresh();

    // Called by the dialogs after a successful create/unlock/change. Also runs
    // vaultbind::reconcile() against the saved sessions.
    void noteUnlocked();

    bool isDialogOpen() const { return m_dialogs > 0; }
    // What the clean-up after the last unlock did (tests, diagnostics).
    const vaultbind::ReconcileResult &lastReconcile() const { return m_lastReconcile; }

signals:
    void lockedChanged(bool unlocked);
    // autoLockMinutes() changed (every window's status-bar indicator follows).
    void autoLockMinutesChanged(int minutes);
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
    int m_minutes = 0;
    int m_testIntervalMs = 0;
    int m_dialogs = 0;
    bool m_announced = false; // lockedChanged(true) was the last state sent
    vaultbind::ReconcileResult m_lastReconcile;
    friend struct DialogScope;
};

} // namespace zterminal
