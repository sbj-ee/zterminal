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
    applyDefaultCursorStyle();
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

void Terminal::setDefaultCursorStyle(int shape, bool blink)
{
    if (shape < VTERM_PROP_CURSORSHAPE_BLOCK || shape >= VTERM_N_PROP_CURSORSHAPES) {
        shape = kDefaultCursorShape;
    }
    m_defaultCursorShape = shape;
    m_defaultCursorBlink = blink;
    applyDefaultCursorStyle();
    flush();
}

void Terminal::applyDefaultCursorStyle()
{
    // Through libvterm's state so DECSC/DECRC and DECRQSS see the same style.
    VTermValue v;
    v.number = m_defaultCursorShape;
    vterm_state_set_termprop(m_state, VTERM_PROP_CURSORSHAPE, &v);
    v.boolean = m_defaultCursorBlink ? 1 : 0;
    vterm_state_set_termprop(m_state, VTERM_PROP_CURSORBLINK, &v);
}

// True right after the last byte of DECSCUSR 0 ("ESC [ SP q", "ESC [ 0 SP q")
// or RIS ("ESC c"): libvterm then sets a blinking block; we restore the default.
bool Terminal::scanForCursorReset(char c)
{
    const ResetScan st = m_resetScan;
    m_resetScan = c == '\x1b' ? ResetScan::Esc : ResetScan::Ground;
    switch (st) {
    case ResetScan::Ground:
        return false;
    case ResetScan::Esc:
        if (c == '[') {
            m_resetScan = ResetScan::Csi;
        }
        return c == 'c';
    case ResetScan::Csi:
        if (c == '0') {
            m_resetScan = ResetScan::Csi;
        } else if (c == ' ') {
            m_resetScan = ResetScan::CsiSpace;
        }
        return false;
    case ResetScan::CsiSpace:
        return c == 'q';
    }
    return false;
}

void Terminal::feed(const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    const char *data = bytes.constData();
    const char *const end = data + bytes.size();
    const char *start = data;
    const char *p = data;
    while (p < end) {
        if (m_resetScan == ResetScan::Ground) {
            // Fast path: nothing pending, skip to the next ESC.
            const void *esc = std::memchr(p, '\x1b', static_cast<size_t>(end - p));
            if (!esc) {
                break;
            }
            p = static_cast<const char *>(esc);
        }
        if (scanForCursorReset(*p++)) {
            vterm_input_write(m_vt, start, static_cast<size_t>(p - start));
            applyDefaultCursorStyle();
            start = p;
        }
    }
    if (start < end) {
        vterm_input_write(m_vt, start, static_cast<size_t>(end - start));
    }
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
    m_scrollbackLimit = lines < 0 ? kUnlimitedScrollback : lines;
    int dropped = 0;
    if (m_scrollbackLimit >= 0 && m_history.size() > m_scrollbackLimit) {
        dropped = m_history.size() - m_scrollbackLimit;
        dropHistoryFront(dropped);
    }
    if (dropped) {
        emit scrolledIntoHistory(0, dropped);
        emit damaged();
    }
}

void Terminal::dropHistoryFront(int n)
{
    m_history.popFront(n);
    m_historyBase += quint64(n);
}

void Terminal::trimHistory()
{
    if (m_scrollbackLimit >= 0 && m_history.size() > m_scrollbackLimit) {
        const int n = m_history.size() - m_scrollbackLimit;
        dropHistoryFront(n);
        m_dropped += n;
    }
}

const VTermScreenCell *Terminal::historyCell(int absLine, int col) const
{
    const quint64 key = m_historyBase + quint64(absLine);
    for (auto &e : m_lineCache) {
        if (e.first == key) {
            return col < static_cast<int>(e.second.size()) ? &e.second[size_t(col)] : nullptr;
        }
    }
    constexpr size_t kCacheLines = 8;
    if (m_lineCache.size() >= kCacheLines) {
        m_lineCache.erase(m_lineCache.begin());
    }
    m_lineCache.emplace_back(key, std::vector<VTermScreenCell>{});
    m_history.line(absLine, &m_lineCache.back().second);
    const auto &v = m_lineCache.back().second;
    return col < static_cast<int>(v.size()) ? &v[size_t(col)] : nullptr;
}

QString Terminal::searchText(int absLine, std::vector<int> *colOfChar) const
{
    const int sb = scrollbackLines();
    if (absLine < 0 || absLine >= totalLines()) {
        return {};
    }
    if (absLine < sb) {
        return m_history.lineText(absLine, colOfChar);
    }
    QString s;
    qsizetype keep = 0;
    size_t keepCols = 0;
    VTermScreenCell c;
    for (int col = 0; col < m_cols; ++col) {
        std::memset(&c, 0, sizeof c);
        vterm_screen_get_cell(m_screen, VTermPos{absLine - sb, col}, &c);
        if (c.chars[0] == kWideGap) {
            continue;
        }
        if (c.chars[0] == 0) {
            s.append(QLatin1Char(' '));
            if (colOfChar) {
                colOfChar->push_back(col);
            }
            continue;
        }
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; ++i) {
            const char32_t cp = c.chars[i];
            const QString u = QString::fromUcs4(&cp, 1);
            s += u;
            if (colOfChar) {
                for (qsizetype k = 0; k < u.size(); ++k) {
                    colOfChar->push_back(col);
                }
            }
        }
        keep = s.size();
        keepCols = colOfChar ? colOfChar->size() : 0;
    }
    s.truncate(keep);
    if (colOfChar) {
        colOfChar->resize(keepCols);
    }
    return s;
}

void Terminal::clearScrollback()
{
    m_history.clear();
    m_lineCache.clear();
    emit scrollbackCleared();
    emit damaged();
}

void Terminal::reset()
{
    vterm_screen_reset(m_screen, 1);
    applyDefaultCursorStyle();
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
        if (const VTermScreenCell *hc = historyCell(absLine, col)) {
            return convert(*hc);
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

namespace {
bool isPasteControl(char32_t u)
{
    return (u < 0x20 && u != U'\t' && u != U'\r') || u == 0x7f || (u >= 0x80 && u < 0xa0);
}
bool isBidiControl(char32_t u)
{
    return u == 0x061c || u == 0x200e || u == 0x200f || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069);
}
} // namespace

QByteArray Terminal::preparePasteBytes(const QString &text)
{
    QString t = text;
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
    t.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    // Drop whole end-of-paste markers first (so "a ESC[201~ b" reads "a b")...
    t.remove(QStringLiteral("\x1b[201~"));
    // ...then every remaining control character. Without ESC no escape
    // sequence can survive, however the markers were split or nested
    // ("ESC[20" + "ESC[201~" + "1~" re-forms one after the removal above).
    QString clean;
    clean.reserve(t.size());
    for (const char32_t u : t.toUcs4()) {
        if (!isPasteControl(u)) {
            clean += QString::fromUcs4(&u, 1);
        }
    }
    return clean.toUtf8();
}

QString Terminal::sanitizeTitle(const QString &title)
{
    QString out;
    out.reserve(title.size());
    for (const char32_t u : title.toUcs4()) {
        if ((u < 0x20) || u == 0x7f || (u >= 0x80 && u < 0xa0) || isBidiControl(u)) {
            continue;
        }
        out += QString::fromUcs4(&u, 1);
    }
    return out;
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
        t->m_dirty = true;
        break;
    case VTERM_PROP_CURSORBLINK:
        t->m_cursorBlink = val->boolean;
        t->m_dirty = true;
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
        // A program can stream an endless OSC title; keep at most kMaxTitleBytes.
        if (t->m_titleBuf.size() < kMaxTitleBytes) {
            const qsizetype room = kMaxTitleBytes - t->m_titleBuf.size();
            t->m_titleBuf.append(val->string.str, std::min(room, static_cast<qsizetype>(val->string.len)));
        }
        if (val->string.final) {
            t->m_title = sanitizeTitle(QString::fromUtf8(t->m_titleBuf));
            t->m_titleBuf.clear();
            t->m_titleBuf.squeeze();
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
    if (t->m_scrollbackLimit == 0) {
        return 1;
    }
    t->m_history.push(cells, cols);
    ++t->m_pushed;
    t->trimHistory();
    return 1;
}

int Terminal::cbPopLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *t = static_cast<Terminal *>(user);
    std::vector<VTermScreenCell> line;
    if (!t->m_history.popBack(&line)) {
        return 0;
    }
    t->m_lineCache.clear();
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
    t->m_history.clear();
    t->m_lineCache.clear();
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
