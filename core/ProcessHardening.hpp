#pragma once

#include <QString>

namespace zterminal {

// Release builds call this first thing in main(): no core dumps (a crash must
// not write decrypted secrets or the vault key to disk) and, on Linux, not
// dumpable (other processes of the same user can't ptrace us or read
// /proc/<pid>/mem; process_vm_readv is refused too). Children (ssh, the
// askpass helper) are unaffected: dumpable resets on exec. Only the soft core
// limit is set to 0 (the hard limit is left alone), and PTY children get the
// original soft limit back (restoreChildResourceLimits), so a user's shell can
// still enable core dumps for its own programs.
// Returns false (and sets *why) if a step failed; the app runs anyway.
bool applyProcessHardening(QString *why = nullptr);

// In a forked child before exec: puts back the core-dump limit zterminal
// started with. Async-signal-safe (only setrlimit). No-op if hardening
// wasn't applied.
void restoreChildResourceLimits();

} // namespace zterminal
