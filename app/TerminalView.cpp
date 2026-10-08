#include "TerminalView.hpp"

#include "Terminal.hpp"

#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyleHints>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace zterminal {

namespace {

VTermModifier vtermMods(Qt::KeyboardModifiers m)
{
    int mod = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier) {
        mod |= VTERM_MOD_SHIFT;
    }
    if (m & Qt::AltModifier) {
        mod |= VTERM_MOD_ALT;
    }
    if (m & Qt::ControlModifier) {
        mod |= VTERM_MOD_CTRL;
    }
    return static_cast<VTermModifier>(mod);
}

VTermKey vtermKey(int key)
{
    switch (key) {
    case Qt::Key_Return:
        return VTERM_KEY_ENTER;
    case Qt::Key_Enter:
        return VTERM_KEY_KP_ENTER;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return VTERM_KEY_TAB;
    case Qt::Key_Backspace:
        return VTERM_KEY_BACKSPACE;
    case Qt::Key_Escape:
        return VTERM_KEY_ESCAPE;
    case Qt::Key_Up:
        return VTERM_KEY_UP;
    case Qt::Key_Down:
        return VTERM_KEY_DOWN;
    case Qt::Key_Left:
        return VTERM_KEY_LEFT;
    case Qt::Key_Right:
        return VTERM_KEY_RIGHT;
    case Qt::Key_Insert:
        return VTERM_KEY_INS;
    case Qt::Key_Delete:
        return VTERM_KEY_DEL;
    case Qt::Key_Home:
        return VTERM_KEY_HOME;
    case Qt::Key_End:
        return VTERM_KEY_END;
    case Qt::Key_PageUp:
        return VTERM_KEY_PAGEUP;
    case Qt::Key_PageDown:
        return VTERM_KEY_PAGEDOWN;
    default:
        break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(key - Qt::Key_F1 + 1));
    }
    return VTERM_KEY_NONE;
}

// Classic control bytes for Ctrl+<key> (Ctrl+A = 0x01 ... Ctrl+[ = ESC, Ctrl+? = DEL).
int controlByte(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return key - Qt::Key_A + 1;
    }
    switch (key) {
    case Qt::Key_Space:
    case Qt::Key_At:
    case Qt::Key_2:
        return 0x00;
    case Qt::Key_BracketLeft:
    case Qt::Key_3:
        return 0x1b;
    case Qt::Key_Backslash:
    case Qt::Key_4:
        return 0x1c;
    case Qt::Key_BracketRight:
    case Qt::Key_5:
        return 0x1d;
    case Qt::Key_AsciiCircum:
    case Qt::Key_6:
        return 0x1e;
    case Qt::Key_Underscore:
    case Qt::Key_Minus:
    case Qt::Key_7:
        return 0x1f;
    case Qt::Key_Question:
    case Qt::Key_8:
        return 0x7f;
    default:
        return -1;
    }
}

int vtermButton(Qt::MouseButton b)
{
    switch (b) {
    case Qt::LeftButton:
        return 1;
    case Qt::MiddleButton:
        return 2;
    case Qt::RightButton:
        return 3;
    default:
        return 0;
    }
}

} // namespace

TerminalView::TerminalView(Terminal *term, QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_term(term)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->setMouseTracking(true);

    QFont f(QStringLiteral("Monospace"));
    f.setStyleHint(QFont::TypeWriter);
    f.setPointSize(11);
    setTerminalFont(f);

    const int flash = QGuiApplication::styleHints()->cursorFlashTime();
    m_blinkTimer.setInterval(flash > 0 ? flash / 2 : 0);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_blinkOn = !m_blinkOn;
        viewport()->update(cursorRect());
    });

    connect(m_term, &Terminal::damaged, this, [this]() {
        updateScrollBar();
        viewport()->update();
        if (m_term->cursorPos() != m_lastCursorPos) {
            m_lastCursorPos = m_term->cursorPos();
            restartBlink();
        }
        updateBlink();
    });
    connect(m_term, &Terminal::scrolledIntoHistory, this, &TerminalView::onScrolledIntoHistory);
    connect(m_term, &Terminal::scrollbackCleared, this, [this]() {
        m_scrollOffset = 0;
        clearFindMatches();
        m_selection.clear();
        updateScrollBar();
        viewport()->update();
    });
}

void TerminalView::setTerminalFont(const QFont &font)
{
    m_font = font;
    m_font.setKerning(false);
    m_font.setStyleHint(QFont::TypeWriter);
    m_fontBold = m_font;
    m_fontBold.setBold(true);
    m_fontItalic = m_font;
    m_fontItalic.setItalic(true);
    m_fontBoldItalic = m_fontBold;
    m_fontBoldItalic.setItalic(true);
    updateMetrics();
    updateGrid();
    viewport()->update();
}

void TerminalView::updateMetrics()
{
    const QFontMetricsF fm(m_font);
    m_cellW = std::max(1, static_cast<int>(std::ceil(fm.horizontalAdvance(QLatin1Char('M')))));
    m_cellH = std::max(1, static_cast<int>(std::ceil(fm.height())));
    m_ascent = static_cast<int>(std::ceil(fm.ascent()));
}

QSize TerminalView::gridForPixels(const QSize &px) const
{
    const int cols = std::max(2, (px.width() - 2 * kMargin) / m_cellW);
    const int rows = std::max(1, (px.height() - 2 * kMargin) / m_cellH);
    return {cols, rows};
}

QSize TerminalView::sizeHint() const
{
    const int sb = verticalScrollBar()->sizeHint().width();
    return {80 * m_cellW + 2 * kMargin + sb, 24 * m_cellH + 2 * kMargin};
}

void TerminalView::updateGrid()
{
    const QSize g = gridForPixels(viewport()->size());
    if (g.width() == m_gridCols && g.height() == m_gridRows && m_term->cols() == g.width()
        && m_term->rows() == g.height()) {
        return;
    }
    m_gridCols = g.width();
    m_gridRows = g.height();
    m_term->resize(m_gridRows, m_gridCols);
    m_selection.clear();
    updateScrollBar();
    emit gridSizeChanged(m_gridRows, m_gridCols);
}

void TerminalView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateGrid();
}

void TerminalView::updateScrollBar()
{
    const int sb = m_term->scrollbackLines();
    m_scrollOffset = std::clamp(m_scrollOffset, 0, sb);
    QScrollBar *bar = verticalScrollBar();
    const QSignalBlocker block(bar);
    bar->setRange(0, sb);
    bar->setPageStep(std::max(1, m_term->rows()));
    bar->setSingleStep(1);
    bar->setValue(sb - m_scrollOffset);
}

void TerminalView::scrollContentsBy(int, int)
{
    m_scrollOffset = m_term->scrollbackLines() - verticalScrollBar()->value();
    viewport()->update();
}

void TerminalView::scrollBy(int lines)
{
    m_scrollOffset = std::clamp(m_scrollOffset + lines, 0, m_term->scrollbackLines());
    updateScrollBar();
    viewport()->update();
}

void TerminalView::scrollToBottom()
{
    if (m_scrollOffset != 0) {
        m_scrollOffset = 0;
        updateScrollBar();
        viewport()->update();
    }
}

void TerminalView::onScrolledIntoHistory(int count, int dropped)
{
    if (dropped > 0) {
        m_selection.linesDropped(dropped);
    }
    if (m_scrollOffset > 0) {
        // Keep the lines the user is reading in place while output continues.
        m_scrollOffset += count - dropped;
    }
    updateScrollBar();
}

void TerminalView::setFindMatches(std::vector<FindMatch> matches, int current)
{
    m_findMatches = std::move(matches);
    m_findCurrent = (current >= 0 && current < int(m_findMatches.size())) ? current : -1;
    viewport()->update();
}

bool TerminalView::isCurrentFindSegment(int i) const
{
    if (m_findCurrent < 0 || i < m_findCurrent) {
        return false;
    }
    // The current match plus its continuation segments (soft-wrapped lines).
    for (int k = m_findCurrent + 1; k <= i; ++k) {
        if (!m_findMatches[std::size_t(k)].continued) {
            return false;
        }
    }
    return true;
}

void TerminalView::setCurrentFindMatch(int current)
{
    current = (current >= 0 && current < int(m_findMatches.size())) ? current : -1;
    if (current != m_findCurrent) {
        m_findCurrent = current;
        viewport()->update();
    }
}

void TerminalView::clearFindMatches()
{
    if (m_findMatches.empty() && m_findCurrent < 0) {
        return;
    }
    m_findMatches.clear();
    m_findCurrent = -1;
    viewport()->update();
}

void TerminalView::scrollToLine(int absLine)
{
    const int sb = m_term->scrollbackLines();
    const int first = firstVisibleLine();
    const int rows = m_term->rows();
    if (absLine >= first && absLine < first + rows) {
        return;
    }
    const int wantFirst = std::clamp(absLine - rows / 2, 0, sb);
    m_scrollOffset = std::clamp(sb - wantFirst, 0, sb);
    updateScrollBar();
    viewport()->update();
}

int TerminalView::firstVisibleLine() const
{
    return m_term->scrollbackLines() - m_scrollOffset;
}

CellPos TerminalView::cellAt(const QPoint &pos, bool roundToBoundary) const
{
    const double fx = static_cast<double>(pos.x() - kMargin) / m_cellW;
    int col = roundToBoundary ? static_cast<int>(std::lround(fx)) : static_cast<int>(std::floor(fx));
    int row = static_cast<int>(std::floor(static_cast<double>(pos.y() - kMargin) / m_cellH));
    col = std::clamp(col, 0, roundToBoundary ? m_term->cols() : m_term->cols() - 1);
    row = std::clamp(row, 0, m_term->rows() - 1);
    return {firstVisibleLine() + row, col};
}

QRect TerminalView::cursorRect() const
{
    const QPoint cur = m_term->cursorPos();
    const int row = m_term->scrollbackLines() + cur.y() - firstVisibleLine();
    if (row < 0 || row >= m_term->rows()) {
        return {};
    }
    // Two cells wide so a wide character under a block cursor repaints whole.
    return {kMargin + cur.x() * m_cellW, kMargin + row * m_cellH, 2 * m_cellW, m_cellH};
}

void TerminalView::updateBlink()
{
    const bool blink = m_focused && m_term->cursorBlink() && m_term->cursorVisible()
        && m_blinkTimer.interval() > 0;
    if (blink == m_blinkTimer.isActive()) {
        return;
    }
    m_blinkOn = true;
    if (blink) {
        m_blinkTimer.start();
    } else {
        m_blinkTimer.stop();
    }
    viewport()->update(cursorRect());
}

void TerminalView::restartBlink()
{
    if (!m_blinkTimer.isActive()) {
        return;
    }
    if (!m_blinkOn) {
        m_blinkOn = true;
        viewport()->update(cursorRect());
    }
    m_blinkTimer.start();
}

void TerminalView::paintEvent(QPaintEvent *e)
{
    QPainter p(viewport());
    const ColorScheme &scheme = m_term->colorScheme();
    const QRect dirty = e->rect();
    p.fillRect(dirty, QColor(scheme.background));

    const int first = firstVisibleLine();
    const int rows = m_term->rows();
    // Only the rows the update touches (a cursor blink repaints one cell).
    const int paintFrom = std::clamp((dirty.top() - kMargin) / m_cellH, 0, rows);
    const int paintTo = std::clamp((dirty.bottom() - kMargin) / m_cellH + 1, paintFrom, rows);
    const int cols = m_term->cols();
    const QPoint cur = m_term->cursorPos();
    const int cursorLine = m_term->scrollbackLines() + cur.y();
    // Blinking: drawn in the "on" half only. The hollow unfocused cursor never blinks.
    const bool cursorShown = m_term->cursorVisible() && (m_blinkOn || !m_focused);

    // Find matches on the visible lines: binary search to the first one.
    auto matchIt = std::lower_bound(m_findMatches.begin(), m_findMatches.end(), first + paintFrom,
                                    [](const FindMatch &m, int line) { return m.line < line; });
    for (int row = paintFrom; row < paintTo; ++row) {
        const int line = first + row;
        const int y = kMargin + row * m_cellH;
        const auto rowBegin = matchIt;
        while (matchIt != m_findMatches.end() && matchIt->line == line) {
            ++matchIt;
        }
        const auto rowEnd = matchIt;
        for (int col = 0; col < cols; ++col) {
            const Cell c = m_term->cell(line, col);
            if (c.width == 0) {
                continue;
            }
            const int x = kMargin + col * m_cellW;
            const int w = m_cellW * std::max(1, c.width);
            const bool selected = m_selection.contains(line, col);
            const bool isCursor = cursorShown && line == cursorLine && col == cur.x()
                && m_focused && m_term->cursorShape() == VTERM_PROP_CURSORSHAPE_BLOCK;
            QRgb fg = c.fg;
            QRgb bg = c.bg;
            bool fillBg = !c.defaultBg;
            for (auto it = rowBegin; it != rowEnd; ++it) {
                if (col >= it->col && col < it->col + it->cols) {
                    const bool current = isCurrentFindSegment(int(it - m_findMatches.begin()));
                    fg = kFindForeground;
                    bg = current ? kFindCurrentBackground : kFindMatchBackground;
                    fillBg = true;
                    if (current) {
                        break;
                    }
                }
            }
            if (selected) {
                fg = scheme.selectionForeground;
                bg = scheme.selectionBackground;
                fillBg = true;
            }
            if (isCursor) {
                fg = scheme.background;
                bg = scheme.cursor;
                fillBg = true;
            }
            if (fillBg) {
                p.fillRect(x, y, w, m_cellH, QColor(bg));
            }
            if (!c.text.isEmpty() && !c.conceal && c.text != QLatin1String(" ")) {
                p.setFont(c.bold ? (c.italic ? m_fontBoldItalic : m_fontBold)
                                 : (c.italic ? m_fontItalic : m_font));
                p.setPen(QColor(fg));
                p.drawText(x, y + m_ascent, c.text);
            }
            if (c.underline) {
                p.setPen(QColor(fg));
                p.drawLine(x, y + m_ascent + 2, x + w - 1, y + m_ascent + 2);
            }
            if (c.strike) {
                p.setPen(QColor(fg));
                p.drawLine(x, y + m_cellH / 2, x + w - 1, y + m_cellH / 2);
            }
        }
    }

    // Non-block cursor shapes, and the hollow cursor when unfocused.
    if (cursorShown && cursorLine >= first && cursorLine < first + rows) {
        const int x = kMargin + cur.x() * m_cellW;
        const int y = kMargin + (cursorLine - first) * m_cellH;
        p.setPen(QColor(scheme.cursor));
        if (!m_focused) {
            p.setBrush(Qt::NoBrush);
            p.drawRect(x, y, m_cellW - 1, m_cellH - 1);
        } else if (m_term->cursorShape() == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
            p.fillRect(x, y + m_cellH - 2, m_cellW, 2, QColor(scheme.cursor));
        } else if (m_term->cursorShape() == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
            p.fillRect(x, y, 2, m_cellH, QColor(scheme.cursor));
        }
    }
}

bool TerminalView::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(e);
        const int combined = ke->keyCombination().toCombined();
        if (!m_appShortcuts.contains(combined)) {
            ke->accept(); // the key belongs to the session, not to a menu shortcut
            return true;
        }
    }
    return QAbstractScrollArea::event(e);
}

bool TerminalView::focusNextPrevChild(bool)
{
    return false; // Tab and Shift+Tab go to the session
}

void TerminalView::focusInEvent(QFocusEvent *e)
{
    m_focused = true;
    m_term->sendFocus(true);
    updateBlink();
    viewport()->update();
    QAbstractScrollArea::focusInEvent(e);
}

void TerminalView::focusOutEvent(QFocusEvent *e)
{
    m_focused = false;
    m_term->sendFocus(false);
    updateBlink();
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(e);
}

void TerminalView::keyPressEvent(QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers();
    const int key = e->key();

    // Local scrollback navigation (never sent to the program).
    if (mods == Qt::ShiftModifier) {
        switch (key) {
        case Qt::Key_PageUp:
            scrollBy(std::max(1, m_term->rows() - 1));
            return;
        case Qt::Key_PageDown:
            scrollBy(-std::max(1, m_term->rows() - 1));
            return;
        case Qt::Key_Home:
            scrollBy(m_term->scrollbackLines());
            return;
        case Qt::Key_End:
            scrollToBottom();
            return;
        default:
            break;
        }
    }
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt
        || key == Qt::Key_Meta || key == Qt::Key_AltGr || key == Qt::Key_CapsLock) {
        return;
    }

    scrollToBottom();
    restartBlink(); // keep the cursor solid while typing
    const VTermModifier vmod = vtermMods(mods & ~Qt::KeypadModifier);

    const VTermKey vk = vtermKey(key);
    if (vk != VTERM_KEY_NONE) {
        m_term->sendKey(vk, key == Qt::Key_Backtab
                                ? static_cast<VTermModifier>(vmod | VTERM_MOD_SHIFT)
                                : vmod);
        return;
    }

    // Requires AA_MacDontSwapCtrlAndMeta (main.cpp) so ControlModifier is ⌃
    // on macOS; otherwise physical Ctrl never reaches this branch.
    if (mods & Qt::ControlModifier) {
        const int b = controlByte(key);
        if (b >= 0) {
            QByteArray bytes;
            if (mods & Qt::AltModifier) {
                bytes.append('\x1b');
            }
            bytes.append(static_cast<char>(b));
            m_term->sendBytes(bytes);
            return;
        }
    }

    const QString text = e->text();
    if (!text.isEmpty()) {
        const VTermModifier charMod = (mods & Qt::AltModifier) ? VTERM_MOD_ALT : VTERM_MOD_NONE;
        for (const char32_t cp : text.toUcs4()) {
            if (cp < 0x20 || cp == 0x7f) {
                m_term->sendBytes(QByteArray(1, static_cast<char>(cp)));
            } else {
                m_term->sendChar(cp, charMod);
            }
        }
    }
}

bool TerminalView::forwardsMouse(const QMouseEvent *e) const
{
    return m_term->mouseMode() != VTERM_PROP_MOUSE_NONE && !(e->modifiers() & Qt::ShiftModifier);
}

void TerminalView::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();

    // Ctrl+right-click always opens our menu and is never sent to the program.
    if (e->button() == Qt::RightButton && (e->modifiers() & Qt::ControlModifier)) {
        emit contextMenuRequested(e->globalPosition().toPoint());
        return;
    }

    if (forwardsMouse(e)) {
        const int b = vtermButton(e->button());
        if (b) {
            const CellPos c = cellAt(pos, false);
            const VTermModifier mod = vtermMods(e->modifiers());
            m_term->sendMouseMove(c.line - firstVisibleLine(), c.col, mod);
            m_term->sendMouseButton(b, true, mod);
            m_forwardedButtons |= e->button();
        }
        return;
    }

    switch (e->button()) {
    case Qt::LeftButton: {
        const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
        const int dist = QGuiApplication::styleHints()->startDragDistance();
        if (m_lastClick.isValid() && m_lastClick.elapsed() < interval
            && (pos - m_lastClickPos).manhattanLength() <= dist) {
            m_clickCount = m_clickCount >= 3 ? 1 : m_clickCount + 1;
        } else {
            m_clickCount = 1;
        }
        m_lastClick.restart();
        m_lastClickPos = pos;
        const Selection::Mode mode = m_clickCount == 3   ? Selection::Mode::Line
                                     : m_clickCount == 2 ? Selection::Mode::Word
                                                         : Selection::Mode::Char;
        m_selection.begin(cellAt(pos, mode == Selection::Mode::Char), mode, *m_term,
                          m_mouse.wordDelimiters);
        m_selecting = true;
        viewport()->update();
        break;
    }
    case Qt::RightButton:
        pasteClipboard();
        break;
    case Qt::MiddleButton:
        switch (m_mouse.middleClick) {
        case MiddleClickAction::PastePrimary:
            pastePrimary();
            break;
        case MiddleClickAction::PasteClipboard:
            pasteClipboard();
            break;
        case MiddleClickAction::Off:
            break;
        }
        break;
    default:
        break;
    }
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Click counting is done in mousePressEvent (so triple-click works too).
    mousePressEvent(e);
}

void TerminalView::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (m_forwardedButtons || (forwardsMouse(e) && m_term->mouseMode() == VTERM_PROP_MOUSE_MOVE)) {
        const CellPos c = cellAt(pos, false);
        m_term->sendMouseMove(c.line - firstVisibleLine(), c.col, vtermMods(e->modifiers()));
        return;
    }
    if (m_selecting && (e->buttons() & Qt::LeftButton)) {
        if (pos.y() < 0) {
            scrollBy(1);
        } else if (pos.y() > viewport()->height()) {
            scrollBy(-1);
        }
        m_selection.extend(cellAt(pos, m_selection.mode() == Selection::Mode::Char), *m_term,
                           m_mouse.wordDelimiters);
        viewport()->update();
    }
}

void TerminalView::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_forwardedButtons & e->button()) {
        m_forwardedButtons &= ~e->button();
        const CellPos c = cellAt(e->position().toPoint(), false);
        const VTermModifier mod = vtermMods(e->modifiers());
        m_term->sendMouseMove(c.line - firstVisibleLine(), c.col, mod);
        m_term->sendMouseButton(vtermButton(e->button()), false, mod);
        return;
    }
    if (e->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        if (m_selection.isEmpty()) {
            m_selection.clear();
        } else {
            copyOnSelect(); // select = copy
        }
        viewport()->update();
    }
}

void TerminalView::wheelEvent(QWheelEvent *e)
{
    const int steps = e->angleDelta().y() / 120;
    if (steps == 0) {
        return;
    }
    if (m_term->mouseMode() != VTERM_PROP_MOUSE_NONE && !(e->modifiers() & Qt::ShiftModifier)) {
        const CellPos c = cellAt(e->position().toPoint(), false);
        const VTermModifier mod = vtermMods(e->modifiers());
        m_term->sendMouseMove(c.line - firstVisibleLine(), c.col, mod);
        for (int i = 0; i < std::abs(steps); ++i) {
            m_term->sendMouseButton(steps > 0 ? 4 : 5, true, mod);
        }
        return;
    }
    if (m_term->altScreen()) {
        // Full-screen programs without mouse support (less, man): wheel = arrow keys.
        for (int i = 0; i < std::abs(steps) * 3; ++i) {
            m_term->sendKey(steps > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
        }
        return;
    }
    scrollBy(steps * 3);
}

QString TerminalView::selectedText() const
{
    return m_selection.text(*m_term, m_trimCopy);
}

void TerminalView::copyToClipboards(bool toClipboard)
{
    const QString text = selectedText();
    if (text.isEmpty()) {
        return;
    }
    m_lastSelectionText = text;
    // QClipboard::setText offers text/plain only: no HTML or colours.
    QClipboard *cb = QGuiApplication::clipboard();
    if (cb->supportsSelection()) {
        cb->setText(text, QClipboard::Selection);
    }
    if (toClipboard) {
        cb->setText(text, QClipboard::Clipboard);
    }
}

void TerminalView::copySelection()
{
    copyToClipboards(true); // Edit > Copy always fills CLIPBOARD
}

void TerminalView::copyOnSelect()
{
    copyToClipboards(m_mouse.copyOnSelectToClipboard);
}

void TerminalView::pasteText(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    if (m_pasteGuard && !m_pasteGuard(text)) {
        return;
    }
    scrollToBottom();
    m_term->paste(text);
}

void TerminalView::pasteClipboard()
{
    pasteText(QGuiApplication::clipboard()->text(QClipboard::Clipboard));
}

void TerminalView::pastePrimary()
{
    QClipboard *cb = QGuiApplication::clipboard();
    pasteText(cb->supportsSelection() ? cb->text(QClipboard::Selection) : m_lastSelectionText);
}

void TerminalView::selectAll()
{
    m_selection.selectAll(*m_term);
    copyOnSelect();
    viewport()->update();
}

void TerminalView::clearSelection()
{
    m_selection.clear();
    viewport()->update();
}

} // namespace zterminal
