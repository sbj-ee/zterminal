#include "ProcessHardening.hpp"

#include <sys/resource.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <cerrno>
#include <cstring>

namespace zterminal {

namespace {
rlimit g_originalCore{};
bool g_haveOriginalCore = false;
} // namespace

bool applyProcessHardening(QString *why)
{
    bool ok = true;
    QString errors;
    rlimit cur{};
    if (!g_haveOriginalCore && ::getrlimit(RLIMIT_CORE, &cur) == 0) {
        g_originalCore = cur;
        g_haveOriginalCore = true;
    }
    // Soft limit 0, hard limit unchanged: children can get the original back.
    const rlimit none{0, g_haveOriginalCore ? g_originalCore.rlim_max : 0};
    if (::setrlimit(RLIMIT_CORE, &none) != 0) {
        ok = false;
        errors += QStringLiteral("setrlimit(RLIMIT_CORE): ") + QString::fromLocal8Bit(std::strerror(errno)) + QLatin1Char(' ');
    }
#if defined(__linux__)
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        ok = false;
        errors += QStringLiteral("prctl(PR_SET_DUMPABLE): ") + QString::fromLocal8Bit(std::strerror(errno));
    }
#endif
    if (why) {
        *why = errors.trimmed();
    }
    return ok;
}

void restoreChildResourceLimits()
{
    if (g_haveOriginalCore) {
        (void)::setrlimit(RLIMIT_CORE, &g_originalCore);
    }
}

} // namespace zterminal
