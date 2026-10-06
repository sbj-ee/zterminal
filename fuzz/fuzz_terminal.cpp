// Bytes from the remote side (ssh, serial, local programs) into the terminal
// emulator, plus the paste and title paths. First byte picks the size.
#include "Terminal.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    using namespace zterminal;
    if (size < 1) {
        return 0;
    }
    const int rows = 1 + data[0] % 40;
    const int cols = 1 + (data[0] / 40) % 120;
    Terminal t(rows, cols);
    const QByteArray bytes(reinterpret_cast<const char *>(data + 1), qsizetype(size - 1));
    // Feed in two pieces with a resize between, to cover split sequences.
    const qsizetype half = bytes.size() / 2;
    t.feed(bytes.left(half));
    t.resize(cols % 7 + 2, rows % 11 + 2);
    t.feed(bytes.mid(half));
    (void)t.title();
    for (int r = 0; r < t.rows(); ++r) {
        (void)t.lineText(r);
    }
    (void)Terminal::preparePasteBytes(QString::fromUtf8(bytes));
    t.paste(QString::fromUtf8(bytes.left(64)));
    return 0;
}
