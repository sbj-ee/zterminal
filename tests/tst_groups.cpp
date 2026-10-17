// Session groups (1.6.0): the GroupStore (one INI per group, 0600, name-keyed
// references that may dangle) and the command line (--group / -g /
// --list-groups / --check-group). The UI side is tst_groupui.
#include "CommandLine.hpp"
#include "Group.hpp"
#include "GroupStore.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace zterminal;

namespace {
GroupConfig lab()
{
    GroupConfig g;
    g.name = QStringLiteral("lab");
    g.openIn = GroupConfig::OpenIn::ThisWindow;
    g.staggerMs = 400;
    g.focusIndex = 2;
    g.members = {QStringLiteral("core-sw1"), QStringLiteral("build box"), QStringLiteral("edge/1 (lab)"),
                 QStringLiteral("core-sw1")};
    return g;
}

void writeRaw(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}
} // namespace

class TstGroups : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    QString dir() const { return tmp.filePath(QStringLiteral("groups")); }

private slots:
    void init() { QDir(tmp.path()).removeRecursively(); QDir().mkpath(tmp.path()); }

    void roundTrip()
    {
        const GroupStore store(dir());
        QVERIFY(store.names().isEmpty());
        QString err;
        QVERIFY2(store.save(lab(), &err), qPrintable(err));
        QVERIFY(store.contains(QStringLiteral("lab")));
        const auto back = store.load(QStringLiteral("lab"));
        QVERIFY(back.has_value());
        QCOMPARE(*back, lab());
        QCOMPARE(store.names(), QStringList{QStringLiteral("lab")});
        QVERIFY(!store.load(QStringLiteral("nope")).has_value());
    }

    void percentEncodedNamesAndPermissions()
    {
        const GroupStore store(dir());
        GroupConfig g = lab();
        g.name = QStringLiteral("core sw/1 (lab) 100%");
        QVERIFY(store.save(g));
        const QString path = store.filePathFor(g.name);
        QVERIFY(path.startsWith(dir() + QLatin1Char('/')));
        QVERIFY(!QFileInfo(path).fileName().contains(QLatin1Char('/')));
        QVERIFY(QFileInfo(path).fileName().contains(QStringLiteral("%2F")));
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(store.names(), QStringList{g.name});
        QCOMPARE(store.load(g.name)->name, g.name);
#if defined(Q_OS_UNIX)
        QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                                             | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther),
                 QFileDevice::Permissions());
        QVERIFY(QFile::permissions(path) & QFileDevice::WriteOwner);
        // The directory is private too.
        QCOMPARE(QFile::permissions(dir()) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup
                                              | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
#endif
        // A name can't make a hidden file or climb out of the directory.
        GroupConfig dot = lab();
        dot.name = QStringLiteral(".hidden");
        QVERIFY(store.save(dot));
        QVERIFY(!QFileInfo(store.filePathFor(dot.name)).fileName().startsWith(QLatin1Char('.')));
        QCOMPARE(QFileInfo(store.filePathFor(QStringLiteral("../x"))).absolutePath(), QFileInfo(dir()).absoluteFilePath());
    }

    void memberOrderAndRepeatsArePreserved()
    {
        const GroupStore store(dir());
        GroupConfig g = lab();
        g.members = {QStringLiteral("z"), QStringLiteral("a"), QStringLiteral("m"), QStringLiteral("a")};
        QVERIFY(store.save(g));
        QCOMPARE(store.load(g.name)->members, g.members);
        g.members = {QStringLiteral("m"), QStringLiteral("z")};
        QVERIFY(store.save(g)); // replaces: nothing of the longer list is left
        QCOMPARE(store.load(g.name)->members, g.members);
        g.members.clear();
        QVERIFY(store.save(g));
        QVERIFY(store.load(g.name)->members.isEmpty());
    }

    void namesAreValidatedLikeSessions()
    {
        const GroupStore store(dir());
        GroupConfig g = lab();
        for (const QString &bad : {QString(), QStringLiteral(" x"), QStringLiteral("-x"), QStringLiteral("ssh"),
                                   QStringLiteral("a\nb")}) {
            g.name = bad;
            QString err;
            QVERIFY2(!store.save(g, &err), qPrintable(bad));
            QVERIFY(!err.isEmpty());
        }
        QVERIFY(store.names().isEmpty());
        QVERIFY(!store.contains(QStringLiteral("-x")));
        QVERIFY(!store.remove(QStringLiteral("-x")));
        g.name = QStringLiteral("ok");
        g.members.clear();
        for (int i = 0; i <= GroupConfig::kMaxMembers; ++i) {
            g.members << QStringLiteral("s%1").arg(i);
        }
        QString err;
        QVERIFY(!store.save(g, &err)); // one over the limit
        QVERIFY(err.contains(QString::number(GroupConfig::kMaxMembers)));
    }

    void unknownAndMissingKeys()
    {
        const GroupStore store(dir());
        // Only a name: everything else takes its default.
        writeRaw(store.filePathFor(QStringLiteral("bare")), "[group]\nname=bare\n");
        const auto bare = store.load(QStringLiteral("bare"));
        QVERIFY(bare.has_value());
        QCOMPARE(bare->openIn, GroupConfig::OpenIn::NewWindow);
        QCOMPARE(bare->staggerMs, GroupConfig::kDefaultStaggerMs);
        QCOMPARE(bare->focusIndex, 0);
        QVERIFY(bare->members.isEmpty());
        // Unknown keys, a future version and unknown values are tolerated; out-of-range values clamp.
        writeRaw(store.filePathFor(QStringLiteral("odd")),
                 "[group]\nname=odd\nversion=7\nopenIn=sideways\nstaggerMs=99999\nfocusIndex=-3\nfuture=yes\n"
                 "[member]\nsize=3\n1\\session=a\n1\\title=ignored\n2\\session=\n3\\session=c\n");
        const auto odd = store.load(QStringLiteral("odd"));
        QVERIFY(odd.has_value());
        QCOMPARE(odd->openIn, GroupConfig::OpenIn::NewWindow);
        QCOMPARE(odd->staggerMs, GroupConfig::kMaxStaggerMs);
        QCOMPARE(odd->focusIndex, 0);
        QCOMPARE(odd->members, (QStringList{QStringLiteral("a"), QStringLiteral("c")})); // the empty slot is dropped
        writeRaw(store.filePathFor(QStringLiteral("neg")), "[group]\nname=neg\nstaggerMs=-5\n");
        QCOMPARE(store.load(QStringLiteral("neg"))->staggerMs, 0);
        // A huge declared size doesn't make the reader loop for ages or allocate that much.
        writeRaw(store.filePathFor(QStringLiteral("huge")), "[group]\nname=huge\n[member]\nsize=2000000000\n1\\session=a\n");
        const auto huge = store.load(QStringLiteral("huge"));
        QVERIFY(huge.has_value());
        QCOMPARE(huge->members, QStringList{QStringLiteral("a")});
        // A file whose inner name doesn't match its file name isn't listed.
        writeRaw(dir() + QStringLiteral("/stray.ini"), "[group]\nname=somethingelse\n");
        QVERIFY(!store.names().contains(QStringLiteral("somethingelse")));
        QVERIFY(store.names().contains(QStringLiteral("odd")));
        // Not an ini at all: skipped, not a crash.
        writeRaw(dir() + QStringLiteral("/garbage.ini"), "\x01\x02 not = an [ini\n");
        QVERIFY(!store.names().isEmpty());
    }

    void danglingReferencesAreTolerated()
    {
        // The store keeps names whether or not such a session exists, and
        // never looks at (or writes to) the session directory.
        const SessionStore sessions(tmp.filePath(QStringLiteral("sessions")));
        const GroupStore store = GroupStore::besides(sessions);
        QCOMPARE(store.directory(), QFileInfo(tmp.filePath(QStringLiteral("groups"))).absoluteFilePath());
        GroupConfig g = lab();
        g.members = {QStringLiteral("ghost-1"), QStringLiteral("ghost-2")};
        QVERIFY(store.save(g));
        QCOMPARE(store.load(g.name)->members, g.members);
        QVERIFY(!sessions.contains(QStringLiteral("ghost-1")));
        QVERIFY(!QFileInfo::exists(sessions.directory()));
        // Older versions only read the sessions directory: groups are invisible to them.
        SessionConfig s;
        s.name = QStringLiteral("real");
        QVERIFY(sessions.save(s));
        QCOMPARE(sessions.names(), QStringList{QStringLiteral("real")});
        QCOMPARE(store.names(), QStringList{QStringLiteral("lab")});
    }

    void removeAndPruneSessionReferences()
    {
        const GroupStore store(dir());
        GroupConfig a = lab();
        a.name = QStringLiteral("lab");
        a.members = {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("x")};
        a.focusIndex = 2;
        GroupConfig b = lab();
        b.name = QStringLiteral("edge");
        b.members = {QStringLiteral("y"), QStringLiteral("z")};
        b.focusIndex = 1;
        GroupConfig c = lab();
        c.name = QStringLiteral("other");
        c.members = {QStringLiteral("z")};
        c.focusIndex = 0;
        for (const GroupConfig &g : {a, b, c}) {
            QVERIFY(store.save(g));
        }
        QCOMPARE(store.groupsUsing(QStringLiteral("y")), (QStringList{QStringLiteral("edge"), QStringLiteral("lab")}));
        QCOMPARE(store.groupsUsing(QStringLiteral("x")), QStringList{QStringLiteral("lab")});
        QVERIFY(store.groupsUsing(QStringLiteral("nobody")).isEmpty());
        QCOMPARE(store.removeSessionRef(QStringLiteral("y")), (QStringList{QStringLiteral("edge"), QStringLiteral("lab")}));
        QCOMPARE(store.load(QStringLiteral("lab"))->members, (QStringList{QStringLiteral("x"), QStringLiteral("x")}));
        QCOMPARE(store.load(QStringLiteral("edge"))->members, QStringList{QStringLiteral("z")});
        QCOMPARE(store.load(QStringLiteral("edge"))->focusIndex, 0); // clamped to the new length
        QCOMPARE(store.load(QStringLiteral("other"))->members, QStringList{QStringLiteral("z")});
        QVERIFY(store.removeSessionRef(QStringLiteral("y")).isEmpty());
        QVERIFY(store.remove(QStringLiteral("lab")));
        QVERIFY(!store.contains(QStringLiteral("lab")));
        QVERIFY(!store.remove(QStringLiteral("lab")));
    }

    void namesSortCaseInsensitively()
    {
        const GroupStore store(dir());
        for (const QString &n : {QStringLiteral("beta"), QStringLiteral("Alpha"), QStringLiteral("gamma")}) {
            GroupConfig g = lab();
            g.name = n;
            QVERIFY(store.save(g));
        }
        QCOMPARE(store.names(), (QStringList{QStringLiteral("Alpha"), QStringLiteral("beta"), QStringLiteral("gamma")}));
    }

    void unknownGroupMessage()
    {
        const GroupStore store(dir());
        QVERIFY(store.unknownGroupMessage(QStringLiteral("x")).contains(QStringLiteral("no saved groups yet")));
        GroupConfig g = lab();
        QVERIFY(store.save(g));
        g.name = QStringLiteral("edge");
        QVERIFY(store.save(g));
        QCOMPARE(store.unknownGroupMessage(QStringLiteral("nope")),
                 QStringLiteral("unknown group \"nope\"; saved groups: edge, lab"));
    }

    void openInStrings()
    {
        QCOMPARE(GroupConfig::openInFromString(GroupConfig::openInToString(GroupConfig::OpenIn::ThisWindow)),
                 GroupConfig::OpenIn::ThisWindow);
        QCOMPARE(GroupConfig::openInFromString(GroupConfig::openInToString(GroupConfig::OpenIn::NewWindow)),
                 GroupConfig::OpenIn::NewWindow);
        QCOMPARE(GroupConfig::openInFromString(QStringLiteral("")), GroupConfig::OpenIn::NewWindow);
    }

    // ---- command line ----

    void commandLineGroup()
    {
        for (const QString &flag : {QStringLiteral("--group"), QStringLiteral("-g")}) {
            const LaunchRequest r = parseCommandLine({flag, QStringLiteral("my lab")});
            QCOMPARE(r.kind, LaunchRequest::Kind::Group);
            QCOMPARE(r.groupName, QStringLiteral("my lab"));
            QVERIFY(r.sessionName.isEmpty());
            QVERIFY(r.displayName().isEmpty()); // not a single session
        }
        const LaunchRequest c = parseCommandLine({QStringLiteral("--check-group"), QStringLiteral("lab")});
        QCOMPARE(c.kind, LaunchRequest::Kind::CheckGroup);
        QCOMPARE(c.groupName, QStringLiteral("lab"));
        QCOMPARE(parseCommandLine({QStringLiteral("--list-groups")}).kind, LaunchRequest::Kind::ListGroups);
    }

    void commandLineGroupErrors()
    {
        for (const QStringList &args : {QStringList{QStringLiteral("--group")},
                                        QStringList{QStringLiteral("-g")},
                                        QStringList{QStringLiteral("--group"), QStringLiteral("a"), QStringLiteral("b")},
                                        QStringList{QStringLiteral("--check-group")},
                                        QStringList{QStringLiteral("--list-groups"), QStringLiteral("x")}}) {
            const LaunchRequest r = parseCommandLine(args);
            QVERIFY2(r.kind == LaunchRequest::Kind::Error, qPrintable(args.join(QLatin1Char(' '))));
            QVERIFY(!r.error.isEmpty());
        }
        // A saved session may still be called "group" or "lab": only a leading flag is special.
        const LaunchRequest s = parseCommandLine({QStringLiteral("group")});
        QCOMPARE(s.kind, LaunchRequest::Kind::SavedSession);
        QCOMPARE(parseCommandLine({QStringLiteral("--groups")}).kind, LaunchRequest::Kind::Error);
        // Existing forms are unchanged.
        QCOMPARE(parseCommandLine({QStringLiteral("--list-sessions")}).kind, LaunchRequest::Kind::ListSessions);
        QCOMPARE(parseCommandLine({QStringLiteral("-g-x")}).kind, LaunchRequest::Kind::Error);
    }

    void usageMentionsGroups()
    {
        const QString u = usageText();
        QVERIFY(u.contains(QStringLiteral("--group")));
        QVERIFY(u.contains(QStringLiteral("--list-groups")));
        QVERIFY(u.contains(QStringLiteral("--check-group")));
    }
};

QTEST_GUILESS_MAIN(TstGroups)
#include "tst_groups.moc"
