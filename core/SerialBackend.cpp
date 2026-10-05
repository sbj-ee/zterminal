#include "SerialBackend.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSerialPortInfo>
#include <QtGlobal>
#include <QTimer>

#include <algorithm>

namespace zterminal {

SerialBackend::SerialBackend(QObject *parent)
    : QObject(parent)
    , m_port(new QSerialPort(this))
    , m_pacer(new QTimer(this))
    , m_breakTimer(new QTimer(this))
    , m_watchdog(new QTimer(this))
{
    // A pty or USB adapter that goes away does not always wake the read
    // notifier (e.g. a pty whose other side closed while idle), so also watch
    // for the device node disappearing, as it does when a USB adapter is unplugged.
    m_watchdog->setInterval(500);
    connect(m_watchdog, &QTimer::timeout, this, [this]() {
        if (m_port->isOpen() && !QFileInfo::exists(m_cfg.serialDevice)) {
            onError(QSerialPort::ResourceError);
        }
    });
    m_pacer->setSingleShot(true);
    m_pacer->setTimerType(Qt::PreciseTimer);
    connect(m_pacer, &QTimer::timeout, this, &SerialBackend::pump);
    m_breakTimer->setSingleShot(true);
    m_breakTimer->setTimerType(Qt::PreciseTimer);
    connect(m_breakTimer, &QTimer::timeout, this, [this]() {
        if (m_port->isOpen()) {
            m_port->setBreakEnabled(false);
        }
        m_breakActive = false;
        emit breakChanged(false);
    });
    connect(m_port, &QSerialPort::readyRead, this, &SerialBackend::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred, this, &SerialBackend::onError);
}

SerialBackend::~SerialBackend()
{
    close();
}

QString SerialBackend::describeError(QSerialPort::SerialPortError error, const QString &device,
                                     const QString &systemText)
{
    switch (error) {
    case QSerialPort::PermissionError:
#if defined(Q_OS_MACOS)
        return QStringLiteral(
                   "Permission denied opening %1. On macOS try the matching /dev/cu.* device "
                   "(not /dev/tty.*), and grant Terminal/zterminal access if prompted.")
            .arg(device);
#else
        return QStringLiteral(
                   "Permission denied opening %1. Serial devices belong to the \"dialout\" group; add yourself with\n"
                   "    sudo usermod -aG dialout $USER\n"
                   "then log out and back in (or reboot) so the new group takes effect.")
            .arg(device);
#endif
    case QSerialPort::DeviceNotFoundError: {
        const QStringList ports = availablePorts();
        return QStringLiteral("%1 was not found (is the adapter plugged in?). %2")
            .arg(device,
                 ports.isEmpty() ? QStringLiteral("No serial ports are detected.")
                                 : QStringLiteral("Detected ports: ") + ports.join(QStringLiteral(", ")));
    }
    case QSerialPort::OpenError:
        return QStringLiteral("%1 is already open in another program.").arg(device);
    case QSerialPort::ResourceError:
        return QStringLiteral("%1 disconnected (device unplugged or I/O error).").arg(device);
    case QSerialPort::UnsupportedOperationError:
        return QStringLiteral("%1 does not support these settings%2.")
            .arg(device, systemText.isEmpty() ? QString() : QStringLiteral(": ") + systemText);
    default:
        break;
    }
    return QStringLiteral("%1: %2").arg(device, systemText.isEmpty() ? QStringLiteral("serial error") : systemText);
}

QStringList SerialBackend::availablePorts()
{
    QStringList out;
    for (const QSerialPortInfo &p : QSerialPortInfo::availablePorts()) {
        out << p.systemLocation();
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

QString SerialBackend::byIdDirectory()
{
    const QString env = qEnvironmentVariable("ZTERMINAL_SERIAL_BY_ID_DIR");
    return env.isEmpty() ? QStringLiteral("/dev/serial/by-id") : QDir::cleanPath(env);
}

bool SerialBackend::isByIdPath(const QString &device)
{
    return QDir::cleanPath(device).startsWith(byIdDirectory() + QLatin1Char('/'));
}

QString SerialBackend::byIdAlias(const QString &device)
{
    if (isByIdPath(device)) {
        return QDir::cleanPath(device);
    }
    const QString target = QFileInfo(device).canonicalFilePath();
    if (target.isEmpty()) {
        return {};
    }
    const QDir dir(byIdDirectory());
    const QFileInfoList links = dir.entryInfoList(QDir::System | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &l : links) {
        if (l.isSymLink() && l.canonicalFilePath() == target) {
            return l.absoluteFilePath();
        }
    }
    return {};
}

bool SerialBackend::open(const SessionConfig &cfg)
{
    close();
    m_cfg = cfg;
    m_error.clear();
    if (const QString e = validateSerial(cfg); !e.isEmpty()) {
        m_error = e;
        return false;
    }
    m_port->setPortName(cfg.serialDevice);
    m_port->setBaudRate(cfg.baudRate);
    m_port->setDataBits(static_cast<QSerialPort::DataBits>(cfg.dataBits));
    const QString &p = cfg.parity;
    m_port->setParity(p == QLatin1String("even")    ? QSerialPort::EvenParity
                      : p == QLatin1String("odd")   ? QSerialPort::OddParity
                      : p == QLatin1String("mark")  ? QSerialPort::MarkParity
                      : p == QLatin1String("space") ? QSerialPort::SpaceParity
                                                     : QSerialPort::NoParity);
    m_port->setStopBits(cfg.stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop);
    m_port->setFlowControl(cfg.flowControl == QLatin1String("rtscts")    ? QSerialPort::HardwareControl
                           : cfg.flowControl == QLatin1String("xonxoff") ? QSerialPort::SoftwareControl
                                                                         : QSerialPort::NoFlowControl);
    // QSerialPort reports a missing device as a generic error on some versions;
    // check first so the message is the helpful one.
    if (!QFileInfo::exists(cfg.serialDevice)) {
        m_error = describeError(QSerialPort::DeviceNotFoundError, cfg.serialDevice);
        return false;
    }
    {
        const QSignalBlocker block(m_port); // open errors are reported via the return value
        if (!m_port->open(QIODevice::ReadWrite)) {
            m_error = describeError(m_port->error(), cfg.serialDevice, m_port->errorString());
            m_port->clearError();
            return false;
        }
    }
    m_watchdog->start();
    return true;
}

void SerialBackend::close()
{
    m_watchdog->stop();
    cancelPending();
    m_breakTimer->stop();
    m_breakActive = false;
    if (m_port->isOpen()) {
        const QSignalBlocker block(m_port);
        m_port->close();
    }
}

bool SerialBackend::isOpen() const
{
    return m_port->isOpen();
}

void SerialBackend::write(const QByteArray &data, bool echo)
{
    if (!m_port->isOpen() || data.isEmpty()) {
        return;
    }
    // The terminal sends CR for Enter (and pasted line ends); map it.
    QByteArray out = data;
    const QByteArray enter = enterSequence(m_cfg.enterSends);
    if (enter != "\r") {
        out.replace('\r', enter);
    }
    if (m_cfg.localEcho && echo) {
        QByteArray shown = out;
        shown.replace("\r\n", "\n");
        shown.replace('\r', '\n');
        shown.replace("\n", "\r\n");
        emit dataReceived(shown);
    }
    m_queue += out;
    if (!m_pacer->isActive()) {
        pump();
    }
}

void SerialBackend::pump()
{
    while (!m_queue.isEmpty() && m_port->isOpen()) {
        // One character at a time with a character delay, else up to the next
        // line end; then wait the matching delay before continuing.
        qsizetype n = m_queue.size();
        if (m_cfg.charDelayMs > 0) {
            n = 1;
        } else if (m_cfg.lineDelayMs > 0) {
            const qsizetype cr = m_queue.indexOf('\r');
            const qsizetype lf = m_queue.indexOf('\n');
            qsizetype end = cr < 0 ? lf : (lf < 0 ? cr : std::min(cr, lf));
            if (end >= 0) {
                // keep a CRLF pair together
                if (m_queue.at(end) == '\r' && end + 1 < m_queue.size() && m_queue.at(end + 1) == '\n') {
                    ++end;
                }
                n = end + 1;
            }
        }
        // Don't split a CRLF pair across the delay either.
        if (n == 1 && m_queue.size() > 1 && m_queue.at(0) == '\r' && m_queue.at(1) == '\n') {
            n = 2;
        }
        const QByteArray chunk = m_queue.left(n);
        m_queue.remove(0, n);
        m_port->write(chunk);
        emit pendingChanged(m_queue.size());
        const char last = chunk.back();
        const bool lineEnd = last == '\r' || last == '\n';
        const int delay = lineEnd ? std::max(m_cfg.lineDelayMs, m_cfg.charDelayMs) : m_cfg.charDelayMs;
        if (delay > 0 && !m_queue.isEmpty()) {
            m_pacer->start(delay);
            return;
        }
    }
}

void SerialBackend::cancelPending()
{
    m_pacer->stop();
    if (!m_queue.isEmpty()) {
        m_queue.clear();
        emit pendingChanged(0);
    }
}

bool SerialBackend::sendBreak()
{
    if (!m_port->isOpen() || m_breakActive) {
        return false;
    }
    if (!m_port->setBreakEnabled(true)) {
        return false;
    }
    m_breakActive = true;
    emit breakChanged(true);
    m_breakTimer->start(m_cfg.breakMs);
    return true;
}

void SerialBackend::onReadyRead()
{
    const QByteArray d = m_port->readAll();
    if (!d.isEmpty()) {
        emit dataReceived(d);
    }
}

void SerialBackend::onError(QSerialPort::SerialPortError e)
{
    if (e == QSerialPort::NoError || e == QSerialPort::TimeoutError) {
        return;
    }
    if (e == QSerialPort::ResourceError || e == QSerialPort::ReadError || e == QSerialPort::WriteError) {
        if (!m_port->isOpen()) {
            return;
        }
        const QString reason = describeError(QSerialPort::ResourceError, m_cfg.serialDevice, m_port->errorString());
        m_port->clearError();
        close();
        emit disconnected(reason);
    }
}

} // namespace zterminal
