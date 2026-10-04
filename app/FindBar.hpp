#pragma once

#include "TerminalSearch.hpp"

#include <optional>
#include <vector>

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QTimer;

namespace zterminal {

class Terminal;
class TerminalView;

// Per-tab Find bar (docs/PLAN.md §4.17), shown under the terminal view:
//   [Find: ______] [Aa case] [.* regex] [wrap]  3 of 120  [▲] [▼] [×]
// Searches the tab's whole scrollback plus the screen. Matches are numbered
// from the newest (1 = closest to the prompt); Enter / Next goes to the next
// older match, Shift+Enter / Previous back toward newer ones, Esc closes.
// Typing re-searches after a short pause; new output re-searches once the
// output pauses, keeping the current match.
class FindBar : public QWidget
{
    Q_OBJECT
public:
    FindBar(Terminal *term, TerminalView *view, QWidget *parent = nullptr);

    void open();   // show, focus the text, select it, search
    void close();  // hide, drop highlights
    void findNext();     // older
    void findPrevious(); // newer
    void search();       // run now (normally debounced)

    QString text() const;
    void setText(const QString &t);
    FindOptions options() const;
    bool wrap() const;
    // Matches (a match across a soft wrap counts once).
    int matchCount() const { return int(m_heads.size()); }
    // Highlight segments, one per physical line (see FindMatch::continued).
    const std::vector<FindMatch> &matches() const { return m_matches; }
    int currentIndex() const { return m_current < 0 ? -1 : m_heads[std::size_t(m_current)]; } // into matches()
    // 1-based number shown in the count label (1 = newest), 0 = none.
    int currentNumber() const { return m_current < 0 ? 0 : int(m_heads.size()) - m_current; }
    QString countText() const;
    QString errorText() const { return m_error; }
    qint64 lastSearchMs() const { return m_lastSearchMs; }

signals:
    void closed();

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void runSearch(bool keepCurrent);
    void setCurrent(int idx, bool scroll);
    void updateCount(const QString &note = {});
    void onOutput();
    void onLinesDropped(int dropped);
    void rebuildHeads();

    Terminal *m_term;
    TerminalView *m_view;
    QLineEdit *m_text = nullptr;
    QCheckBox *m_case = nullptr;
    QCheckBox *m_regex = nullptr;
    QCheckBox *m_wrap = nullptr;
    QLabel *m_count = nullptr;
    QToolButton *m_prev = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_close = nullptr;
    QTimer *m_typing = nullptr; // debounce while typing
    QTimer *m_idle = nullptr;   // re-search after output pauses
    std::vector<FindMatch> m_matches;
    std::vector<int> m_heads; // indices of the first segment of each match
    int m_current = -1;       // into m_heads
    bool m_stale = false;
    std::optional<FindMatch> m_anchor; // where to resume after the current match was dropped
    QString m_error;
    qint64 m_lastSearchMs = 0;
};

} // namespace zterminal
