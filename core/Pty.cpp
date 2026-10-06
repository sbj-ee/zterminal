#include "Pty.hpp"

#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#if defined(__APPLE__)
#include <util.h>   // openpty / forkpty
#else
#include <pty.h>
#endif
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace zterminal {

namespace {

// Home directory for the PTY child: passwd entry first (reliable when a GUI
// app is launched from Finder/Dock with cwd=/), then $HOME, then "/".
QByteArray userHomeDir(const passwd *pw)
{
    if (pw && pw->pw_dir && *pw->pw_dir) {
        return QByteArray(pw->pw_dir);
    }
    const QByteArray home = qgetenv("HOME");
    if (!home.isEmpty()) {
        return home;
    }
    return QByteArrayLiteral("/");
}

// Fill identity vars a GUI-launched process may lack, and put Homebrew plus
// the usual UNIX dirs on PATH when they exist (Finder PATH is often just
// /usr/bin:/bin:/usr/sbin:/sbin). Login shells still rewrite PATH via
// path_helper / brew shellenv; this is the baseline before that runs.
void ensureUserEnvironment(QProcessEnvironment &env, const passwd *pw, const QString &shell)
{
    const QByteArray home = userHomeDir(pw);
    if (!env.contains(QStringLiteral("HOME")) || env.value(QStringLiteral("HOME")).isEmpty()) {
        env.insert(QStringLiteral("HOME"), QString::fromLocal8Bit(home));
    }
    if (pw && pw->pw_name && *pw->pw_name) {
        const QString name = QString::fromLocal8Bit(pw->pw_name);
        if (!env.contains(QStringLiteral("USER")) || env.value(QStringLiteral("USER")).isEmpty()) {
            env.insert(QStringLiteral("USER"), name);
        }
        if (!env.contains(QStringLiteral("LOGNAME")) || env.value(QStringLiteral("LOGNAME")).isEmpty()) {
            env.insert(QStringLiteral("LOGNAME"), name);
        }
    }
    if (!env.contains(QStringLiteral("SHELL")) || env.value(QStringLiteral("SHELL")).isEmpty()) {
        env.insert(QStringLiteral("SHELL"), shell);
    }

    QStringList parts = env.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    auto ensureDir = [&](const QString &dir, bool prepend) {
        if (!QFileInfo(dir).isDir() || parts.contains(dir)) {
            return;
        }
        if (prepend) {
            parts.prepend(dir);
        } else {
            parts.append(dir);
        }
    };
#if defined(__APPLE__)
    // Apple Silicon Homebrew first; Intel Homebrew /usr/local next.
    ensureDir(QStringLiteral("/opt/homebrew/bin"), true);
    ensureDir(QStringLiteral("/opt/homebrew/sbin"), true);
#endif
    ensureDir(QStringLiteral("/usr/local/bin"), true);
    ensureDir(QStringLiteral("/usr/local/sbin"), true);
    for (const char *d : {"/usr/bin", "/bin", "/usr/sbin", "/sbin"}) {
        ensureDir(QString::fromLatin1(d), false);
    }
    if (!parts.isEmpty()) {
        env.insert(QStringLiteral("PATH"), parts.join(QLatin1Char(':')));
    }
}


// Resolve prog to an absolute path using env's PATH (execve does not search
// PATH; tests put a fake `ssh` first on PATH, and GUI launches often have a
// minimal PATH we already enriched above).
QByteArray resolveExecPath(const QString &prog, const QProcessEnvironment &env)
{
    const QByteArray raw = prog.toLocal8Bit();
    if (prog.startsWith(QLatin1Char('/'))) {
        return raw;
    }
    const QStringList dirs = env.value(QStringLiteral("PATH")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &dir : dirs) {
        const QString candidate = dir + QLatin1Char('/') + prog;
        const QByteArray c = candidate.toLocal8Bit();
        if (::access(c.constData(), X_OK) == 0) {
            return c;
        }
    }
    return raw; // execve will fail; child prints the usual 127 message
}

} // namespace

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
    // Empty program = the user's login shell (local-shell tab). Non-empty is
    // ssh / -e / a saved-session command: keep argv as given, still start in $HOME.
    const bool loginShell = program.isEmpty();
    const QString prog = loginShell ? defaultShell() : program;
    const passwd *pw = ::getpwuid(::getuid());
    const QByteArray home = userHomeDir(pw);

    // Build env before argv: PATH may include a test fake `ssh`, and we must
    // resolve through that PATH because execve does not search it.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    env.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));
    env.insert(QStringLiteral("TERM_PROGRAM"), QStringLiteral("zterminal"));
    ensureUserEnvironment(env, pw, prog);
    for (const QString &kv : extraEnv) {
        const qsizetype eq = kv.indexOf(QLatin1Char('='));
        if (eq > 0) {
            env.insert(kv.left(eq), kv.mid(eq + 1));
        }
    }
    const QByteArray execPath = resolveExecPath(prog, env);

    // Build argv/envp before fork(): the child may only call async-signal-safe functions.
    std::vector<QByteArray> argvStore;
    if (loginShell) {
        // argv0 starting with '-' makes bash/zsh/sh a login shell (loads
        // /etc/zprofile, ~/.zprofile — where Homebrew's shellenv usually lives).
        // execve uses execPath; argv[0] is only the name the shell sees.
        argvStore.push_back(QByteArray("-") + QFileInfo(prog).fileName().toLocal8Bit());
    } else {
        argvStore.push_back(execPath);
    }
    for (const QString &a : args) {
        argvStore.push_back(a.toLocal8Bit());
    }
    std::vector<char *> argv;
    for (QByteArray &a : argvStore) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

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
        // Child. Only async-signal-safe calls from here.
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGCHLD, SIG_DFL);
        // GUI apps on macOS often inherit cwd=/ from LaunchServices; Terminal.app
        // always starts in the user's home. Do the same for every PTY child.
        if (!home.isEmpty()) {
            if (::chdir(home.constData()) != 0) {
                // keep the inherited directory
            }
        }
        // execve is POSIX (unlike GNU execvpe) and takes envp on every platform,
        // so we can use a login argv0 that is not a path.
        ::execve(execPath.constData(), argv.data(), envp.data());
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

bool Pty::isSecretInputMode() const
{
    termios t {};
    if (m_master < 0 || ::tcgetattr(m_master, &t) != 0) { // on Linux the master reports the slave's modes
        return false;
    }
    return !(t.c_lflag & ECHO) && (t.c_lflag & ICANON);
}

} // namespace zterminal
