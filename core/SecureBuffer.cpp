#include "SecureBuffer.hpp"

#include <sodium.h>

#include <cstring>
#include <new>
#include <utility>

namespace zterminal {

bool SecureBuffer::ensureSodium()
{
    static const bool ok = sodium_init() >= 0;
    return ok;
}

SecureBuffer::SecureBuffer(std::size_t size)
{
    if (!ensureSodium()) {
        throw std::bad_alloc();
    }
    if (size == 0) {
        return;
    }
    m_data = static_cast<unsigned char *>(sodium_malloc(size));
    if (!m_data) {
        throw std::bad_alloc();
    }
    m_size = size;
    sodium_memzero(m_data, size);
}

SecureBuffer::SecureBuffer(const unsigned char *data, std::size_t size)
    : SecureBuffer(size)
{
    if (size) {
        std::memcpy(m_data, data, size);
    }
}

SecureBuffer::~SecureBuffer()
{
    reset();
}

SecureBuffer::SecureBuffer(SecureBuffer &&o) noexcept
    : m_data(std::exchange(o.m_data, nullptr))
    , m_size(std::exchange(o.m_size, 0))
{
}

SecureBuffer &SecureBuffer::operator=(SecureBuffer &&o) noexcept
{
    if (this != &o) {
        reset();
        m_data = std::exchange(o.m_data, nullptr);
        m_size = std::exchange(o.m_size, 0);
    }
    return *this;
}

SecureBuffer SecureBuffer::clone() const
{
    return SecureBuffer(m_data, m_size);
}

bool SecureBuffer::equals(const SecureBuffer &o) const
{
    return m_size == o.m_size && (m_size == 0 || sodium_memcmp(m_data, o.m_data, m_size) == 0);
}

void SecureBuffer::wipe()
{
    if (m_data) {
        sodium_memzero(m_data, m_size);
    }
}

void SecureBuffer::reset()
{
    if (m_data) {
        sodium_free(m_data); // zeroes before unmapping
    }
    m_data = nullptr;
    m_size = 0;
}

SecureBuffer SecureBuffer::fromQString(QStringView s)
{
    // Pass 1: size; pass 2: encode. Surrogate pairs -> 4 bytes; lone surrogates -> U+FFFD.
    auto codePointAt = [&s](qsizetype &i) -> char32_t {
        const char16_t c = s[i].unicode();
        if (QChar::isHighSurrogate(c) && i + 1 < s.size() && QChar::isLowSurrogate(s[i + 1].unicode())) {
            const char32_t cp = QChar::surrogateToUcs4(c, s[i + 1].unicode());
            ++i;
            return cp;
        }
        if (QChar::isSurrogate(c)) {
            return 0xFFFD;
        }
        return c;
    };
    auto lengthOf = [](char32_t cp) -> std::size_t { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; };
    std::size_t n = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        n += lengthOf(codePointAt(i));
    }
    SecureBuffer out(n);
    if (n == 0) {
        return out; // empty string: no buffer to fill
    }
    unsigned char *p = out.data();
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char32_t cp = codePointAt(i);
        switch (lengthOf(cp)) {
        case 1:
            *p++ = static_cast<unsigned char>(cp);
            break;
        case 2:
            *p++ = static_cast<unsigned char>(0xC0 | (cp >> 6));
            *p++ = static_cast<unsigned char>(0x80 | (cp & 0x3F));
            break;
        case 3:
            *p++ = static_cast<unsigned char>(0xE0 | (cp >> 12));
            *p++ = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
            *p++ = static_cast<unsigned char>(0x80 | (cp & 0x3F));
            break;
        default:
            *p++ = static_cast<unsigned char>(0xF0 | (cp >> 18));
            *p++ = static_cast<unsigned char>(0x80 | ((cp >> 12) & 0x3F));
            *p++ = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
            *p++ = static_cast<unsigned char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

void wipeByteArray(QByteArray &b)
{
    if (b.isDetached() && b.capacity() > 0) {
        b.resize(b.capacity()); // new bytes are not initialised: the old contents
        sodium_memzero(b.data(), std::size_t(b.size()));
    }
    b.clear();
}

} // namespace zterminal
