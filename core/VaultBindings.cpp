#include "VaultBindings.hpp"

#include <QDir>
#include <QFile>
#include <QSaveFile>

#include <map>

namespace zterminal::vaultbind {

namespace {
const QString kSsh = QStringLiteral("ssh-password");
const QString kSerial = QStringLiteral("serial-password");
const QString kBinding = QStringLiteral("binding/");

QString bindingOf(const Vault &vault, const QString &secretKey)
{
    const SecureBuffer *b = vault.secret(bindingKey(secretKey));
    if (!b || b->isEmpty()) {
        return {};
    }
    return QString::fromUtf8(reinterpret_cast<const char *>(b->data()), qsizetype(b->size()));
}

SecureBuffer bindingValue(const QString &binding)
{
    const QByteArray u = binding.toUtf8();
    return SecureBuffer(reinterpret_cast<const unsigned char *>(u.constData()), std::size_t(u.size()));
}

void writePending(const QString &path, const QStringList &lines)
{
    if (lines.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return;
    }
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
    f.commit();
}
} // namespace

QString sshSecretKey(const QString &sessionName)
{
    return Vault::secretKeyFor(kSsh, sessionName);
}

QString serialSecretKey(const QString &sessionName)
{
    return Vault::secretKeyFor(kSerial, sessionName);
}

QString bindingKey(const QString &secretKey)
{
    return kBinding + secretKey;
}

QString secretKeyFor(const SessionConfig &s)
{
    switch (s.type) {
    case SessionConfig::Type::Ssh:
        return sshSecretKey(s.name);
    case SessionConfig::Type::Serial:
        return serialSecretKey(s.name);
    case SessionConfig::Type::LocalShell:
        break;
    }
    return {};
}

Status check(const Vault &vault, const SessionConfig &s, QString *stored)
{
    const QString key = secretKeyFor(s);
    if (key.isEmpty() || !vault.secret(key)) {
        return Status::NoSecret;
    }
    const QString b = bindingOf(vault, key);
    if (stored) {
        *stored = b;
    }
    if (b.isEmpty()) {
        return Status::Unbound;
    }
    return b == vaultBindingFor(s) ? Status::Match : Status::Mismatch;
}

bool storeSecret(Vault &vault, const SessionConfig &s, SecureBuffer secret)
{
    const QString key = secretKeyFor(s);
    if (key.isEmpty()) {
        return false;
    }
    std::map<QString, SecureBuffer> set;
    set[key] = std::move(secret);
    set[bindingKey(key)] = bindingValue(vaultBindingFor(s));
    return vault.update(std::move(set));
}

bool rebind(Vault &vault, const SessionConfig &s)
{
    const QString key = secretKeyFor(s);
    if (key.isEmpty() || !vault.secret(key)) {
        return false;
    }
    std::map<QString, SecureBuffer> set;
    set[bindingKey(key)] = bindingValue(vaultBindingFor(s));
    return vault.update(std::move(set));
}

bool forget(Vault &vault, const QString &sessionName)
{
    const QString a = sshSecretKey(sessionName);
    const QString b = serialSecretKey(sessionName);
    return vault.update({}, {a, bindingKey(a), b, bindingKey(b)});
}

QString pendingDeletionsPath(const Vault &vault)
{
    return vault.path() + QStringLiteral(".pending-deletions");
}

QStringList pendingDeletions(const Vault &vault)
{
    QFile f(pendingDeletionsPath(vault));
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QStringList out;
    for (const QByteArray &l : f.readAll().split('\n')) {
        if (!l.trimmed().isEmpty()) {
            out << QString::fromUtf8(l);
        }
    }
    return out;
}

void queueDeletion(const Vault &vault, const SessionConfig &deleted)
{
    QStringList lines = pendingDeletions(vault);
    // Both kinds: the session may have had either (type changed over time).
    SessionConfig asSsh = deleted;
    asSsh.type = SessionConfig::Type::Ssh;
    SessionConfig asSerial = deleted;
    asSerial.type = SessionConfig::Type::Serial;
    for (const SessionConfig &c : {asSsh, asSerial}) {
        // Names can't contain control characters (validateSessionName), so
        // tab and newline are safe separators.
        const QString line = secretKeyFor(c) + QLatin1Char('\t') + vaultBindingFor(c);
        if (!lines.contains(line)) {
            lines << line;
        }
    }
    writePending(pendingDeletionsPath(vault), lines);
}

ReconcileResult reconcile(Vault &vault, const SessionStore &store)
{
    ReconcileResult r;
    if (!vault.isUnlocked()) {
        r.ok = false;
        r.error = QStringLiteral("The vault is locked.");
        return r;
    }
    QStringList remove;
    std::map<QString, SecureBuffer> set;

    // 1. Deletions queued while locked.
    const QStringList queued = pendingDeletions(vault);
    for (const QString &line : queued) {
        const QString key = line.section(QLatin1Char('\t'), 0, 0);
        const QString binding = line.section(QLatin1Char('\t'), 1);
        if (key.isEmpty() || !vault.secret(key)) {
            continue;
        }
        const QString stored = bindingOf(vault, key);
        if (stored.isEmpty() || stored == binding) {
            remove << key << bindingKey(key);
            r.deleted << key;
        }
    }

    // 2 + 3. Orphans and migration. Without a readable sessions directory
    // (first run, unusual XDG setup) nothing can be judged: leave everything.
    const bool haveStore = QDir(store.directory()).exists();
    for (const QString &key : haveStore ? vault.keys() : QStringList{}) {
        if (remove.contains(key)) {
            continue;
        }
        if (key.startsWith(kBinding)) {
            const QString secretKey = key.mid(kBinding.size());
            if (!vault.secret(secretKey) || remove.contains(secretKey)) {
                remove << key;
                r.pruned << key;
            }
            continue;
        }
        const QString kind = key.section(QLatin1Char('/'), 0, 0);
        const QString name = key.section(QLatin1Char('/'), 1);
        SessionConfig::Type type;
        if (kind == kSsh) {
            type = SessionConfig::Type::Ssh;
        } else if (kind == kSerial) {
            type = SessionConfig::Type::Serial;
        } else {
            continue; // not ours to judge
        }
        const auto session = store.load(name);
        if (!session || session->type != type) {
            remove << key << bindingKey(key);
            r.pruned << key;
            continue;
        }
        if (bindingOf(vault, key).isEmpty() && session->approved) {
            // Stored before bindings existed: trust the session as it is now.
            // An imported-but-unapproved session is not trusted; its secret
            // stays unbound and is refused until the user re-binds it.
            set[bindingKey(key)] = bindingValue(vaultBindingFor(*session));
            r.migrated << key;
        }
    }
    if (!remove.isEmpty() || !set.empty()) {
        if (!vault.update(std::move(set), remove)) {
            r.ok = false;
            r.error = vault.lastError();
            return r;
        }
    }
    if (!queued.isEmpty()) {
        writePending(pendingDeletionsPath(vault), {});
    }
    return r;
}

} // namespace zterminal::vaultbind
