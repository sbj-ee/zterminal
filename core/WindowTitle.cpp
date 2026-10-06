#include "WindowTitle.hpp"

#include "Terminal.hpp"
#include "version.hpp"

namespace zterminal {

QString makeWindowTitle(const QString &sessionName, const QString &programTitle)
{
    const QString dash = QStringLiteral(" \u2014 ");
    QString title = QStringLiteral("zterminal ") + QString::fromLatin1(kVersionString);
    const QString session = Terminal::sanitizeTitle(sessionName).trimmed();
    if (!session.isEmpty()) {
        title += dash + session;
    }
    // Also sanitized here: the program title may come from anywhere.
    const QString prog = Terminal::sanitizeTitle(programTitle).trimmed();
    if (!prog.isEmpty() && prog != session) {
        title += dash + prog;
    }
    return title;
}

} // namespace zterminal
