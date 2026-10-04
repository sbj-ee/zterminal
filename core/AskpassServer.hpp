#pragma once

#include "SecureBuffer.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;
class QTimer;

namespace zterminal {

// Hands one stored password to zterminal-askpass, which ssh runs (via
// SSH_ASKPASS + SSH_ASKPASS_REQUIRE=force) when it needs a password.
//
// Why a socket and not an inherited pipe: ssh closes every inherited fd above
// stderr at startup (closefrom), so a pipe from zterminal can't reach the
// helper ssh spawns. Instead:
//   - a private 0700 directory under $XDG_RUNTIME_DIR (else /tmp) holds a
//     Unix socket; only its *path* goes in the environment
//     (ZTERMINAL_ASKPASS_SOCKET), never the secret;
//   - a connecting peer must have our uid (SO_PEERCRED) and be a descendant
//     of the ssh process we started;
//   - the secret is written once with ::write() straight from secure memory,
//     then wiped; the socket and directory are removed. A second password
//     prompt therefore finds no socket and the helper asks the user on the
//     terminal: one automatic attempt, no loop.
//   - unused, it expires after timeoutMs (default 2 min).
class AskpassServer : public QObject
{
    Q_OBJECT
public:
    explicit AskpassServer(SecureBuffer secret, QObject *parent = nullptr);
    ~AskpassServer() override;

    bool listen(QString *error = nullptr);
    QString socketPath() const { return m_socketPath; }
    // Only this process (normally the ssh pid) and its descendants are served.
    void setAllowedAncestor(qint64 pid) { m_ancestor = pid; }
    void setTimeoutMs(int ms);
    bool isActive() const { return m_listenFd >= 0; }

    // The environment for ssh: SSH_ASKPASS, SSH_ASKPASS_REQUIRE, ZTERMINAL_ASKPASS_SOCKET.
    QStringList sshEnvironment(const QString &helperPath) const;
    // $ZTERMINAL_ASKPASS, else zterminal-askpass next to the app, else
    // <prefix>/libexec/zterminal/zterminal-askpass. Empty if none exists.
    static QString findHelper();

    // True if `pid` is `ancestor` or descends from it (walks /proc ppid links).
    static bool isDescendant(qint64 pid, qint64 ancestor);

signals:
    void served();
    void rejected(const QString &why);
    void expired();

private:
    void onConnection();
    void shutdown();

    SecureBuffer m_secret;
    QString m_dir;
    QString m_socketPath;
    int m_listenFd = -1;
    qint64 m_ancestor = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace zterminal
