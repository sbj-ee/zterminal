#include "Vault.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <sodium.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zterminal {

namespace {

constexpr char kMagic[8] = {'Z', 'T', 'V', 'A', 'U', 'L', 'T', '\0'};
constexpr unsigned char kKdfArgon2id13 = 1;
constexpr unsigned char kAeadXChaCha20Poly1305 = 1;
constexpr std::uint64_t kMaxOpsAccepted = 100;
constexpr std::uint64_t kMaxMemAccepted = 1024ull * 1024 * 1024; // 1 GiB
constexpr std::size_t kKeyBytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;
constexpr std::size_t kTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
static_assert(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES == 24);
static_assert(crypto_pwhash_SALTBYTES == 16);

std::atomic<std::uint64_t> g_overrideOps {0};
std::atomic<std::uint64_t> g_overrideMem {0};

void putLe(unsigned char *p, std::uint64_t v, int n)
{
    for (int i = 0; i < n; ++i) {
        p[i] = static_cast<unsigned char>(v >> (8 * i));
    }
}

std::uint64_t getLe(const unsigned char *p, int n)
{
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) {
        v |= std::uint64_t(p[i]) << (8 * i);
    }
    return v;
}

// RAII flock on "<vault>.lock" so read-modify-write cycles from several
// zterminal windows don't lose each other's updates.
class FileLock
{
public:
    explicit FileLock(const QString &path)
    {
        m_fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_fd >= 0) {
            while (::flock(m_fd, LOCK_EX) < 0 && errno == EINTR) {
            }
        }
    }
    ~FileLock()
    {
        if (m_fd >= 0) {
            ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
        }
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

private:
    int m_fd = -1;
};

bool writeAll(int fd, const unsigned char *p, std::size_t n)
{
    while (n > 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

} // namespace

Vault::Vault(QString path)
    : m_path(std::move(path))
{
    SecureBuffer::ensureSodium();
}

Vault::~Vault()
{
    lock();
}

QString Vault::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/zterminal/vault.bin");
}

bool Vault::exists() const
{
    return QFileInfo::exists(m_path);
}

bool Vault::fail(const QString &msg)
{
    m_error = msg;
    return false;
}

void Vault::lock()
{
    m_key.reset();
    m_entries.clear(); // each SecureBuffer zeroes + frees itself
    m_header = Header {};
    m_params = {};
}

std::uint64_t Vault::opsForTiming(double secondsPerOp)
{
    if (!(secondsPerOp > 0)) {
        return kMinOps;
    }
    const double ops = std::round(kTargetSeconds / secondsPerOp);
    if (ops < double(kMinOps)) {
        return kMinOps;
    }
    if (ops > double(kMaxOps)) {
        return kMaxOps;
    }
    return static_cast<std::uint64_t>(ops);
}

Vault::KdfParams Vault::productionParams()
{
    static std::once_flag once;
    static KdfParams params;
    std::call_once(once, [] {
        SecureBuffer::ensureSodium();
        // Time ops=1 and ops=2: the difference is the cost of one pass, the
        // rest is fixed overhead (allocating and filling 256 MiB).
        unsigned char out[32];
        unsigned char salt[crypto_pwhash_SALTBYTES] = {};
        auto timeOps = [&](std::uint64_t ops) -> double {
            QElapsedTimer t;
            t.start();
            if (crypto_pwhash(out, sizeof out, "calibrate", 9, salt, ops, kProductionMemLimit,
                              crypto_pwhash_ALG_ARGON2ID13)
                != 0) {
                return -1;
            }
            return double(t.nsecsElapsed()) / 1e9;
        };
        const double t1 = timeOps(1);
        const double t2 = t1 < 0 ? -1 : timeOps(2);
        sodium_memzero(out, sizeof out);
        params.memLimit = kProductionMemLimit;
        if (t1 < 0 || t2 < 0) {
            params.opsLimit = kMinOps;
        } else {
            const double perPass = std::max(t2 - t1, t1 / 4);
            const double overhead = std::max(0.0, t1 - perPass);
            params.opsLimit = opsForTiming(perPass * kTargetSeconds / std::max(0.05, kTargetSeconds - overhead));
        }
    });
    return params;
}

void Vault::setKdfOverrideForTests(std::uint64_t ops, std::uint64_t mem)
{
    g_overrideOps = ops;
    g_overrideMem = mem;
}

void Vault::clearKdfOverrideForTests()
{
    g_overrideOps = 0;
    g_overrideMem = 0;
}

Vault::KdfParams Vault::paramsForNewVault()
{
    if (g_overrideOps && g_overrideMem) {
        return {g_overrideOps, g_overrideMem};
    }
    return productionParams();
}

bool Vault::readFile(QByteArray *data)
{
    QFile f(m_path);
    if (!f.exists()) {
        return fail(QStringLiteral("No vault exists at %1.").arg(m_path));
    }
    if (!f.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("Cannot read %1: %2").arg(m_path, f.errorString()));
    }
    if (f.size() > 64 * 1024 * 1024) {
        return fail(QStringLiteral("The vault file is implausibly large."));
    }
    *data = f.readAll();
    return true;
}

bool Vault::parseHeader(const QByteArray &data, Header *h)
{
    if (data.size() < kHeaderSize + qsizetype(kTagBytes)) {
        return fail(QStringLiteral("The vault file is truncated or not a zterminal vault."));
    }
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData());
    if (std::memcmp(p, kMagic, sizeof kMagic) != 0) {
        return fail(QStringLiteral("Not a zterminal vault file (bad magic)."));
    }
    if (getLe(p + 8, 2) != kFormatVersion) {
        return fail(QStringLiteral("Unsupported vault format version %1.").arg(getLe(p + 8, 2)));
    }
    if (p[10] != kKdfArgon2id13 || p[11] != kAeadXChaCha20Poly1305) {
        return fail(QStringLiteral("Unsupported vault KDF or cipher."));
    }
    h->params.opsLimit = getLe(p + 12, 8);
    h->params.memLimit = getLe(p + 20, 8);
    if (h->params.opsLimit < crypto_pwhash_OPSLIMIT_MIN || h->params.opsLimit > kMaxOpsAccepted
        || h->params.memLimit < crypto_pwhash_MEMLIMIT_MIN || h->params.memLimit > kMaxMemAccepted) {
        return fail(QStringLiteral("The vault header has out-of-range KDF parameters."));
    }
    std::memcpy(h->salt, p + 28, sizeof h->salt);
    std::memcpy(h->nonce, p + 44, sizeof h->nonce);
    return true;
}

bool Vault::deriveKey(const SecureBuffer &password, const Header &h, SecureBuffer *key)
{
    SecureBuffer k(kKeyBytes);
    if (crypto_pwhash(k.data(), k.size(), reinterpret_cast<const char *>(password.data()), password.size(),
                      h.salt, h.params.opsLimit, static_cast<std::size_t>(h.params.memLimit),
                      crypto_pwhash_ALG_ARGON2ID13)
        != 0) {
        return fail(QStringLiteral("Key derivation failed (out of memory?)."));
    }
    *key = std::move(k);
    return true;
}

static void encodeHeader(unsigned char *out, const Vault::KdfParams &params, const unsigned char *salt,
                         const unsigned char *nonce)
{
    std::memcpy(out, kMagic, sizeof kMagic);
    putLe(out + 8, Vault::kFormatVersion, 2);
    out[10] = kKdfArgon2id13;
    out[11] = kAeadXChaCha20Poly1305;
    putLe(out + 12, params.opsLimit, 8);
    putLe(out + 20, params.memLimit, 8);
    std::memcpy(out + 28, salt, 16);
    std::memcpy(out + 44, nonce, 24);
}

bool Vault::decryptWith(const QByteArray &data, const Header &h, const SecureBuffer &key,
                        std::map<QString, SecureBuffer> *out)
{
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData());
    const std::size_t ctLen = std::size_t(data.size()) - kHeaderSize;
    SecureBuffer plain(ctLen - kTagBytes);
    unsigned long long plainLen = 0;
    // Associated data = the header exactly as stored on disk.
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain.data(), &plainLen, nullptr, p + kHeaderSize, ctLen, p,
                                                   kHeaderSize, h.nonce, key.data())
        != 0) {
        return fail(QStringLiteral("Wrong master password, or the vault file has been modified or damaged."));
    }
    std::map<QString, SecureBuffer> entries;
    const unsigned char *q = plain.data();
    const unsigned char *end = q + plainLen;
    auto need = [&](std::size_t n) { return std::size_t(end - q) >= n; };
    if (!need(4)) {
        return fail(QStringLiteral("The vault contents are malformed."));
    }
    const std::uint64_t count = getLe(q, 4);
    q += 4;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!need(2)) {
            return fail(QStringLiteral("The vault contents are malformed."));
        }
        const std::size_t kl = getLe(q, 2);
        q += 2;
        if (!need(kl + 4)) {
            return fail(QStringLiteral("The vault contents are malformed."));
        }
        const QString k = QString::fromUtf8(reinterpret_cast<const char *>(q), qsizetype(kl));
        q += kl;
        const std::size_t vl = getLe(q, 4);
        q += 4;
        if (!need(vl)) {
            return fail(QStringLiteral("The vault contents are malformed."));
        }
        entries[k] = SecureBuffer(q, vl);
        q += vl;
    }
    *out = std::move(entries);
    return true; // `plain` is zeroed and freed here
}

bool Vault::writeWith(const Header &hIn, const SecureBuffer &key, const std::map<QString, SecureBuffer> &entries)
{
    // Serialize into secure memory.
    std::size_t len = 4;
    std::vector<QByteArray> keyBytes; // keys are names, not secrets
    keyBytes.reserve(entries.size());
    for (const auto &[k, v] : entries) {
        keyBytes.push_back(k.toUtf8());
        if (keyBytes.back().size() > 0xFFFF) {
            return fail(QStringLiteral("Vault key too long."));
        }
        len += 2 + std::size_t(keyBytes.back().size()) + 4 + v.size();
    }
    SecureBuffer plain(len);
    unsigned char *q = plain.data();
    putLe(q, entries.size(), 4);
    q += 4;
    std::size_t idx = 0;
    for (const auto &[k, v] : entries) {
        const QByteArray &kb = keyBytes[idx++];
        putLe(q, std::uint64_t(kb.size()), 2);
        q += 2;
        std::memcpy(q, kb.constData(), std::size_t(kb.size()));
        q += kb.size();
        putLe(q, v.size(), 4);
        q += 4;
        if (v.size()) {
            std::memcpy(q, v.data(), v.size());
        }
        q += v.size();
    }

    Header h = hIn;
    randombytes_buf(h.nonce, sizeof h.nonce); // never reuse a nonce under one key
    unsigned char hdr[kHeaderSize];
    encodeHeader(hdr, h.params, h.salt, h.nonce);
    std::vector<unsigned char> file(kHeaderSize + len + kTagBytes);
    std::memcpy(file.data(), hdr, sizeof hdr);
    unsigned long long ctLen = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(file.data() + kHeaderSize, &ctLen, plain.data(), plain.size(),
                                               hdr, kHeaderSize, nullptr, h.nonce, key.data());
    plain.reset();

    // Atomic replace: temp file in the same directory, fsync, rename, fsync dir.
    const QFileInfo fi(m_path);
    const QString dir = fi.absolutePath();
    if (!QDir().mkpath(dir)) {
        return fail(QStringLiteral("Cannot create %1.").arg(dir));
    }
    QByteArray tmpl = QFile::encodeName(dir + QStringLiteral("/.vault.bin.XXXXXX"));
    const int fd = ::mkostemp(tmpl.data(), O_CLOEXEC);
    if (fd < 0) {
        return fail(QStringLiteral("Cannot create a temporary file in %1: %2").arg(dir, QString::fromLocal8Bit(std::strerror(errno))));
    }
    bool ok = ::fchmod(fd, 0600) == 0 && writeAll(fd, file.data(), file.size()) && ::fsync(fd) == 0;
    const int savedErrno = errno;
    ok = (::close(fd) == 0) && ok;
    if (ok) {
        ok = ::rename(tmpl.constData(), QFile::encodeName(m_path).constData()) == 0;
    }
    if (!ok) {
        ::unlink(tmpl.constData());
        return fail(QStringLiteral("Cannot write the vault: %1").arg(QString::fromLocal8Bit(std::strerror(savedErrno ? savedErrno : errno))));
    }
    const int dfd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    m_header = h;
    return true;
}

bool Vault::create(const SecureBuffer &password)
{
    FileLock fl(m_path + QStringLiteral(".lock"));
    if (exists()) {
        return fail(QStringLiteral("A vault already exists at %1.").arg(m_path));
    }
    if (password.isEmpty()) {
        return fail(QStringLiteral("The master password must not be empty."));
    }
    lock();
    Header h {};
    h.params = paramsForNewVault();
    randombytes_buf(h.salt, sizeof h.salt);
    SecureBuffer key;
    if (!deriveKey(password, h, &key)) {
        return false;
    }
    std::map<QString, SecureBuffer> empty;
    if (!writeWith(h, key, empty)) {
        return false;
    }
    m_key = std::move(key);
    m_params = h.params;
    m_error.clear();
    return true;
}

bool Vault::unlock(const SecureBuffer &password)
{
    lock();
    QByteArray data;
    Header h {};
    if (!readFile(&data) || !parseHeader(data, &h)) {
        return false;
    }
    SecureBuffer key;
    std::map<QString, SecureBuffer> entries;
    if (!deriveKey(password, h, &key) || !decryptWith(data, h, key, &entries)) {
        return false;
    }
    m_key = std::move(key);
    m_header = h;
    m_params = h.params;
    m_entries = std::move(entries);
    m_error.clear();
    return true;
}

bool Vault::refresh()
{
    if (!isUnlocked()) {
        return fail(QStringLiteral("The vault is locked."));
    }
    QByteArray data;
    Header h {};
    if (!readFile(&data) || !parseHeader(data, &h)) {
        lock();
        return false;
    }
    if (sodium_memcmp(h.salt, m_header.salt, sizeof h.salt) != 0 || h.params.opsLimit != m_params.opsLimit
        || h.params.memLimit != m_params.memLimit) {
        lock();
        return fail(QStringLiteral("The master password was changed in another window; unlock again."));
    }
    std::map<QString, SecureBuffer> entries;
    if (!decryptWith(data, h, m_key, &entries)) {
        lock();
        return false;
    }
    m_header = h;
    m_entries = std::move(entries);
    return true;
}

const SecureBuffer *Vault::secret(const QString &key) const
{
    const auto it = m_entries.find(key);
    return it == m_entries.end() ? nullptr : &it->second;
}

QStringList Vault::keys() const
{
    QStringList out;
    for (const auto &kv : m_entries) {
        out << kv.first;
    }
    return out;
}

bool Vault::setSecret(const QString &key, SecureBuffer value)
{
    FileLock fl(m_path + QStringLiteral(".lock"));
    if (!refresh()) {
        return false;
    }
    std::map<QString, SecureBuffer> next;
    for (const auto &[k, v] : m_entries) {
        next[k] = v.clone();
    }
    next[key] = std::move(value);
    if (!writeWith(m_header, m_key, next)) {
        return false;
    }
    m_entries = std::move(next);
    return true;
}

bool Vault::removeSecret(const QString &key)
{
    FileLock fl(m_path + QStringLiteral(".lock"));
    if (!refresh()) {
        return false;
    }
    if (m_entries.find(key) == m_entries.end()) {
        return true;
    }
    std::map<QString, SecureBuffer> next;
    for (const auto &[k, v] : m_entries) {
        if (k != key) {
            next[k] = v.clone();
        }
    }
    if (!writeWith(m_header, m_key, next)) {
        return false;
    }
    m_entries = std::move(next);
    return true;
}

bool Vault::changePassword(const SecureBuffer &oldPassword, const SecureBuffer &newPassword)
{
    FileLock fl(m_path + QStringLiteral(".lock"));
    if (newPassword.isEmpty()) {
        return fail(QStringLiteral("The master password must not be empty."));
    }
    // Always re-verify the old password against the file, even if unlocked.
    QByteArray data;
    Header h {};
    if (!readFile(&data) || !parseHeader(data, &h)) {
        return false;
    }
    SecureBuffer oldKey;
    std::map<QString, SecureBuffer> entries;
    if (!deriveKey(oldPassword, h, &oldKey) || !decryptWith(data, h, oldKey, &entries)) {
        return false;
    }
    oldKey.reset();
    Header nh {};
    nh.params = paramsForNewVault();
    randombytes_buf(nh.salt, sizeof nh.salt); // new salt -> new key
    SecureBuffer newKey;
    if (!deriveKey(newPassword, nh, &newKey) || !writeWith(nh, newKey, entries)) {
        return false;
    }
    m_key = std::move(newKey);
    m_params = nh.params;
    m_entries = std::move(entries);
    m_error.clear();
    return true;
}

} // namespace zterminal
