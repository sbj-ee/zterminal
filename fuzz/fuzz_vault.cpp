// The vault's untrusted-input parsers: the file header (read before any key
// derivation) and the decrypted entry table.
#include "SecureBuffer.hpp"
#include "Vault.hpp"

#include <cstddef>
#include <cstdint>
#include <map>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    using namespace zterminal;
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    (void)Vault::checkHeader(bytes);
    std::map<QString, SecureBuffer> entries;
    (void)Vault::parseEntries(data, size, &entries);
    return 0;
}
