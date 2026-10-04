#pragma once

#include <QString>

namespace zterminal {

// What a paste would do, for the multi-line paste confirmation (PLAN.md §4.14).
struct PasteInfo {
    int lines = 0;               // lines of text (a final newline doesn't add one)
    bool endsWithNewline = false; // the last line would run too
    int controlChars = 0;        // C0/C1 controls other than TAB/CR/LF, and DEL
    qsizetype bytes = 0;         // UTF-8 bytes that would be sent
    QString preview;             // first lines, controls made visible, long lines cut
    int previewLines = 0;        // lines shown in preview
    // A paste with any line break needs confirming: it would run commands.
    bool needsConfirm() const { return lines > 1 || endsWithNewline; }

    static PasteInfo analyze(const QString &text, int maxPreviewLines = 12, int maxLineChars = 200);
    // Rough time a paced serial send takes: charDelay after every byte, the
    // larger of the two after each line end.
    static qint64 pacedMilliseconds(const QByteArray &bytes, int charDelayMs, int lineDelayMs);
};

} // namespace zterminal
