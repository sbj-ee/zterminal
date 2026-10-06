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
//   - a connecting peer must have our uid (SO_PEERCRED on Linux, getpeereid
//     + LOCAL_PEERPID on macOS) and be a descendant of the ssh process we
//     started;
//   - the secret is written once with ::write() straight from secure memory,
//     then wiped; the socket and directory are removed. A second password
//     prompt therefore finds no socket and the helper asks the user on the
//     terminal: one automatic attempt, no loop.
//   - unused, it expires after timeoutMs (default 2 min).
//
// Which prompt gets the password: the helper only answers a prompt that ssh
// labels with the session's own target ("(user@host) ..." or
// "user@host's password: "), or a bare "Password:" when no jump host/proxy is
// involved (askpass/askpass_policy.h). So with -J, the jump host never
// receives the target's password. The target goes to the helper in
// ZTERMINAL_ASKPASS_TARGET / _HOSTKEYALIAS / _VIA_JUMP (not secret).
struct AskpassTarget {
    QString user;         // may be empty (ssh's default user)
    QString host;         // as ssh prints it: HostName after ~/.ssh/config
    QString hostKeyAlias; // optional
    bool viaJump = false; // ProxyJump / -J / ProxyCommand in effect
};

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

    void setTarget(const AskpassTarget &t) { m_target = t; }
    AskpassTarget target() const { return m_target; }

    // The environment for ssh: SSH_ASKPASS, SSH_ASKPASS_REQUIRE,
    // ZTERMINAL_ASKPASS_SOCKET, ZTERMINAL_ASKPASS_TARGET and, when set,
    // ZTERMINAL_ASKPASS_HOSTKEYALIAS and ZTERMINAL_ASKPASS_VIA_JUMP=1.
    QStringList sshEnvironment(const QString &helperPath) const;
    // zterminal-askpass next to the app, else <prefix>/libexec/zterminal/,
    // else Contents/Helpers (macOS). Empty if none exists. Builds with
    // ZTERMINAL_DEV_OVERRIDES (debug/test builds only) also honour
    // $ZTERMINAL_ASKPASS; release builds never read it.
    static QString findHelper();
    // Tests: use this helper (empty: back to the normal search).
    static void setHelperPathForTests(const QString &path);

    // What ssh will really connect to, for the helper's prompt check:
    // `ssh -G <args>` (same program and options as the session, so
    // ~/.ssh/config HostName/User/ProxyJump are honoured), falling back to
    // `fallback` (the session's fields) if that fails. Never connects.
    static AskpassTarget resolveTarget(const QString &sshProgram, const QStringList &sshArgs,
                                       const AskpassTarget &fallback, int timeoutMs = 3000);
    // Parses `ssh -G` output on top of `fallback` (public for tests).
    static AskpassTarget parseSshConfigDump(const QByteArray &dump, const AskpassTarget &fallback);

    // True if `pid` is `ancestor` or descends from it (/proc on Linux, sysctl on macOS).
    static bool isDescendant(qint64 pid, qint64 ancestor);

signals:
    void served();
    void rejected(const QString &why);
    void expired();

private:
    void onConnection();
    void shutdown();

    SecureBuffer m_secret;
    AskpassTarget m_target;
    QString m_dir;
    QString m_socketPath;
    int m_listenFd = -1;
    qint64 m_ancestor = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace zterminal
