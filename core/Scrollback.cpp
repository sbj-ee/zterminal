#include "Scrollback.hpp"

#include <QtGlobal>

#include <algorithm>
#include <cstring>

namespace zterminal {

namespace {

constexpr uint32_t kWideGap = static_cast<uint32_t>(-1);

void putU16(QByteArray &b, uint32_t v)
{
    b.append(static_cast<char>(v & 0xff));
    b.append(static_cast<char>((v >> 8) & 0xff));
}

void putU32(QByteArray &b, uint32_t v)
{
    putU16(b, v & 0xffff);
    putU16(b, v >> 16);
}

uint32_t getU16(const unsigned char *p)
{
    return p[0] | (uint32_t(p[1]) << 8);
}

uint32_t getU32(const unsigned char *p)
{
    return getU16(p) | (getU16(p + 2) << 16);
}

// Colours and attributes packed explicitly (bitfields and unions may carry
// uninitialised padding, so no memcmp/memcpy of the libvterm structs).
uint32_t packColor(const VTermColor &c)
{
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        return c.type | (uint32_t(c.indexed.idx) << 8);
    }
    return c.type | (uint32_t(c.rgb.red) << 8) | (uint32_t(c.rgb.green) << 16) | (uint32_t(c.rgb.blue) << 24);
}

VTermColor unpackColor(uint32_t v)
{
    VTermColor c;
    std::memset(&c, 0, sizeof c);
    c.type = static_cast<uint8_t>(v & 0xff);
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        c.indexed.idx = static_cast<uint8_t>((v >> 8) & 0xff);
    } else {
        c.rgb.red = static_cast<uint8_t>((v >> 8) & 0xff);
        c.rgb.green = static_cast<uint8_t>((v >> 16) & 0xff);
        c.rgb.blue = static_cast<uint8_t>((v >> 24) & 0xff);
    }
    return c;
}

uint32_t packAttrs(const VTermScreenCellAttrs &a)
{
    return a.bold | (a.underline << 1) | (a.italic << 3) | (a.blink << 4) | (a.reverse << 5) | (a.conceal << 6)
        | (a.strike << 7) | (a.font << 8) | (a.dwl << 12) | (a.dhl << 13) | (a.small << 15) | (a.baseline << 16);
}

VTermScreenCellAttrs unpackAttrs(uint32_t v)
{
    VTermScreenCellAttrs a;
    std::memset(&a, 0, sizeof a);
    a.bold = v & 1;
    a.underline = (v >> 1) & 3;
    a.italic = (v >> 3) & 1;
    a.blink = (v >> 4) & 1;
    a.reverse = (v >> 5) & 1;
    a.conceal = (v >> 6) & 1;
    a.strike = (v >> 7) & 1;
    a.font = (v >> 8) & 15;
    a.dwl = (v >> 12) & 1;
    a.dhl = (v >> 13) & 3;
    a.small = (v >> 15) & 1;
    a.baseline = (v >> 16) & 3;
    return a;
}

struct Style {
    uint32_t fg, bg, attrs;
    bool operator==(const Style &o) const { return fg == o.fg && bg == o.bg && attrs == o.attrs; }
};

Style styleOf(const VTermScreenCell &c)
{
    return {packColor(c.fg), packColor(c.bg), packAttrs(c.attrs)};
}

void appendUtf8(QByteArray &b, uint32_t cp)
{
    if (cp < 0x80) {
        b.append(static_cast<char>(cp));
    } else if (cp < 0x800) {
        b.append(static_cast<char>(0xc0 | (cp >> 6)));
        b.append(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        b.append(static_cast<char>(0xe0 | (cp >> 12)));
        b.append(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        b.append(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        b.append(static_cast<char>(0xf0 | (cp >> 18)));
        b.append(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        b.append(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        b.append(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

uint32_t readUtf8(const unsigned char *&p, const unsigned char *end)
{
    const unsigned char c = *p++;
    int extra = 0;
    uint32_t cp = c;
    if (c >= 0xf0) {
        extra = 3;
        cp = c & 0x07;
    } else if (c >= 0xe0) {
        extra = 2;
        cp = c & 0x0f;
    } else if (c >= 0xc0) {
        extra = 1;
        cp = c & 0x1f;
    }
    while (extra-- > 0 && p < end) {
        cp = (cp << 6) | (*p++ & 0x3f);
    }
    return cp;
}

// Shared walk over a line's cells: fn(col, width, chars, nchars) for every
// stored cell (nchars 0 = blank, -1 = wide right half).
struct Header {
    int cols = 0, stored = 0, runs = 0;
    const unsigned char *runData = nullptr;
    const unsigned char *cells = nullptr;
};

Header readHeader(const char *data, qsizetype len)
{
    Header h;
    if (len < 6) {
        return h;
    }
    const auto *p = reinterpret_cast<const unsigned char *>(data);
    h.cols = static_cast<int>(getU16(p));
    h.stored = static_cast<int>(getU16(p + 2));
    h.runs = static_cast<int>(getU16(p + 4));
    h.runData = p + 6;
    h.cells = h.runData + qsizetype(h.runs) * 14;
    return h;
}

template <typename Fn>
void walkCells(const Header &h, const unsigned char *end, Fn fn)
{
    const unsigned char *p = h.cells;
    uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
    for (int col = 0; col < h.stored && p < end; ++col) {
        const unsigned char b = *p++;
        if (b < 0x80) {
            chars[0] = b;
            fn(col, 1, chars, b ? 1 : 0);
        } else if (b == 0x80) {
            fn(col, 1, chars, -1);
        } else {
            const int n = std::min<int>(b & 0x0f, VTERM_MAX_CHARS_PER_CELL);
            const int width = (b & 0x10) ? 2 : 1;
            for (int k = 0; k < n && p < end; ++k) {
                chars[k] = readUtf8(p, end);
            }
            fn(col, width, chars, n);
        }
    }
}

} // namespace

QByteArray Scrollback::encode(const VTermScreenCell *cells, int cols)
{
    cols = std::clamp(cols, 0, 0xffff);
    // Trailing blank cells with the last cell's style aren't stored.
    int stored = cols;
    if (cols > 0) {
        const Style last = styleOf(cells[cols - 1]);
        while (stored > 0 && cells[stored - 1].chars[0] == 0 && cells[stored - 1].width <= 1
               && styleOf(cells[stored - 1]) == last) {
            --stored;
        }
    }
    QByteArray b;
    b.reserve(6 + 14 + stored + 8);
    putU16(b, uint32_t(cols));
    putU16(b, uint32_t(stored));
    putU16(b, 0); // runs, patched below
    int runs = 0;
    for (int col = 0; col < cols;) {
        const Style s = styleOf(cells[col]);
        int len = 1;
        while (col + len < cols && styleOf(cells[col + len]) == s) {
            ++len;
        }
        putU16(b, uint32_t(len));
        putU32(b, s.fg);
        putU32(b, s.bg);
        putU32(b, s.attrs);
        ++runs;
        col += len;
    }
    b[4] = static_cast<char>(runs & 0xff);
    b[5] = static_cast<char>((runs >> 8) & 0xff);
    for (int col = 0; col < stored; ++col) {
        const VTermScreenCell &c = cells[col];
        if (c.chars[0] == kWideGap) {
            b.append(static_cast<char>(0x80));
        } else if (c.chars[0] < 0x80 && (c.chars[1] == 0 || c.chars[0] == 0) && c.width <= 1) {
            b.append(static_cast<char>(c.chars[0])); // 0 = blank
        } else {
            int n = 0;
            while (n < VTERM_MAX_CHARS_PER_CELL && c.chars[n]) {
                ++n;
            }
            b.append(static_cast<char>(0x80 | n | (c.width == 2 ? 0x10 : 0)));
            for (int k = 0; k < n; ++k) {
                appendUtf8(b, c.chars[k]);
            }
        }
    }
    return b;
}

void Scrollback::decode(const char *data, qsizetype len, std::vector<VTermScreenCell> *out)
{
    const Header h = readHeader(data, len);
    out->resize(size_t(h.cols));
    const auto *end = reinterpret_cast<const unsigned char *>(data) + len;
    // Styles from the runs, then characters.
    int col = 0;
    for (int r = 0; r < h.runs; ++r) {
        const unsigned char *p = h.runData + r * 14;
        const int n = static_cast<int>(getU16(p));
        VTermScreenCell tmpl;
        std::memset(&tmpl, 0, sizeof tmpl);
        tmpl.width = 1;
        tmpl.fg = unpackColor(getU32(p + 2));
        tmpl.bg = unpackColor(getU32(p + 6));
        tmpl.attrs = unpackAttrs(getU32(p + 10));
        for (int k = 0; k < n && col < h.cols; ++k, ++col) {
            (*out)[size_t(col)] = tmpl;
        }
    }
    walkCells(h, end, [out](int c, int width, const uint32_t *chars, int n) {
        VTermScreenCell &cell = (*out)[size_t(c)];
        if (n < 0) {
            cell.chars[0] = kWideGap;
            return;
        }
        cell.width = static_cast<char>(width);
        for (int k = 0; k < n; ++k) {
            cell.chars[k] = chars[k];
        }
    });
}

QString Scrollback::decodeText(const char *data, qsizetype len, std::vector<int> *colOfChar)
{
    const Header h = readHeader(data, len);
    const auto *end = reinterpret_cast<const unsigned char *>(data) + len;
    QString s;
    s.reserve(h.stored);
    qsizetype keep = 0;
    walkCells(h, end, [&](int col, int, const uint32_t *chars, int n) {
        if (n < 0) {
            return;
        }
        if (n == 0) {
            s.append(QLatin1Char(' '));
            if (colOfChar) {
                colOfChar->push_back(col);
            }
            return;
        }
        for (int k = 0; k < n; ++k) {
            const char32_t cp = chars[k];
            if (cp < 0x10000) {
                s.append(QChar(static_cast<char16_t>(cp)));
                if (colOfChar) {
                    colOfChar->push_back(col);
                }
            } else {
                s.append(QString::fromUcs4(&cp, 1));
                if (colOfChar) {
                    colOfChar->push_back(col);
                    colOfChar->push_back(col);
                }
            }
        }
        keep = s.size();
    });
    s.truncate(keep);
    if (colOfChar) {
        colOfChar->resize(size_t(keep));
    }
    return s;
}

void Scrollback::push(const VTermScreenCell *cells, int cols)
{
    if (m_chunks.empty() || m_chunks.back().lines() >= kLinesPerChunk) {
        Chunk c;
        c.id = m_nextId++;
        c.offsets.push_back(0);
        c.raw.reserve(kLinesPerChunk * 64);
        m_chunks.push_back(std::move(c));
    }
    Chunk &c = m_chunks.back();
    c.raw += encode(cells, cols);
    c.offsets.push_back(static_cast<uint32_t>(c.raw.size()));
    ++m_size;
    sealIfFull();
}

void Scrollback::sealIfFull()
{
    if (m_chunks.back().lines() < kLinesPerChunk) {
        return;
    }
    m_chunks.back().raw.squeeze();
    // Compress the sealed chunk that just fell out of the raw window.
    const qsizetype idx = qsizetype(m_chunks.size()) - 1 - kRawChunks;
    if (idx >= 0) {
        Chunk &old = m_chunks[size_t(idx)];
        if (!old.raw.isEmpty()) {
            old.packed = qCompress(old.raw, 1);
            old.packed.squeeze(); // qCompress allocates compressBound(raw)
            old.raw = QByteArray();
        }
    }
}

const QByteArray &Scrollback::rawOf(const Chunk &c) const
{
    if (c.packed.isEmpty()) {
        return c.raw;
    }
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
        if (it->first == c.id) {
            m_cache.splice(m_cache.begin(), m_cache, it);
            return m_cache.front().second;
        }
    }
    m_cache.emplace_front(c.id, qUncompress(c.packed));
    while (m_cache.size() > size_t(kCachedChunks)) {
        m_cache.pop_back();
    }
    return m_cache.front().second;
}

void Scrollback::locate(int i, const Chunk **chunk, int *index) const
{
    // All chunks but the last are full, so this is arithmetic.
    const int abs = i + m_frontSkip;
    *chunk = &m_chunks[size_t(abs / kLinesPerChunk)];
    *index = abs % kLinesPerChunk;
}

void Scrollback::line(int i, std::vector<VTermScreenCell> *out) const
{
    out->clear();
    if (i < 0 || i >= m_size) {
        return;
    }
    const Chunk *c = nullptr;
    int k = 0;
    locate(i, &c, &k);
    const QByteArray &raw = rawOf(*c);
    const uint32_t from = c->offsets[size_t(k)];
    decode(raw.constData() + from, qsizetype(c->offsets[size_t(k) + 1] - from), out);
}

QString Scrollback::lineText(int i, std::vector<int> *colOfChar) const
{
    if (i < 0 || i >= m_size) {
        return {};
    }
    const Chunk *c = nullptr;
    int k = 0;
    locate(i, &c, &k);
    const QByteArray &raw = rawOf(*c);
    const uint32_t from = c->offsets[size_t(k)];
    return decodeText(raw.constData() + from, qsizetype(c->offsets[size_t(k) + 1] - from), colOfChar);
}

void Scrollback::popFront(int count)
{
    count = std::min(count, m_size);
    m_size -= count;
    m_frontSkip += count;
    while (!m_chunks.empty() && m_frontSkip >= m_chunks.front().lines() && m_chunks.size() > 1) {
        m_frontSkip -= m_chunks.front().lines();
        const quint64 id = m_chunks.front().id;
        m_cache.remove_if([id](const auto &e) { return e.first == id; });
        m_chunks.pop_front();
    }
    if (m_size == 0) {
        clear();
    }
}

bool Scrollback::popBack(std::vector<VTermScreenCell> *out)
{
    if (m_size == 0) {
        return false;
    }
    line(m_size - 1, out);
    Chunk &c = m_chunks.back();
    if (!c.packed.isEmpty()) {
        c.raw = rawOf(c);
        c.packed = QByteArray();
        const quint64 id = c.id;
        m_cache.remove_if([id](const auto &e) { return e.first == id; });
    }
    c.offsets.pop_back();
    c.raw.truncate(qsizetype(c.offsets.back()));
    --m_size;
    if (m_size == 0) {
        clear();
    } else if (c.lines() == 0) {
        {
            m_chunks.pop_back();
            // The previous chunk becomes the one being filled: keep it raw.
            Chunk &prev = m_chunks.back();
            if (!prev.packed.isEmpty()) {
                prev.raw = rawOf(prev);
                prev.packed = QByteArray();
                const quint64 id = prev.id;
                m_cache.remove_if([id](const auto &e) { return e.first == id; });
            }
        }
    }
    return true;
}

void Scrollback::clear()
{
    m_chunks.clear();
    m_cache.clear();
    m_frontSkip = 0;
    m_size = 0;
}

qsizetype Scrollback::memoryBytes() const
{
    qsizetype n = 0;
    for (const Chunk &c : m_chunks) {
        n += c.raw.capacity() + c.packed.capacity() + qsizetype(c.offsets.capacity() * sizeof(uint32_t))
            + qsizetype(sizeof(Chunk));
    }
    for (const auto &e : m_cache) {
        n += e.second.capacity();
    }
    return n;
}

int Scrollback::compressedChunks() const
{
    int n = 0;
    for (const Chunk &c : m_chunks) {
        n += c.packed.isEmpty() ? 0 : 1;
    }
    return n;
}

} // namespace zterminal
