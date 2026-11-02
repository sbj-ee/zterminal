#include "RestoreFile.hpp"

#include "CommandLine.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zterminal {

namespace {
constexpr qint64 kMaxRestoreFile = 1024 * 1024;
const QString kFormat = QStringLiteral("zterminal-restore");
} // namespace

bool isRestorableTab(const QStringList &args)
{
    using K = LaunchRequest::Kind;
    switch (parseCommandLine(args).kind) {
    case K::LocalShell:
    case K::SavedSession:
    case K::Ssh:
    case K::Serial:
    case K::Command:
        return true;
    default:
        return false;
    }
}

QByteArray restoreToJson(const QList<RestoreWindow> &windows)
{
    QJsonArray ws;
    for (const RestoreWindow &w : windows) {
        QJsonArray tabs;
        for (const QStringList &t : w.tabs) {
            tabs.append(QJsonArray::fromStringList(t));
        }
        ws.append(QJsonObject{{QStringLiteral("tabs"), tabs}, {QStringLiteral("current"), w.current}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("format"), kFormat}, {QStringLiteral("windows"), ws}})
        .toJson(QJsonDocument::Compact);
}

std::optional<QList<RestoreWindow>> restoreFromJson(const QByteArray &json, QString *error)
{
    auto fail = [error](const QString &m) -> std::optional<QList<RestoreWindow>> {
        if (error) {
            *error = m;
        }
        return std::nullopt;
    };
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("not a restore file"));
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != kFormat || !root.value(QStringLiteral("windows")).isArray()) {
        return fail(QStringLiteral("not a restore file"));
    }
    QList<RestoreWindow> out;
    for (const QJsonValue &wv : root.value(QStringLiteral("windows")).toArray()) {
        const QJsonObject wo = wv.toObject();
        RestoreWindow w;
        const int current = wo.value(QStringLiteral("current")).toInt();
        int index = 0;
        for (const QJsonValue &tv : wo.value(QStringLiteral("tabs")).toArray()) {
            QStringList args;
            for (const QJsonValue &a : tv.toArray()) {
                args << a.toString();
            }
            if (isRestorableTab(args)) {
                if (index == current) {
                    w.current = int(w.tabs.size());
                }
                w.tabs << args;
            }
            ++index;
        }
        if (!w.tabs.isEmpty()) {
            out << w;
        }
    }
    return out;
}

QString writeRestoreFile(const QList<RestoreWindow> &windows)
{
    QTemporaryFile f(QDir::tempPath() + QStringLiteral("/zterminal-restore-XXXXXX.json"));
    f.setAutoRemove(false);
    if (!f.open()) { // QTemporaryFile creates it 0600
        return {};
    }
    const QByteArray data = restoreToJson(windows);
    if (f.write(data) != data.size() || !f.flush()) {
        f.remove();
        return {};
    }
    return f.fileName();
}

std::optional<QList<RestoreWindow>> takeRestoreFile(const QString &path, QString *error)
{
    auto fail = [error](const QString &m) -> std::optional<QList<RestoreWindow>> {
        if (error) {
            *error = m;
        }
        return std::nullopt;
    };
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return fail(QStringLiteral("cannot open %1").arg(path));
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 077) != 0
        || st.st_size > kMaxRestoreFile) {
        ::close(fd);
        return fail(QStringLiteral("%1 is not a private restore file of this user").arg(path));
    }
    QFile f;
    if (!f.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return fail(QStringLiteral("cannot read %1").arg(path));
    }
    const QByteArray data = f.read(kMaxRestoreFile);
    f.close();
    QFile::remove(path);
    return restoreFromJson(data, error);
}

} // namespace zterminal
