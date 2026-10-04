#pragma once

#include <QString>

class QSettings;

namespace zterminal {

// Approved defaults (docs/PLAN.md §4.6): select copies to PRIMARY + CLIPBOARD,
// right-click pastes CLIPBOARD, Ctrl+right-click opens the menu, and middle-click
// is configurable (default: paste PRIMARY).
enum class MiddleClickAction { PastePrimary, PasteClipboard, Off };

struct MouseSettings {
    MiddleClickAction middleClick = MiddleClickAction::PastePrimary;
    bool copyOnSelectToClipboard = true; // PRIMARY is always written when supported
    QString wordDelimiters = QStringLiteral(" \t\"'`()[]{}<>|;,");

    static QString toString(MiddleClickAction a);
    // Unknown strings map to the default (PastePrimary).
    static MiddleClickAction middleClickFromString(const QString &s);

    void load(const QSettings &s);
    void save(QSettings &s) const;
};

} // namespace zterminal
