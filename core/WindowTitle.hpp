#pragma once

#include <QString>

namespace zterminal {

// "zterminal <version> — <session>[ — <program title>]".
// The prefix always comes from the build (kVersionString); a title set by the
// program (OSC 0/2) is appended, never substituted.
QString makeWindowTitle(const QString &sessionName, const QString &programTitle = {});

} // namespace zterminal
