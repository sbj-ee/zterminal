#include "CommandLine.hpp"
#include "RestoreFile.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>

using namespace zterminal;

class TstRestore : public QObject
{
    Q_OBJECT

private slots:
    void commandLine()
    {
        const LaunchRequest r = parseCommandLine({QStringLiteral("--restore"), QStringLiteral("/tmp/x.json")});
        QCOMPARE(r.kind, LaunchRequest::Kind::Restore);
        QCOMPARE(r.program, QStringLiteral("/tmp/x.json"));
        QCOMPARE(parseCommandLine({QStringLiteral("--restore")}).kind, LaunchRequest::Kind::Error);
        QCOMPARE(parseCommandLine({QStringLiteral("--restore"), QStringLiteral("a"), QStringLiteral("b")}).kind,
                 LaunchRequest::Kind::Error);
    }

    void restorableTabs()
    {
        QVERIFY(isRestorableTab({}));
        QVERIFY(isRestorableTab({QStringLiteral("user@example.com")}));
        QVERIFY(isRestorableTab({QStringLiteral("-e"), QStringLiteral("top")}));
        QVERIFY(!isRestorableTab({QStringLiteral("--restore"), QStringLiteral("f")}));
        QVERIFY(!isRestorableTab({QStringLiteral("--group"), QStringLiteral("g")}));
        QVERIFY(!isRestorableTab({QStringLiteral("--version")}));
    }

    void roundTrip()
    {
        const QList<RestoreWindow> in{
            {{{}, {QStringLiteral("-e"), QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("exec cat")}}, 1},
            {{{QStringLiteral("user@example.com")}}, 0}};
        const auto out = restoreFromJson(restoreToJson(in));
        QVERIFY(out);
        QCOMPARE(out->size(), 2);
        QCOMPARE(out->at(0).tabs, in.at(0).tabs);
        QCOMPARE(out->at(0).current, 1);
        QCOMPARE(out->at(1).tabs, in.at(1).tabs);
    }

    void dropsUnrestorableTabs()
    {
        const QList<RestoreWindow> in{
            {{{QStringLiteral("--group"), QStringLiteral("g")}, {QStringLiteral("user@example.com")}}, 1},
            {{{QStringLiteral("--restore"), QStringLiteral("f")}}, 0}};
        const auto out = restoreFromJson(restoreToJson(in));
        QVERIFY(out);
        QCOMPARE(out->size(), 1); // the second window had nothing to restore
        QCOMPARE(out->at(0).tabs, QList<QStringList>{{QStringLiteral("user@example.com")}});
        QCOMPARE(out->at(0).current, 0);
    }

    void rejectsGarbage()
    {
        QString err;
        QVERIFY(!restoreFromJson("nope", &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(!restoreFromJson(R"({"format":"other","windows":[]})"));
    }

    void fileIsPrivateAndConsumed()
    {
        const QList<RestoreWindow> in{{{{}, {QStringLiteral("user@example.com")}}, 0}};
        const QString path = writeRestoreFile(in);
        QVERIFY(!path.isEmpty());
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &st), 0);
        QCOMPARE(int(st.st_mode & 0777), 0600);
        const auto out = takeRestoreFile(path);
        QVERIFY(out);
        QCOMPARE(out->at(0).tabs, in.at(0).tabs);
        QVERIFY(!QFile::exists(path)); // read once, then deleted
        QVERIFY(!takeRestoreFile(path));
    }

    void refusesSharedOrLinkedFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("r.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(restoreToJson({{{{}}, 0}}));
        f.close();
        QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther));
        QString err;
        QVERIFY(!takeRestoreFile(path, &err));
        QVERIFY(QFile::exists(path)); // not consumed
        QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const QString link = dir.filePath(QStringLiteral("link.json"));
        QVERIFY(QFile::link(path, link));
        QVERIFY(!takeRestoreFile(link));
        QVERIFY(takeRestoreFile(path));
    }
};

QTEST_MAIN(TstRestore)
#include "tst_restore.moc"
