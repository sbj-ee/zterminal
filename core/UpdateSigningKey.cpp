// The public key that release SHA256SUMS files are signed with (minisign).
// Not secret. Release builds refuse to install an update unless
// SHA256SUMS.minisig verifies against this key.
//
// TODO(release key): paste the second line of minisign.pub (it starts with
// "RW") between the quotes, once the key exists. See docs/RELEASING.md,
// "Signing key". Until then, in-app Install is refused and users download
// from the release page.
#include "Update.hpp"

namespace zterminal {

namespace {
constexpr const char kUpdateSigningPublicKey[] = "";

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
