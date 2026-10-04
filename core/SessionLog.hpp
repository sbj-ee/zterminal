#pragma once

#include "LogTextFilter.hpp"

#include <QDateTime>
#include <QFile>
#include <QString>

#include <functional>
#include <memory>

namespace zterminal {

// Session > Start Logging: the session's output as plain text in
// <dir>/<sanitized session>-<YYYYMMDD-HHMMSS>.log (dir created 0700, file
// created 0600 with O_EXCL; an existing name gets -2, -3, ...).
//
// Only *output* is logged (what the terminal displays), never keystrokes, so a
// password typed at a no-echo prompt is not in the stream at all. On top of
// that, suspend() drops output while something secret may be on the wire:
// the vault dialogs, Send Stored Login, and secret-input mode on the PTY
// (canonical + ECHO off). Each pause leaves a marker line in the log.
class SessionLog
{
public:
    using Clock = std::function<QDateTime()>;
    SessionLog();
    ~SessionLog();

    static QString defaultDirectory();              // ~/zterminal-logs
    static QString sanitizeName(const QString &name); // safe single path component
    static QString fileNameFor(const QString &sessionName, const QDateTime &when);

    // Opens a new file; false (with errorString()) on failure.
    bool start(const QString &directory, const QString &sessionName, bool timestamps);
    void stop();
    bool isActive() const { return m_file.isOpen(); }
    QString path() const { return m_path; }
    QString errorString() const { return m_error; }

    void feed(const QByteArray &output);
    void setColumns(int columns);

    // Pauses keyed by reason (a set: repeating a reason is a no-op); output
    // arriving while any pause is active is dropped.
    void suspend(const QString &reason);
    void resume(const QString &reason);
    bool isSuspended() const { return !m_pauses.isEmpty(); }
    // Connection events (docs/PLAN.md §4.18), written even while paused:
    //   --- disconnected 2026-10-03 21:58:12 (Timeout, server not responding.) ---
    //   --- reconnected 2026-10-03 21:58:40 (attempt 3) ---
    // The line model restarts, so a half line before the drop stays on its own.
    void marker(const QString &event, const QString &detail = {});

    void setClockForTests(Clock c) { m_clock = std::move(c); }

private:
    void writeLine(const QString &line);
    void writeRaw(const QString &text);

    QFile m_file;
    QString m_path;
    QString m_error;
    bool m_timestamps = false;
    int m_columns = 0;
    QStringList m_pauses;
    Clock m_clock;
    std::unique_ptr<LogTextFilter> m_filter;
};

} // namespace zterminal
