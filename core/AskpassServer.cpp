#include "AskpassServer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
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

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace zterminal {

namespace {

int makeListenSocket(QString *error)
{
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (fd < 0) {
        if (error) {
            *error = QStringLiteral("socket: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        return -1;
    }
#if !defined(SOCK_CLOEXEC) || !defined(SOCK_NONBLOCK)
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
#if defined(__APPLE__)
    // Avoid SIGPIPE on send() when the helper goes away mid-write.
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

int acceptHelper(int listenFd)
{
#if defined(__APPLE__) || !defined(SOCK_CLOEXEC)
    const int fd = ::accept(listenFd, nullptr, nullptr);
    if (fd >= 0) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(__APPLE__)
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }
    return fd;
#else
    return ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
#endif
}

struct PeerCred {
    uid_t uid = static_cast<uid_t>(-1);
    pid_t pid = -1;
    bool ok = false;
};

PeerCred peerCredentials(int fd)
{
    PeerCred out;
#if defined(__APPLE__)
    uid_t euid = static_cast<uid_t>(-1);
    gid_t egid = static_cast<gid_t>(-1);
    if (::getpeereid(fd, &euid, &egid) != 0) {
        return out;
    }
    out.uid = euid;
    // LOCAL_PEERPID (sys/un.h) yields the connecting process id on Darwin.
    pid_t peerPid = -1;
    socklen_t len = sizeof(peerPid);
    if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peerPid, &len) == 0) {
        out.pid = peerPid;
    }
    out.ok = true;
#else
    ucred cred {};
    socklen_t len = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return out;
    }
    out.uid = cred.uid;
    out.pid = cred.pid;
    out.ok = true;
#endif
    return out;
}

#if defined(__APPLE__)
pid_t parentOf(pid_t pid)
{
    if (pid <= 0) {
        return -1;
    }
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (::sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size < sizeof(info)) {
        return -1;
    }
    return info.kp_eproc.e_ppid;
}
#endif

} // namespace

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
    QString sockErr;
    m_listenFd = makeListenSocket(&sockErr);
    if (m_listenFd < 0) {
        return fail(sockErr);
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
    QStringList env{QStringLiteral("SSH_ASKPASS=") + helperPath, QStringLiteral("SSH_ASKPASS_REQUIRE=force"),
                    QStringLiteral("ZTERMINAL_ASKPASS_SOCKET=") + m_socketPath};
    QString host = m_target.host;
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']'))) {
        host = host.mid(1, host.size() - 2);
    }
    if (!host.isEmpty()) {
        env << QStringLiteral("ZTERMINAL_ASKPASS_TARGET=") + m_target.user + QLatin1Char('@') + host;
    }
    if (!m_target.hostKeyAlias.isEmpty()) {
        env << QStringLiteral("ZTERMINAL_ASKPASS_HOSTKEYALIAS=") + m_target.hostKeyAlias;
    }
    if (m_target.viaJump) {
        env << QStringLiteral("ZTERMINAL_ASKPASS_VIA_JUMP=1");
    }
    return env;
}

namespace {
QString &testHelperPath()
{
    static QString p;
    return p;
}
} // namespace

void AskpassServer::setHelperPathForTests(const QString &path)
{
    testHelperPath() = path;
}

AskpassTarget AskpassServer::parseSshConfigDump(const QByteArray &dump, const AskpassTarget &fallback)
{
    AskpassTarget t = fallback;
    bool sawHost = false;
    for (const QByteArray &raw : dump.split('\n')) {
        const QByteArray line = raw.trimmed();
        const qsizetype sp = line.indexOf(' ');
        if (sp <= 0) {
            continue;
        }
        const QByteArray key = line.left(sp).toLower();
        const QString val = QString::fromUtf8(line.mid(sp + 1).trimmed());
        if (val.isEmpty()) {
            continue;
        }
        const bool none = val.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0;
        if (key == "hostname") {
            t.host = val;
            sawHost = true;
        } else if (key == "user") {
            t.user = val;
        } else if (key == "hostkeyalias" && !none) {
            t.hostKeyAlias = val;
        } else if ((key == "proxyjump" || key == "proxycommand") && !none) {
            t.viaJump = true;
        }
    }
    if (!sawHost) {
        return fallback; // not ssh -G output
    }
    t.viaJump = t.viaJump || fallback.viaJump;
    return t;
}

AskpassTarget AskpassServer::resolveTarget(const QString &sshProgram, const QStringList &sshArgs,
                                           const AskpassTarget &fallback, int timeoutMs)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.setStandardInputFile(QProcess::nullDevice());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("SSH_ASKPASS")); // -G never authenticates; be sure
    env.remove(QStringLiteral("ZTERMINAL_ASKPASS_SOCKET"));
    p.setProcessEnvironment(env);
    p.start(sshProgram, QStringList{QStringLiteral("-G")} + sshArgs);
    if (!p.waitForStarted(timeoutMs) || !p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return fallback;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        return fallback;
    }
    return parseSshConfigDump(p.readAllStandardOutput(), fallback);
}

QString AskpassServer::findHelper()
{
    if (!testHelperPath().isEmpty()) {
        return QFileInfo(testHelperPath()).isExecutable() ? testHelperPath() : QString();
    }
#if defined(ZTERMINAL_DEV_OVERRIDES)
    const QString env = qEnvironmentVariable("ZTERMINAL_ASKPASS");
    if (!env.isEmpty()) {
        return QFileInfo(env).isExecutable() ? env : QString();
    }
#endif
    const QString appDir = QCoreApplication::applicationDirPath();
    // Build tree / Linux install / macOS .app (Contents/MacOS).
    for (const QString &c : {appDir + QStringLiteral("/zterminal-askpass"),
                             appDir + QStringLiteral("/../libexec/zterminal/zterminal-askpass"),
                             appDir + QStringLiteral("/../Helpers/zterminal-askpass")}) {
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
#if defined(__APPLE__)
        const pid_t ppid = parentOf(static_cast<pid_t>(pid));
        if (ppid <= 0) {
            return false;
        }
        pid = ppid;
#else
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
#endif
    }
    return false;
}

void AskpassServer::onConnection()
{
    const int fd = acceptHelper(m_listenFd);
    if (fd < 0) {
        return;
    }
    const PeerCred cred = peerCredentials(fd);
    QString why;
    if (!cred.ok) {
        why = QStringLiteral("no peer credentials");
    } else if (cred.uid != ::getuid()) {
        why = QStringLiteral("peer uid %1 is not ours").arg(cred.uid);
    } else if (cred.pid > 0 && !isDescendant(cred.pid, m_ancestor)) {
        why = QStringLiteral("peer pid %1 is not part of this ssh session").arg(cred.pid);
    } else if (cred.pid <= 0 && m_ancestor > 0) {
        // Darwin without LOCAL_PEERPID: refuse rather than serve an unverified peer.
        why = QStringLiteral("peer pid unavailable");
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
#if defined(MSG_NOSIGNAL)
        const ssize_t w = ::send(fd, p, left, MSG_NOSIGNAL);
#else
        const ssize_t w = ::send(fd, p, left, 0);
#endif
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
