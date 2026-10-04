#pragma once

#include <QByteArray>
#include <QString>
#include <QStringDecoder>
#include <QStringList>

#include <functional>

namespace zterminal {

// Turns a terminal output stream into plain text lines for session logs.
//
// A small line model, not a full emulator: printable characters are written at
// a cursor column in the current line, so
//   CR            -> column 0 (later text overwrites: "10%\r20%" logs "20%")
//   BS            -> one column left (readline's "abc\b \b" erases the c)
//   TAB           -> next multiple of 8
//   LF            -> the line is complete (trailing spaces trimmed) and emitted
//   CSI K / C / D / G / P / @  -> erase in line, cursor right/left/column,
//                    delete/insert characters (what line editors use)
//   CSI H / f / d -> a jump to another row: the current line is emitted
// Every other escape sequence (SGR colours, OSC titles/hyperlinks, DCS/APC/PM/
// SOS strings, charset selection, private modes, ...) and C0/C1 control
// character is dropped. Output on the alternate screen (vim, htop, less) is not
// logged: entering it emits the pending line, leaving it resumes normal logging.
// Input is decoded as UTF-8 across chunk boundaries.
class LogTextFilter
{
public:
    using LineSink = std::function<void(const QString &line)>;
    explicit LogTextFilter(LineSink sink);

    // Terminal width, so autowrap is understood: a logical line longer than
    // the window continues on the next row, and CR/BS/CSI G/A then act on that
    // row (readline wraps long command lines with " \r"). 0 = unknown/infinite.
    void setColumns(int columns) { m_columns = columns > 0 ? columns : 0; }

    void feed(const QByteArray &bytes);
    // Emit the partial current line, if it has any text (e.g. a prompt at stop).
    void flush();
    bool onAlternateScreen() const { return m_alt; }

    static QStringList filterAll(const QByteArray &bytes, int columns = 0); // convenience for tests

private:
    enum class State { Ground, Esc, EscIntermediate, Csi, String, StringEsc };
    void put(QChar c);
    void endLine();
    void csi(QChar final);
    int param(int index, int dflt) const;

    LineSink m_sink;
    QStringDecoder m_decoder {QStringDecoder::Utf8};
    State m_state = State::Ground;
    QString m_line;
    int m_col = 0;
    int m_rowStart = 0; // column where the current screen row of this logical line starts
    int m_columns = 0;
    QString m_params; // CSI parameter/intermediate bytes
    bool m_alt = false;
};

} // namespace zterminal
