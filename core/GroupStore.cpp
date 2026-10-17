#include "GroupStore.hpp"

#include "ConfigPaths.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QUrl>

#include <algorithm>

namespace zterminal {

QString GroupConfig::openInToString(OpenIn o)
{
    return o == OpenIn::ThisWindow ? QStringLiteral("thisWindow") : QStringLiteral("newWindow");
}

GroupConfig::OpenIn GroupConfig::openInFromString(const QString &s)
{
    return s.compare(QStringLiteral("thisWindow"), Qt::CaseInsensitive) == 0 ? OpenIn::ThisWindow : OpenIn::NewWindow;
}

bool GroupConfig::operator==(const GroupConfig &o) const
{
    return name == o.name && openIn == o.openIn && staggerMs == o.staggerMs && focusIndex == o.focusIndex
        && members == o.members;
}

namespace {
const QString kSuffix = QStringLiteral(".ini");
constexpr int kFormatVersion = 1;

std::optional<GroupConfig> readFile(const QString &path)
{
    if (!QFileInfo::exists(path)) {
        return std::nullopt;
    }
    QSettings f(path, QSettings::IniFormat);
    if (f.status() != QSettings::NoError) {
        return std::nullopt;
    }
    GroupConfig g;
    QString fallback = QFileInfo(path).fileName();
    fallback.chop(kSuffix.size());
    g.name = f.value(QStringLiteral("group/name"), QUrl::fromPercentEncoding(fallback.toUtf8())).toString();
    g.openIn = GroupConfig::openInFromString(f.value(QStringLiteral("group/openIn")).toString());
    // Unknown or out-of-range values fall back to / are clamped to something usable.
    g.staggerMs = std::clamp(f.value(QStringLiteral("group/staggerMs"), GroupConfig::kDefaultStaggerMs).toInt(), 0,
                             GroupConfig::kMaxStaggerMs);
    g.focusIndex = std::max(0, f.value(QStringLiteral("group/focusIndex"), 0).toInt());
    const int n = std::clamp(f.beginReadArray(QStringLiteral("member")), 0, GroupConfig::kMaxMembers);
    for (int i = 0; i < n; ++i) {
        f.setArrayIndex(i);
        const QString m = f.value(QStringLiteral("session")).toString();
        if (!m.isEmpty()) {
            g.members << m;
        }
    }
    f.endArray();
    return g;
}
} // namespace

GroupStore::GroupStore()
    : m_dir(defaultDirectory())
{
}

GroupStore::GroupStore(const QString &directory)
    : m_dir(directory)
{
}

GroupStore GroupStore::besides(const SessionStore &sessions)
{
    return GroupStore(QFileInfo(sessions.directory()).absolutePath() + QStringLiteral("/groups"));
}

QString GroupStore::defaultDirectory()
{
    return configHome() + QStringLiteral("/zterminal/groups");
}

QString GroupStore::filePathFor(const QString &name) const
{
    // Same encoding as SessionStore: a name can never escape the directory.
    QString enc = QString::fromLatin1(QUrl::toPercentEncoding(name, QByteArrayLiteral(" @+,=()")));
    if (enc.startsWith(QLatin1Char('.'))) {
        enc.replace(0, 1, QStringLiteral("%2E"));
    }
    return m_dir + QLatin1Char('/') + enc + kSuffix;
}

QStringList GroupStore::names() const
{
    QStringList out;
    const QDir d(m_dir);
    for (const QString &file : d.entryList({QStringLiteral("*.ini")}, QDir::Files | QDir::Readable)) {
        const auto g = readFile(d.filePath(file));
        if (g && validateSessionName(g->name).isEmpty() && filePathFor(g->name) == d.filePath(file)) {
            out << g->name;
        }
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        const int c = QString::compare(a, b, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a < b;
    });
    return out;
}

bool GroupStore::contains(const QString &name) const
{
    return validateSessionName(name).isEmpty() && QFileInfo::exists(filePathFor(name));
}

std::optional<GroupConfig> GroupStore::load(const QString &name) const
{
    if (!validateSessionName(name).isEmpty()) {
        return std::nullopt;
    }
    auto g = readFile(filePathFor(name));
    if (g) {
        g->name = name;
    }
    return g;
}

bool GroupStore::save(const GroupConfig &g, QString *error) const
{
    auto fail = [error](const QString &m) {
        if (error) {
            *error = m;
        }
        return false;
    };
    if (const QString e = validateSessionName(g.name); !e.isEmpty()) {
        return fail(e);
    }
    if (g.members.size() > GroupConfig::kMaxMembers) {
        return fail(QStringLiteral("A group holds at most %1 sessions.").arg(GroupConfig::kMaxMembers));
    }
    if (!QDir().mkpath(m_dir)) {
        return fail(QStringLiteral("Can't create %1").arg(m_dir));
    }
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString path = filePathFor(g.name);
    {
        QSettings f(path, QSettings::IniFormat);
        f.clear();
        f.setValue(QStringLiteral("group/name"), g.name);
        f.setValue(QStringLiteral("group/version"), kFormatVersion);
        f.setValue(QStringLiteral("group/openIn"), GroupConfig::openInToString(g.openIn));
        f.setValue(QStringLiteral("group/staggerMs"), std::clamp(g.staggerMs, 0, GroupConfig::kMaxStaggerMs));
        f.setValue(QStringLiteral("group/focusIndex"), std::max(0, g.focusIndex));
        f.beginWriteArray(QStringLiteral("member"), int(g.members.size()));
        for (int i = 0; i < g.members.size(); ++i) {
            f.setArrayIndex(i);
            f.setValue(QStringLiteral("session"), g.members.at(i));
        }
        f.endArray();
        f.sync();
        if (f.status() != QSettings::NoError) {
            return fail(QStringLiteral("Can't write %1").arg(path));
        }
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool GroupStore::remove(const QString &name) const
{
    return validateSessionName(name).isEmpty() && QFile::remove(filePathFor(name));
}

QStringList GroupStore::groupsUsing(const QString &session) const
{
    QStringList out;
    for (const QString &n : names()) {
        if (const auto g = load(n); g && g->members.contains(session)) {
            out << n;
        }
    }
    return out;
}

QStringList GroupStore::removeSessionRef(const QString &session) const
{
    QStringList changed;
    for (const QString &n : names()) {
        auto g = load(n);
        if (!g || !g->members.contains(session)) {
            continue;
        }
        g->members.removeAll(session);
        g->focusIndex = std::min<int>(g->focusIndex, std::max<int>(0, int(g->members.size()) - 1));
        if (save(*g)) {
            changed << n;
        }
    }
    return changed;
}

QString GroupStore::unknownGroupMessage(const QString &name) const
{
    const QStringList all = names();
    QString m = QStringLiteral("unknown group \"%1\"").arg(name);
    if (all.isEmpty()) {
        m += QStringLiteral("; no saved groups yet (File > Manage Groups) in %1").arg(m_dir);
    } else {
        m += QStringLiteral("; saved groups: ") + all.join(QStringLiteral(", "));
    }
    return m;
}

} // namespace zterminal
