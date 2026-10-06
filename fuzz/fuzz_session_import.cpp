// Session import (File > Import Sessions): JSON from an untrusted file,
// then everything zterminal derives from a parsed session.
#include "Session.hpp"
#include "SessionExport.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    using namespace zterminal;
    const QByteArray json(reinterpret_cast<const char *>(data), qsizetype(size));
    QString err;
    const auto doc = sessionsFromExportJson(json, &err);
    if (!doc) {
        return 0;
    }
    for (const SessionConfig &s : doc->sessions) {
        (void)validateSessionName(s.name);
        (void)validateSshHost(s.host);
        (void)validateSshUser(s.user);
        (void)validateJumpHost(s.jumpHost);
        (void)validateSerial(s);
        (void)enterSequence(s.enterSends);
        const SshCommand cmd = buildSshCommand(s);
        if (cmd.ok()) {
            (void)sessionFromSshArgs(cmd.args);
        }
        (void)sessionToJson(s);
    }
    (void)sessionsToExportJson(doc->sessions);
    return 0;
}
