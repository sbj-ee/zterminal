#pragma once

#include "Group.hpp"

#include <QString>
#include <QStringList>

#include <optional>

namespace zterminal {

class SessionStore;

// Saved groups: one INI file per group in
// $XDG_CONFIG_HOME/zterminal/groups/ when set; else GenericConfigLocation
// (Linux ~/.config/..., macOS ~/Library/Application Support/...). Mirrors
// SessionStore: the file name is the group name percent-encoded, the real name
// is also stored inside, files are 0600 and the directory 0700, and a group
// name follows the session-name rules (validateSessionName).
//
// A separate directory means zterminal 1.5.0 and older never see groups.
// A group holds session names only; nothing in it can carry a secret.
class GroupStore
{
public:
    // Default location (respects XDG_CONFIG_HOME).
    GroupStore();
    explicit GroupStore(const QString &directory);
    // The groups directory that sits next to this session store's directory.
    static GroupStore besides(const SessionStore &sessions);

    QString directory() const { return m_dir; }
    QString filePathFor(const QString &name) const;

    // Names of all valid saved groups, sorted case-insensitively.
    QStringList names() const;
    bool contains(const QString &name) const;
    std::optional<GroupConfig> load(const QString &name) const;
    // Validates the name, then writes (replacing any earlier file).
    bool save(const GroupConfig &g, QString *error = nullptr) const;
    bool remove(const QString &name) const;

    // Names of the groups that list `session` as a member.
    QStringList groupsUsing(const QString &session) const;
    // Drops every reference to `session` from every group; returns the names
    // of the groups that changed.
    QStringList removeSessionRef(const QString &session) const;

    // "unknown group \"x\"; saved groups: a, b" (or "no saved groups yet").
    QString unknownGroupMessage(const QString &name) const;

    static QString defaultDirectory();

private:
    QString m_dir;
};

} // namespace zterminal
