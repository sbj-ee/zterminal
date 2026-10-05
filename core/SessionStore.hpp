#pragma once

#include "Session.hpp"

#include <QString>
#include <QStringList>

#include <optional>

namespace zterminal {

// Saved sessions: one INI file per session in
// $XDG_CONFIG_HOME/zterminal/sessions/ when set; else GenericConfigLocation
// (Linux ~/.config/…, macOS ~/Library/Application Support/…).
// The file name is the session name, percent-encoded (so "core sw/1" becomes
// "core%20sw%2F1.ini"), and the real name is also stored inside as session/name.
// Files are plain, human-editable INI. No password is ever written; stored
// passwords live only in the encrypted vault (Vault.hpp).
class SessionStore
{
public:
    // Default location (respects XDG_CONFIG_HOME).
    SessionStore();
    explicit SessionStore(const QString &directory);

    QString directory() const { return m_dir; }
    QString filePathFor(const QString &name) const;

    // Names of all valid saved sessions, sorted case-insensitively.
    QStringList names() const;
    bool contains(const QString &name) const;
    std::optional<SessionConfig> load(const QString &name) const;
    // Validates the name, then writes (replacing any earlier file). On failure
    // returns false and sets *error.
    bool save(const SessionConfig &s, QString *error = nullptr) const;
    bool remove(const QString &name) const;

    // "unknown session \"x\"; saved sessions: a, b" (or "no saved sessions yet").
    QString unknownSessionMessage(const QString &name) const;

    static QString defaultDirectory();

private:
    QString m_dir;
};

} // namespace zterminal
