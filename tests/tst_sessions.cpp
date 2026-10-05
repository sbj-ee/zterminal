// Saved sessions (0.2.0): INI store round-trip, safe ssh argv building
// (including option-injection attempts), and command-line parsing for zt.
#include "CommandLine.hpp"
#include "Session.hpp"
#include "SessionStore.hpp"
#include "SessionExport.hpp"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using namespace zterminal;

namespace {
SessionConfig sshSession()
{
    SessionConfig s;
    s.name = QStringLiteral("core sw/1 (lab)");
    s.type = SessionConfig::Type::Ssh;
    s.host = QStringLiteral("10.0.0.1");
    s.user = QStringLiteral("admin");
    s.port = 2222;
    s.keyFile = QStringLiteral("~/.ssh/id_lab");
    s.jumpHost = QStringLiteral("sbj@vertex:22");
    s.extraArgs = QStringLiteral("-o ServerAliveInterval=30 -4");
    s.fontFamily = QStringLiteral("DejaVu Sans Mono");
    s.fontSize = 13;
    s.colorScheme = QStringLiteral("solarized-dark");
    return s;
}
} // namespace

class TstSessions : public QObject
{
    Q_OBJECT

private slots:
    // ---- store ---------------------------------------------------------
    void storeRoundTrip()
    {
        QTemporaryDir dir;
        const SessionStore store(dir.filePath(QStringLiteral("sessions")));
        QVERIFY(store.names().isEmpty());

        const SessionConfig in = sshSession();
        QString err;
        QVERIFY2(store.save(in, &err), qPrintable(err));
        QVERIFY(store.contains(in.name));
        const auto out = store.load(in.name);
        QVERIFY(out);
        QVERIFY(*out == in);
        QCOMPARE(out->extraArgs, in.extraArgs);
        QCOMPARE(out->keyFile, in.keyFile); // stored as typed; expanded only when run

        SessionConfig local;
        local.name = QStringLiteral("Local");
        QVERIFY(store.save(local));
        QVERIFY(*store.load(QStringLiteral("Local")) == local);

        // Sorted case-insensitively; one .ini per session in the directory.
        QCOMPARE(store.names(), (QStringList{in.name, QStringLiteral("Local")}));
        QCOMPARE(QDir(store.directory()).entryList(QDir::Files).size(), 2);

        // Overwrite drops fields of the old type.
        SessionConfig changed = in;
        changed.type = SessionConfig::Type::LocalShell;
        changed.fontFamily.clear();
        changed.fontSize = 0;
        changed.colorScheme.clear();
        QVERIFY(store.save(changed));
        const QSettings raw(store.filePathFor(in.name), QSettings::IniFormat);
        QVERIFY(!raw.contains(QStringLiteral("ssh/host")));
        QVERIFY(!raw.contains(QStringLiteral("appearance/fontSize")));
        QCOMPARE(store.load(in.name)->type, SessionConfig::Type::LocalShell);

        QVERIFY(store.remove(in.name));
        QVERIFY(!store.contains(in.name));
        QCOMPARE(store.names(), QStringList{QStringLiteral("Local")});
        QVERIFY(!store.remove(QStringLiteral("nope")));
    }

    void fileLayoutAndNoPasswords()
    {
        QTemporaryDir dir;
        const SessionStore store(dir.filePath(QStringLiteral("sessions")));
        const SessionConfig in = sshSession();
        QVERIFY(store.save(in));
        const QString path = store.filePathFor(in.name);
        // The name is percent-encoded: '/' can't escape the directory.
        QCOMPARE(QFileInfo(path).dir().absolutePath(), QDir(store.directory()).absolutePath());
        QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("core sw%2F1 (lab).ini"));
        QVERIFY(QFileInfo(store.filePathFor(QStringLiteral(".."))).fileName().startsWith(QStringLiteral("%2E")));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray text = f.readAll().toLower();
        QVERIFY(!text.contains("pass"));
        QVERIFY(text.contains("name=core sw/1 (lab)"));
        // Owner-only permissions.
        QCOMPARE(QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions{});

        // A hand-added password key is ignored (there is no field for it) and
        // dropped on the next save.
        {
            QSettings s(path, QSettings::IniFormat);
            s.setValue(QStringLiteral("ssh/password"), QStringLiteral("hunter2"));
        }
        const auto loaded = store.load(in.name);
        QVERIFY(loaded && *loaded == in);
        QVERIFY(store.save(*loaded));
        QVERIFY(!QSettings(path, QSettings::IniFormat).contains(QStringLiteral("ssh/password")));
    }

    void sessionNamesAreValidated_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("valid");
        QTest::newRow("plain") << QStringLiteral("core-sw1") << true;
        QTest::newRow("spaces") << QStringLiteral("lab box 2") << true;
        QTest::newRow("unicode") << QStringLiteral("Zürich edge") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("blank") << QStringLiteral("   ") << false;
        QTest::newRow("padded") << QStringLiteral(" x") << false;
        QTest::newRow("dash") << QStringLiteral("-oProxyCommand=x") << false;
        QTest::newRow("newline") << QStringLiteral("a\nb") << false;
        QTest::newRow("ssh") << QStringLiteral("ssh") << false;
        QTest::newRow("serial") << QStringLiteral("serial") << false;
    }
    void sessionNamesAreValidated()
    {
        QFETCH(QString, name);
        QFETCH(bool, valid);
        QCOMPARE(validateSessionName(name).isEmpty(), valid);
        QTemporaryDir dir;
        const SessionStore store(dir.path());
        SessionConfig s;
        s.name = name;
        QString err;
        QCOMPARE(store.save(s, &err), valid);
        QCOMPARE(err.isEmpty(), valid);
    }

    void unknownSessionMessageListsSaved()
    {
        QTemporaryDir dir;
        const SessionStore store(dir.path());
        QVERIFY(store.unknownSessionMessage(QStringLiteral("x")).contains(QStringLiteral("no saved sessions yet")));
        SessionConfig a;
        a.name = QStringLiteral("beta");
        store.save(a);
        a.name = QStringLiteral("Alpha");
        store.save(a);
        QCOMPARE(store.unknownSessionMessage(QStringLiteral("x")),
                 QStringLiteral("unknown session \"x\"; saved sessions: Alpha, beta"));
    }

    // ---- ssh argv ------------------------------------------------------
    void sshArgvIsBuiltSafely()
    {
        SessionConfig s;
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("sw1.example.net");
        SshCommand c = buildSshCommand(s);
        QVERIFY(c.ok());
        QCOMPARE(c.program, QStringLiteral("ssh"));
        // Keepalive (0.9.0) is on by default: 30 s x 3.
        QCOMPARE(c.args, (QStringList{QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"), QStringLiteral("-o"),
                                      QStringLiteral("ServerAliveCountMax=3"), QStringLiteral("--"),
                                      QStringLiteral("sw1.example.net")}));
        s.keepaliveInterval = 0;
        QCOMPARE(buildSshCommand(s).args, (QStringList{QStringLiteral("--"), QStringLiteral("sw1.example.net")}));

        s = sshSession();
        c = buildSshCommand(s);
        QVERIFY2(c.ok(), qPrintable(c.error));
        QCOMPARE(c.args, (QStringList{QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"), QStringLiteral("-4"),
                                      QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=30"), QStringLiteral("-o"),
                                      QStringLiteral("ServerAliveCountMax=3"), QStringLiteral("-p"), QStringLiteral("2222"), QStringLiteral("-l"), QStringLiteral("admin"),
                                      QStringLiteral("-i"), QDir::homePath() + QStringLiteral("/.ssh/id_lab"),
                                      QStringLiteral("-J"), QStringLiteral("sbj@vertex:22"),
                                      QStringLiteral("--"), QStringLiteral("10.0.0.1")}));

        // Quoted extra values stay one argument and are never shell-evaluated.
        s.extraArgs = QStringLiteral("-o \"ProxyJump none\" -o 'SetEnv=A=$(id)'");
        c = buildSshCommand(s);
        QVERIFY2(c.ok(), qPrintable(c.error));
        QCOMPARE(c.args.mid(0, 4), (QStringList{QStringLiteral("-o"), QStringLiteral("ProxyJump none"),
                                               QStringLiteral("-o"), QStringLiteral("'SetEnv=A=$(id)'")}));

        // IPv6 and bracketed IPv6.
        s = SessionConfig{};
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("fe80::1%eth0");
        QVERIFY(buildSshCommand(s).ok());
        s.host = QStringLiteral("[2001:db8::1]");
        QVERIFY(buildSshCommand(s).ok());
    }

    void sshInjectionIsRejected_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("value");
        QTest::newRow("host option") << "host" << "-oProxyCommand=touch /tmp/pwned";
        QTest::newRow("host dash") << "host" << "-p";
        QTest::newRow("host space") << "host" << "sw1 -oProxyCommand=x";
        QTest::newRow("host semicolon") << "host" << "sw1;id";
        QTest::newRow("host backtick") << "host" << "`id`";
        QTest::newRow("host user@") << "host" << "root@sw1";
        QTest::newRow("host newline") << "host" << "sw1\n-oX";
        QTest::newRow("host empty") << "host" << "";
        QTest::newRow("user option") << "user" << "-oProxyCommand=x";
        QTest::newRow("user at") << "user" << "a@b";
        QTest::newRow("user space") << "user" << "a b";
        QTest::newRow("jump option") << "jump" << "-oProxyCommand=x";
        QTest::newRow("jump hop option") << "jump" << "ok,-oProxyCommand=x";
        QTest::newRow("jump space") << "jump" << "a b";
        QTest::newRow("jump empty hop") << "jump" << "a,,b";
        QTest::newRow("extra destination") << "extra" << "-4 evil.example";
        QTest::newRow("extra remote cmd") << "extra" << "-o X=1 rm";
        QTest::newRow("extra double dash") << "extra" << "-- other";
        QTest::newRow("extra long opt") << "extra" << "--help";
        QTest::newRow("extra missing value") << "extra" << "-o";
        QTest::newRow("port zero") << "port" << "0";
        QTest::newRow("port big") << "port" << "70000";
    }
    void sshInjectionIsRejected()
    {
        QFETCH(QString, field);
        QFETCH(QString, value);
        SessionConfig s;
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("sw1");
        if (field == QLatin1String("host")) {
            s.host = value;
        } else if (field == QLatin1String("user")) {
            s.user = value;
        } else if (field == QLatin1String("jump")) {
            s.jumpHost = value;
        } else if (field == QLatin1String("extra")) {
            s.extraArgs = value;
        } else {
            s.port = value.toInt();
        }
        const SshCommand c = buildSshCommand(s);
        QVERIFY2(!c.ok(), qPrintable(c.args.join(QLatin1Char(' '))));
        QVERIFY(c.args.isEmpty());
    }

    void hostIsAlwaysAfterDoubleDash()
    {
        SessionConfig s;
        s.type = SessionConfig::Type::Ssh;
        s.host = QStringLiteral("h");
        s.extraArgs = QStringLiteral("-vo StrictHostKeyChecking=no -L 8080:localhost:80");
        const SshCommand c = buildSshCommand(s);
        QVERIFY2(c.ok(), qPrintable(c.error));
        QCOMPARE(c.args.size() >= 2, true);
        QCOMPARE(c.args.at(c.args.size() - 2), QStringLiteral("--"));
        QCOMPARE(c.args.last(), QStringLiteral("h"));
        QCOMPARE(c.args.count(QStringLiteral("--")), 1);
    }

    void adHocSshArgsToSession()
    {
        QString why;
        auto s = sessionFromSshArgs({QStringLiteral("-p"), QStringLiteral("2222"), QStringLiteral("-J"), QStringLiteral("vertex"),
                                     QStringLiteral("-o"), QStringLiteral("ServerAliveInterval 30"), QStringLiteral("admin@sw1")},
                                    &why);
        QVERIFY2(s, qPrintable(why));
        QCOMPARE(s->host, QStringLiteral("sw1"));
        QCOMPARE(s->user, QStringLiteral("admin"));
        QCOMPARE(s->port, 2222);
        QCOMPARE(s->jumpHost, QStringLiteral("vertex"));
        const SshCommand c = buildSshCommand(*s);
        QVERIFY(c.args.contains(QStringLiteral("ServerAliveInterval 30")));

        QVERIFY(!sessionFromSshArgs({QStringLiteral("sw1"), QStringLiteral("uptime")}, &why));
        QVERIFY(why.contains(QStringLiteral("remote command")));
        QVERIFY(!sessionFromSshArgs({QStringLiteral("--"), QStringLiteral("-oProxyCommand=x")}, &why));
    }

    // ---- command line (zt) -----------------------------------------------
    void commandLineForSessions_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("name");
        using K = LaunchRequest::Kind;
        QTest::newRow("saved") << QStringList{QStringLiteral("core-sw1")} << int(K::SavedSession) << QStringLiteral("core-sw1");
        QTest::newRow("saved spaces") << QStringList{QStringLiteral("lab box")} << int(K::SavedSession) << QStringLiteral("lab box");
        QTest::newRow("list") << QStringList{QStringLiteral("--list-sessions")} << int(K::ListSessions) << QString();
        QTest::newRow("list extra") << QStringList{QStringLiteral("--list-sessions"), QStringLiteral("x")} << int(K::Error) << QString();
        QTest::newRow("check") << QStringList{QStringLiteral("--check-session"), QStringLiteral("lab box")}
                               << int(K::CheckSession) << QStringLiteral("lab box");
        QTest::newRow("check missing") << QStringList{QStringLiteral("--check-session")} << int(K::Error) << QString();
        QTest::newRow("dash name") << QStringList{QStringLiteral("-oProxyCommand=x")} << int(K::Error) << QString();
        QTest::newRow("serial") << QStringList{QStringLiteral("serial"), QStringLiteral("/dev/ttyUSB0")} << int(K::Serial) << QString();
        QTest::newRow("serial baud") << QStringList{QStringLiteral("serial"), QStringLiteral("/dev/ttyACM0"), QStringLiteral("115200")}
                                     << int(K::Serial) << QString();
        QTest::newRow("serial none") << QStringList{QStringLiteral("serial")} << int(K::Error) << QString();
        QTest::newRow("serial relative") << QStringList{QStringLiteral("serial"), QStringLiteral("ttyUSB0")} << int(K::Error) << QString();
        QTest::newRow("serial bad baud") << QStringList{QStringLiteral("serial"), QStringLiteral("/dev/ttyUSB0"), QStringLiteral("fast")}
                                         << int(K::Error) << QString();
        QTest::newRow("serial extra") << QStringList{QStringLiteral("serial"), QStringLiteral("/dev/ttyUSB0"), QStringLiteral("9600"), QStringLiteral("x")}
                                      << int(K::Error) << QString();
        QTest::newRow("two words") << QStringList{QStringLiteral("lab"), QStringLiteral("box")} << int(K::Error) << QString();
    }
    void commandLineForSessions()
    {
        QFETCH(QStringList, args);
        QFETCH(int, kind);
        QFETCH(QString, name);
        const LaunchRequest r = parseCommandLine(args);
        QCOMPARE(int(r.kind), kind);
        QCOMPARE(r.sessionName, name);
        if (r.kind == LaunchRequest::Kind::SavedSession) {
            QCOMPARE(r.displayName(), name);
        }
        if (r.kind == LaunchRequest::Kind::Serial) {
            QCOMPARE(r.program, args.at(1));
            QCOMPARE(r.baudRate, args.size() == 3 ? args.at(2).toInt() : 9600);
            QCOMPARE(r.displayName(), QStringLiteral("serial ") + args.at(1));
        }
    }

    // ---- JSON export / import -------------------------------------------
    void exportJsonRoundTripAndNoSecrets()
    {
        SessionConfig ssh = sshSession();
        ssh.useStoredPassword = true;
        ssh.autoLog = true;
        ssh.autoReconnect = true;
        ssh.keepaliveInterval = 15;
        ssh.keepaliveCountMax = 4;

        SessionConfig serial;
        serial.name = QStringLiteral("console-1");
        serial.type = SessionConfig::Type::Serial;
        serial.serialDevice = QStringLiteral("/dev/ttyUSB0");
        serial.baudRate = 115200;
        serial.loginUser = QStringLiteral("cisco");
        serial.localEcho = true;
        serial.charDelayMs = 5;

        SessionConfig local;
        local.name = QStringLiteral("Local");
        local.fontFamily = QStringLiteral("monospace");
        local.fontSize = 12;

        const QByteArray json = sessionsToExportJson({ssh, serial, local});
        QVERIFY(json.contains(R"("format": "zterminal-sessions")") || json.contains(R"("format":"zterminal-sessions")"));
        QVERIFY(json.contains(R"("formatVersion": 1)") || json.contains(R"("formatVersion":1)"));
        // Secrets must never appear — even if someone stuffed one into a field name elsewhere.
        const QByteArray lower = json.toLower();
        QVERIFY(!lower.contains("password\":"));
        QVERIFY(!lower.contains("hunter"));
        QVERIFY(!lower.contains("secret\":"));
        QVERIFY(json.contains(R"("passwordStored": true)") || json.contains(R"("passwordStored":true)"));

        QString err;
        const auto doc = sessionsFromExportJson(json, &err);
        QVERIFY2(doc, qPrintable(err));
        QCOMPARE(doc->formatVersion, kSessionsExportFormatVersion);
        QCOMPARE(doc->sessions.size(), 3);
        QCOMPARE(doc->sessions.at(0), ssh);
        QCOMPARE(doc->sessions.at(1), serial);
        QCOMPARE(doc->sessions.at(2), local);

        // Per-session helpers.
        QCOMPARE(sessionFromJson(sessionToJson(ssh)).value(), ssh);
        QCOMPARE(sessionFromJson(sessionToJson(serial)).value(), serial);
        QCOMPARE(sessionFromJson(sessionToJson(local)).value(), local);
    }

    void exportImportViaStore()
    {
        QTemporaryDir dir;
        SessionStore store(dir.filePath(QStringLiteral("sessions")));
        SessionConfig a = sshSession();
        a.useStoredPassword = true;
        SessionConfig b;
        b.name = QStringLiteral("lab box");
        QVERIFY(store.save(a));
        QVERIFY(store.save(b));

        const QString path = dir.filePath(QStringLiteral("out.json"));
        QString err;
        QVERIFY2(exportSessionsToFile(store, path, &err), qPrintable(err));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        QVERIFY(!bytes.toLower().contains("pass\":") || bytes.contains("passwordStored"));
        // No plaintext password value keys.
        QVERIFY(!QString::fromUtf8(bytes).contains(QStringLiteral("\"password\"")));

        SessionStore dest(dir.filePath(QStringLiteral("imported")));
        QVERIFY2(importSessionsFromFile(dest, path, SessionImportConflict::Overwrite, &err), qPrintable(err));
        QCOMPARE(dest.names().size(), 2);
        QCOMPARE(*dest.load(a.name), a);
        QCOMPARE(*dest.load(b.name), b);

        // Skip policy leaves an existing session alone.
        SessionConfig changed = a;
        changed.host = QStringLiteral("9.9.9.9");
        QVERIFY(dest.save(changed));
        const auto r = importSessionsFromJson(dest, bytes, SessionImportConflict::Skip, &err);
        QCOMPARE(r.skipped, 2); // both names already exist
        QCOMPARE(r.imported, 0);
        QCOMPARE(r.overwritten, 0);
        QCOMPARE(dest.load(a.name)->host, QStringLiteral("9.9.9.9"));

        const auto r2 = importSessionsFromJson(dest, bytes, SessionImportConflict::Overwrite, &err);
        QCOMPARE(r2.overwritten, 2);
        QCOMPARE(dest.load(a.name)->host, a.host);
    }

    void exportRejectsBadDocuments()
    {
        QString err;
        QVERIFY(!sessionsFromExportJson("{}", &err));
        QVERIFY(err.contains(QStringLiteral("zterminal-sessions")));
        QVERIFY(!sessionsFromExportJson(R"({"format":"zterminal-sessions","formatVersion":99,"sessions":[]})", &err));
        QVERIFY(err.contains(QStringLiteral("formatVersion")));
        QVERIFY(!sessionsFromExportJson(R"({"format":"zterminal-sessions","formatVersion":1})", &err));
        QVERIFY(err.contains(QStringLiteral("sessions")));
        QVERIFY(!sessionFromJson(QJsonObject{}, &err));
        QVERIFY(!sessionFromJson(QJsonObject{{QStringLiteral("name"), QStringLiteral("-bad")}}, &err));
    }

    void defaultExportFileNameLooksRight()
    {
        const QString n = defaultSessionsExportFileName();
        QVERIFY(n.startsWith(QStringLiteral("zterminal-sessions-")));
        QVERIFY(n.endsWith(QStringLiteral(".json")));
        QCOMPARE(n.size(), QStringLiteral("zterminal-sessions-YYYYMMDD.json").size());
    }
};

QTEST_GUILESS_MAIN(TstSessions)
#include "tst_sessions.moc"
