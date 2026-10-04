#include "TerminalSearch.hpp"

#include "Terminal.hpp"

#include <QRegularExpression>

namespace zterminal {

namespace {

// One logical line: consecutive physical lines joined across soft wraps.
struct Logical {
    QString text;
    std::vector<int> col;  // cell column of each UTF-16 unit
    std::vector<int> line; // absolute line of each UTF-16 unit
};

// Whether a physical line reaches the last column (and so may wrap on): its
// last character sits in the last column, or one before it and is wide.
bool fillsLine(const std::vector<int> &cols, const QString &text, int width)
{
    if (cols.empty()) {
        return false;
    }
    const int last = cols.back();
    if (last >= width - 1) {
        return true;
    }
    return last == width - 2 && !text.isEmpty() && (text.back().isSurrogate() || text.back().unicode() >= 0x1100);
}

// Emit [from, to) of a logical line as one entry per physical line.
void emitSpan(const Logical &l, qsizetype from, qsizetype to, std::vector<FindMatch> &out)
{
    qsizetype i = from;
    bool first = true;
    while (i < to) {
        const int line = l.line[std::size_t(i)];
        qsizetype j = i;
        while (j < to && l.line[std::size_t(j)] == line) {
            ++j;
        }
        FindMatch m;
        m.line = line;
        m.col = l.col[std::size_t(i)];
        int end = l.col[std::size_t(j - 1)] + 1;
        // A wide character covers two columns: extend to the next character's column.
        if (std::size_t(j) < l.col.size() && l.line[std::size_t(j)] == line && l.col[std::size_t(j)] > end) {
            end = l.col[std::size_t(j)];
        }
        m.cols = std::max(1, end - m.col);
        m.continued = !first;
        out.push_back(m);
        first = false;
        i = j;
    }
}

} // namespace

std::vector<FindMatch> TerminalSearch::findAll(const Terminal &term, const QString &pattern,
                                               const FindOptions &opts, QString *error)
{
    std::vector<FindMatch> out;
    if (error) {
        error->clear();
    }
    if (pattern.isEmpty()) {
        return out;
    }
    const Qt::CaseSensitivity cs = opts.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QRegularExpression re;
    if (opts.regex) {
        re = QRegularExpression(pattern, opts.caseSensitive ? QRegularExpression::NoPatternOption
                                                           : QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) {
            if (error) {
                *error = re.errorString();
            }
            return out;
        }
        re.optimize();
    }
    const int total = term.totalLines();
    const int width = term.cols();
    std::vector<int> cols;
    std::vector<qsizetype> hits;
    Logical l;

    // Matches in text: [start, end) pairs.
    auto scan = [&](const QString &text, std::vector<std::pair<qsizetype, qsizetype>> &found) {
        found.clear();
        if (opts.regex) {
            auto it = re.globalMatch(text);
            while (it.hasNext()) {
                const auto m = it.next();
                if (m.capturedLength() > 0) {
                    found.emplace_back(m.capturedStart(), m.capturedEnd());
                }
            }
        } else {
            qsizetype from = 0;
            while ((from = text.indexOf(pattern, from, cs)) >= 0) {
                found.emplace_back(from, from + pattern.size());
                from += pattern.size();
            }
        }
    };
    std::vector<std::pair<qsizetype, qsizetype>> found;

    // Whether a line reaches the last column. Text with only narrow, non-
    // combining characters has one UTF-16 unit per cell, so its length says;
    // anything else asks for the column map.
    auto fills = [&](int ln, const QString &t) {
        if (t.size() >= width) {
            return true;
        }
        if (t.size() < width / 2) {
            return false;
        }
        for (QChar ch : t) {
            if (ch.unicode() >= 0x300) {
                cols.clear();
                term.searchText(ln, &cols);
                return fillsLine(cols, t, width);
            }
        }
        return false;
    };
    std::vector<int> joinedLines;
    int line = 0;
    while (line < total) {
        // Text only first; the column maps are built just for logical lines with a hit.
        l.text = term.searchText(line);
        joinedLines.assign(1, line);
        int cur = line;
        while (cur + 1 < total && int(joinedLines.size()) < kMaxJoinedLines
               && fills(cur, cur == line ? l.text : term.searchText(cur))) {
            ++cur;
            l.text += term.searchText(cur);
            joinedLines.push_back(cur);
        }
        line = cur + 1;
        if (l.text.isEmpty()) {
            continue;
        }
        scan(l.text, found);
        if (found.empty()) {
            continue;
        }
        l.col.clear();
        l.line.clear();
        for (int ln : joinedLines) {
            cols.clear();
            term.searchText(ln, &cols);
            l.col.insert(l.col.end(), cols.begin(), cols.end());
            l.line.insert(l.line.end(), cols.size(), ln);
        }
        for (const auto &[a, b] : found) {
            emitSpan(l, a, b, out);
        }
    }
    return out;
}

} // namespace zterminal
