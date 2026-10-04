#include "SessionStore.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace zterminal {

namespace {
const QString kSuffix = QStringLiteral(".ini");

std::optional<SessionConfig> readFile(const QString &path)
{
    if (!QFileInfo::exists(path)) {
        return std::nullopt;
    }
    const QSettings f(path, QSettings::IniFormat);
    if (f.status() != QSettings::NoError) {
        return std::nullopt;
    }
    SessionConfig s;
    QString fallback = QFileInfo(path).fileName();
    fallback.chop(kSuffix.size());
    s.name = f.value(QStringLiteral("session/name"), QUrl::fromPercentEncoding(fallback.toUtf8())).toString();
    s.type = SessionConfig::typeFromString(f.value(QStringLiteral("session/type")).toString());
    s.host = f.value(QStringLiteral("ssh/host")).toString();
    s.user = f.value(QStringLiteral("ssh/user")).toString();
    s.port = f.value(QStringLiteral("ssh/port"), 22).toInt();
    s.keyFile = f.value(QStringLiteral("ssh/keyFile")).toString();
    s.jumpHost = f.value(QStringLiteral("ssh/jumpHost")).toString();
    s.extraArgs = f.value(QStringLiteral("ssh/extraArgs")).toString();
    s.useStoredPassword = f.value(QStringLiteral("ssh/authFromVault"), false).toBool();
    const SessionConfig d;
    s.serialDevice = f.value(QStringLiteral("serial/device")).toString();
    s.baudRate = f.value(QStringLiteral("serial/baudRate"), d.baudRate).toInt();
    s.dataBits = f.value(QStringLiteral("serial/dataBits"), d.dataBits).toInt();
    s.parity = f.value(QStringLiteral("serial/parity"), d.parity).toString();
    s.stopBits = f.value(QStringLiteral("serial/stopBits"), d.stopBits).toInt();
    s.flowControl = f.value(QStringLiteral("serial/flowControl"), d.flowControl).toString();
    s.localEcho = f.value(QStringLiteral("serial/localEcho"), d.localEcho).toBool();
    s.enterSends = f.value(QStringLiteral("serial/enterSends"), d.enterSends).toString();
    s.charDelayMs = f.value(QStringLiteral("serial/charDelayMs"), d.charDelayMs).toInt();
    s.lineDelayMs = f.value(QStringLiteral("serial/lineDelayMs"), d.lineDelayMs).toInt();
    s.breakMs = f.value(QStringLiteral("serial/breakMs"), d.breakMs).toInt();
    s.loginUser = f.value(QStringLiteral("serial/loginUser")).toString();
    s.autoLog = f.value(QStringLiteral("logging/auto"), false).toBool();
    s.fontFamily = f.value(QStringLiteral("appearance/fontFamily")).toString();
    s.fontSize = f.value(QStringLiteral("appearance/fontSize"), 0).toInt();
    s.colorScheme = f.value(QStringLiteral("appearance/colorScheme")).toString();
    // Any password-like key someone adds by hand is ignored: there is no field for it
    // (stored passwords live only in the encrypted vault).
    return s;
}
} // namespace

SessionStore::SessionStore()
    : m_dir(defaultDirectory())
{
}

SessionStore::SessionStore(const QString &directory)
    : m_dir(directory)
{
}

QString SessionStore::defaultDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/zterminal/sessions");
}

QString SessionStore::filePathFor(const QString &name) const
{
    // Unreserved characters stay readable; '/', '%', control characters and
    // everything else are encoded, so a name can never escape the directory.
    QString enc = QString::fromLatin1(QUrl::toPercentEncoding(name, QByteArrayLiteral(" @+,=()")));
    if (enc.startsWith(QLatin1Char('.'))) {
        enc.replace(0, 1, QStringLiteral("%2E")); // no hidden files, no "." / ".."
    }
    return m_dir + QLatin1Char('/') + enc + kSuffix;
}

QStringList SessionStore::names() const
{
    QStringList out;
    const QDir d(m_dir);
    for (const QString &file : d.entryList({QStringLiteral("*.ini")}, QDir::Files | QDir::Readable)) {
        const auto s = readFile(d.filePath(file));
        if (s && validateSessionName(s->name).isEmpty() && filePathFor(s->name) == d.filePath(file)) {
            out << s->name;
        }
    }
    std::sort(out.begin(), out.end(), [](const QString &a, const QString &b) {
        const int c = QString::compare(a, b, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : a < b;
    });
    return out;
}

bool SessionStore::contains(const QString &name) const
{
    return validateSessionName(name).isEmpty() && QFileInfo::exists(filePathFor(name));
}

std::optional<SessionConfig> SessionStore::load(const QString &name) const
{
    if (!validateSessionName(name).isEmpty()) {
        return std::nullopt;
    }
    auto s = readFile(filePathFor(name));
    if (s) {
        s->name = name;
    }
    return s;
}

bool SessionStore::save(const SessionConfig &s, QString *error) const
{
    auto fail = [error](const QString &m) {
        if (error) {
            *error = m;
        }
        return false;
    };
    if (const QString e = validateSessionName(s.name); !e.isEmpty()) {
        return fail(e);
    }
    if (!QDir().mkpath(m_dir)) {
        return fail(QStringLiteral("Can't create %1").arg(m_dir));
    }
    QFile::setPermissions(m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString path = filePathFor(s.name);
    {
        QSettings f(path, QSettings::IniFormat);
        f.clear();
        f.setValue(QStringLiteral("session/name"), s.name);
        f.setValue(QStringLiteral("session/type"), SessionConfig::typeToString(s.type));
        if (s.type == SessionConfig::Type::Ssh) {
            f.setValue(QStringLiteral("ssh/host"), s.host);
            f.setValue(QStringLiteral("ssh/user"), s.user);
            f.setValue(QStringLiteral("ssh/port"), s.port);
            f.setValue(QStringLiteral("ssh/keyFile"), s.keyFile);
            f.setValue(QStringLiteral("ssh/jumpHost"), s.jumpHost);
            f.setValue(QStringLiteral("ssh/extraArgs"), s.extraArgs);
            // Only a flag: the password itself is in the encrypted vault.
            f.setValue(QStringLiteral("ssh/authFromVault"), s.useStoredPassword);
        }
        if (s.type == SessionConfig::Type::Serial) {
            f.setValue(QStringLiteral("serial/device"), s.serialDevice);
            f.setValue(QStringLiteral("serial/baudRate"), s.baudRate);
            f.setValue(QStringLiteral("serial/dataBits"), s.dataBits);
            f.setValue(QStringLiteral("serial/parity"), s.parity);
            f.setValue(QStringLiteral("serial/stopBits"), s.stopBits);
            f.setValue(QStringLiteral("serial/flowControl"), s.flowControl);
            f.setValue(QStringLiteral("serial/localEcho"), s.localEcho);
            f.setValue(QStringLiteral("serial/enterSends"), s.enterSends);
            f.setValue(QStringLiteral("serial/charDelayMs"), s.charDelayMs);
            f.setValue(QStringLiteral("serial/lineDelayMs"), s.lineDelayMs);
            f.setValue(QStringLiteral("serial/breakMs"), s.breakMs);
            if (!s.loginUser.isEmpty()) {
                f.setValue(QStringLiteral("serial/loginUser"), s.loginUser); // a user name, not a secret
            }
        }
        if (s.autoLog) {
            f.setValue(QStringLiteral("logging/auto"), true);
        }
        if (!s.fontFamily.isEmpty()) {
            f.setValue(QStringLiteral("appearance/fontFamily"), s.fontFamily);
        }
        if (s.fontSize > 0) {
            f.setValue(QStringLiteral("appearance/fontSize"), s.fontSize);
        }
        if (!s.colorScheme.isEmpty()) {
            f.setValue(QStringLiteral("appearance/colorScheme"), s.colorScheme);
        }
        f.sync();
        if (f.status() != QSettings::NoError) {
            return fail(QStringLiteral("Can't write %1").arg(path));
        }
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool SessionStore::remove(const QString &name) const
{
    return validateSessionName(name).isEmpty() && QFile::remove(filePathFor(name));
}

QString SessionStore::unknownSessionMessage(const QString &name) const
{
    const QStringList all = names();
    QString m = QStringLiteral("unknown session \"%1\"").arg(name);
    if (all.isEmpty()) {
        m += QStringLiteral("; no saved sessions yet (File > New Session, then Save) in %1").arg(m_dir);
    } else {
        m += QStringLiteral("; saved sessions: ") + all.join(QStringLiteral(", "));
    }
    return m;
}

} // namespace zterminal
