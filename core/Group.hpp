#pragma once

#include <QString>
#include <QStringList>

namespace zterminal {

// A session group (docs/PLAN.md §4.19): an ordered, flat list of saved-session
// NAMES that opens together as tabs. Members are references, not copies: the
// session itself (host, user, keys, appearance, the vault password keyed by
// name) stays in its own file, so editing a session changes it everywhere.
// A member naming a session that no longer exists is tolerated ("dangling");
// opening skips it and says so.
struct GroupConfig {
    enum class OpenIn { NewWindow, ThisWindow };

    static constexpr int kDefaultStaggerMs = 250;
    static constexpr int kMaxStaggerMs = 5000;
    static constexpr int kMaxMembers = 256;

    QString name;
    OpenIn openIn = OpenIn::NewWindow;
    // Delay between starting one member and the next (also keeps sshd
    // MaxStartups / fail2ban style limits calm). 0..kMaxStaggerMs.
    int staggerMs = kDefaultStaggerMs;
    // Which member (0-based, list order) gets the focus once the group is open.
    int focusIndex = 0;
    QStringList members; // saved-session names, tab order; a name may repeat

    static QString openInToString(OpenIn o);
    static OpenIn openInFromString(const QString &s); // unknown -> NewWindow

    bool operator==(const GroupConfig &o) const;
    bool operator!=(const GroupConfig &o) const { return !(*this == o); }
};

} // namespace zterminal
