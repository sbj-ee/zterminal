// The two filters between untrusted text and the user: the paste filter
// (clipboard -> pty; must never let the clipboard end bracketed paste) and
// the window-title path (OSC 0/2 from the remote side -> tab and window
// titles). Memory safety and UB only: the "a paste never ends bracketed
// paste" invariant is enforced by tst_terminal (paste filter PR #27); main's
// one-pass marker removal is bypassable by nesting, which #27 fixes.
#include "Terminal.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    using namespace zterminal;
    const QByteArray raw(reinterpret_cast<const char *>(data), qsizetype(size));
    Terminal t0(2, 10);
    t0.feed("\x1b[?2004h"); // bracketed paste on, as shells enable it

    (void)Terminal::preparePasteBytes(QString::fromUtf8(raw));
    t0.paste(QString::fromUtf8(raw));

    // Title: as an OSC 2 payload (BEL- and ST-terminated, and split).
    Terminal t(4, 20);
    t.feed("\x1b]2;" + raw + "\x07");
    (void)t.title();
    t.feed("\x1b]0;");
    t.feed(raw);
    t.feed("\x1b\\");
    (void)t.title();
    return 0;
}
