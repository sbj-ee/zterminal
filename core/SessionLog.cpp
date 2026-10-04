#include "SessionLog.hpp"

#include "version.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace zterminal {

namespace {
const char *kStamp = "yyyy-MM-ddTHH:mm:ss.zzz"; // ISO 8601, local time

// mkdir -p, giving every directory *we* create mode 0700.
bool makePrivateDirs(const QString &dir, QString *error)
{
    const QString abs = QDir::cleanPath(QDir(dir).absolutePath());
    if (QFileInfo(abs).isDir()) {
        return true;
    }
    const QString parent = QFileInfo(abs).absolutePath();
    if (parent != abs && !makePrivateDirs(parent, error)) {
        return false;
    }
    if (::mkdir(QFile::encodeName(abs).constData(), 0700) != 0 && errno != EEXIST) {
        *error = QStringLiteral("Cannot create %1: %2").arg(abs, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    ::chmod(QFile::encodeName(abs).constData(), 0700); // umask can't widen or narrow it
    return true;
}
} // namespace

SessionLog::SessionLog()
    : m_clock([] { return QDateTime::currentDateTime(); })
{
}

SessionLog::~SessionLog()
{
    stop();
}

QString SessionLog::defaultDirectory()
{
    return QDir::homePath() + QStringLiteral("/zterminal-logs");
}

QString SessionLog::sanitizeName(const QString &name)
{
    QString s;
    for (const QChar c : name) {
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('-') || c == QLatin1Char('_')
            || c == QLatin1Char('.');
        s += ok ? c : QLatin1Char('_');
    }
    static const QRegularExpression runs(QStringLiteral("_+"));
    s.replace(runs, QStringLiteral("_"));
    while (s.startsWith(QLatin1Char('.')) || s.startsWith(QLatin1Char('_')) || s.startsWith(QLatin1Char('-'))) {
        s.remove(0, 1); // no hidden files, no option-looking names
    }
    while (s.endsWith(QLatin1Char('_')) || s.endsWith(QLatin1Char('.'))) {
        s.chop(1);
    }
    s.truncate(64);
    return s.isEmpty() ? QStringLiteral("session") : s;
}

QString SessionLog::fileNameFor(const QString &sessionName, const QDateTime &when)
{
    return sanitizeName(sessionName) + QLatin1Char('-') + when.toString(QStringLiteral("yyyyMMdd-HHmmss"))
        + QStringLiteral(".log");
}

bool SessionLog::start(const QString &directory, const QString &sessionName, bool timestamps)
{
    stop();
    m_error.clear();
    const QString dir = directory.isEmpty() ? defaultDirectory() : directory;
    if (!makePrivateDirs(dir, &m_error)) {
        return false;
    }
    const QDateTime now = m_clock();
    const QString base = fileNameFor(sessionName, now);
    int fd = -1;
    QString path;
    for (int n = 1; n < 1000 && fd < 0; ++n) {
        path = QDir(dir).filePath(n == 1 ? base : QString(base).replace(QStringLiteral(".log"), QStringLiteral("-%1.log").arg(n)));
        fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 && errno != EEXIST) {
            break;
        }
    }
    if (fd < 0) {
        m_error = QStringLiteral("Cannot create a log file in %1: %2").arg(dir, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    ::fchmod(fd, 0600);
    if (!m_file.open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        m_error = m_file.errorString();
        return false;
    }
    m_path = path;
    m_timestamps = timestamps;
    m_pauses.clear();
    m_filter = std::make_unique<LogTextFilter>([this](const QString &l) { writeLine(l); });
    m_filter->setColumns(m_columns);
    writeRaw(QStringLiteral("=== zterminal %1 log of \"%2\" started %3 ===\n")
                 .arg(QString::fromLatin1(kVersionString), sessionName, now.toString(QString::fromLatin1(kStamp))));
    return true;
}

void SessionLog::stop()
{
    if (!m_file.isOpen()) {
        return;
    }
    m_filter->flush();
    writeRaw(QStringLiteral("=== log stopped %1 ===\n").arg(m_clock().toString(QString::fromLatin1(kStamp))));
    m_file.close();
    m_filter.reset();
    m_pauses.clear();
}

void SessionLog::writeRaw(const QString &text)
{
    m_file.write(text.toUtf8());
    m_file.flush(); // a crash loses nothing already shown
}

void SessionLog::writeLine(const QString &line)
{
    if (m_timestamps) {
        const QString stamp = m_clock().toString(QString::fromLatin1(kStamp));
        writeRaw(line.isEmpty() ? stamp + QLatin1Char('\n') : stamp + QLatin1Char(' ') + line + QLatin1Char('\n'));
    } else {
        writeRaw(line + QLatin1Char('\n'));
    }
}

void SessionLog::setColumns(int columns)
{
    m_columns = columns;
    if (m_filter) {
        m_filter->setColumns(columns);
    }
}

void SessionLog::feed(const QByteArray &output)
{
    if (!m_file.isOpen() || isSuspended()) {
        return;
    }
    m_filter->feed(output);
}

void SessionLog::marker(const QString &event, const QString &detail)
{
    if (!m_file.isOpen()) {
        return;
    }
    m_filter->flush();
    QString line = QStringLiteral("--- %1 %2").arg(event, m_clock().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    if (!detail.isEmpty()) {
        line += QStringLiteral(" (%1)").arg(QString(detail).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    }
    writeRaw(line + QStringLiteral(" ---\n"));
    m_filter = std::make_unique<LogTextFilter>([this](const QString &l) { writeLine(l); });
    m_filter->setColumns(m_columns);
}

void SessionLog::suspend(const QString &reason)
{
    if (!m_file.isOpen() || m_pauses.contains(reason)) {
        return; // one entry per reason: suspending twice for the same thing is a no-op
    }
    if (m_pauses.isEmpty()) {
        m_filter->flush();
        writeLine(QStringLiteral("[zterminal: logging paused: %1]").arg(reason));
    }
    m_pauses << reason;
}

void SessionLog::resume(const QString &reason)
{
    if (!m_file.isOpen() || !m_pauses.removeOne(reason) || !m_pauses.isEmpty()) {
        return;
    }
    // Restart the line model: half a line from before the pause must not
    // merge with what comes after it.
    m_filter = std::make_unique<LogTextFilter>([this](const QString &l) { writeLine(l); });
    m_filter->setColumns(m_columns);
    writeLine(QStringLiteral("[zterminal: logging resumed]"));
}

} // namespace zterminal
