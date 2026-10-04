#include "Reconnect.hpp"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>

namespace zterminal {

namespace {

// Last non-empty line of the output, without escape sequences.
QString lastLine(const QByteArray &tail)
{
    QString s = QString::fromUtf8(tail);
    static const QRegularExpression esc(QStringLiteral("\\x1b\\[[0-9;?]*[A-Za-z]|\\x1b\\][^\\x07]*\\x07"));
    s.remove(esc);
    const QStringList lines = s.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString t = it->trimmed();
        if (!t.isEmpty()) {
            return t.left(200);
        }
    }
    return {};
}

} // namespace

SshExit SshExit::classify(int exitCode, bool crashed, const QByteArray &outputTail)
{
    SshExit e;
    if (crashed) {
        e.kind = Kind::Killed;
        e.reason = QStringLiteral("terminated");
        return e;
    }
    if (exitCode == 0) {
        e.kind = Kind::Clean;
        return e;
    }
    if (exitCode != 255) {
        e.kind = Kind::RemoteStatus; // ssh passes the remote status through
        e.reason = QStringLiteral("exit status %1").arg(exitCode);
        return e;
    }
    // 255: ssh's own error. Retrying helps with network trouble, not with these:
    const QString text = QString::fromUtf8(outputTail);
    static const char *const fatal[] = {
        "Permission denied",          "Host key verification failed", "REMOTE HOST IDENTIFICATION HAS CHANGED",
        "Too many authentication failures", "no matching host key type", "no matching key exchange",
        "Bad configuration option",   "Bad port",                     "Authentication failed"};
    for (const char *f : fatal) {
        if (text.contains(QLatin1String(f), Qt::CaseInsensitive)) {
            e.kind = Kind::Refused;
            e.reason = lastLine(outputTail);
            return e;
        }
    }
    e.kind = Kind::Dropped;
    e.reason = lastLine(outputTail);
    if (e.reason.isEmpty()) {
        e.reason = QStringLiteral("ssh lost the connection");
    }
    return e;
}

int Backoff::delaySeconds(int attempt) const
{
    attempt = std::max(1, attempt);
    qint64 d = firstSeconds;
    for (int i = 1; i < attempt && d < maxSeconds; ++i) {
        d *= 2;
    }
    return int(std::min<qint64>(d, maxSeconds));
}

ReconnectScheduler::ReconnectScheduler(QObject *parent)
    : QObject(parent)
{
    m_elapsed.start();
    m_clock = [this]() { return m_elapsed.elapsed(); };
    m_timer = new QTimer(this);
    m_timer->setInterval(m_tickMs);
    connect(m_timer, &QTimer::timeout, this, &ReconnectScheduler::tick);
}

void ReconnectScheduler::setClock(Clock c)
{
    m_clock = std::move(c);
}

void ReconnectScheduler::setTickInterval(int ms)
{
    m_tickMs = ms;
    if (ms <= 0) {
        m_timer->stop();
    } else {
        m_timer->setInterval(ms);
        if (isWaiting()) {
            m_timer->start();
        }
    }
}

void ReconnectScheduler::scheduleNext()
{
    m_attempting = false;
    ++m_attempt;
    m_dueAt = m_clock() + qint64(m_backoff.delaySeconds(m_attempt)) * 1000;
    m_lastShown = -1;
    if (m_tickMs > 0) {
        m_timer->start();
    }
    tick();
}

void ReconnectScheduler::reset()
{
    m_attempt = 0;
    m_dueAt = -1;
    m_attempting = false;
    m_timer->stop();
}

void ReconnectScheduler::cancel()
{
    reset();
}

int ReconnectScheduler::secondsLeft() const
{
    if (m_dueAt < 0) {
        return 0;
    }
    const qint64 ms = std::max<qint64>(0, m_dueAt - m_clock());
    return int((ms + 999) / 1000);
}

void ReconnectScheduler::tick()
{
    if (m_dueAt < 0) {
        return;
    }
    if (m_clock() >= m_dueAt) {
        m_dueAt = -1;
        m_timer->stop();
        m_attempting = true;
        emit attemptDue(m_attempt);
        return;
    }
    const int left = secondsLeft();
    if (left != m_lastShown) {
        m_lastShown = left;
        emit countdown(m_attempt, left);
    }
}

} // namespace zterminal
