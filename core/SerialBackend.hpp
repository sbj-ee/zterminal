#pragma once

#include "Session.hpp"

#include <QByteArray>
#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QStringList>

class QTimer;

namespace zterminal {

// A serial console session (QSerialPort). Outgoing data has CR mapped to the
// session's Enter sequence, is optionally echoed locally, and goes through a
// pacer: with a per-character and/or per-line delay set (Cisco consoles drop
// characters on fast pastes), bytes are queued and sent asynchronously, and the
// queue can be cancelled.
class SerialBackend : public QObject
{
    Q_OBJECT
public:
    explicit SerialBackend(QObject *parent = nullptr);
    ~SerialBackend() override;

    // Opens cfg.serialDevice with cfg's line settings. On failure returns false
    // and errorString() explains it (permission -> dialout group, etc.).
    bool open(const SessionConfig &cfg);
    void close();
    bool isOpen() const;
    QString errorString() const { return m_error; }
    QString device() const { return m_cfg.serialDevice; }
    const SessionConfig &config() const { return m_cfg; }

    // echo=false: never local-echo (used for stored passwords).
    void write(const QByteArray &data, bool echo = true);
    // Bytes still queued by the pacer.
    qint64 pending() const { return m_queue.size(); }
    bool pacingEnabled() const { return m_cfg.charDelayMs > 0 || m_cfg.lineDelayMs > 0; }
    void cancelPending();

    // Holds the line in break for cfg.breakMs (default 300 ms).
    bool sendBreak();
    bool isBreakActive() const { return m_breakActive; }

    QSerialPort *port() const { return m_port; }

    // User-facing text for an open/IO error on `device`.
    static QString describeError(QSerialPort::SerialPortError error, const QString &device,
                                 const QString &systemText = {});
    // Detected ports (/dev/ttyUSB0, /dev/ttyACM0, ...), sorted.
    static QStringList availablePorts();

signals:
    void dataReceived(const QByteArray &data);
    // The device went away (unplugged) or failed while open.
    void disconnected(const QString &reason);
    void pendingChanged(qint64 remaining);
    void breakChanged(bool active);

private:
    void pump();
    void onReadyRead();
    void onError(QSerialPort::SerialPortError e);

    SessionConfig m_cfg;
    QSerialPort *m_port = nullptr;
    QTimer *m_pacer = nullptr;
    QTimer *m_breakTimer = nullptr;
    QTimer *m_watchdog = nullptr;
    QByteArray m_queue;
    QString m_error;
    bool m_breakActive = false;
};

} // namespace zterminal
