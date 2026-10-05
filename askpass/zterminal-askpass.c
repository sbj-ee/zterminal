/*
 * zterminal-askpass: the SSH_ASKPASS helper zterminal gives ssh.
 *
 * ssh runs it as `zterminal-askpass "<prompt>"` and reads the answer from its
 * stdout. Behaviour:
 *   - A password prompt ("...assword...") with $ZTERMINAL_ASKPASS_SOCKET set:
 *     connect, read the stored password zterminal sends (one shot), print it.
 *   - Anything else, or when the socket is gone (a second password prompt
 *     after a failed attempt), or empty: ask on /dev/tty, the terminal the
 *     session runs in, like ssh itself would. Yes/no questions echo; secrets
 *     don't. No /dev/tty: exit 1 (ssh treats that as "cancelled").
 * The answer is never logged, never put in argv or the environment, and the
 * buffer is wiped before exit.
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#define MAX_SECRET 4096

static char buf[MAX_SECRET + 2];

static void wipe(void *p, size_t n)
{
#if defined(__APPLE__)
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--)
        *v++ = 0;
#else
    explicit_bzero(p, n);
#endif
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Returns the number of bytes read from zterminal, or -1. */
static ssize_t from_socket(const char *path)
{
    struct sockaddr_un addr;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof addr.sun_path)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, len);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    for (;;) {
        if (got >= MAX_SECRET) {
            close(fd);
            wipe(buf, sizeof buf);
            return -1;
        }
        ssize_t r = read(fd, buf + got, MAX_SECRET - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            close(fd);
            wipe(buf, sizeof buf);
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    return (ssize_t)got;
}

static ssize_t from_tty(const char *prompt, int echo)
{
    int tty = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (tty < 0)
        return -1;
    struct termios saved, t;
    int have_termios = tcgetattr(tty, &saved) == 0;
    if (have_termios && !echo) {
        t = saved;
        t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL);
        t.c_lflag |= ICANON;
        tcsetattr(tty, TCSAFLUSH, &t);
    }
    write_all(tty, prompt, strlen(prompt));
    size_t got = 0;
    for (;;) {
        char c;
        ssize_t r = read(tty, &c, 1);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || c == '\n' || c == '\r')
            break;
        if (got < MAX_SECRET)
            buf[got++] = c;
    }
    if (have_termios && !echo) {
        tcsetattr(tty, TCSAFLUSH, &saved);
        write_all(tty, "\r\n", 2);
    }
    close(tty);
    return (ssize_t)got;
}

int main(int argc, char **argv)
{
    const char *prompt = argc > 1 ? argv[1] : "Password: ";
    const char *sock = getenv("ZTERMINAL_ASKPASS_SOCKET");
    int is_password = strstr(prompt, "assword") != NULL;
    int is_question = strstr(prompt, "yes/no") != NULL;
    ssize_t n = -1;

    if (is_password && sock && *sock)
        n = from_socket(sock);
    if (n <= 0)
        n = from_tty(prompt, is_question);
    if (n < 0) {
        wipe(buf, sizeof buf);
        return 1;
    }
    buf[n] = '\n';
    int rc = write_all(STDOUT_FILENO, buf, (size_t)n + 1) == 0 ? 0 : 1;
    wipe(buf, sizeof buf);
    return rc;
}
