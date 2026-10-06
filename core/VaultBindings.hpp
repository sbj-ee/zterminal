#pragma once

#include "Session.hpp"
#include "SessionStore.hpp"
#include "Vault.hpp"

#include <QString>
#include <QStringList>

namespace zterminal {

// Stored passwords are keyed by session name ("ssh-password/<name>",
// "serial-password/<name>"), and a session can be renamed, overwritten by an
// import or edited to point at another host. So every secret also carries a
// binding, the target it was stored for (vaultBindingFor(): "ssh:user@host:port"
// or "serial:<device>"), in the vault entry "binding/<secret key>". The binding
// is encrypted and authenticated with the rest of the vault.
//
// A stored password is only used when the session's current binding equals
// the stored one; on a mismatch the app asks the user before re-binding
// (SessionWidget), and never sends it silently.
namespace vaultbind {

QString sshSecretKey(const QString &sessionName);    // "ssh-password/<name>"
QString serialSecretKey(const QString &sessionName); // "serial-password/<name>"
QString bindingKey(const QString &secretKey);         // "binding/<secret key>"
// The secret key a session's password lives under (empty for local shells).
QString secretKeyFor(const SessionConfig &s);

enum class Status {
    NoSecret, // nothing stored under the session's key
    Match,    // stored binding == the session's current target
    Mismatch, // stored for a different target (or the session changed)
    Unbound,  // a secret without a binding (only before migration)
};
// `stored` gets the stored binding (empty when unbound).
Status check(const Vault &vault, const SessionConfig &s, QString *stored = nullptr);

// Store `secret` for `s` together with its current binding (one vault write).
bool storeSecret(Vault &vault, const SessionConfig &s, SecureBuffer secret);
// Re-bind an existing secret to the session's current target.
bool rebind(Vault &vault, const SessionConfig &s);
// Remove the session's secret and binding (both kinds of key for `name`).
bool forget(Vault &vault, const QString &sessionName);

// Deletions requested while the vault was locked (a deleted session's secret
// must not outlive it): "<vault path>.pending-deletions", one
// "<secret key>\t<binding at deletion time>" per line. Names only, no secrets.
QString pendingDeletionsPath(const Vault &vault);
void queueDeletion(const Vault &vault, const SessionConfig &deleted);
QStringList pendingDeletions(const Vault &vault); // raw lines (tests)

struct ReconcileResult {
    QStringList deleted;  // queued deletions carried out
    QStringList pruned;   // secrets (or bindings) with no matching session
    QStringList migrated; // unbound secrets bound to their session's current target
    bool ok = true;
    QString error;
};
// Run right after every unlock (VaultManager::noteUnlocked):
//   1. carry out queued deletions (only if the secret is still bound to the
//      deleted session's target, so a password stored meanwhile for a new
//      session of the same name survives), then clear the queue;
//   2. prune secrets whose session no longer exists (or has the wrong type),
//      and bindings without a secret;
//   3. migrate: bind each still-unbound secret (stored by zterminal <= 1.0.x)
//      to its session's current target.
ReconcileResult reconcile(Vault &vault, const SessionStore &store);

} // namespace vaultbind
} // namespace zterminal
