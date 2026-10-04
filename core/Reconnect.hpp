#pragma once

#include <functional>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QTimer;

namespace zterminal {

// Keepalive and reconnect (docs/PLAN.md §4.18).

// Why an ssh process ended, from its exit status and the last output.
struct SshExit {
    enum class Kind {
        Clean,        // status 0: the user logged out; never reconnect
        RemoteStatus, // the remote shell/command's own non-zero status; not a drop
        Dropped,      // ssh's 255 with a network error (timeout, reset, closed, refused ...)
        Refused,      // ssh's 255 that retrying can't fix (auth, host key, bad config)
        Killed        // terminated by a signal (Restart Session, closing the tab)
    };
    Kind kind = Kind::Clean;
    QString reason; // one line for the banner, e.g. "Timeout, server not responding."
    bool isDrop() const { return kind == Kind::Dropped; }

    static SshExit classify(int exitCode, bool crashed, const QByteArray &outputTail);
};

// 2, 4, 8, 16, 32, 60, 60 ... seconds (attempt is 1-based).
struct Backoff {
    int firstSeconds = 2;
    int maxSeconds = 60;
    int delaySeconds(int attempt) const;
};

// Counts down to the next reconnect attempt on an injectable clock: the app
// ticks it from a QTimer, tests set the clock and call tick() directly.
class ReconnectScheduler : public QObject
{
    Q_OBJECT
public:
    using Clock = std::function<qint64()>; // milliseconds, monotonic

    explicit ReconnectScheduler(QObject *parent = nullptr);

    void setClock(Clock c);
    void setBackoff(const Backoff &b) { m_backoff = b; }
    const Backoff &backoff() const { return m_backoff; }
    // Real timer driving tick(); 0 stops it (tests).
    void setTickInterval(int ms);

    // The connection dropped: schedule the next attempt (attempt 1 after a
    // connection that had been up, attempt n+1 after a failed attempt).
    void scheduleNext();
    // The attempt is under way (attemptDue fired); waiting for the outcome.
    bool isAttempting() const { return m_attempting; }
    // The connection is up again: the next drop starts over at attempt 1.
    void reset();
    // Cancel button / user action: stop counting down.
    void cancel();

    bool isWaiting() const { return m_dueAt >= 0; }
    int attempt() const { return m_attempt; } // the scheduled or running attempt, 0 = none
    int secondsLeft() const;                  // until the scheduled attempt, rounded up
    void tick();

signals:
    void countdown(int attempt, int secondsLeft); // once per second while waiting
    void attemptDue(int attempt);

private:
    QElapsedTimer m_elapsed;
    Clock m_clock;
    Backoff m_backoff;
    QTimer *m_timer = nullptr;
    int m_tickMs = 250;
    int m_attempt = 0;
    qint64 m_dueAt = -1;
    int m_lastShown = -1;
    bool m_attempting = false;
};

} // namespace zterminal
