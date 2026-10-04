#pragma once

#include <QString>

#include <cstddef>

namespace zterminal {

// A byte buffer in libsodium guarded memory: sodium_malloc (guard pages,
// canary, mlock'd so it isn't swapped) and sodium_free (which zeroes it).
// Move-only. Used for the vault key and every decrypted secret.
class SecureBuffer
{
public:
    SecureBuffer() = default;
    explicit SecureBuffer(std::size_t size); // zero-filled
    SecureBuffer(const unsigned char *data, std::size_t size);
    ~SecureBuffer();
    SecureBuffer(SecureBuffer &&o) noexcept;
    SecureBuffer &operator=(SecureBuffer &&o) noexcept;
    SecureBuffer(const SecureBuffer &) = delete;
    SecureBuffer &operator=(const SecureBuffer &) = delete;

    unsigned char *data() { return m_data; }
    const unsigned char *data() const { return m_data; }
    std::size_t size() const { return m_size; }
    bool isEmpty() const { return m_size == 0; }
    SecureBuffer clone() const;
    bool equals(const SecureBuffer &o) const; // constant-time

    // sodium_memzero the contents (the buffer stays allocated).
    void wipe();
    // Free (and zero) the memory now.
    void reset();

    // UTF-8 encode `s` straight into secure memory: no intermediate QByteArray.
    // The QString itself is the caller's to clear (see README: residual risk).
    static SecureBuffer fromQString(QStringView s);
    // sodium_init() once; false if libsodium can't initialise.
    static bool ensureSodium();

private:
    unsigned char *m_data = nullptr;
    std::size_t m_size = 0;
};

} // namespace zterminal
