#pragma once

#include <QString>

namespace zterminal {

// Release builds call this first thing in main(): no core dumps (a crash must
// not write decrypted secrets or the vault key to disk) and, on Linux, not
// dumpable (other processes of the same user can't ptrace us or read
// /proc/<pid>/mem; process_vm_readv is refused too). Children (ssh, the
// askpass helper) are unaffected: dumpable resets on exec, and the zero core
// limit is inherited, which is fine.
// Returns false (and sets *why) if a step failed; the app runs anyway.
bool applyProcessHardening(QString *why = nullptr);

} // namespace zterminal
