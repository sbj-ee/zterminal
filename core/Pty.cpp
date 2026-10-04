#include "Pty.hpp"

#include <QProcessEnvironment>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace zterminal {

Pty::Pty(QObject *parent)
    : QObject(parent)
{
}

Pty::~Pty()
{
    terminate();
}

QString Pty::defaultShell()
{
    const QByteArray env = qgetenv("SHELL");
    if (!env.isEmpty() && ::access(env.constData(), X_OK) == 0) {
        return QString::fromLocal8Bit(env);
    }
    if (const passwd *pw = ::getpwuid(::getuid()); pw && pw->pw_shell && *pw->pw_shell) {
        return QString::fromLocal8Bit(pw->pw_shell);
    }
    return QStringLiteral("/bin/sh");
}

bool Pty::start(const QString &program, const QStringList &args, int rows, int cols,
                const QStringList &extraEnv)
{
    if (isRunning()) {
        m_error = QStringLiteral("already running");
        return false;
    }
    const QString prog = program.isEmpty() ? defaultShell() : program;

    // Build argv/envp before fork(): the child may only call async-signal-safe functions.
    std::vector<QByteArray> argvStore;
    argvStore.push_back(prog.toLocal8Bit());
    for (const QString &a : args) {
        argvStore.push_back(a.toLocal8Bit());
    }
    std::vector<char *> argv;
    for (QByteArray &a : argvStore) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    env.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("zterminal"));
    for (const QString &kv : extraEnv) {
        const qsizetype eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0) {
            env.insert(kv.left(eq), kv.mid(eq + 1));
        }
    }
    std::vector<QByteArray> envStore;
    for (const QString &kv : env.toStringList()) {
        envStore.push_back(kv.toLocal8Bit());
    }
    std::vector<char *> envp;
    for (QByteArray &e : envStore) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);

    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        m_error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child.
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGCHLD, SIG_DFL);
        ::execvpe(argv[0], argv.data(), envp.data());
        const char msg[] = "zterminal: failed to execute program\r\n";
        (void)!::write(STDERR_FILENO, msg, sizeof msg - 1);
        ::_exit(127);
    }

    m_pid = pid;
    m_master = master;
    m_error.clear();
    ::fcntl(m_master, F_SETFL, ::fcntl(m_master, F_GETFL) | O_NONBLOCK);
    ::fcntl(m_master, F_SETFD, FD_CLOEXEC);

    m_readNotifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &Pty::onReadable);
    m_writeNotifier = new QSocketNotifier(m_master, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &Pty::onWritable);
    return true;
}

void Pty::onReadable()
{
    char buf[16384];
    for (;;) {
        const ssize_t n = ::read(m_master, buf, sizeof buf);
        if (n > 0) {
            emit dataReceived(QByteArray(buf, static_cast<qsizetype>(n)));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // EOF / EIO: the slave side is closed, so the child has exited (or closed its tty).
        closeMaster();
        reap();
        return;
    }
}

void Pty::write(const QByteArray &data)
{
    if (m_master < 0 || data.isEmpty()) {
        return;
    }
    m_pendingWrite.append(data);
    onWritable();
}

void Pty::onWritable()
{
    while (!m_pendingWrite.isEmpty() && m_master >= 0) {
        const ssize_t n = ::write(m_master, m_pendingWrite.constData(),
                                  static_cast<size_t>(m_pendingWrite.size()));
        if (n > 0) {
            m_pendingWrite.remove(0, static_cast<qsizetype>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break; // EAGAIN or error: wait for the notifier
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(!m_pendingWrite.isEmpty());
    }
}

void Pty::resize(int rows, int cols)
{
    if (m_master < 0 || rows <= 0 || cols <= 0) {
        return;
    }
    winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ::ioctl(m_master, TIOCSWINSZ, &ws); // the kernel sends SIGWINCH to the foreground group
}

void Pty::closeMaster()
{
    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    m_pendingWrite.clear();
}

void Pty::reap()
{
    if (m_pid <= 0) {
        return;
    }
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(static_cast<pid_t>(m_pid), &status, 0);
    } while (r < 0 && errno == EINTR);
    m_pid = -1;
    if (r < 0) {
        emit finished(-1, true);
    } else if (WIFEXITED(status)) {
        emit finished(WEXITSTATUS(status), false);
    } else {
        emit finished(WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1, true);
    }
}

void Pty::terminate()
{
    if (m_pid > 0) {
        ::kill(-static_cast<pid_t>(m_pid), SIGHUP);
        ::kill(static_cast<pid_t>(m_pid), SIGHUP);
    }
    closeMaster();
    if (m_pid > 0) {
        // Give the child a moment, then make sure it is gone so we never leak a zombie.
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            if (::waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG) != 0) {
                m_pid = -1;
                return;
            }
            ::usleep(10000);
        }
        ::kill(static_cast<pid_t>(m_pid), SIGKILL);
        ::waitpid(static_cast<pid_t>(m_pid), &status, 0);
        m_pid = -1;
    }
}

} // namespace zterminal
