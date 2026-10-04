#include "Selection.hpp"

#include "Terminal.hpp"

#include <QStringList>
#include <algorithm>

namespace zterminal {

namespace {
bool isDelimiter(const Cell &c, const QString &delimiters)
{
    if (c.width == 0) {
        return false; // right half of a wide char belongs to its word
    }
    if (c.text.isEmpty()) {
        return true; // blank
    }
    return c.text.size() == 1 && delimiters.contains(c.text.at(0));
}
} // namespace

void Selection::wordBounds(const Terminal &term, int line, int col, const QString &delimiters,
                           int *startCol, int *endCol)
{
    const int cols = term.cols();
    col = std::clamp(col, 0, cols - 1);
    if (isDelimiter(term.cell(line, col), delimiters)) {
        *startCol = col;
        *endCol = col + 1;
        return;
    }
    int s = col;
    while (s > 0 && !isDelimiter(term.cell(line, s - 1), delimiters)) {
        --s;
    }
    int e = col + 1;
    while (e < cols && !isDelimiter(term.cell(line, e), delimiters)) {
        ++e;
    }
    *startCol = s;
    *endCol = e;
}

void Selection::begin(const CellPos &pos, Mode mode, const Terminal &term, const QString &delimiters)
{
    m_active = true;
    m_mode = mode;
    m_anchor = pos;
    m_point = pos;
    recompute(term, delimiters);
}

void Selection::extend(const CellPos &pos, const Terminal &term, const QString &delimiters)
{
    if (!m_active) {
        return;
    }
    m_point = pos;
    recompute(term, delimiters);
}

void Selection::recompute(const Terminal &term, const QString &delimiters)
{
    CellPos a = m_anchor;
    CellPos b = m_point;
    if (b < a) {
        std::swap(a, b);
    }
    switch (m_mode) {
    case Mode::Char:
        m_start = a;
        m_end = b; // exclusive: a plain click selects nothing
        break;
    case Mode::Word: {
        int s = 0;
        int e = 0;
        wordBounds(term, a.line, a.col, delimiters, &s, &e);
        m_start = {a.line, s};
        wordBounds(term, b.line, b.col, delimiters, &s, &e);
        m_end = {b.line, e};
        break;
    }
    case Mode::Line:
        m_start = {a.line, 0};
        m_end = {b.line, term.cols()};
        break;
    }
}

void Selection::selectAll(const Terminal &term)
{
    m_active = true;
    m_mode = Mode::Char;
    m_anchor = {0, 0};
    m_point = {term.totalLines() - 1, term.cols()};
    m_start = m_anchor;
    m_end = m_point;
}

void Selection::clear()
{
    m_active = false;
    m_start = m_end = m_anchor = m_point = {};
}

bool Selection::isEmpty() const
{
    return !m_active || !(m_start < m_end);
}

bool Selection::contains(int line, int col) const
{
    if (isEmpty()) {
        return false;
    }
    const CellPos p{line, col};
    return !(p < m_start) && p < m_end;
}

void Selection::linesDropped(int count)
{
    if (!m_active || count <= 0) {
        return;
    }
    m_anchor.line -= count;
    m_point.line -= count;
    m_start.line -= count;
    m_end.line -= count;
    if (m_end.line < 0) {
        clear();
        return;
    }
    if (m_start.line < 0) {
        m_start = {0, 0};
    }
    if (m_anchor.line < 0) {
        m_anchor = {0, 0};
    }
    if (m_point.line < 0) {
        m_point = {0, 0};
    }
}

QString Selection::text(const Terminal &term) const
{
    if (isEmpty()) {
        return {};
    }
    QStringList lines;
    const int last = std::min(m_end.line, term.totalLines() - 1);
    for (int line = std::max(m_start.line, 0); line <= last; ++line) {
        const int from = line == m_start.line ? m_start.col : 0;
        const bool toEnd = line != m_end.line || m_end.col >= term.cols();
        if (toEnd) {
            lines << term.lineText(line, from, -1);
        } else {
            lines << term.lineText(line, from, m_end.col);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace zterminal
