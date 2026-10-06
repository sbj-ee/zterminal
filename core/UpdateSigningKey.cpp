// The public key that release SHA256SUMS files are signed with (minisign).
// Not secret. Release builds refuse to install an update unless
// SHA256SUMS.minisig verifies against this key.
//
// Release key ID 32960BBA77B36F1F (minisign -G, October 2026). To rotate it, see
// docs/RELEASING.md, "Signing key".
#include "Update.hpp"

namespace zterminal {

namespace {
constexpr const char kUpdateSigningPublicKey[] = "RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9";

QString &testKey()
{
    static QString k;
    return k;
}
} // namespace

QString updateSigningPublicKey()
{
    if (!testKey().isEmpty()) {
        return testKey();
    }
    return QString::fromLatin1(kUpdateSigningPublicKey);
}

void setUpdateSigningPublicKeyForTests(const QString &base64)
{
    testKey() = base64;
}

} // namespace zterminal
