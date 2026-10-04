#pragma once

#include "Update.hpp"

#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
#include <QNetworkReply>
class QFile;

namespace zterminal {

// Asks GitHub for the latest release (public API, no token):
// GET https://api.github.com/repos/sbj-ee/zterminal/releases/latest
// One request per check(); finished() always fires exactly once per check.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    enum class Outcome { Release, NoRelease, Error };
    struct Result {
        Outcome outcome = Outcome::Error;
        ReleaseInfo release; // when outcome == Release
        QString error;       // human-readable, for NoRelease / Error
        bool rateLimited = false;
    };

    static QString defaultApiUrl();
    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    // Default: defaultApiUrl(), or $ZTERMINAL_UPDATE_URL (tests, mirrors).
    void setApiUrl(const QUrl &url) { m_url = url; }
    QUrl apiUrl() const { return m_url; }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    bool isRunning() const { return m_reply != nullptr; }

    void check();
    void abort();

    // Message for a failed reply: offline, timeout, rate limit (with the reset
    // time), 404, other HTTP errors. Public for the tests.
    static QString failureReason(QNetworkReply *reply, bool *rateLimited = nullptr);

signals:
    void finished(const zterminal::UpdateChecker::Result &result);

private:
    void onFinished();

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QUrl m_url;
    int m_timeoutMs = 5000;
};

// Downloads a release's .deb and SHA256SUMS into `dir` and verifies the
// package before reporting success. Refuses (finished(false, ...)) when an
// asset is missing, a download fails, or the checksum doesn't match.
class UpdateDownloader : public QObject {
    Q_OBJECT
public:
    explicit UpdateDownloader(QObject *parent = nullptr);
    ~UpdateDownloader() override;

    void start(const ReleaseInfo &release, const QString &dir);
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }
    void setStallTimeoutMs(int ms) { m_stallMs = ms; }

signals:
    // bytesTotal is -1 when unknown.
    void progress(const QString &assetName, qint64 bytesReceived, qint64 bytesTotal);
    // ok: debPath is the verified package. !ok: error says why (cancelled too).
    void finished(bool ok, const QString &debPath, const QString &error);

private:
    void fetch(const ReleaseAsset &asset, const QString &path);
    void onReadyRead();
    void onFinished();
    void fail(const QString &error);

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QFile *m_file = nullptr;
    ReleaseAsset m_deb;
    ReleaseAsset m_sums;
    QString m_dir;
    QString m_current; // asset being downloaded
    int m_stage = 0;   // 0 idle, 1 SHA256SUMS, 2 .deb
    int m_stallMs = 30000;
    bool m_cancelled = false;
};

} // namespace zterminal
