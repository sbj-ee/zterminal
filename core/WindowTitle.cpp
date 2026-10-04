#include "WindowTitle.hpp"

#include "version.hpp"

namespace zterminal {

QString makeWindowTitle(const QString &sessionName, const QString &programTitle)
{
    const QString dash = QStringLiteral(" \u2014 ");
    QString title = QStringLiteral("zterminal ") + QString::fromLatin1(kVersionString);
    const QString session = sessionName.trimmed();
    if (!session.isEmpty()) {
        title += dash + session;
    }
    const QString prog = programTitle.trimmed();
    if (!prog.isEmpty() && prog != session) {
        title += dash + prog;
    }
    return title;
}

} // namespace zterminal
