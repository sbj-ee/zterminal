// core/Vault: file format, Argon2id/XChaCha20-Poly1305 round trip, tamper and
// truncation detection, password change, permissions, atomic writes, and the
// production KDF defaults. Uses cheap KDF parameters via the test hook except
// in productionDefaults*.
#include "SecureBuffer.hpp"
#include "Session.hpp"
#include "SessionExport.hpp"
#include "SessionStore.hpp"
#include "Vault.hpp"
#include "VaultBindings.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <sodium.h>

#include <sys/stat.h>
#include <unistd.h>

using namespace zterminal;

namespace {

SecureBuffer pw(const char *s)
{
    return SecureBuffer::fromQString(QString::fromUtf8(s));
}

QString str(const SecureBuffer *b)
{
    return b ? QString::fromUtf8(reinterpret_cast<const char *>(b->data()), qsizetype(b->size())) : QStringLiteral("<null>");
}

QByteArray readAll(const QString &p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeAll(const QString &p, const QByteArray &d)
{
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(d);
}

std::uint64_t le64(const QByteArray &d, int off)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= std::uint64_t(static_cast<unsigned char>(d[off + i])) << (8 * i);
    }
    return v;
}

} // namespace

class TstVault : public QObject
{
    Q_OBJECT
    QTemporaryDir tmp;
    int n = 0;

    QString freshPath() { return tmp.filePath(QStringLiteral("v%1/vault.bin").arg(++n)); }

    // A vault with two secrets, locked again.
    QString makeVault()
    {
        const QString p = freshPath();
        Vault v(p);
        if (!v.create(pw("correct horse")) || !v.setSecret(QStringLiteral("ssh-password/core-sw1"), pw("s3cret-ssh"))
            || !v.setSecret(QStringLiteral("serial-password/console"), pw("ciscö🔑"))) {
            qFatal("makeVault: %s", qPrintable(v.lastError()));
        }
        return p;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(SecureBuffer::ensureSodium());
        Vault::setKdfOverrideForTests(1, 8192); // crypto_pwhash minimums: fast tests
    }

    void productionDefaults()
    {
        // The parameters real vaults get (the hook only lowers them in tests).
        QCOMPARE(Vault::kProductionMemLimit, std::uint64_t(256) * 1024 * 1024);
        QCOMPARE(Vault::kMinOps, std::uint64_t(2));
        QCOMPARE(Vault::kMaxOps, std::uint64_t(10));
        QVERIFY(Vault::kTargetSeconds >= 0.5 && Vault::kTargetSeconds <= 1.0);
        QCOMPARE(int(crypto_pwhash_ALG_ARGON2ID13), crypto_pwhash_alg_argon2id13());
        // Calibration: ops = round(target / seconds-per-pass), clamped to [2, 10].
        QCOMPARE(Vault::opsForTiming(0.25), std::uint64_t(3));
        QCOMPARE(Vault::opsForTiming(0.15), std::uint64_t(5));
        QCOMPARE(Vault::opsForTiming(2.0), std::uint64_t(2));   // slow machine: floor
        QCOMPARE(Vault::opsForTiming(0.001), std::uint64_t(10)); // fast machine: cap
        QCOMPARE(Vault::opsForTiming(0.0), std::uint64_t(2));
        const Vault::KdfParams p = Vault::productionParams();
        QCOMPARE(p.memLimit, Vault::kProductionMemLimit);
        QVERIFY(p.opsLimit >= 2 && p.opsLimit <= 10);
        // With the hook set, new vaults are cheap; without it they get production params.
        QCOMPARE(Vault::paramsForNewVault().memLimit, std::uint64_t(8192));
        Vault::clearKdfOverrideForTests();
        QCOMPARE(Vault::paramsForNewVault().memLimit, Vault::kProductionMemLimit);
        Vault::setKdfOverrideForTests(1, 8192);
    }

    void productionDefaultsRealVault()
    {
        // One real 256 MiB derivation: the header records the production params.
        Vault::clearKdfOverrideForTests();
        const QString p = freshPath();
        {
            Vault v(p);
            QElapsedTimer t;
            t.start();
            QVERIFY2(v.create(pw("correct horse")), qPrintable(v.lastError()));
            qInfo("production KDF: opslimit=%s memlimit=%s MiB, create took %s ms",
                  qPrintable(QString::number(v.kdfParams().opsLimit)),
                  qPrintable(QString::number(v.kdfParams().memLimit >> 20)), qPrintable(QString::number(t.elapsed())));
        }
        const QByteArray d = readAll(p);
        QCOMPARE(le64(d, 20), Vault::kProductionMemLimit);
        QCOMPARE(le64(d, 12), Vault::productionParams().opsLimit);
        Vault v(p);
        QVERIFY(v.unlock(pw("correct horse")));
        Vault::setKdfOverrideForTests(1, 8192);
    }

    void headerLayout()
    {
        const QString p = makeVault();
        const QByteArray d = readAll(p);
        QVERIFY(d.size() >= Vault::kHeaderSize + 16);
        QCOMPARE(d.left(8), QByteArray("ZTVAULT\0", 8));
        QCOMPARE(int(uchar(d[8])) | (int(uchar(d[9])) << 8), 1); // format version
        QCOMPARE(int(uchar(d[10])), 1); // KDF: argon2id13
        QCOMPARE(int(uchar(d[11])), 1); // AEAD: xchacha20poly1305-ietf
        QCOMPARE(le64(d, 12), std::uint64_t(1));
        QCOMPARE(le64(d, 20), std::uint64_t(8192));
        // Nothing readable in the file.
        QVERIFY(!d.contains("s3cret"));
        QVERIFY(!d.contains("core-sw1"));
    }

    // The untrusted-input parsers (also fuzzed: fuzz/fuzz_vault.cpp).
    void parsersRejectMalformedInput()
    {
        QString err;
        QVERIFY(!Vault::checkHeader(QByteArray(), &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(!Vault::checkHeader(QByteArray(200, 'x'), &err));
        std::map<QString, SecureBuffer> out;
        const unsigned char none[1] = {0};
        QVERIFY(!Vault::parseEntries(none, 0, &out));
        // count = 0xFFFFFFFF with no data: refused before allocating anything.
        const unsigned char huge[4] = {0xff, 0xff, 0xff, 0xff};
        QVERIFY(!Vault::parseEntries(huge, sizeof huge, &out));
        // More entries than any real vault, each well-formed (empty key/value).
        QByteArray many;
        const quint32 entryCount = 20000;
        many.append(reinterpret_cast<const char *>(&entryCount), 4); // little-endian hosts (CI)
        many.append(QByteArray(int(entryCount) * 6, '\0'));
        QVERIFY(!Vault::parseEntries(reinterpret_cast<const unsigned char *>(many.constData()), std::size_t(many.size()), &out));
        // A good table.
        const unsigned char good[] = {1, 0, 0, 0, 1, 0, 'k', 2, 0, 0, 0, 'v', 'w'};
        QVERIFY(Vault::parseEntries(good, sizeof good, &out));
        QCOMPARE(out.size(), std::size_t(1));
        QCOMPARE(out.begin()->first, QStringLiteral("k"));
        QCOMPARE(out.begin()->second.size(), std::size_t(2));
        // Truncated value.
        QVERIFY(!Vault::parseEntries(good, sizeof good - 1, &out));
    }

    void roundTrip()
    {
        const QString p = makeVault();
        Vault v(p);
        QVERIFY(v.exists());
        QVERIFY(!v.isUnlocked());
        QVERIFY(!v.secret(QStringLiteral("ssh-password/core-sw1")));
        QVERIFY2(v.unlock(pw("correct horse")), qPrintable(v.lastError()));
        QCOMPARE(str(v.secret(QStringLiteral("ssh-password/core-sw1"))), QStringLiteral("s3cret-ssh"));
        QCOMPARE(str(v.secret(QStringLiteral("serial-password/console"))), QString::fromUtf8("ciscö🔑"));
        QCOMPARE(v.keys().size(), 2);
        QVERIFY(v.removeSecret(QStringLiteral("serial-password/console")));
        v.lock();
        QVERIFY(v.unlock(pw("correct horse")));
        QCOMPARE(v.keys(), QStringList{QStringLiteral("ssh-password/core-sw1")});
    }

    void wrongPasswordFails()
    {
        const QString p = makeVault();
        Vault v(p);
        QVERIFY(!v.unlock(pw("correct horsf")));
        QVERIFY(!v.isUnlocked());
        QVERIFY(v.lastError().contains(QStringLiteral("Wrong master password")));
        QVERIFY(!v.unlock(pw("")));
        QVERIFY(v.unlock(pw("correct horse")));
    }

    void createRefusesExistingAndEmpty()
    {
        const QString p = makeVault();
        Vault v(p);
        QVERIFY(!v.create(pw("other")));
        Vault e(freshPath());
        QVERIFY(!e.create(pw("")));
        QVERIFY(!e.exists());
    }

    void everyTamperedHeaderByteIsDetected()
    {
        const QString p = makeVault();
        const QByteArray good = readAll(p);
        for (int i = 0; i < Vault::kHeaderSize; ++i) {
            QByteArray bad = good;
            bad[i] = char(bad[i] ^ 0x01);
            writeAll(p, bad);
            Vault v(p);
            QVERIFY2(!v.unlock(pw("correct horse")), qPrintable(QStringLiteral("header byte %1 not detected").arg(i)));
            QVERIFY(!v.isUnlocked());
        }
        writeAll(p, good);
        Vault v(p);
        QVERIFY(v.unlock(pw("correct horse")));
    }

    void tamperedCiphertextIsDetected()
    {
        const QString p = makeVault();
        const QByteArray good = readAll(p);
        // First / middle ciphertext byte and the last (tag) byte.
        for (qsizetype i : {qsizetype(Vault::kHeaderSize), (Vault::kHeaderSize + good.size()) / 2, good.size() - 1}) {
            QByteArray bad = good;
            bad[i] = char(bad[i] ^ 0x80);
            writeAll(p, bad);
            Vault v(p);
            QVERIFY2(!v.unlock(pw("correct horse")), qPrintable(QStringLiteral("byte %1 not detected").arg(i)));
            QVERIFY(v.lastError().contains(QStringLiteral("modified or damaged")));
        }
    }

    void truncatedFileIsDetected()
    {
        const QString p = makeVault();
        const QByteArray good = readAll(p);
        for (qsizetype len : {qsizetype(0), qsizetype(7), qsizetype(Vault::kHeaderSize - 1), qsizetype(Vault::kHeaderSize),
                              qsizetype(Vault::kHeaderSize + 15), good.size() - 1}) {
            writeAll(p, good.left(len));
            Vault v(p);
            QVERIFY2(!v.unlock(pw("correct horse")), qPrintable(QStringLiteral("truncation to %1 not detected").arg(len)));
        }
        // Appended garbage is detected as well.
        writeAll(p, good + "x");
        Vault v(p);
        QVERIFY(!v.unlock(pw("correct horse")));
    }

    void changePasswordReencrypts()
    {
        const QString p = makeVault();
        const QByteArray before = readAll(p);
        Vault v(p);
        QVERIFY(v.unlock(pw("correct horse")));
        QVERIFY(!v.changePassword(pw("not it"), pw("battery staple")));
        QVERIFY(v.isUnlocked()); // a failed change doesn't lock or alter anything
        QCOMPARE(readAll(p), before);
        QVERIFY2(v.changePassword(pw("correct horse"), pw("battery staple")), qPrintable(v.lastError()));
        const QByteArray after = readAll(p);
        QVERIFY(after.mid(28, 16) != before.mid(28, 16)); // new salt
        QVERIFY(after.mid(44, 24) != before.mid(44, 24)); // new nonce
        QCOMPARE(str(v.secret(QStringLiteral("ssh-password/core-sw1"))), QStringLiteral("s3cret-ssh"));
        v.lock();
        QVERIFY(!v.unlock(pw("correct horse"))); // old password no longer works
        QVERIFY(v.unlock(pw("battery staple")));
        QCOMPARE(str(v.secret(QStringLiteral("serial-password/console"))), QString::fromUtf8("ciscö🔑"));
        // Another window holding the old key notices and locks instead of clobbering.
        Vault other(p);
        QVERIFY(other.unlock(pw("battery staple")));
        QVERIFY(v.changePassword(pw("battery staple"), pw("third one!")));
        QVERIFY(!other.setSecret(QStringLiteral("x"), pw("y")));
        QVERIFY(!other.isUnlocked());
        QVERIFY(other.lastError().contains(QStringLiteral("another window")));
    }

    void twoWindowsDontLoseUpdates()
    {
        const QString p = makeVault();
        Vault a(p), b(p);
        QVERIFY(a.unlock(pw("correct horse")));
        QVERIFY(b.unlock(pw("correct horse")));
        QVERIFY(a.setSecret(QStringLiteral("ssh-password/a"), pw("A")));
        QVERIFY(b.setSecret(QStringLiteral("ssh-password/b"), pw("B"))); // b re-reads first
        Vault c(p);
        QVERIFY(c.unlock(pw("correct horse")));
        QCOMPARE(str(c.secret(QStringLiteral("ssh-password/a"))), QStringLiteral("A"));
        QCOMPARE(str(c.secret(QStringLiteral("ssh-password/b"))), QStringLiteral("B"));
    }

    void fileIsPrivateAndWrittenAtomically()
    {
        const QString p = makeVault();
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(p).constData(), &st), 0);
        QCOMPARE(st.st_mode & 0777, mode_t(0600));
        Vault v(p);
        QVERIFY(v.unlock(pw("correct horse")));
        const ino_t inodeBefore = st.st_ino;
        QVERIFY(v.setSecret(QStringLiteral("k"), pw("v")));
        QCOMPARE(::stat(QFile::encodeName(p).constData(), &st), 0);
        QVERIFY(st.st_ino != inodeBefore); // replaced by rename(), not rewritten in place
        QCOMPARE(st.st_mode & 0777, mode_t(0600));
        // No temp files left behind.
        const QStringList left = QFileInfo(p).dir().entryList(QDir::Files | QDir::Hidden);
        QCOMPARE(left, (QStringList{QStringLiteral("vault.bin"), QStringLiteral("vault.bin.lock")}));
        // A failed write (read-only dir) leaves the old file intact.
        const QByteArray good = readAll(p);
        QFile::setPermissions(QFileInfo(p).absolutePath(), QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        if (::access(QFile::encodeName(QFileInfo(p).absolutePath()).constData(), W_OK) != 0) { // not root
            QVERIFY(!v.setSecret(QStringLiteral("k2"), pw("v2")));
            QCOMPARE(readAll(p), good);
        }
        QFile::setPermissions(QFileInfo(p).absolutePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    void lockWipesEverything()
    {
        const QString p = makeVault();
        Vault v(p);
        QVERIFY(v.unlock(pw("correct horse")));
        QVERIFY(v.secret(QStringLiteral("ssh-password/core-sw1")));
        v.lock();
        QVERIFY(!v.isUnlocked());
        QVERIFY(!v.secret(QStringLiteral("ssh-password/core-sw1")));
        QVERIFY(v.keys().isEmpty());
        QVERIFY(!v.setSecret(QStringLiteral("k"), pw("v"))); // locked: refuses
    }

    void secureBuffer()
    {
        const QString s = QString::fromUtf8("pässwörd 🔑 \xe2\x82\xac");
        SecureBuffer b = SecureBuffer::fromQString(s);
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(b.data()), qsizetype(b.size())), s.toUtf8());
        SecureBuffer c = b.clone();
        QVERIFY(b.equals(c));
        c.wipe();
        QCOMPARE(c.size(), b.size());
        for (std::size_t i = 0; i < c.size(); ++i) {
            QCOMPARE(int(c.data()[i]), 0);
        }
        SecureBuffer moved = std::move(b);
        QVERIFY(b.isEmpty());
        QVERIFY(!moved.isEmpty());
        moved.reset();
        QVERIFY(moved.isEmpty() && moved.data() == nullptr);
    }
    // ---- Secrets bound to their target (review item 1) ---------------------
    static SessionConfig ssh(const QString &name, const QString &host)
    {
        SessionConfig s;
        s.name = name;
        s.type = SessionConfig::Type::Ssh;
        s.host = host;
        s.user = QStringLiteral("admin");
        s.useStoredPassword = true;
        return s;
    }

    void bindingBlocksRedirectByImport()
    {
        const QString p = freshPath();
        SessionStore store(QFileInfo(p).absolutePath() + QStringLiteral("/sessions"));
        Vault v(p);
        QVERIFY(v.create(pw("m")));
        const SessionConfig mine = ssh(QStringLiteral("core-sw1"), QStringLiteral("10.0.0.5"));
        QVERIFY(store.save(mine));
        QVERIFY(vaultbind::storeSecret(v, mine, pw("s3cret")));
        QCOMPARE(vaultbind::check(v, mine), vaultbind::Status::Match);
        QCOMPARE(str(v.secret(QStringLiteral("binding/ssh-password/core-sw1"))), QStringLiteral("ssh:admin@10.0.0.5:22"));

        // An import replaces core-sw1 with a session for evil.example that asks
        // for the stored password, then the user (or a later edit) turns it back on.
        SessionConfig evil = ssh(QStringLiteral("core-sw1"), QStringLiteral("evil.example"));
        QString err;
        importSessionsFromJson(store, sessionsToExportJson({evil}), SessionImportConflict::Overwrite, &err,
                               ImportApproval::Approved);
        SessionConfig now = *store.load(QStringLiteral("core-sw1"));
        QVERIFY(!now.useStoredPassword);
        now.useStoredPassword = true;
        QString stored;
        QCOMPARE(vaultbind::check(v, now, &stored), vaultbind::Status::Mismatch);
        QCOMPARE(stored, QStringLiteral("ssh:admin@10.0.0.5:22"));
        // Same host but an extra -o HostName / -l / -p redirect is a mismatch too.
        SessionConfig sneaky = mine;
        sneaky.extraArgs = QStringLiteral("-l root");
        QCOMPARE(vaultbind::check(v, sneaky), vaultbind::Status::Mismatch);
        sneaky.extraArgs = QStringLiteral("-p 2222");
        QCOMPARE(vaultbind::check(v, sneaky), vaultbind::Status::Mismatch);
        // Reconcile at unlock never re-binds a mismatch (the session exists).
        const auto r = vaultbind::reconcile(v, store);
        QVERIFY(r.ok);
        QVERIFY(r.migrated.isEmpty());
        QCOMPARE(vaultbind::check(v, now), vaultbind::Status::Mismatch);
        // Only an explicit re-bind makes it usable for the new target.
        QVERIFY(vaultbind::rebind(v, now));
        QCOMPARE(vaultbind::check(v, now), vaultbind::Status::Match);
        QCOMPARE(str(v.secret(QStringLiteral("ssh-password/core-sw1"))), QStringLiteral("s3cret"));
    }

    void serialBindingFollowsDevice()
    {
        Vault v(freshPath());
        QVERIFY(v.create(pw("m")));
        SessionConfig s;
        s.name = QStringLiteral("con");
        s.type = SessionConfig::Type::Serial;
        s.serialDevice = QStringLiteral("/dev/ttyUSB0");
        QVERIFY(vaultbind::storeSecret(v, s, pw("c1sco")));
        QCOMPARE(vaultbind::check(v, s), vaultbind::Status::Match);
        s.serialDevice = QStringLiteral("/dev/pts/7");
        QCOMPARE(vaultbind::check(v, s), vaultbind::Status::Mismatch);
    }

    void reconcileMigratesPrunesAndRunsQueuedDeletions()
    {
        const QString p = freshPath();
        SessionStore store(QFileInfo(p).absolutePath() + QStringLiteral("/sessions"));
        Vault v(p);
        QVERIFY(v.create(pw("m")));
        const SessionConfig a = ssh(QStringLiteral("a"), QStringLiteral("a.example"));
        const SessionConfig b = ssh(QStringLiteral("b"), QStringLiteral("b.example"));
        SessionConfig imported = ssh(QStringLiteral("imp"), QStringLiteral("i.example"));
        imported.approved = false;
        QVERIFY(store.save(a));
        QVERIFY(store.save(b));
        QVERIFY(store.save(imported));
        // Stored by zterminal 1.0.0: no bindings.
        QVERIFY(v.setSecret(QStringLiteral("ssh-password/a"), pw("pa")));
        QVERIFY(v.setSecret(QStringLiteral("ssh-password/b"), pw("pb")));
        QVERIFY(v.setSecret(QStringLiteral("ssh-password/imp"), pw("pi")));
        QVERIFY(v.setSecret(QStringLiteral("ssh-password/gone"), pw("orphan")));
        QVERIFY(v.setSecret(QStringLiteral("serial-password/a"), pw("wrong type")));
        QVERIFY(v.setSecret(QStringLiteral("binding/ssh-password/nothing"), pw("ssh:x@y:22")));
        QCOMPARE(vaultbind::check(v, a), vaultbind::Status::Unbound);

        auto r = vaultbind::reconcile(v, store);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(vaultbind::check(v, a), vaultbind::Status::Match);
        QCOMPARE(vaultbind::check(v, b), vaultbind::Status::Match);
        // An unapproved (imported) session is not trusted for migration.
        QCOMPARE(vaultbind::check(v, imported), vaultbind::Status::Unbound);
        QVERIFY(!v.secret(QStringLiteral("ssh-password/gone")));
        QVERIFY(!v.secret(QStringLiteral("serial-password/a")));
        QVERIFY(!v.secret(QStringLiteral("binding/ssh-password/nothing")));
        QVERIFY(r.pruned.contains(QStringLiteral("ssh-password/gone")));
        QCOMPARE(r.migrated.size(), 2);

        // Deleting "b" while the vault is locked queues its deletion...
        v.lock();
        QVERIFY(store.remove(b.name));
        vaultbind::queueDeletion(v, b);
        QCOMPARE(vaultbind::pendingDeletions(v).size(), 2); // ssh + serial key
        // ...and a new session called "b" for another host, saved while locked,
        // must not inherit the old password.
        const SessionConfig b2 = ssh(QStringLiteral("b"), QStringLiteral("other.example"));
        QVERIFY(store.save(b2));
        QVERIFY(v.unlock(pw("m")));
        r = vaultbind::reconcile(v, store);
        QVERIFY(r.ok);
        QVERIFY(r.deleted.contains(QStringLiteral("ssh-password/b")));
        QVERIFY(!v.secret(QStringLiteral("ssh-password/b")));
        QVERIFY(!v.secret(QStringLiteral("binding/ssh-password/b")));
        QCOMPARE(vaultbind::check(v, b2), vaultbind::Status::NoSecret);
        QVERIFY(vaultbind::pendingDeletions(v).isEmpty());
        QVERIFY(!QFile::exists(vaultbind::pendingDeletionsPath(v)));
        QCOMPARE(str(v.secret(QStringLiteral("ssh-password/a"))), QStringLiteral("pa"));

        // A queued deletion doesn't remove a password stored later for a
        // different target under the same name (another window, vault unlocked there).
        v.lock();
        vaultbind::queueDeletion(v, a);
        QVERIFY(v.unlock(pw("m")));
        SessionConfig a2 = a;
        a2.host = QStringLiteral("new-a.example");
        QVERIFY(store.save(a2));
        QVERIFY(vaultbind::storeSecret(v, a2, pw("new pa")));
        r = vaultbind::reconcile(v, store);
        QVERIFY(r.deleted.isEmpty());
        QCOMPARE(str(v.secret(QStringLiteral("ssh-password/a"))), QStringLiteral("new pa"));
        QCOMPARE(vaultbind::check(v, a2), vaultbind::Status::Match);

        // forget() removes both kinds and their bindings in one write.
        QVERIFY(vaultbind::forget(v, a2.name));
        QCOMPARE(vaultbind::check(v, a2), vaultbind::Status::NoSecret);
        QVERIFY(!v.secret(QStringLiteral("binding/ssh-password/a")));
    }

    void reconcileLeavesEverythingWithoutSessionsDir()
    {
        const QString p = freshPath();
        Vault v(p);
        QVERIFY(v.create(pw("m")));
        QVERIFY(v.setSecret(QStringLiteral("ssh-password/x"), pw("px")));
        const auto r = vaultbind::reconcile(v, SessionStore(QFileInfo(p).absolutePath() + QStringLiteral("/nope")));
        QVERIFY(r.ok);
        QVERIFY(v.secret(QStringLiteral("ssh-password/x")));
    }

    void updateIsOneAtomicChange()
    {
        Vault v(freshPath());
        QVERIFY(v.create(pw("m")));
        std::map<QString, SecureBuffer> set;
        set[QStringLiteral("k1")] = pw("1");
        set[QStringLiteral("k2")] = pw("2");
        QVERIFY(v.update(std::move(set)));
        QVERIFY(v.update({}, {QStringLiteral("k1"), QStringLiteral("absent")}));
        QVERIFY(!v.secret(QStringLiteral("k1")));
        QCOMPARE(str(v.secret(QStringLiteral("k2"))), QStringLiteral("2"));
    }
};

QTEST_GUILESS_MAIN(TstVault)
#include "tst_vault.moc"
