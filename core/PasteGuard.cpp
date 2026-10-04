#include "PasteGuard.hpp"

#include "Terminal.hpp"

#include <QStringList>

#include <algorithm>

namespace zterminal {

namespace {

bool isControl(char32_t u)
{
    return (u < 0x20 && u != u'\t' && u != u'\r' && u != u'\n') || u == 0x7f || (u >= 0x80 && u < 0xa0);
}

// ESC -> ␛ etc. (Control Pictures block), so the preview can't move the cursor
// or hide text, and the user sees what's there.
QString visible(const QString &line)
{
    QString out;
    for (const char32_t u : line.toUcs4()) {
        if (u < 0x20) {
            out += QChar(0x2400 + static_cast<char16_t>(u));
        } else if (u == 0x7f) {
            out += QChar(0x2421);
        } else if (u >= 0x80 && u < 0xa0) {
            out += QStringLiteral("<%1>").arg(static_cast<uint>(u), 2, 16, QLatin1Char('0'));
        } else {
            out += QString::fromUcs4(&u, 1);
        }
    }
    return out;
}

} // namespace

PasteInfo PasteInfo::analyze(const QString &text, int maxPreviewLines, int maxLineChars)
{
    PasteInfo info;
    if (text.isEmpty()) {
        return info;
    }
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    info.endsWithNewline = t.endsWith(QLatin1Char('\n'));
    if (info.endsWithNewline) {
        t.chop(1);
    }
    const QStringList lines = t.split(QLatin1Char('\n'));
    info.lines = static_cast<int>(lines.size());
    for (const char32_t u : text.toUcs4()) {
        if (isControl(u)) {
            ++info.controlChars;
        }
    }
    info.bytes = Terminal::preparePasteBytes(text).size();
    QStringList shown;
    for (const QString &l : lines) {
        if (shown.size() >= maxPreviewLines) {
            break;
        }
        QString v = visible(l);
        if (v.size() > maxLineChars) {
            v = v.left(maxLineChars) + QChar(0x2026);
        }
        shown << v;
    }
    info.previewLines = static_cast<int>(shown.size());
    info.preview = shown.join(QLatin1Char('\n'));
    return info;
}

qint64 PasteInfo::pacedMilliseconds(const QByteArray &bytes, int charDelayMs, int lineDelayMs)
{
    qint64 ms = 0;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const bool lineEnd = bytes.at(i) == '\r' || bytes.at(i) == '\n';
        ms += lineEnd ? std::max(charDelayMs, lineDelayMs) : charDelayMs;
    }
    return ms;
}

} // namespace zterminal
