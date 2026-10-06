// The update checker's inputs from the network: the GitHub release JSON and
// SHA256SUMS.
#include "Update.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    using namespace zterminal;
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    QString err;
    if (const auto rel = ReleaseInfo::fromJson(bytes, &err)) {
        (void)rel->packageAsset();
        (void)rel->checksumAsset();
        (void)rel->debAsset();
        (void)rel->dmgAsset();
        (void)isNewerVersion(rel->tag, QStringLiteral("1.0.0"));
    }
    (void)checksumFor(bytes, QStringLiteral("zterminal_1.0.0_amd64.deb"));
    (void)SemVer::parse(QString::fromUtf8(bytes.left(64)));
    return 0;
}
