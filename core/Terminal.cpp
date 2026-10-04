#include "Terminal.hpp"

#include <algorithm>
#include <cstring>

namespace zterminal {

namespace {
constexpr uint32_t kWideGap = static_cast<uint32_t>(-1);

VTermColor rgbColor(QRgb c)
{
    VTermColor v;
    vterm_color_rgb(&v, static_cast<uint8_t>(qRed(c)), static_cast<uint8_t>(qGreen(c)),
                    static_cast<uint8_t>(qBlue(c)));
    return v;
}
} // namespace

Terminal::Terminal(int rows, int cols, QObject *parent)
    : QObject(parent)
    , m_rows(std::max(rows, 1))
    , m_cols(std::max(cols, 1))
    , m_scheme(ColorScheme::byId(QStringLiteral("xterm")))
{
    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, &Terminal::cbOutput, this);

    m_state = vterm_obtain_state(m_vt);
    vterm_state_set_bold_highbright(m_state, 1);

    m_screen = vterm_obtain_screen(m_vt);
    static const VTermScreenCallbacks callbacks = {
        &Terminal::cbDamage,      nullptr /* moverect: full damage is fine */,
        &Terminal::cbMoveCursor,  &Terminal::cbSetTermProp,
        &Terminal::cbBell,        nullptr /* resize */,
        &Terminal::cbPushLine,    &Terminal::cbPopLine,
        &Terminal::cbSbClear,
    };
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    applySchemeToVterm();
    vterm_screen_reset(m_screen, 1);
}

Terminal::~Terminal()
{
    vterm_free(m_vt);
}

void Terminal::applySchemeToVterm()
{
    const VTermColor fg = rgbColor(m_scheme.foreground);
    const VTermColor bg = rgbColor(m_scheme.background);
    vterm_state_set_default_colors(m_state, &fg, &bg);
    for (int i = 0; i < 16; ++i) {
        const VTermColor c = rgbColor(m_scheme.ansi[static_cast<size_t>(i)]);
        vterm_state_set_palette_color(m_state, i, &c);
    }
}

void Terminal::setColorScheme(const ColorScheme &scheme)
{
    m_scheme = scheme;
    applySchemeToVterm();
    emit damaged();
}

void Terminal::feed(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    vterm_input_write(m_vt, bytes.constData(), static_cast<size_t>(bytes.size()));
    flush();
}

void Terminal::flush()
{
    vterm_screen_flush_damage(m_screen);
    if (m_pushed || m_dropped) {
        const int p = m_pushed;
        const int d = m_dropped;
        m_pushed = 0;
        m_dropped = 0;
        emit scrolledIntoHistory(p, d);
    }
    if (m_dirty) {
        m_dirty = false;
        emit damaged();
    }
}

void Terminal::resize(int rows, int cols)
{
    rows = std::max(rows, 1);
    cols = std::max(cols, 1);
    if (rows == m_rows && cols == m_cols) {
        return;
    }
    m_rows = rows;
    m_cols = cols;
    vterm_set_size(m_vt, rows, cols);
    m_dirty = true;
    flush();
}

void Terminal::setScrollbackLimit(int lines)
{
    m_scrollbackLimit = std::max(lines, 0);
    int dropped = 0;
    while (static_cast<int>(m_scrollback.size()) > m_scrollbackLimit) {
        m_scrollback.pop_front();
        ++dropped;
    }
    if (dropped) {
        emit scrolledIntoHistory(0, dropped);
        emit damaged();
    }
}

void Terminal::clearScrollback()
{
    m_scrollback.clear();
    emit scrollbackCleared();
    emit damaged();
}

void Terminal::reset()
{
    vterm_screen_reset(m_screen, 1);
    m_title.clear();
    m_mouseMode = VTERM_PROP_MOUSE_NONE;
    m_altScreen = false;
    m_cursorVisible = true;
    emit titleChanged(m_title);
    m_dirty = true;
    flush();
}

QRgb Terminal::resolve(const VTermColor &c, bool isFg) const
{
    if (isFg && VTERM_COLOR_IS_DEFAULT_FG(&c)) {
        return m_scheme.foreground;
    }
    if (!isFg && VTERM_COLOR_IS_DEFAULT_BG(&c)) {
        return m_scheme.background;
    }
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        return xterm256(c.indexed.idx, m_scheme.ansi);
    }
    return qRgb(c.rgb.red, c.rgb.green, c.rgb.blue);
}

Cell Terminal::convert(const VTermScreenCell &c) const
{
    Cell out;
    if (c.chars[0] == kWideGap) {
        out.width = 0;
    } else {
        out.width = c.width > 0 ? c.width : 1;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; ++i) {
            const char32_t cp = c.chars[i];
            out.text += QString::fromUcs4(&cp, 1);
        }
    }
    out.fg = resolve(c.fg, true);
    out.bg = resolve(c.bg, false);
    out.defaultBg = VTERM_COLOR_IS_DEFAULT_BG(&c.bg);
    out.bold = c.attrs.bold;
    out.italic = c.attrs.italic;
    out.underline = c.attrs.underline != VTERM_UNDERLINE_OFF;
    out.strike = c.attrs.strike;
    out.conceal = c.attrs.conceal;
    if (c.attrs.reverse) {
        std::swap(out.fg, out.bg);
        out.defaultBg = false;
    }
    return out;
}

Cell Terminal::cell(int absLine, int col) const
{
    const int sb = scrollbackLines();
    if (absLine < 0 || absLine >= totalLines() || col < 0 || col >= m_cols) {
        Cell blank;
        blank.fg = m_scheme.foreground;
        blank.bg = m_scheme.background;
        return blank;
    }
    if (absLine < sb) {
        const auto &line = m_scrollback[static_cast<size_t>(absLine)];
        if (col < static_cast<int>(line.size())) {
            return convert(line[static_cast<size_t>(col)]);
        }
        Cell blank;
        blank.fg = m_scheme.foreground;
        blank.bg = m_scheme.background;
        return blank;
    }
    VTermScreenCell c;
    std::memset(&c, 0, sizeof c);
    vterm_screen_get_cell(m_screen, VTermPos{absLine - sb, col}, &c);
    return convert(c);
}

QString Terminal::lineText(int absLine, int startCol, int endCol, bool keepPrintedSpaces) const
{
    const bool toEnd = endCol < 0;
    if (toEnd || endCol > m_cols) {
        endCol = m_cols;
    }
    QString s;
    qsizetype keep = 0; // length up to the last printed cell
    for (int col = std::max(startCol, 0); col < endCol; ++col) {
        const Cell c = cell(absLine, col);
        if (c.width == 0) {
            continue;
        }
        s += c.text.isEmpty() ? QStringLiteral(" ") : c.text;
        if (!c.text.isEmpty()) {
            keep = s.size();
        }
    }
    if (toEnd) {
        if (keepPrintedSpaces) {
            s.truncate(keep);
        } else {
            while (s.endsWith(QLatin1Char(' '))) {
                s.chop(1);
            }
        }
    }
    return s;
}

QPoint Terminal::cursorPos() const
{
    return {m_cursor.col, m_cursor.row};
}

void Terminal::sendKey(VTermKey key, VTermModifier mod)
{
    vterm_keyboard_key(m_vt, key, mod);
}

void Terminal::sendChar(uint32_t codepoint, VTermModifier mod)
{
    vterm_keyboard_unichar(m_vt, codepoint, mod);
}

void Terminal::sendMouseMove(int row, int col, VTermModifier mod)
{
    vterm_mouse_move(m_vt, row, col, mod);
}

void Terminal::sendMouseButton(int button, bool pressed, VTermModifier mod)
{
    vterm_mouse_button(m_vt, button, pressed, mod);
}

QByteArray Terminal::preparePasteBytes(const QString &text)
{
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    t.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    // Never let pasted text end a bracketed paste early.
    t.remove(QStringLiteral("\x1b[201~"));
    return t.toUtf8();
}

bool Terminal::bracketedPasteEnabled() const
{
    m_probing = true;
    m_probeOut.clear();
    // Only emits (CSI 200~); keyboard.c changes no state here.
    vterm_keyboard_start_paste(m_vt);
    m_probing = false;
    const bool on = !m_probeOut.isEmpty();
    m_probeOut.clear();
    return on;
}

void Terminal::paste(const QString &text)
{
    const QByteArray bytes = preparePasteBytes(text);
    if (bytes.isEmpty()) {
        return;
    }
    vterm_keyboard_start_paste(m_vt); // emits ESC[200~ only if mode 2004 is on
    emit output(bytes);
    vterm_keyboard_end_paste(m_vt);
}

// ---- libvterm callbacks ----

int Terminal::cbDamage(VTermRect, void *user)
{
    static_cast<Terminal *>(user)->m_dirty = true;
    return 1;
}

int Terminal::cbMoveCursor(VTermPos pos, VTermPos, int visible, void *user)
{
    auto *t = static_cast<Terminal *>(user);
    t->m_cursor = pos;
    t->m_cursorVisible = visible;
    t->m_dirty = true;
    return 1;
}

int Terminal::cbSetTermProp(VTermProp prop, VTermValue *val, void *user)
{
    auto *t = static_cast<Terminal *>(user);
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        t->m_cursorVisible = val->boolean;
        break;
    case VTERM_PROP_CURSORSHAPE:
        t->m_cursorShape = val->number;
        break;
    case VTERM_PROP_ALTSCREEN:
        t->m_altScreen = val->boolean;
        break;
    case VTERM_PROP_MOUSE:
        t->m_mouseMode = val->number;
        break;
    case VTERM_PROP_TITLE:
        if (val->string.initial) {
            t->m_titleBuf.clear();
        }
        t->m_titleBuf.append(val->string.str, static_cast<qsizetype>(val->string.len));
        if (val->string.final) {
            t->m_title = QString::fromUtf8(t->m_titleBuf);
            emit t->titleChanged(t->m_title);
        }
        break;
    default:
        break;
    }
    t->m_dirty = true;
    return 1;
}

int Terminal::cbBell(void *user)
{
    emit static_cast<Terminal *>(user)->bell();
    return 1;
}

int Terminal::cbPushLine(int cols, const VTermScreenCell *cells, void *user)
{
    auto *t = static_cast<Terminal *>(user);
    if (t->m_scrollbackLimit <= 0) {
        return 1;
    }
    t->m_scrollback.emplace_back(cells, cells + cols);
    ++t->m_pushed;
    while (static_cast<int>(t->m_scrollback.size()) > t->m_scrollbackLimit) {
        t->m_scrollback.pop_front();
        ++t->m_dropped;
    }
    return 1;
}

int Terminal::cbPopLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *t = static_cast<Terminal *>(user);
    if (t->m_scrollback.empty()) {
        return 0;
    }
    const std::vector<VTermScreenCell> line = std::move(t->m_scrollback.back());
    t->m_scrollback.pop_back();
    VTermColor fg;
    VTermColor bg;
    vterm_state_get_default_colors(t->m_state, &fg, &bg);
    for (int i = 0; i < cols; ++i) {
        if (i < static_cast<int>(line.size())) {
            cells[i] = line[static_cast<size_t>(i)];
        } else {
            std::memset(&cells[i], 0, sizeof cells[i]);
            cells[i].width = 1;
            cells[i].fg = fg;
            cells[i].bg = bg;
        }
    }
    --t->m_pushed; // net effect for listeners
    return 1;
}

int Terminal::cbSbClear(void *user)
{
    auto *t = static_cast<Terminal *>(user);
    t->m_scrollback.clear();
    emit t->scrollbackCleared();
    t->m_dirty = true;
    return 1;
}

void Terminal::cbOutput(const char *s, size_t len, void *user)
{
    auto *t = static_cast<Terminal *>(user);
    if (t->m_probing) {
        t->m_probeOut.append(s, static_cast<qsizetype>(len));
        return;
    }
    emit t->output(QByteArray(s, static_cast<qsizetype>(len)));
}

} // namespace zterminal
