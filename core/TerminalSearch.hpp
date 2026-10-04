#pragma once

#include <vector>

#include <QString>

namespace zterminal {

class Terminal;

// One Find hit, in cell coordinates: absolute line (Terminal numbering:
// history oldest first, then the screen), first column and width in columns.
struct FindMatch {
    int line = 0;
    int col = 0;
    int cols = 0;
    // Continues the previous entry's match: the line soft-wraps and the match
    // runs on from the line above. Count/navigate only entries without it.
    bool continued = false;
    bool operator==(const FindMatch &o) const
    {
        return line == o.line && col == o.col && cols == o.cols && continued == o.continued;
    }
};

struct FindOptions {
    bool caseSensitive = false;
    bool regex = false;
};

// Find across a Terminal's whole scrollback plus its screen (docs/PLAN.md §4.17).
// Soft-wrapped lines (a line filled to the last column runs on into the next)
// are joined, so a match can span the wrap; it is returned as one entry per
// physical line, the later ones flagged `continued`. History lines
// are scanned as text straight from the compact store, so 100k lines take tens
// of milliseconds; column mapping is done only for lines that have a hit.
class TerminalSearch
{
public:
    // All matches in document order (oldest line first, left to right). Empty
    // term = no matches. An invalid regex returns no matches and sets *error.
    static std::vector<FindMatch> findAll(const Terminal &term, const QString &pattern, const FindOptions &opts,
                                          QString *error = nullptr);
    static constexpr int kMaxJoinedLines = 64; // cap for one logical (wrapped) line
};

} // namespace zterminal
