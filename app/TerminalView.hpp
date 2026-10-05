#pragma once

#include <functional>

#include "MouseSettings.hpp"
#include "Selection.hpp"
#include "TerminalSearch.hpp"

#include <vector>

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QFont>
#include <QPoint>
#include <QSet>
#include <QTimer>

namespace zterminal {

class Terminal;

// Renders a Terminal (QPainter on a cell grid) and turns keyboard/mouse input
// into terminal input, selection and clipboard actions (docs/PLAN.md §4.6):
//   - left-drag selects; release copies to PRIMARY (when supported) and CLIPBOARD
//   - double-click selects a word, triple-click a line
//   - right-click pastes CLIPBOARD; Ctrl+right-click requests the context menu
//   - middle-click: paste PRIMARY (default) / paste CLIPBOARD / off
//   - when the program grabbed the mouse, events go to it unless Shift is held
class TerminalView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit TerminalView(Terminal *term, QWidget *parent = nullptr);

    void setTerminalFont(const QFont &font);
    QFont terminalFont() const { return m_font; }
    void setMouseSettings(const MouseSettings &s) { m_mouse = s; }
    const MouseSettings &mouseSettings() const { return m_mouse; }
    // Copy: trim trailing whitespace on every copied line (Preferences, default on).
    void setTrimCopiedWhitespace(bool on) { m_trimCopy = on; }
    // Called with the clipboard text before every paste; returning false drops it
    // (MainWindow's multi-line paste confirmation).
    void setPasteGuard(std::function<bool(const QString &)> guard) { m_pasteGuard = std::move(guard); }
    // Key combinations reserved for app shortcuts; all other keys go to the session.
    void setAppShortcuts(const QSet<int> &combined) { m_appShortcuts = combined; }

    QSize cellSize() const { return {m_cellW, m_cellH}; }
    int gridRows() const { return m_gridRows; }
    int gridCols() const { return m_gridCols; }
    // Terminal size that fits a viewport of the given pixel size.
    QSize gridForPixels(const QSize &px) const;
    QSize sizeHint() const override;

    const Selection &selection() const { return m_selection; }
    QString selectedText() const;
    int scrollOffset() const { return m_scrollOffset; } // lines above the live screen
    int firstVisibleLine() const;
    // Find highlights (docs/PLAN.md §4.17): every match painted in the match
    // colour, `current` (index into matches, -1 = none) in the current-match colour.
    void setFindMatches(std::vector<FindMatch> matches, int current);
    void clearFindMatches();
    void setCurrentFindMatch(int current);
    const std::vector<FindMatch> &findMatches() const { return m_findMatches; }
    int currentFindMatch() const { return m_findCurrent; }
    // Scroll so the absolute line is visible (centred when it was off-screen).
    void scrollToLine(int absLine);
    static constexpr QRgb kFindMatchBackground = 0xfff0c040;
    static constexpr QRgb kFindCurrentBackground = 0xffff7a00;
    static constexpr QRgb kFindForeground = 0xff000000;
    // Cursor blink (Terminal::cursorBlink(), default on): runs only while the
    // view has focus; the cursor stays solid for a full phase after typing or
    // moving. Half-period from the platform's cursor flash time (0 = no blink).
    bool cursorBlinking() const { return m_blinkTimer.isActive(); }
    bool cursorBlinkPhaseOn() const { return m_blinkOn; }
    int cursorBlinkInterval() const { return m_blinkTimer.interval(); }

public slots:
    void copySelection();   // Edit > Copy: PRIMARY (if supported) + CLIPBOARD
    void pasteClipboard();
    void pastePrimary();
    void selectAll();
    void clearSelection();
    void scrollToBottom();

signals:
    void gridSizeChanged(int rows, int cols);
    void contextMenuRequested(const QPoint &globalPos);

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;
    bool focusNextPrevChild(bool next) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void updateMetrics();
    void updateGrid();
    void updateScrollBar();
    void onScrolledIntoHistory(int count, int dropped);
    CellPos cellAt(const QPoint &pos, bool roundToBoundary) const;
    bool forwardsMouse(const QMouseEvent *e) const;
    void pasteText(const QString &text);
    void copyToClipboards(bool toClipboard);
    void copyOnSelect(); // after a mouse selection: PRIMARY, + CLIPBOARD if configured
    void scrollBy(int lines);
    bool isCurrentFindSegment(int i) const;
    QRect cursorRect() const; // viewport pixels of the cursor cell (empty if off-screen)
    void updateBlink();       // start/stop the blink timer for focus and terminal state
    void restartBlink();      // cursor solid now, next toggle a full phase later

    Terminal *m_term;
    QFont m_font;
    QFont m_fontBold;
    QFont m_fontItalic;
    QFont m_fontBoldItalic;
    int m_cellW = 8;
    int m_cellH = 16;
    int m_ascent = 12;
    int m_gridRows = 24;
    int m_gridCols = 80;
    int m_scrollOffset = 0;
    static constexpr int kMargin = 2;

    MouseSettings m_mouse;
    Selection m_selection;
    bool m_selecting = false;
    int m_clickCount = 0;
    QElapsedTimer m_lastClick;
    QPoint m_lastClickPos;
    bool m_trimCopy = true;
    std::function<bool(const QString &)> m_pasteGuard;
    QString m_lastSelectionText; // PRIMARY fallback where the platform has none
    Qt::MouseButtons m_forwardedButtons;
    QSet<int> m_appShortcuts;
    bool m_focused = false;
    std::vector<FindMatch> m_findMatches; // sorted by (line, col)
    int m_findCurrent = -1;
    QTimer m_blinkTimer;
    bool m_blinkOn = true;
    QPoint m_lastCursorPos{-1, -1};
};

} // namespace zterminal
