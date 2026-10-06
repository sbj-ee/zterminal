#include "ProcessHardening.hpp"

#include <sys/resource.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <cerrno>
#include <cstring>

namespace zterminal {

bool applyProcessHardening(QString *why)
{
    bool ok = true;
    QString errors;
    const rlimit none{0, 0};
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

} // namespace zterminal
