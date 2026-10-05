#include "UpdateChecker.hpp"

#include <QtGlobal>

#include "version.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace zterminal {

namespace {

QNetworkRequest makeRequest(const QUrl &url, int timeoutMs, const QByteArray &accept)
{
    QNetworkRequest req(url);
    // GitHub's API rejects requests without a User-Agent.
    req.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("zterminal/") + QByteArray(kVersionString));
    req.setRawHeader("Accept", accept);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(timeoutMs); // no bytes for this long -> OperationCanceledError
    return req;
}

} // namespace

QString UpdateChecker::defaultApiUrl()
{
    return QStringLiteral("https://api.github.com/repos/sbj-ee/zterminal/releases/latest");
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    const QByteArray env = qgetenv("ZTERMINAL_UPDATE_URL");
    m_url = QUrl(env.isEmpty() ? defaultApiUrl() : QString::fromUtf8(env));
}

UpdateChecker::~UpdateChecker()
{
    if (QNetworkReply *r = m_reply.data()) {
        r->disconnect(this);
        r->abort();
    }
}

void UpdateChecker::check()
{
    if (m_reply) {
        return; // one at a time; the running check will report
    }
    m_reply = m_nam->get(makeRequest(m_url, m_timeoutMs, "application/vnd.github+json"));
    connect(m_reply, &QNetworkReply::finished, this, &UpdateChecker::onFinished);
}

void UpdateChecker::abort()
{
    if (QNetworkReply *r = m_reply.data()) {
        r->abort(); // finished() still fires, with an error
    }
}

QString UpdateChecker::failureReason(QNetworkReply *reply, bool *rateLimited)
{
    if (rateLimited) {
        *rateLimited = false;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403 || status == 429) {
        const bool exhausted = reply->rawHeader("x-ratelimit-remaining") == "0" || status == 429;
        if (exhausted) {
            if (rateLimited) {
                *rateLimited = true;
            }
            const qint64 reset = reply->rawHeader("x-ratelimit-reset").toLongLong();
            if (reset > 0) {
                return QStringLiteral("GitHub's rate limit for update checks was reached. Try again after %1.")
                    .arg(QDateTime::fromSecsSinceEpoch(reset).toLocalTime().toString(QStringLiteral("HH:mm")));
            }
            return QStringLiteral("GitHub's rate limit for update checks was reached. Try again later.");
        }
        return QStringLiteral("GitHub refused the update check (HTTP %1).").arg(status);
    }
    if (status == 404) {
        return QStringLiteral("No published zterminal release was found on GitHub.");
    }
    if (status >= 400) {
        return QStringLiteral("GitHub answered the update check with HTTP %1.").arg(status);
    }
    switch (reply->error()) {
    case QNetworkReply::OperationCanceledError:
    case QNetworkReply::TimeoutError:
        return QStringLiteral("The update check timed out. Check the network connection and try again.");
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyNotFoundError:
        return QStringLiteral("Couldn't reach GitHub to check for updates (no network?).");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("The secure connection to GitHub failed: %1").arg(reply->errorString());
    default:
        break;
    }
    return QStringLiteral("The update check failed: %1").arg(reply->errorString());
}

void UpdateChecker::onFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply) {
        return;
    }
    reply->deleteLater();
    Result r;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        r.error = failureReason(reply, &r.rateLimited);
        r.outcome = status == 404 ? Outcome::NoRelease : Outcome::Error;
        emit finished(r);
        return;
    }
    QString err;
    const auto info = ReleaseInfo::fromJson(reply->readAll(), &err);
    if (!info) {
        r.error = err;
        emit finished(r);
        return;
    }
    r.outcome = Outcome::Release;
    r.release = *info;
    emit finished(r);
}

UpdateDownloader::UpdateDownloader(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

UpdateDownloader::~UpdateDownloader()
{
    if (QNetworkReply *r = m_reply.data()) {
        r->disconnect(this);
        r->abort();
    }
    delete m_file;
}

void UpdateDownloader::start(const ReleaseInfo &release, const QString &dir)
{
    if (m_reply) {
        return;
    }
    m_cancelled = false;
    m_dir = dir;
    const auto pkg = release.packageAsset();
    const auto sums = release.checksumAsset();
    if (!pkg || !sums) {
        QStringList missing;
        if (!pkg) {
#if defined(Q_OS_MACOS)
            missing << QStringLiteral("zterminal-%1-Darwin.dmg").arg(release.versionString());
#else
            missing << QStringLiteral("zterminal_%1_amd64.deb").arg(release.versionString());
#endif
        }
        if (!sums) {
            missing << QStringLiteral("SHA256SUMS");
        }
        // Report asynchronously, like every other outcome.
        const QString msg = QStringLiteral("Release %1 is missing %2, so it can't be installed automatically.")
                                .arg(release.tag, missing.join(QStringLiteral(" and ")));
        QMetaObject::invokeMethod(this, [this, msg] { emit finished(false, QString(), msg); }, Qt::QueuedConnection);
        return;
    }
    m_deb = *pkg;
    m_sums = *sums;
    m_stage = 1;
    fetch(m_sums, QDir(m_dir).filePath(m_sums.name));
}

void UpdateDownloader::fetch(const ReleaseAsset &asset, const QString &path)
{
    delete m_file;
    m_file = new QFile(path);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(QStringLiteral("Couldn't write %1: %2").arg(path, m_file->errorString()));
        return;
    }
    m_current = asset.name;
    m_reply = m_nam->get(makeRequest(QUrl(asset.url), m_stallMs, "application/octet-stream"));
    connect(m_reply, &QNetworkReply::readyRead, this, &UpdateDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this, asset](qint64 got, qint64 total) {
        emit progress(asset.name, got, total > 0 ? total : (asset.size > 0 ? asset.size : -1));
    });
    connect(m_reply, &QNetworkReply::finished, this, &UpdateDownloader::onFinished);
}

void UpdateDownloader::onReadyRead()
{
    if (m_reply && m_file) {
        m_file->write(m_reply->readAll());
    }
}

void UpdateDownloader::cancel()
{
    m_cancelled = true;
    if (QNetworkReply *r = m_reply.data()) {
        r->abort();
    }
}

void UpdateDownloader::fail(const QString &error)
{
    m_stage = 0;
    if (m_file) {
        m_file->close();
        m_file->remove();
        delete m_file;
        m_file = nullptr;
    }
    emit finished(false, QString(), error);
}

void UpdateDownloader::onFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply) {
        return;
    }
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_cancelled) {
        fail(QStringLiteral("Download cancelled."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status >= 400) {
        QString why;
        if (status == 404) {
            why = QStringLiteral("%1 is no longer on the release page.").arg(m_current);
        } else if (reply->error() == QNetworkReply::OperationCanceledError) {
            why = QStringLiteral("The download of %1 stalled. Check the network connection and try again.").arg(m_current);
        } else {
            why = QStringLiteral("Downloading %1 failed: %2").arg(m_current, reply->errorString());
        }
        fail(why);
        return;
    }
    m_file->write(reply->readAll());
    m_file->close();
    if (m_stage == 1) {
        m_stage = 2;
        fetch(m_deb, QDir(m_dir).filePath(m_deb.name));
        return;
    }
    // Both files present: verify before anyone may install it.
    const QString debPath = m_file->fileName();
    QFile sumsFile(QDir(m_dir).filePath(m_sums.name));
    QByteArray sums;
    if (sumsFile.open(QIODevice::ReadOnly)) {
        sums = sumsFile.readAll();
    }
    const ChecksumResult check = verifyChecksum(debPath, sums, m_deb.name);
    if (!check.ok()) {
        fail(check.message()); // also deletes the unverified package
        return;
    }
    delete m_file;
    m_file = nullptr;
    m_stage = 0;
    emit finished(true, debPath, QString());
}

} // namespace zterminal
