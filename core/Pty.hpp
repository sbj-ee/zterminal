#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;

namespace zterminal {

// A child process on a pseudo-terminal (forkpty). The local-shell and
// ssh-via-system-ssh backends both use this.
class Pty : public QObject
{
    Q_OBJECT
public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    // Starts `program` with `args` (argv list, never a shell string).
    // An empty program starts the user's login shell ($SHELL, else passwd,
    // else /bin/sh) with argv0 = "-basename" so profile files load (e.g.
    // ~/.zprofile / Homebrew shellenv on macOS). Every child chdirs to the
    // user's home and gets HOME/USER/LOGNAME/SHELL plus a sane PATH.
    bool start(const QString &program, const QStringList &args, int rows, int cols,
               const QStringList &extraEnv = {});
    void write(const QByteArray &data);
    void resize(int rows, int cols);
    // Sends SIGHUP to the child's process group and closes the master side.
    void terminate();

    bool isRunning() const { return m_pid > 0; }
    qint64 pid() const { return m_pid; }
    QString errorString() const { return m_error; }

    static QString defaultShell();

    // The terminal is in "type a secret" mode: canonical input with ECHO off
    // (what sudo, passwd, ssh's own prompt and zterminal-askpass's manual
    // fallback set). Raw mode (ssh's interactive session, vim) is *not* this.
    bool isSecretInputMode() const;

signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode, bool crashed);

private:
    void onReadable();
    void onWritable();
    void reap();
    void closeMaster();

    int m_master = -1;
    qint64 m_pid = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_pendingWrite;
    QString m_error;
};

} // namespace zterminal
