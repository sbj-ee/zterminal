#pragma once

#include <QString>

namespace zterminal {

class Terminal;

// A position in absolute line coordinates (see Terminal).
struct CellPos {
    int line = 0;
    int col = 0;
    friend bool operator==(const CellPos &a, const CellPos &b) { return a.line == b.line && a.col == b.col; }
    friend bool operator<(const CellPos &a, const CellPos &b)
    {
        return a.line < b.line || (a.line == b.line && a.col < b.col);
    }
};

// Mouse selection: character (drag), word (double-click) or line (triple-click).
class Selection
{
public:
    enum class Mode { Char, Word, Line };

    void begin(const CellPos &pos, Mode mode, const Terminal &term, const QString &delimiters);
    void extend(const CellPos &pos, const Terminal &term, const QString &delimiters);
    void clear();
    void selectAll(const Terminal &term);

    bool isEmpty() const;
    bool isActive() const { return m_active; }
    Mode mode() const { return m_mode; }
    // Inclusive start, exclusive end, ordered.
    CellPos start() const { return m_start; }
    CellPos end() const { return m_end; }
    bool contains(int line, int col) const;

    // History lines were discarded: shift coordinates up; clears if the selection fell off.
    void linesDropped(int count);

    // Plain text of the selection, one '\n' between rows. trimTrailingWhitespace
    // strips spaces/tabs at the end of every line (also of a partly selected
    // last line); without it, spaces the program printed are kept and only
    // never-written cells past the end of a line are dropped.
    QString text(const Terminal &term, bool trimTrailingWhitespace = true) const;

    // Word boundaries on a line around `col` (inclusive start, exclusive end).
    static void wordBounds(const Terminal &term, int line, int col, const QString &delimiters,
                           int *startCol, int *endCol);

private:
    void recompute(const Terminal &term, const QString &delimiters);

    bool m_active = false;
    Mode m_mode = Mode::Char;
    CellPos m_anchor;
    CellPos m_point;
    CellPos m_start;
    CellPos m_end;
};

} // namespace zterminal
