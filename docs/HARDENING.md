# Build, CI and release hardening

## Compiler and linker flags (`cmake/Hardening.cmake`)

On by default for every target (`-DZTERMINAL_HARDENING=OFF` to skip):

| Flag | Where |
| --- | --- |
| PIE (`-fPIE`/`-pie`) | all |
| `-fstack-protector-strong` | all |
| `-fstack-clash-protection` | Linux |
| `-fcf-protection=full` (CET) | x86_64 |
| `-D_FORTIFY_SOURCE=3` (Linux) / `=2` (macOS) | optimised builds, not with sanitizers |
| `-z relro -z now` (full RELRO, BIND_NOW), `-z noexecstack` | Linux |

Check a Linux build with `readelf -d build/app/zterminal | grep -E 'BIND_NOW|FLAGS'`,
`readelf -h … | grep DYN` (PIE) and `readelf -lW … | grep GNU_RELRO`.

macOS signing (`cmake/MacDeploy.cmake.in`): every nested framework, dylib,
plugin and helper is ad-hoc signed with the **same** identity (`codesign -s -`),
then the `.app` last. Ad-hoc builds intentionally **omit** `--options runtime`
(hardened runtime). Hardened runtime turns on library validation, which
requires matching Team IDs; ad-hoc signatures have none, so dyld rejects
bundled Qt frameworks (e.g. QtSerialPort) with "different Team IDs" — that was
the v1.3.0 Mac launch crash. v1.2.0 signed without hardened runtime and worked.
`tools/macos/check-bundle.sh` fails CI if nested Team IDs / Signature kinds
diverge, or if ad-hoc + hardened runtime creeps back in. CI still launches the
signed app as a smoke test.

A **Developer ID** signature and **notarization** need Stephen's Apple
Developer account; until then Gatekeeper still asks on first launch. To add
them later: import the Developer ID Application certificate into the runner
keychain (a secret), replace `-` with the identity, add `--options runtime`
and `--timestamp`, then `xcrun notarytool submit --wait` and
`xcrun stapler staple` the .dmg. With a real Team ID, hardened runtime is fine
because every nested binary shares that Team ID.

## Sanitizers

    cmake -B build-san -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZTERMINAL_SANITIZE=address,undefined
    cmake --build build-san
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      LSAN_OPTIONS=suppressions=$PWD/tools/sanitizers/lsan.supp ctest --test-dir build-san

CI job **Sanitizers** runs the whole test suite like this on every PR.
UBSan is built with `-fno-sanitize-recover`, so any undefined behaviour fails
the test.

## Fuzzing (`fuzz/`)

libFuzzer harnesses (clang only) for everything that parses untrusted input:

| Harness | Input |
| --- | --- |
| `fuzz_terminal` | program output into the emulator (libvterm + zterminal callbacks), with a resize mid-stream |
| `fuzz_paste_title` | the paste filter (clipboard to pty) and OSC 0/2 window titles |
| `fuzz_session_import` | session import JSON, then validation and ssh argument building |
| `fuzz_vault` | the vault file header and decrypted entry parser |
| `fuzz_update_json` | the GitHub release JSON the updater reads |

    cmake -B build-fuzz -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DZTERMINAL_FUZZ=ON -DZTERMINAL_BUILD_TESTS=OFF
    cmake --build build-fuzz
    cp -r fuzz/corpus/terminal /tmp/corpus
    QT_QPA_PLATFORM=offscreen build-fuzz/fuzz/fuzz_terminal -max_total_time=600 /tmp/corpus

CI job **Fuzz** runs each harness for 60 s on a copy of `fuzz/corpus/` and
uploads crashing inputs as an artifact. Add a minimised reproducer to
`fuzz/corpus/<harness>/` and a unit test when you fix a finding.

Bugs found so far, all fixed and with a test: ten in the bundled libvterm,
listed in `third_party/libvterm/README.zterminal.md`. They include two remotely
triggerable heap overflows, an out-of-bounds read, a length underflow, a hang,
an `abort()` and a negative-size `memmove`. Also: an unbounded entry count in
the vault parser (memory exhaustion) and an undefined double-to-integer
conversion of the release asset size.

## GitHub Actions

- `permissions: contents: read` at the top of both workflows. Only the release
  `publish` job gets `contents: write`. The release build jobs get
  `id-token: write` + `attestations: write` solely for provenance.
- Every action is pinned to a full commit SHA with the version in a comment.
  Dependabot (`.github/dependabot.yml`) proposes updates weekly.
- Releases carry signed SLSA build provenance for the .deb and .dmg:
  `gh attestation verify zterminal_<version>_amd64.deb -R sbj-ee/zterminal`.
