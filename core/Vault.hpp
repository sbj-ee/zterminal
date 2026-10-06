#pragma once

#include "SecureBuffer.hpp"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <map>

namespace zterminal {

// Encrypted password vault: ~/.config/zterminal/vault.bin.
//
// File format v1 (all integers little-endian):
//   offset  size  field
//        0     8  magic "ZTVAULT\0"
//        8     2  format version (1)
//       10     1  KDF id   (1 = crypto_pwhash ALG_ARGON2ID13)
//       11     1  AEAD id  (1 = crypto_aead_xchacha20poly1305_ietf)
//       12     8  KDF opslimit
//       20     8  KDF memlimit (bytes)
//       28    16  KDF salt
//       44    24  AEAD nonce (fresh for every write)
//       68     *  ciphertext || 16-byte Poly1305 tag
// The whole 68-byte header is the AEAD associated data, so changing any
// header byte (including the KDF parameters or salt) makes decryption fail.
//
// Plaintext: u32 count, then count x { u16 keyLen, key (UTF-8), u32 valLen, value }.
// Keys: "ssh-password/<session>", "serial-password/<session>".
//
// The derived key and every decrypted value live in SecureBuffer
// (sodium_malloc'd, mlock'd, zeroed on free). lock() frees them all.
class Vault
{
public:
    struct KdfParams {
        std::uint64_t opsLimit = 0;
        std::uint64_t memLimit = 0;
    };

    static constexpr std::uint64_t kProductionMemLimit = 256ull * 1024 * 1024; // 256 MiB
    static constexpr double kTargetSeconds = 0.75; // aim for ~0.5-1 s per derivation
    static constexpr std::uint64_t kMinOps = 2;
    static constexpr std::uint64_t kMaxOps = 10;
    static constexpr int kHeaderSize = 68;
    static constexpr int kFormatVersion = 1;

    explicit Vault(QString path = defaultPath());
    ~Vault();
    Vault(const Vault &) = delete;
    Vault &operator=(const Vault &) = delete;

    static QString defaultPath(); // $XDG_CONFIG_HOME/zterminal/vault.bin

    QString path() const { return m_path; }
    bool exists() const;
    bool isUnlocked() const { return !m_key.isEmpty(); }
    QString lastError() const { return m_error; }

    // Create a new, empty vault (fails if one exists). Leaves it unlocked.
    bool create(const SecureBuffer &password);
    // Derive the key from the header's parameters and decrypt.
    bool unlock(const SecureBuffer &password);
    // Wipe and free the key and all decrypted secrets.
    void lock();
    // New salt (and current production KDF params), re-encrypt, atomic write.
    bool changePassword(const SecureBuffer &oldPassword, const SecureBuffer &newPassword);

    // Re-read the file with the in-memory key (picks up writes by other
    // windows). Fails (and locks) if the password was changed elsewhere.
    bool refresh();
    // nullptr if locked or absent.
    const SecureBuffer *secret(const QString &key) const;
    // Read-modify-write under an flock: refresh, apply, write atomically.
    bool setSecret(const QString &key, SecureBuffer value);
    bool removeSecret(const QString &key);
    // Several changes in one locked read-modify-write (one file write):
    // remove every key in `remove`, then set every entry of `set`.
    bool update(std::map<QString, SecureBuffer> set, const QStringList &remove = {});
    QStringList keys() const;

    // KDF parameters recorded in the current header (valid while unlocked).
    KdfParams kdfParams() const { return m_params; }

    // Production parameters: memlimit 256 MiB, opslimit calibrated once per
    // process so a derivation takes ~kTargetSeconds (clamped to [2, 10]). Calibration runs
    // only when a vault is created or its password changed; unlock uses the header's values.
    static KdfParams productionParams();
    // round(kTargetSeconds / secondsPerOp) clamped to [kMinOps, kMaxOps]. productionParams()
    // feeds it the per-pass cost corrected for fixed overhead (ops=2 vs ops=1 timing).
    static std::uint64_t opsForTiming(double secondsPerOp);
    // Test hook: use cheap parameters for new vaults/password changes.
    static void setKdfOverrideForTests(std::uint64_t ops, std::uint64_t mem);
    static void clearKdfOverrideForTests();
    // Parameters new vaults would get right now (override or production).
    static KdfParams paramsForNewVault();

    static QString secretKeyFor(const QString &kind, const QString &sessionName)
    {
        return kind + QLatin1Char('/') + sessionName;
    }

    // The two parsers that see untrusted bytes, for tests and fuzzers: the
    // file header (before any key derivation) and the decrypted entry table.
    // Both return false on malformed input and never read out of bounds.
    static bool checkHeader(const QByteArray &file, QString *error = nullptr);
    static bool parseEntries(const unsigned char *data, std::size_t len, std::map<QString, SecureBuffer> *out);

private:
    struct Header {
        KdfParams params;
        unsigned char salt[16];
        unsigned char nonce[24];
    };
    static bool parseHeaderStatic(const QByteArray &data, Header *h, QString *error);
    bool readFile(QByteArray *data);
    bool parseHeader(const QByteArray &data, Header *h);
    bool deriveKey(const SecureBuffer &password, const Header &h, SecureBuffer *key);
    bool decryptWith(const QByteArray &data, const Header &h, const SecureBuffer &key,
                     std::map<QString, SecureBuffer> *out);
    bool writeWith(const Header &h, const SecureBuffer &key, const std::map<QString, SecureBuffer> &entries);
    bool fail(const QString &msg);

    QString m_path;
    QString m_error;
    SecureBuffer m_key;
    Header m_header {};
    KdfParams m_params;
    std::map<QString, SecureBuffer> m_entries;
};

} // namespace zterminal
