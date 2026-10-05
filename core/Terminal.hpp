#pragma once

#include "ColorScheme.hpp"
#include "Scrollback.hpp"

#include <QByteArray>
#include <QObject>
#include <QPoint>
#include <QRgb>
#include <QString>

#include <deque>
#include <vector>

extern "C" {
#include <vterm.h>
}

namespace zterminal {

// One rendered cell, colours already resolved through the active scheme and
// reverse video applied.
struct Cell {
    QString text;       // empty for a blank cell
    int width = 1;      // 1, 2 (wide char), or 0 (right half of a wide char)
    QRgb fg = 0;
    QRgb bg = 0;
    bool defaultBg = true; // background is the scheme default (no fill needed)
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool conceal = false;
};

// libvterm-backed emulation core plus a scrollback ring.
//
// Line coordinates are "absolute": 0 .. scrollbackLines()-1 are history
// (oldest first), scrollbackLines() .. totalLines()-1 are the live screen.
// A line keeps its absolute index while new output scrolls it into history,
// except when the scrollback limit discards old lines (linesDropped()).
class Terminal : public QObject
{
    Q_OBJECT
public:
    explicit Terminal(int rows = 24, int cols = 80, QObject *parent = nullptr);
    ~Terminal() override;
    Terminal(const Terminal &) = delete;
    Terminal &operator=(const Terminal &) = delete;

    void feed(const QByteArray &bytes);
    void resize(int rows, int cols);
    int rows() const { return m_rows; }
    int cols() const { return m_cols; }

    // History lines kept; 0 = none, kUnlimitedScrollback (< 0) = no limit.
    static constexpr int kUnlimitedScrollback = -1;
    void setScrollbackLimit(int lines);
    int scrollbackLimit() const { return m_scrollbackLimit; }
    int scrollbackLines() const { return m_history.size(); }
    // Compact/compressed history storage (Scrollback) in bytes.
    qsizetype scrollbackMemoryBytes() const { return m_history.memoryBytes(); }
    const Scrollback &history() const { return m_history; }
    int totalLines() const { return scrollbackLines() + m_rows; }
    void clearScrollback();
    void reset(); // RIS-like hard reset of the screen state (scrollback kept)

    Cell cell(int absLine, int col) const;
    // Text of [startCol, endCol) of a line; endCol < 0 means to the end of the line
    // with trailing blanks trimmed. With keepPrintedSpaces, only cells nothing was
    // ever written to (or that were erased) are trimmed; spaces the program
    // actually printed stay.
    QString lineText(int absLine, int startCol = 0, int endCol = -1, bool keepPrintedSpaces = false) const;
    // Text of a whole line for Find: one character per cell (blank = space),
    // never-written trailing cells dropped; colOfChar gets each UTF-16 unit's
    // cell column. Fast for history lines (no cell decoding).
    QString searchText(int absLine, std::vector<int> *colOfChar = nullptr) const;
    // Whether the program turned on bracketed paste (DECSET 2004). libvterm keeps
    // the mode private, so this asks it to start a paste and sees whether it
    // emits ESC[200~ (captured, never sent).
    bool bracketedPasteEnabled() const;

    QPoint cursorPos() const; // x = col, y = screen row
    bool cursorVisible() const { return m_cursorVisible; }
    // VTERM_PROP_CURSORSHAPE_BLOCK/UNDERLINE/BAR_LEFT and blink, as currently in
    // effect: the default style below unless the program changed it (DECSCUSR,
    // DECSET 12).
    int cursorShape() const { return m_cursorShape; }
    bool cursorBlink() const { return m_cursorBlink; }
    // The user's cursor style: used at start-up, after a reset (RIS / reset())
    // and whenever the program asks for "the default" (DECSCUSR 0, ESC[ q),
    // which libvterm would otherwise turn into a blinking block.
    static constexpr int kDefaultCursorShape = VTERM_PROP_CURSORSHAPE_BAR_LEFT;
    static constexpr bool kDefaultCursorBlink = true;
    void setDefaultCursorStyle(int shape, bool blink);
    int defaultCursorShape() const { return m_defaultCursorShape; }
    bool defaultCursorBlink() const { return m_defaultCursorBlink; }
    bool altScreen() const { return m_altScreen; }
    // VTERM_PROP_MOUSE_NONE/CLICK/DRAG/MOVE: whether the program grabbed the mouse.
    int mouseMode() const { return m_mouseMode; }
    QString title() const { return m_title; }

    void setColorScheme(const ColorScheme &scheme);
    const ColorScheme &colorScheme() const { return m_scheme; }

    // Input towards the program; resulting bytes are emitted via output().
    void sendKey(VTermKey key, VTermModifier mod);
    void sendChar(uint32_t codepoint, VTermModifier mod);
    void sendMouseMove(int row, int col, VTermModifier mod);
    void sendMouseButton(int button, bool pressed, VTermModifier mod);
    // Paste: newline -> CR, wrapped in bracketed-paste markers when the program
    // enabled mode 2004. Embedded end-of-paste markers are stripped.
    void paste(const QString &text);
    // Raw bytes to the program (control characters typed by the user).
    void sendBytes(const QByteArray &bytes) { emit output(bytes); }
    static QByteArray preparePasteBytes(const QString &text);

signals:
    void output(const QByteArray &bytes); // write to the PTY
    void damaged();                      // repaint needed
    void titleChanged(const QString &title);
    void bell();
    // `count` lines moved from the screen into history; `dropped` old lines discarded.
    void scrolledIntoHistory(int count, int dropped);
    void scrollbackCleared();

private:
    static int cbDamage(VTermRect rect, void *user);
    static int cbMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
    static int cbSetTermProp(VTermProp prop, VTermValue *val, void *user);
    static int cbBell(void *user);
    static int cbPushLine(int cols, const VTermScreenCell *cells, void *user);
    static int cbPopLine(int cols, VTermScreenCell *cells, void *user);
    static int cbSbClear(void *user);
    static void cbOutput(const char *s, size_t len, void *user);

    Cell convert(const VTermScreenCell &c) const;
    QRgb resolve(const VTermColor &c, bool isFg) const;
    void applySchemeToVterm();
    void applyDefaultCursorStyle();
    bool scanForCursorReset(char c);
    void flush();

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    VTermState *m_state = nullptr;
    int m_rows;
    int m_cols;
    int m_scrollbackLimit = 10000;
    Scrollback m_history;
    quint64 m_historyBase = 0; // lines ever dropped from the front (cache keys)
    // Decoded history lines for cell() (the renderer asks cell by cell).
    mutable std::vector<std::pair<quint64, std::vector<VTermScreenCell>>> m_lineCache;
    const VTermScreenCell *historyCell(int absLine, int col) const;
    void dropHistoryFront(int n);
    void trimHistory();
    ColorScheme m_scheme;
    VTermPos m_cursor{0, 0};
    bool m_cursorVisible = true;
    int m_cursorShape = kDefaultCursorShape;
    bool m_cursorBlink = kDefaultCursorBlink;
    int m_defaultCursorShape = kDefaultCursorShape;
    bool m_defaultCursorBlink = kDefaultCursorBlink;
    // feed() watches for DECSCUSR 0 (ESC [ 0* SP q) and RIS (ESC c), across chunks.
    enum class ResetScan : quint8 { Ground, Esc, Csi, CsiSpace };
    ResetScan m_resetScan = ResetScan::Ground;
    bool m_altScreen = false;
    mutable bool m_probing = false;
    mutable QByteArray m_probeOut;
    int m_mouseMode = VTERM_PROP_MOUSE_NONE;
    QString m_title;
    QByteArray m_titleBuf;
    bool m_dirty = false;
    int m_pushed = 0;
    int m_dropped = 0;
};

} // namespace zterminal
