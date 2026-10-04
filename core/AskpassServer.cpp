#include "AskpassServer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace zterminal {

AskpassServer::AskpassServer(SecureBuffer secret, QObject *parent)
    : QObject(parent)
    , m_secret(std::move(secret))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(120 * 1000);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        shutdown();
        emit expired();
    });
}

AskpassServer::~AskpassServer()
{
    shutdown();
}

void AskpassServer::setTimeoutMs(int ms)
{
    m_timer->setInterval(ms);
    if (m_timer->isActive()) {
        m_timer->start();
    }
}

bool AskpassServer::listen(QString *error)
{
    auto fail = [&](const QString &m) {
        if (error) {
            *error = m;
        }
        shutdown();
        return false;
    };
    QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty() || !QFileInfo(base).isDir()) {
        base = QDir::tempPath();
    }
    QByteArray tmpl = QFile::encodeName(base + QStringLiteral("/zterminal-askpass-XXXXXX"));
    if (!::mkdtemp(tmpl.data())) { // created 0700
        return fail(QStringLiteral("cannot create a private directory: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    m_dir = QFile::decodeName(tmpl);
    m_socketPath = m_dir + QStringLiteral("/s");
    const QByteArray sp = QFile::encodeName(m_socketPath);
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (std::size_t(sp.size()) >= sizeof addr.sun_path) {
        return fail(QStringLiteral("socket path too long"));
    }
    std::memcpy(addr.sun_path, sp.constData(), std::size_t(sp.size()));
    m_listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m_listenFd < 0) {
        return fail(QStringLiteral("socket: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    const mode_t old = ::umask(0077);
    const int rc = ::bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    ::umask(old);
    if (rc < 0 || ::listen(m_listenFd, 4) < 0) {
        return fail(QStringLiteral("bind/listen: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    m_notifier = new QSocketNotifier(m_listenFd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &AskpassServer::onConnection);
    m_timer->start();
    return true;
}

QStringList AskpassServer::sshEnvironment(const QString &helperPath) const
{
    return {QStringLiteral("SSH_ASKPASS=") + helperPath, QStringLiteral("SSH_ASKPASS_REQUIRE=force"),
            QStringLiteral("ZTERMINAL_ASKPASS_SOCKET=") + m_socketPath};
}

QString AskpassServer::findHelper()
{
    const QString env = qEnvironmentVariable("ZTERMINAL_ASKPASS");
    if (!env.isEmpty()) {
        return QFileInfo(env).isExecutable() ? env : QString();
    }
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &c : {appDir + QStringLiteral("/zterminal-askpass"),
                             appDir + QStringLiteral("/../libexec/zterminal/zterminal-askpass")}) {
        if (QFileInfo(c).isExecutable()) {
            return QFileInfo(c).canonicalFilePath();
        }
    }
    return {};
}

bool AskpassServer::isDescendant(qint64 pid, qint64 ancestor)
{
    if (pid <= 0 || ancestor <= 1) { // never "anything under init"
        return false;
    }
    for (int depth = 0; depth < 16 && pid > 1; ++depth) {
        if (pid == ancestor) {
            return true;
        }
        QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
        if (!stat.open(QIODevice::ReadOnly)) {
            return false;
        }
        const QByteArray line = stat.readAll();
        // "pid (comm) state ppid ..." - comm may contain spaces/parens; use the last ')'.
        const qsizetype close = line.lastIndexOf(')');
        if (close < 0) {
            return false;
        }
        const QList<QByteArray> rest = line.mid(close + 2).split(' ');
        if (rest.size() < 2) {
            return false;
        }
        pid = rest.at(1).toLongLong();
    }
    return false;
}

void AskpassServer::onConnection()
{
    const int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        return;
    }
    ucred cred {};
    socklen_t len = sizeof cred;
    QString why;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        why = QStringLiteral("no peer credentials");
    } else if (cred.uid != ::getuid()) {
        why = QStringLiteral("peer uid %1 is not ours").arg(cred.uid);
    } else if (!isDescendant(cred.pid, m_ancestor)) {
        why = QStringLiteral("peer pid %1 is not part of this ssh session").arg(cred.pid);
    }
    if (!why.isEmpty()) {
        ::close(fd); // keep listening for the real helper
        emit rejected(why);
        return;
    }
    // Blocking write of a few bytes straight from secure memory (no Qt buffers).
    const int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    const unsigned char *p = m_secret.data();
    std::size_t left = m_secret.size();
    while (left > 0) {
        const ssize_t w = ::send(fd, p, left, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            break;
        }
        p += w;
        left -= std::size_t(w);
    }
    ::close(fd);
    shutdown(); // one shot: wipe, close, unlink
    emit served();
}

void AskpassServer::shutdown()
{
    m_secret.reset();
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    if (!m_socketPath.isEmpty()) {
        ::unlink(QFile::encodeName(m_socketPath).constData());
    }
    if (!m_dir.isEmpty()) {
        ::rmdir(QFile::encodeName(m_dir).constData());
        m_dir.clear();
    }
    if (m_timer) {
        m_timer->stop();
    }
}

} // namespace zterminal
