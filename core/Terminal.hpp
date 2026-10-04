#pragma once

#include "ColorScheme.hpp"

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

    void setScrollbackLimit(int lines);
    int scrollbackLimit() const { return m_scrollbackLimit; }
    int scrollbackLines() const { return static_cast<int>(m_scrollback.size()); }
    int totalLines() const { return scrollbackLines() + m_rows; }
    void clearScrollback();
    void reset(); // RIS-like hard reset of the screen state (scrollback kept)

    Cell cell(int absLine, int col) const;
    // Text of [startCol, endCol) of a line; endCol < 0 means to the end of the line
    // with trailing blanks trimmed.
    QString lineText(int absLine, int startCol = 0, int endCol = -1) const;

    QPoint cursorPos() const; // x = col, y = screen row
    bool cursorVisible() const { return m_cursorVisible; }
    int cursorShape() const { return m_cursorShape; }
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
    void flush();

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    VTermState *m_state = nullptr;
    int m_rows;
    int m_cols;
    int m_scrollbackLimit = 10000;
    std::deque<std::vector<VTermScreenCell>> m_scrollback;
    ColorScheme m_scheme;
    VTermPos m_cursor{0, 0};
    bool m_cursorVisible = true;
    int m_cursorShape = VTERM_PROP_CURSORSHAPE_BLOCK;
    bool m_altScreen = false;
    int m_mouseMode = VTERM_PROP_MOUSE_NONE;
    QString m_title;
    QByteArray m_titleBuf;
    bool m_dirty = false;
    int m_pushed = 0;
    int m_dropped = 0;
};

} // namespace zterminal
