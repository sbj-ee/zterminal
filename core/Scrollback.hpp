#pragma once

#include <vterm.h>

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <vector>

namespace zterminal {

// Scrollback history (docs/PLAN.md §4.16): lines are stored compactly and in
// chunks, and older chunks are zlib-compressed (qCompress), so 100,000 lines
// cost a few MB instead of ~300 MB of VTermScreenCell.
//
// Line encoding: [u16 cols][u16 stored cells][u16 runs] runs × {u16 length,
// 4-byte fg, 4-byte bg, u32 attrs} then one byte per plain-ASCII cell (0 =
// blank, 0x01-0x7f = that character), 0x80 = right half of a wide character,
// 0x80|n (+0x10 for width 2) followed by n UTF-8 code points otherwise.
// Trailing blank cells are not stored; they take the last run's attributes.
class Scrollback
{
public:
    static constexpr int kLinesPerChunk = 512;
    static constexpr int kRawChunks = 2;      // newest sealed chunks kept uncompressed
    static constexpr int kCachedChunks = 6;   // decompressed chunks kept for reading

    int size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    void push(const VTermScreenCell *cells, int cols);
    // Oldest line out (scrollback limit).
    void popFront(int count = 1);
    // Newest line back to the screen (reflow on resize); false if empty.
    bool popBack(std::vector<VTermScreenCell> *out);
    void clear();

    // Line i (0 = oldest), decoded; padded/truncated to `cols` when cols > 0.
    void line(int i, std::vector<VTermScreenCell> *out) const;
    // Plain text of line i: one character per cell (blank = space, wide right
    // half = nothing), trailing never-written cells dropped. With colOfChar,
    // the cell column of every UTF-16 unit is appended (for Find).
    QString lineText(int i, std::vector<int> *colOfChar = nullptr) const;

    // Bytes held: encoded/compressed chunk data, offsets, and the read cache.
    qsizetype memoryBytes() const;
    int compressedChunks() const;

    static QByteArray encode(const VTermScreenCell *cells, int cols);
    static void decode(const char *data, qsizetype len, std::vector<VTermScreenCell> *out);
    static QString decodeText(const char *data, qsizetype len, std::vector<int> *colOfChar);

private:
    struct Chunk {
        QByteArray raw;                 // concatenated lines (empty while compressed)
        QByteArray packed;              // qCompress(raw) once sealed and old
        std::vector<uint32_t> offsets;  // start of each line in raw; plus end
        quint64 id = 0;
        int lines() const { return static_cast<int>(offsets.size()) - 1; }
    };
    const QByteArray &rawOf(const Chunk &c) const;
    void sealIfFull();
    void locate(int i, const Chunk **chunk, int *index) const;

    std::deque<Chunk> m_chunks; // oldest first; the last one is being filled
    int m_frontSkip = 0;        // lines of chunk 0 already dropped
    int m_size = 0;
    quint64 m_nextId = 1;
    mutable std::list<std::pair<quint64, QByteArray>> m_cache; // id -> decompressed raw (MRU first)
};

} // namespace zterminal
