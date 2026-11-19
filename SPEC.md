# zterminal — Project specification

This document specifies zterminal so that it can be configured, built, tested,
packaged and released repeatably. It is derived from the repository itself
(`CMakeLists.txt`, `cmake/`, `.github/`, sources, `third_party/libvterm/`,
`packaging/`, `scripts/`, `tools/` and the existing docs) as of version
**1.7.5** (main tip `3587e8c` = 1.7.4 plus this document). Where the repository
is silent, ambiguous or inconsistent, that is called out in
[§14 Open questions / ambiguities](#14-open-questions--ambiguities) rather than
guessed.

Authoritative sources, in order: the build files and workflows (what actually
runs), then the source code, then [README.md](README.md) (user-facing
behaviour), then `docs/` ([PLAN.md](docs/PLAN.md) is historical design notes and
loses to the README where they differ). If this file disagrees with the build
files or workflows, those win and this file should be corrected.

Contents:

1. [Purpose and scope](#1-purpose-and-scope)
2. [Feature set (as implemented)](#2-feature-set-as-implemented)
3. [Non-goals](#3-non-goals)
4. [Architecture and modules](#4-architecture-and-modules)
5. [Toolchain and dependencies](#5-toolchain-and-dependencies)
6. [Configure, build and test](#6-configure-build-and-test)
7. [Packaging](#7-packaging)
8. [Signing and release](#8-signing-and-release)
9. [In-app updater](#9-in-app-updater)
10. [Runtime files and configuration](#10-runtime-files-and-configuration)
11. [CI and Dependabot](#11-ci-and-dependabot)
12. [Versioning rules](#12-versioning-rules)
13. [Reproducing a release build](#13-reproducing-a-release-build)
14. [Open questions / ambiguities](#14-open-questions--ambiguities)

---

## 1. Purpose and scope

zterminal is a PuTTY-style terminal emulator for **Linux amd64** and **macOS
Apple Silicon (arm64, macOS 14+)**, written in C++20 with Qt 6 Widgets and a
bundled, patched [libvterm](https://www.leonerd.org.uk/code/libvterm/) 0.3.3.
It provides a menu bar, a session dialog with saved sessions, one session per
tab, and PuTTY/X11 mouse behaviour. Sessions are a local shell, SSH through the
**system `ssh`** (zterminal does not implement SSH), or a serial port.

- Repository: `sbj-ee/zterminal` (public). License: MIT (`LICENSE`); bundled
  libvterm is MIT (`third_party/libvterm/LICENSE`).
- Deliverables per release: `zterminal_<ver>_amd64.deb`,
  `zterminal-<ver>-Darwin.dmg`, `SHA256SUMS`, `SHA256SUMS.minisig`.
- macOS bundle id: `ee.sbj.zterminal`.

## 2. Feature set (as implemented)

Behavioural reference: README "Features" and the sections it links. Summary:

| Area | Implemented behaviour | Main code |
|---|---|---|
| Sessions | Local (`$SHELL`, `TERM=xterm-256color`, `COLORTERM=truecolor`; macOS: login shell in `$HOME`), SSH (system `ssh`, validated argv, host after `--`, optional jump host / key file / extra args), serial (QSerialPort, 9600 8N1 default) | `core/Pty`, `core/Session*`, `core/SerialBackend`, `app/SessionWidget` |
| Tabs / windows | One independent session per tab; multiple windows per process share one vault unlock | `app/MainWindow`, `app/SessionWidget` |
| Emulation | libvterm: 16/256/truecolor, bold, **faint (SGR 2, via local patch 14)**, italic, underline, reverse, strike, alt screen, wide chars, bracketed paste, mouse 1000/1002/1003/1006, focus reporting 1004, DECSCUSR cursor shapes | `core/Terminal`, `core/Contrast`, `app/TerminalView` |
| Scrollback | 100,000 lines/tab default or Unlimited; zlib-compressed | `core/Scrollback` |
| Selection / clipboard | Select copies to PRIMARY (+ CLIPBOARD unless off); right-click pastes CLIPBOARD; middle-click configurable; Shift overrides mouse grab; ⌘C/⌘V on macOS | `core/Selection`, `core/MouseSettings` |
| Safe paste | Multi-line paste confirmation; C0 (except tab/newlines), DEL, C1, ESC stripped | `core/PasteGuard`, `app/PasteConfirmDialog` |
| Titles | OSC 0/2 cut at 4 KB; control and bidi formatting characters removed; window title `zterminal <ver> — <session>` | `core/WindowTitle` |
| Find | Ctrl+Shift+F over scrollback + screen; case, regex, wrap, highlight all, count | `core/TerminalSearch`, `app/FindBar` |
| Saved sessions | One INI per session; never a password | `core/SessionStore` |
| Session import/export | JSON `format: zterminal-sessions`, `formatVersion` 1; imports are untrusted: review dialog, ssh-option allowlist, "Use stored password" switched off, unreviewed imports marked unapproved | `core/SessionExport`, `app/ImportReviewDialog` |
| Session groups | Named ordered list of saved sessions opened as tabs (stagger, focus tab, new/this window); `zt --group` | `core/GroupStore`, `app/Group*` |
| Password vault | Optional; Argon2id + XChaCha20-Poly1305 (libsodium); passwords bound to the real target; SSH via `zterminal-askpass` over a private one-shot Unix socket; serial "Send Stored Login" | `core/Vault*`, `core/SecureBuffer`, `core/AskpassServer`, `askpass/`, `app/Vault*` |
| Session logging | Plain text, escapes stripped, full-screen programs omitted, paused at password prompts | `core/SessionLog`, `core/LogTextFilter` |
| Keepalive / reconnect | SSH keepalives (30 s × 3 default); Reconnect banner; optional auto-reconnect with backoff | `core/Reconnect` |
| Themes | Built-in xterm, PuTTY, Solarized Dark, Boilermakers, Badgers, Packers; custom `*.ztheme.json` themes; zmail themes listed read-only | `core/ColorScheme`, `core/BrandThemes.h`, `core/ThemeFile` |
| Theme editor | Duplicate/edit palette roles, 16 ANSI colours, cursor/selection colours, cursor style/blink, optional font; live preview; save/rename/delete/apply/import/export | `app/ThemeEditorDialog` |
| Preferences | PuTTY-style settings applied live across windows (file watcher on the INI) | `app/PreferencesDialog`, `app/AppSettings` |
| Updates | Startup check (≤ once/day, can be disabled) + Help → Check for Updates; signature-verified install (§9) | `core/Update*`, `core/Minisign`, `app/Update*` |
| Restart after update | Reopens every window with all tabs via a private restore file (`--restore FILE`) | `core/RestoreFile` |
| Process hardening | Release builds disable core dumps; Linux sets non-dumpable; children get the original limit back | `core/ProcessHardening` |
| CLI | `zterminal`/`zt`: local shell, `<session>`, `ssh …`, `serial DEV [BAUD]`, `-e prog`, `--group/-g`, `--list-sessions`, `--list-groups`, `--check-session`, `--check-group`, `--version`, `--help` | `core/CommandLine`, `app/main.cpp`, `packaging/zt` |

`zt` (Linux only, installed to `/usr/bin/zt`) is a POSIX `sh` launcher that
runs `--help`/`--version` in the foreground and otherwise starts `zterminal`
detached (`setsid -f`, fallback `nohup`). `ZTERMINAL_BIN` overrides the binary
(tests).

## 3. Non-goals

Deliberate, from README "Status and known gaps" and `docs/PLAN.md`:

- **No split panes — by design.** One session per tab, tabs only. The
  maintainer uses tmux for splits; split panes are not planned.
- No hand-off to a running instance (a separately started `zt` is its own
  process with its own vault unlock); no `zterminal://` URLs.
- No built-in SSH or Telnet/raw TCP implementation; no PuTTY session import.
- Not implemented: OSC 52 clipboard write (OSC 52 read is never allowed), OSC 8
  hyperlinks / click-to-open URLs, sixel/kitty graphics, vttest/esctest
  conformance baseline.
- Platforms: no Windows, no Intel Mac, no Linux arm64 package.
- macOS app is ad-hoc signed, not Developer ID signed or notarized.
- Session groups are launch lists only (no per-member title/command/cwd,
  nesting, sidebar, or group export/import).

## 4. Architecture and modules

### 4.1 Directory layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Top level: version, C++ standard, options, `find_package`, subdirectories |
| `cmake/` | `CompilerWarnings.cmake`, `Sanitizers.cmake`, `Hardening.cmake`, `Packaging.cmake` (install + CPack), `MacBundle.cmake`, `MacDeploy.cmake.in`, `Info.plist.in` |
| `src/version.hpp.in` | Template for `build/generated/version.hpp` (`kVersionString`, major/minor/patch, `kRepoUrl`) |
| `third_party/libvterm/` | Bundled libvterm 0.3.3 + local patches; zterminal's own `CMakeLists.txt` and `README.zterminal.md` |
| `core/` | Static lib `zterminal_core`: no widgets (Qt Core/Gui/SerialPort/Network) |
| `app/` | Static lib `zterminal_ui` (Qt Widgets) + executable `zterminal` (`main.cpp`) |
| `askpass/` | Plain C11 `zterminal-askpass` + static lib `zterminal_askpass_policy` |
| `tests/` | Qt Test suites, CLI tests, fixtures (`tests/data/`), `scripts/colortest.sh` |
| `fuzz/` | libFuzzer harnesses + seed corpus |
| `packaging/` | `zt` launcher, `zterminal.desktop` |
| `assets/icons/` | `zterminal-{16,24,32,48,64,128,256,512,1024}.png` |
| `scripts/check-version.sh` | Version-drift check (§12) |
| `tools/macos/check-bundle.sh` | `.app` linkage/rpath/signature check |
| `tools/sanitizers/lsan.supp` | LeakSanitizer suppressions |
| `docs/` | `PLAN.md` (design notes), `RELEASING.md`, `HARDENING.md`, `THEMES.md`, screenshots |
| `.clang-tidy` | Static-analysis checks (`bugprone-*`, `cert-*`, `clang-analyzer-*` minus listed exclusions) |

### 4.2 Build targets

| Target | Type | Language / standard | Links |
|---|---|---|---|
| `vterm` | static | C99, PIC, warnings off (`-w`) | — |
| `zterminal_core` | static | C++20 | `Qt6::Core Gui SerialPort Network`, `vterm`, `PkgConfig::SODIUM`, private `util` (forkpty) |
| `zterminal_askpass_policy` | static | C11, PIC | — |
| `zterminal-askpass` | executable (output dir `build/app/`) | C11 | `zterminal_askpass_policy` (no Qt) |
| `zterminal_ui` | static | C++20 | `zterminal_core`, `Qt6::Gui Widgets` |
| `zterminal` | executable (macOS: `.app` bundle) | C++20 | `zterminal_ui`; depends on `zterminal-askpass` |
| `zterminal-icns` | custom (macOS) | — | `iconutil` → `zterminal.icns` |
| `tst_*` | tests (§6.3) | C++20 | core/ui + `Qt6::Test` |
| `fuzz_*` | libFuzzer (opt-in) | C++20 | `zterminal_core`, `-fsanitize=fuzzer` |

CMake settings: `CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_STANDARD_REQUIRED ON`,
`CMAKE_CXX_EXTENSIONS OFF`, `CMAKE_AUTOMOC ON`,
`CMAKE_EXPORT_COMPILE_COMMANDS ON`; default build type `RelWithDebInfo` when
none is given (single-config generators).

### 4.3 Main classes

- **core:** `Pty` (forkpty, non-blocking I/O), `Terminal` (libvterm wrapper,
  callbacks, cell attributes incl. dim), `Contrast` (faint blending and WCAG
  contrast floors), `Scrollback`, `Selection`, `TerminalSearch`, `PasteGuard`,
  `WindowTitle`, `ColorScheme` + `BrandThemes.h` + `ThemeFile` (ztheme JSON,
  byte-identical with zmail's copy), `MouseSettings`, `CommandLine`
  (`LaunchRequest`), `Session`/`SessionStore`/`SessionExport`,
  `Group`/`GroupStore`, `SerialBackend`, `Vault`/`VaultBindings`/`SecureBuffer`,
  `AskpassServer`, `SessionLog`/`LogTextFilter`, `Reconnect`, `RestoreFile`,
  `ProcessHardening`, `Update` (`SemVer`, `ReleaseInfo`, asset selection,
  `UpdatePolicy`, `UpdateState`), `UpdateChecker`/`UpdateDownloader`
  (QtNetwork), `Minisign` (Ed25519 verify via libsodium), `UpdateSigningKey`,
  `ConfigPaths.hpp` (`configHome()`).
- **app:** `MainWindow`, `TerminalView`, `SessionWidget`, `FindBar`,
  `PreferencesDialog`, `ThemeEditorDialog`, `AppSettings`, `SessionDialog`,
  `ImportReviewDialog`, `GroupDialog`/`GroupOpener`/`GroupPicker`,
  `VaultManager`/`VaultDialogs`, `PasteConfirmDialog`,
  `UpdateDialog`/`UpdateManager`; `main.cpp` sets
  `Qt::AA_MacDontSwapCtrlAndMeta` (physical Ctrl stays Ctrl on macOS),
  organisation/application name `zterminal`, desktop file name `zterminal`, and
  `QSettings::IniFormat` as default.

### 4.4 libvterm vendoring and local patches

- Upstream libvterm **0.3.3** from `libvterm-0.3.3.tar.gz`, sha256
  `09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`.
  Imported: `include/`, `src/` (with pre-generated `src/encoding/*.inc` and
  `src/fullwidth.inc`), `LICENSE`. Not imported: upstream `Makefile`, `bin/`, `t/`.
- Built from the nine `src/*.c` files listed in
  `third_party/libvterm/CMakeLists.txt` as static `vterm`.
- **Fourteen local patches**, each marked `zterminal patch` in the source and
  documented in `third_party/libvterm/README.zterminal.md`:
  1–13 are memory-safety / UB / hang fixes found by fuzzing (out-of-bounds
  read and `abort()` in `resize_buffer`, wide-char NULL write, CSI/OSC numeric
  overflow, CSI argument heap overflow, scroll-region `memmove` after shrink,
  OSC/DCS length underflow, REP infinite loop, DECSC/DECRC after shrink, DECRQSS
  signed shift, `moverect_internal` NULL `memmove`, C1-as-UTF-8 negative
  column, double-width line on a 1-column screen).
  **14** adds the faint attribute (SGR 2): `VTERM_ATTR_DIM`,
  `VTERM_ATTR_DIM_MASK`, `VTermScreenCellAttrs.dim` (appended after existing
  attributes); SGR 2 sets, SGR 22/SGR 0 clear, DECSC/DECRC save/restore,
  DECRQSS reports. Rendered by `core/Contrast.hpp` (foreground blended toward
  the cell background keeping 66 %, then held to ≥ 3:1 contrast).
- Regression tests: `tst_terminal` (`resizeAfterOutputDoesNotAbort`,
  `wideCharInOneColumn`, `hugeCsiArguments`, `tooManyCsiArguments`,
  `scrollRegionAfterShrink`, `oscEscC0AtChunkEnd`, `repWithoutPreviousChar`,
  `restoreCursorAfterShrink`, `utf8C1DoesNotMoveCursorBack`,
  `doubleWidthLineInOneColumn`, `dim*`), `tst_scrollback::dimSurvivesScrollback`.
- **Updating libvterm:** replace `include/` and `src/`, update version and hash
  in `README.zterminal.md`, re-apply (or confirm upstream fixed) all 14 patches,
  run the full suite including the Sanitizers and Fuzz jobs.

### 4.5 Vault

- File `<config>/zterminal/vault.bin`, format v1 (little-endian): magic
  `ZTVAULT\0` (8) · version u16 = 1 · KDF id u8 = 1 (`crypto_pwhash`
  Argon2id13) · AEAD id u8 = 1 (`crypto_aead_xchacha20poly1305_ietf`) · opslimit
  u64 · memlimit u64 · salt 16 · nonce 24 (fresh per write) · ciphertext + 16-byte
  tag. The whole 68-byte header is AEAD associated data.
- Plaintext: u32 count, then `{u16 keyLen, key, u32 valLen, value}`; keys
  `ssh-password/<session>`, `serial-password/<session>`.
- Production KDF: memlimit 256 MiB, opslimit calibrated once; files with
  opslimit > 100 or memlimit > 1 GiB are refused.
- Secrets live in `SecureBuffer` (`sodium_malloc`, mlock, zeroed on free).
  Pending deletions while locked: `vault.bin.pending-deletions` (names only).
- KDF work runs on a worker thread (`Vault::adopt`) so the UI keeps painting.

### 4.6 Askpass (SSH stored passwords)

ssh is started with `SSH_ASKPASS=<helper>`, `SSH_ASKPASS_REQUIRE=force` and a
private socket (`ZTERMINAL_ASKPASS_SOCKET`, in a `zterminal-askpass-XXXXXX`
temp dir). The helper (plain C, no Qt) is looked up next to the binary, in
`../libexec/zterminal/` (Linux package) or `../Helpers/`; the macOS bundle
places it in `Contents/MacOS/`. It answers only the target's own password
prompt as resolved by `ssh -G` (rules in README "Password vault"). The
`ZTERMINAL_ASKPASS` override is honoured only in `ZTERMINAL_DEV_OVERRIDES`
builds.

### 4.7 Session import (untrusted input)

Import parses `zterminal-sessions` JSON (`formatVersion` 1), shows every
session's real target, jump host, extra options and replacements in
`ImportReviewDialog`, and refuses extra ssh arguments outside the allowlist in
README "Importing sessions" (e.g. `ProxyCommand`,
`StrictHostKeyChecking=no`). Fuzzed by `fuzz_session_import`.

### 4.8 Themes and theme editor

Built-in schemes in `core/ColorScheme` / `core/BrandThemes.h`; brand palettes
and contrast rules in [docs/THEMES.md](docs/THEMES.md). Custom themes are
`<name>.ztheme.json` (`format: "ztheme"`, `version: 1`, `name`, `palette`
required; optional `basedOn`, `terminal`, `fonts`, `ui`; ≤ 64 KB; unknown keys
ignored). Ids: `custom:<stem>` (own), `zmail:<stem>` (zmail's, read-only).
`core/ThemeFile.{h,cpp}` must stay byte-identical with zmail's
`src/ui/ThemeFile.{h,cpp}`; shared fixtures in `tests/data/themes/`.

### 4.9 Updater

See §9.

## 5. Toolchain and dependencies

### 5.1 Declared minimums (in the build files)

| Item | Requirement | Where |
|---|---|---|
| CMake | ≥ **3.21** (`CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE` needs ≥ 3.23 to take effect) | `CMakeLists.txt`, `cmake/Packaging.cmake` |
| C++ | **C++20**, no extensions | `CMakeLists.txt` |
| C | C11 (askpass), C99 (libvterm) | `askpass/`, `third_party/libvterm/` |
| Compiler | GCC or Clang (warning, hardening and sanitizer flags are GCC/Clang syntax); fuzzing needs Clang | `cmake/*.cmake` |
| Qt | **Qt 6 ≥ 6.4**: Core, Gui, Widgets, SerialPort, Network; Test for tests; DBus optional (Linux `tst_globalmenu`) | `CMakeLists.txt`, `tests/CMakeLists.txt` |
| libsodium | ≥ **1.0.18** via pkg-config | `CMakeLists.txt` |
| pkg-config | required | `CMakeLists.txt` |
| libutil | `forkpty` (`util`) | `core/CMakeLists.txt` |
| Generator | Ninja (used everywhere in docs and CI; not enforced) | README, workflows |
| macOS | arm64 only (`CMAKE_OSX_ARCHITECTURES=arm64` forced before `project()` unless given); deployment target / `LSMinimumSystemVersion` **14.0**; `iconutil`, `macdeployqt` required | `CMakeLists.txt`, `cmake/MacBundle.cmake` |

### 5.2 What CI actually uses (observed in CI run 44205630356, main `3587e8c`, 2026-11-17)

Nothing below is pinned except the runner labels and the Actions SHAs; package
versions float with the runner image, Ubuntu archive and Homebrew.

| | Linux (`ubuntu-24.04`) | macOS (`macos-15`) |
|---|---|---|
| Runner image | `ubuntu-24.04` 20261111.x | `macos-15-arm64` 20261104.0514.1 |
| Install | `apt-get install qt6-base-dev qt6-qpa-plugins libgl1-mesa-dev cmake ninja-build g++ dpkg-dev fonts-dejavu-core xvfb xauth dbus qt6-serialport-dev socat libsodium-dev pkg-config` | `brew install qt cmake ninja libsodium pkg-config socat` |
| Compiler | GCC 13.3.0 (Linux/Sanitizers jobs); Clang 18.1.3 (clang-tidy/Fuzz jobs) | AppleClang 17.0.0 (Xcode 16.4) |
| Qt | 6.4.2 (`6.4.2+dfsg-21.1build5`) | 6.12.0 (Homebrew `qtbase 6.12.0_2`, `arm64_sequoia` bottles) |
| libsodium | 1.0.18 (`1.0.18-1ubuntu0.24.04.1`) | 1.0.22 |
| CMake | apt `3.28.3` installed (runner may also provide its own; which one runs is not logged) | Homebrew 4.4.5 |
| Ninja | 1.11.1 | 1.13.2 |
| clang-tidy | 18 (`1:18.0-59~exp2`) | — |

#### Toolchain drift (documented, deliberately not pinned)

The toolchain is **intentionally left unpinned** (maintainer decision,
2026-11-19). Each CI or release run takes whatever the runner image, the Ubuntu
24.04 archive and Homebrew provide on that day. As a result:

- **Qt differs by platform.** Linux builds against Ubuntu 24.04's Qt **6.4.2**
  (the minimum the build accepts is 6.4). macOS builds against Homebrew's
  current `qt` (**6.12.0** for 1.7.4), which changes whenever Homebrew updates
  it. A feature or behaviour change in Qt after 6.4 can therefore show up on
  macOS only. Code must still build and pass on 6.4.2.
- **Xcode / AppleClang is the runner default** (`macos-15` image; Xcode 16.4 /
  AppleClang 17 for 1.7.4). There is no `xcode-select` step.
- **Runner images** follow the `ubuntu-24.04` and `macos-15` labels. Only
  the labels are fixed, not image versions.
- **macOS 14 minimum is declared, not tested.** `CMAKE_OSX_DEPLOYMENT_TARGET`
  and `LSMinimumSystemVersion` are 14.0, but CI runs on macOS 15. Homebrew
  pours `arm64_sequoia` (macOS 15) bottles there, and those are what
  `macdeployqt` bundles. Nothing in CI launches the `.dmg` on macOS 14. The
  `CMakeLists.txt` comment about "bottles built for macOS 14" describes the
  intent, not something verified.
- **libsodium** is 1.0.18 on Linux and Homebrew's current version (1.0.22) in
  the macOS bundle.
- **CMake on Linux:** apt installs 3.28.3. If the runner image also has its
  own CMake earlier on `PATH`, that one runs; the logs don't record which.
- To see exactly what a given release used, read its release run log:
  runner image version, `The CXX compiler identification is …`, the apt
  `Setting up …` lines, and Homebrew's `Pouring …` lines.

### 5.3 Runtime dependencies

- **.deb:** `Depends: qt6-wayland, libqt6serialport6, libsodium23` plus what
  `dpkg-shlibdeps` adds (CI asserts `libqt6network6`); `Recommends: pkexec |
  policykit-1`. Serial use needs membership of `dialout`.
- **.dmg:** self-contained `zterminal.app` (Qt frameworks/plugins and
  libsodium copied by `macdeployqt`); no Homebrew paths may remain
  (`tools/macos/check-bundle.sh`).
- Both: system `ssh` for SSH sessions.

## 6. Configure, build and test

### 6.1 CMake options

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` if unset | CI/release: `Release` |
| `ZTERMINAL_BUILD_TESTS` | `ON` | Qt Test suite + CLI tests |
| `ZTERMINAL_WERROR` | `OFF` | `-Werror` on zterminal's own targets (CI Linux/macOS: ON) |
| `ZTERMINAL_HARDENING` | `ON` | PIE, `-fstack-protector-strong`, `-fstack-clash-protection` (not macOS), `-fcf-protection=full` (x86_64), `_FORTIFY_SOURCE=3` Linux / `2` macOS in non-Debug non-sanitizer builds, Linux `-z relro -z now -z noexecstack` |
| `ZTERMINAL_SANITIZE` | `""` | e.g. `address,undefined`; adds `-fno-omit-frame-pointer -fno-sanitize-recover=undefined` |
| `ZTERMINAL_FUZZ` | `OFF` | Clang only; builds `fuzz/` with `fuzzer-no-link,address,undefined` |
| `ZTERMINAL_DEV_OVERRIDES` | `ON` only for `Debug` | Compiles in env overrides (`ZTERMINAL_ASKPASS`, `ZTERMINAL_UPDATE_URL`); **must be OFF for packages** |
| `CMAKE_OSX_ARCHITECTURES` | `arm64` on macOS hosts | |
| `CMAKE_OSX_DEPLOYMENT_TARGET` | `14.0` on macOS hosts | |

Warnings for own C++ targets: `-Wall -Wextra -Wpedantic -Wshadow
-Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
-Wnull-dereference`; C targets: `-Wall -Wextra`; libvterm: `-w`.

### 6.2 Commands

Linux (Debian/Ubuntu 24.04):

```sh
sudo apt-get install -y qt6-base-dev qt6-qpa-plugins qt6-serialport-dev libsodium-dev pkg-config \
  libgl1-mesa-dev cmake ninja-build g++ dpkg-dev fonts-dejavu-core xvfb xauth dbus socat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DZTERMINAL_WERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
cd build && cpack -G DEB        # -> zterminal_<ver>_amd64.deb
```

macOS (Apple Silicon, Homebrew):

```sh
brew install qt cmake ninja libsodium pkg-config socat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DZTERMINAL_WERROR=ON \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0
cmake --build build
ctest --test-dir build --output-on-failure
cd build && cpack -G DragNDrop  # -> zterminal-<ver>-Darwin.dmg
```

(The release workflow configures without `-DZTERMINAL_WERROR=ON`; see §14.)

Sanitizers (CI job "Sanitizers"):

```sh
cmake -B build-san -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DZTERMINAL_SANITIZE=address,undefined
cmake --build build-san
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
LSAN_OPTIONS=suppressions=$PWD/tools/sanitizers/lsan.supp:print_suppressions=0 \
  ctest --test-dir build-san --output-on-failure
```

clang-tidy (CI job "clang-tidy"):

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DZTERMINAL_BUILD_TESTS=OFF
cmake --build build
run-clang-tidy -quiet -p build "$PWD/(core|app|askpass)/.*\.(cpp|c)$"
```

Fuzzing (CI job "Fuzz"):

```sh
cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DZTERMINAL_FUZZ=ON -DZTERMINAL_BUILD_TESTS=OFF
cmake --build build-fuzz --target fuzz_vault fuzz_session_import fuzz_terminal fuzz_paste_title fuzz_update_json
cp -r fuzz/corpus/terminal /tmp/corpus
QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 \
  build-fuzz/fuzz/fuzz_terminal -max_total_time=60 -rss_limit_mb=2048 -timeout=10 /tmp/corpus
```

### 6.3 Test suite

- **30 Qt Test executables** registered by `zterminal_add_test()`
  (`tst_terminal`, `tst_selection`, `tst_pty`, `tst_misc`, `tst_themefile`,
  `tst_view`, `tst_settings`, `tst_sessions`, `tst_sessionui`, `tst_groups`,
  `tst_restore`, `tst_serial`, `tst_serialui`, `tst_vault`, `tst_askpass`,
  `tst_hardening`, `tst_vaultui`, `tst_sessionlog`, `tst_logui`,
  `tst_pasteguard`, `tst_safepaste`, `tst_scrollback`, `tst_find`,
  `tst_scrollbench`, `tst_reconnect`, `tst_minisign`, `tst_update`, `tst_tabs`,
  `tst_groupui`, `tst_vaultshared`) plus **`tst_globalmenu`**, built only when
  `Qt6::DBus`, `xvfb-run` and `dbus-run-session` are found (Linux CI requires
  it; macOS CI requires it to be absent).
- **22 CLI tests** (`zterminal_version`, `zt_version`, `zt_help`,
  `--list-sessions`/`--list-groups`/`--check-*` against the read-only fixture
  config `tests/data/config`, error-path tests).
- **Headless environment:** every suite runs with `QT_QPA_PLATFORM=offscreen`
  and its own `XDG_CONFIG_HOME=<build>/tests/config-<name>` (never the real
  config) and gets `ZTERMINAL_EXPECTED_VERSION=<PROJECT_VERSION>`. Default
  timeout 60 s (`tst_update`, `tst_tabs`, `tst_logui`, `tst_vaultui`,
  `tst_vaultshared` 120 s; `tst_reconnect`, `tst_groupui` 180 s;
  `tst_scrollbench` 300 s). `tst_globalmenu` uses `QT_QPA_PLATFORM=xcb` under
  `dbus-run-session -- xvfb-run -a`.
- **socat** is required for the serial suites and Send Stored Login; CI fails
  if `tst_serial*`, `tst_vault*`, `tst_askpass`, `tst_sessionlog`, `tst_logui`,
  `tst_pasteguard`, `tst_safepaste`, `tst_tabs`, `tst_scrollback`, `tst_find`,
  `tst_scrollbench`, `tst_reconnect`, `tst_update`, `tst_groups`,
  `tst_groupui` report any `SKIP`.
- Minisign test fixtures: `tests/data/minisign/` (test key only, never the
  release key). Update JSON fixtures: `tests/data/update/`.
- Manual: `tests/scripts/colortest.sh` inside zterminal (16/256/truecolor).

## 7. Packaging

`cmake/Packaging.cmake` (CPack); version only from `PROJECT_VERSION`.
Vendor/contact: `Stephen B. Johnson <49662809+sbj-ee@users.noreply.github.com>`;
homepage `https://github.com/sbj-ee/zterminal`.

### 7.1 Linux `.deb` (`cpack -G DEB`)

File name `zterminal_<ver>_amd64.deb` (`DEB-DEFAULT`; the updater depends on
this name). Section `x11`, arch `amd64`, `SHLIBDEPS ON`. Contents:

| Installed path | Source |
|---|---|
| `/usr/bin/zterminal` | app |
| `/usr/bin/zt` | `packaging/zt` |
| `/usr/libexec/zterminal/zterminal-askpass` | askpass |
| `/usr/share/applications/zterminal.desktop` | `packaging/zterminal.desktop` |
| `/usr/share/doc/zterminal/copyright` | `LICENSE` |
| `/usr/share/icons/hicolor/<N>x<N>/apps/zterminal.png` (N = 16…512) | `assets/icons/` |

CI checks the `zt`, `zterminal` and askpass paths, `Depends` (serialport,
sodium, network) and `Recommends` (pkexec).

### 7.2 macOS `.dmg` (`cpack -G DragNDrop`)

File name `zterminal-<ver>-Darwin.dmg`, volume `zterminal <ver>`, no SLA
prompt; `zterminal.app` at the root next to an `Applications` symlink.

- `Info.plist` from `cmake/Info.plist.in`: `CFBundleIdentifier` via
  `@MACOSX_BUNDLE_GUI_IDENTIFIER@` (= `ee.sbj.zterminal`; must not be empty or
  macOS Local Network privacy breaks), versions = `PROJECT_VERSION`,
  `LSMinimumSystemVersion` 14.0, `NSLocalNetworkUsageDescription`,
  category developer-tools.
- `zterminal.icns` generated from the PNGs with `iconutil`;
  `zterminal-askpass` in `Contents/MacOS/`.
- At install time `MacDeploy.cmake` runs `macdeployqt -always-overwrite` with
  libsodium/Homebrew `-libpath`s, writes `Contents/Resources/qt.conf`
  (`Plugins = PlugIns`) if macdeployqt did not (Qt 6.12 no longer does),
  removes `PlugIns/platforminputcontexts`, deletes non-`@` rpaths, rewrites
  install names into the bundle, then **ad-hoc signs** dylibs → frameworks →
  helpers → app (`codesign --force --timestamp=none -s -`, **no**
  `--options runtime`), verifies with `codesign --verify --deep --strict`, and
  runs `tools/macos/check-bundle.sh` (no Homebrew/`/usr/local` links or rpaths,
  consistent ad-hoc signatures, no hardened runtime).
- CI additionally mounts the dmg, checks the plist and signature flags, runs
  `--version`, and launch-smoke-tests the app for 8 s.

## 8. Signing and release

### 8.1 Keys and secrets

- Release signing: **minisign** (Ed25519). Public key (not secret), key ID
  `32960BBA77B36F1F`:
  `RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9`.
  Compiled in as `kUpdateSigningPublicKey` in `core/UpdateSigningKey.cpp`;
  repeated in README and `docs/RELEASING.md`.
- CI secrets (names only): **`MINISIGN_SECRET_KEY`** (whole minisign secret
  key file) and **`MINISIGN_PASSWORD`**, stored only in the protected GitHub
  environment **`release`** (no repository-wide copies).
- `release` environment protection (repository settings, not in git): required
  reviewer **sbj-ee**; deployments limited to tags matching `v*`.
- Key generation/rotation procedure: `docs/RELEASING.md`. Rotation means old
  clients cannot verify new releases and must install once by hand.

### 8.2 Release procedure

1. Every PR already bumped the version and added a CHANGELOG entry (§12). The
   top CHANGELOG heading must be dated `## X.Y.Z — YYYY-MM-DD` before tagging.
2. Merge to `main`, then `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. `.github/workflows/release.yml` (trigger: push of `v*` tag; concurrency
   `release-<ref>`, no cancel):
   - **linux** (`ubuntu-24.04`): `scripts/check-version.sh --tag vX.Y.Z`;
     install deps; configure `Release`; assert tag == `CMAKE_PROJECT_VERSION`;
     build; `ctest`; `cpack -G DEB`; write `SHA256SUMS.linux`; SLSA provenance
     (`actions/attest-build-provenance`); upload artifact
     `zterminal-release-linux`.
   - **macos** (`macos-15`): same checks; configure arm64 / 14.0; build; ctest;
     `cpack -G DragNDrop`; `SHA256SUMS.macos`; mount and `check-bundle.sh` +
     `codesign --verify`; provenance; artifact `zterminal-release-macos`.
   - **publish** (`ubuntu-24.04`, `environment: release`, `contents: write`):
     **waits for sbj-ee to approve the deployment** (Actions → run → Review
     deployments). Concatenates both checksum files into `SHA256SUMS`, runs
     `sha256sum -c`, refuses if `MINISIGN_SECRET_KEY` is empty, signs with
     trusted comment **`zterminal vX.Y.Z SHA256SUMS`**, deletes the temp key,
     re-verifies the signature against the public key read from
     `core/UpdateSigningKey.cpp` **at the tagged commit**, then
     `gh release create vX.Y.Z --title "zterminal X.Y.Z" --generate-notes
     --verify-tag`.
4. Release assets (exactly four):

   ```
   zterminal_X.Y.Z_amd64.deb
   zterminal-X.Y.Z-Darwin.dmg
   SHA256SUMS            # "<sha256>  zterminal_X.Y.Z_amd64.deb" and "<sha256>  zterminal-X.Y.Z-Darwin.dmg"
   SHA256SUMS.minisig
   ```

5. Manual verification:

   ```sh
   minisign -Vm SHA256SUMS -P RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9
   sha256sum -c --ignore-missing SHA256SUMS        # Linux
   shasum -a 256 -c --ignore-missing SHA256SUMS    # macOS
   gh attestation verify zterminal_X.Y.Z_amd64.deb -R sbj-ee/zterminal
   ```

6. Update README `Current release: **X.Y.Z**` (allowed by the version check
   as soon as the tag exists).

## 9. In-app updater

- **Check:** startup check if enabled in Preferences (`updates/checkOnStartup`)
  and the last check is ≥ 24 h old (or the clock went backwards); manual Help →
  Check for Updates. One unauthenticated GET to
  `https://api.github.com/repos/sbj-ee/zterminal/releases/latest`
  (`Accept: application/vnd.github+json`, `User-Agent: zterminal/<ver>`,
  transfer timeout). `ZTERMINAL_UPDATE_URL` overrides only in
  `ZTERMINAL_DEV_OVERRIDES` builds.
- **Compare:** `tag_name` vs `kVersionString` by SemVer (leading `v` allowed,
  numeric compare, pre-release < release, build metadata ignored, Debian `~`
  read as `-`). Drafts are ignored. Dialog: Install / Later / Skip this version
  (skipped tag stored in `update-state.ini`; anything newer is offered again).
- **Assets:** Linux selects `zterminal_<ver>_amd64.deb`, macOS
  `zterminal-<ver>-Darwin.dmg`; both require `SHA256SUMS` and
  `SHA256SUMS.minisig`. Downloads only from
  `https://github.com/sbj-ee/zterminal/releases/download/<tag>/<name>`,
  following redirects only to `release-assets.githubusercontent.com` /
  `objects.githubusercontent.com`.
- **Verify:** minisign signature over `SHA256SUMS` against the compiled-in key
  (`core/Minisign`, libsodium `crypto_sign_verify_detached`, prehashed and
  legacy forms, global signature over the trusted comment); trusted comment
  must equal `zterminal <tag> SHA256SUMS` (prevents replay of an older
  release's signature); package SHA-256 must match its line.
- **Install, Linux:** re-verified private copy; `/usr/bin/pkexec /usr/bin/apt
  install <deb>` (absolute paths). Fallbacks (no pkexec/apt, not running as
  `/usr/bin/zterminal`, pkexec dismissed 126 / not authorised 127, apt failure):
  open the release page and show the verified `.deb` path for a manual
  `sudo apt install`. On success: Restart Now / Later (warning when live
  sessions exist); restart writes a private restore file and launches
  `/usr/bin/zterminal --restore FILE`, which reopens every window and tab.
- **Install, macOS:** same verification, then opens the `.dmg` for a
  drag-to-Applications install (no automated replace).
- Releases before 1.3.0 are unsigned; 1.0.x clients only check SHA-256.

## 10. Runtime files and configuration

Two different roots are used, and they coincide on Linux but **not on macOS**:

- **`<config>`**: `zterminal::configHome()` (`core/ConfigPaths.hpp`). It is
  `$XDG_CONFIG_HOME` if set (on any platform), otherwise
  `QStandardPaths::writableLocation(GenericConfigLocation)`. Sessions, groups,
  the vault, update state and themes live under `<config>/zterminal/`.
- **`<ini>`**: the location of the global preferences file. `main.cpp` sets
  `QSettings::setDefaultFormat(IniFormat)` with organisation and application
  `zterminal`, and `AppSettings` uses default-constructed `QSettings`. For
  IniFormat in user scope Qt uses `$XDG_CONFIG_HOME`, or `$HOME/.config` when
  that is unset. Qt does this on **macOS as well** (only XDG platforms go
  through `QStandardPaths` there). The file is `<ini>/zterminal/zterminal.ini`.

| Platform | `<config>` (no `XDG_CONFIG_HOME`) | `<ini>` (no `XDG_CONFIG_HOME`) |
|---|---|---|
| Linux | `~/.config` | `~/.config` |
| macOS | `~/Library/Preferences` (Qt's `GenericConfigLocation` on macOS) | `~/.config` |

Observed on a Mac running 1.7.4: `~/.config/zterminal/zterminal.ini`;
`~/Library/Preferences/zterminal/` with `sessions/`, `update-state.ini`,
`vault.bin` and `vault.bin.lock`; no `~/Library/Application Support/zterminal`.
A `~/Library/Preferences/ee.sbj.zterminal.plist` may also exist. zterminal's
code never writes it: there is no native-format `QSettings`, and IniFormat
has been the default since the first commit. It is the app's macOS
user-defaults domain (named after the bundle id introduced on 2026-10-05).
Most likely AppKit / Qt's Cocoa platform plugin creates it. zterminal does not
read it, and it can be ignored.

With `XDG_CONFIG_HOME` set (the test suite sets it per test), both roots are
that directory on both platforms.

| File | Format | Notes |
|---|---|---|
| `<ini>/zterminal/zterminal.ini` | QSettings INI (org `zterminal`, app `zterminal`) | Global preferences: `appearance/*`, `clipboard/*`, `logging/*`, `mouse/*`, `terminal/scrollbackLines`, `updates/checkOnStartup`, `vault/autoLock*`, `view/menuBarVisible` (see `app/AppSettings.cpp`); watched with `QFileSystemWatcher` so all windows apply changes |
| `<config>/zterminal/sessions/<name>.ini` | INI, owner-only | `[session]` name/type/approved/autoReconnect, `[ssh]` host/user/port/keyFile/jumpHost/extraArgs/keepalive*/authFromVault, `[serial]` device/baud/data/parity/stop/flow/delays/localEcho/loginUser/enterSends, `[appearance]` per-session scheme/font. Never a password |
| `<config>/zterminal/groups/<name>.ini` | INI, owner-only | `[group]` name, `version=1`, `openIn`, `staggerMs` (0–5000, default 250), `focusIndex`; `[member]` array of session names |
| `<config>/zterminal/vault.bin` | binary (§4.5) | plus `vault.bin.lock` (`flock` for read-modify-write across processes) and `vault.bin.pending-deletions` |
| `<config>/zterminal/update-state.ini` | INI | last check time, skipped version |
| `<config>/zterminal/themes/*.ztheme.json` | JSON (§4.8) | own custom themes |
| `<config>/zmail/themes/*.ztheme.json` | JSON | zmail's themes, read-only |
| `~/zterminal-logs/<session>-<YYYYMMDD-HHMMSS>.log` | plain text | folder configurable (`logging/directory`) |
| `$TMPDIR/zterminal-restore-XXXXXX.json` | JSON `format: zterminal-restore` | one-shot, consumed by `--restore` |
| `$TMPDIR/zterminal-askpass-XXXXXX/s` | Unix socket | one-shot, per ssh launch |

Environment variables read: `XDG_CONFIG_HOME`, `SHELL`; tests/dev only:
`ZTERMINAL_BIN` (zt), `ZTERMINAL_SERIAL_BY_ID_DIR`, and with
`ZTERMINAL_DEV_OVERRIDES` `ZTERMINAL_ASKPASS`, `ZTERMINAL_UPDATE_URL`.

## 11. CI and Dependabot

All workflows: top-level `permissions: contents: read`; every action pinned to
a full commit SHA with its version in a comment (checkout v7.0.1,
upload-artifact v7.0.2, download-artifact v8.0.2,
attest-build-provenance v4.2.2, cache/restore + cache/save v6.1.0).

### 11.1 `ci.yml` — on push to `main`, every pull request, and `workflow_dispatch`

Concurrency `ci-<workflow>-<ref>`, cancel in progress. Six jobs:

| Job (check name) | Runner | What it does |
|---|---|---|
| `version` — **Version consistency** | ubuntu-24.04 | `fetch-depth: 0`; `scripts/check-version.sh` |
| `linux` — **Linux (ubuntu-24.04)** | ubuntu-24.04 | Release + `-Werror`; build; ctest with the must-run/no-skip assertions (§6.3); `cpack -G DEB` + content/Depends checks; uploads `zterminal-deb` |
| `macos` — **macOS arm64** | macos-15 | Release + `-Werror`, arm64, 14.0; ctest (+ `tst_globalmenu` must not exist); `cpack -G DragNDrop`; dmg mount, plist, ad-hoc/no-runtime signature check, `--version`, 8 s launch smoke test; uploads `zterminal-macos-arm64-dmg` |
| `sanitizers` — **Sanitizers (ASan + UBSan, ubuntu-24.04)** | ubuntu-24.04 | RelWithDebInfo, `address,undefined`; full ctest with LSan suppressions |
| `clang-tidy` — **clang-tidy (ubuntu-24.04)** | ubuntu-24.04 | clang build, tests off; `run-clang-tidy` on `core/`, `app/`, `askpass/`; any finding fails |
| `fuzz` — **Fuzz (libFuzzer, 60 s per harness)** | ubuntu-24.04 | five harnesses × 60 s on a copy of `fuzz/corpus/`; uploads `fuzz-crashes` on failure |

### 11.2 `fuzz-nightly.yml`

Cron `17 7 * * *` (07:17 UTC), `workflow_dispatch` (`minutes`, default 20),
and PRs touching the workflow file (1 min, cache read-only). Matrix over the
five harnesses, 90-min timeout; corpus persisted in the Actions cache
(`fuzz-corpus-<harness>-<run_id>`, restored by prefix, minimised with
`-merge=1`); crashes uploaded as `fuzz-crashes-<harness>`.

### 11.3 `release.yml`

See §8.2.

### 11.4 Dependabot

- `.github/dependabot.yml`: ecosystem `github-actions` only, weekly,
  **cooldown 7 days**, minor + patch grouped as `actions`, commit prefix `ci`.
  Majors arrive as separate PRs.
- `.github/workflows/dependabot-auto-merge.yml`: on `pull_request`, only when
  both PR author and actor are `dependabot[bot]`; if every listed dependency is
  `semver-minor`/`semver-patch`, `gh pr merge --auto --merge`; otherwise
  disables auto-merge and leaves the PR for review. No third-party action.
- An auto-merged Dependabot PR does not trigger a push run on `main`; run
  `gh workflow run ci.yml --ref main` by hand.
- Dependabot PRs do not bump the version; they are exempt from the every-PR
  bump rule (§12).

### 11.5 Repository settings (outside git, read via the API on 2026-11-19)

- Branch protection on `main`: "require branches to be up to date" (strict),
  enforced for admins, and since 2026-11-19 **all six `ci.yml` jobs are
  required status checks** (bound to the GitHub Actions app):
  `Version consistency`, `Linux (ubuntu-24.04)`, `macOS arm64`,
  `Sanitizers (ASan + UBSan, ubuntu-24.04)`, `clang-tidy (ubuntu-24.04)`,
  `Fuzz (libFuzzer, 60 s per harness)`. None of them is path-filtered or
  conditional, so they report on every PR, docs-only and Dependabot PRs
  included. Dependabot auto-merge therefore waits for all six. A check name
  that changes in `ci.yml` must be changed in the protection settings too, or
  every PR will wait forever for the old name.
- Merge methods: merge, squash and rebase allowed; auto-merge allowed; head
  branches deleted on merge.
- Environment `release`: required reviewer `sbj-ee`; deployment policy: tags
  `v*`.

## 12. Versioning rules

- **Single source of truth:** `project(zterminal VERSION X.Y.Z)` in
  `CMakeLists.txt`. It flows to `generated/version.hpp` (window title, About,
  `--version`, update comparison), `Info.plist`, CPack file names, and the
  tests (`ZTERMINAL_EXPECTED_VERSION`, `zterminal_version`/`zt_version`).
- **Every pull request bumps the version** (rule of 2026-10-17; README "Version
  bumps", CHANGELOG preamble): **patch** for fixes *and documentation*,
  **minor** for features (SemVer). The same PR adds a `CHANGELOG.md` entry.
  Tags and releases happen separately, only after review. **Exempt:**
  Dependabot's PRs (pinned GitHub Actions updates); they never bump.
- **Also updated by hand in the same PR** (enforced by
  `scripts/check-version.sh`): the top `## X.Y.Z …` heading in `CHANGELOG.md`,
  the README header `**X.Y.Z**`, and the README title example
  `zterminal X.Y.Z — <session>`. README `Current release: **A.B.C**` must name
  either this version or the newest *other* `v*` tag.
- **Changelog dating convention:** the PR dates its own heading,
  `## X.Y.Z — YYYY-MM-DD` (em dash), with the date the PR is opened. If it
  lands on a later day, the date is updated before merge. There are no
  "unreleased" headings: `check-version.sh --tag` refuses an undated top
  heading, and the 1.7.1 release build failed on exactly that. The date
  records when that version was finished. It is **not** changed again at
  release time, because under the every-PR rule a re-dating edit would itself
  need a new version; the GitHub Release records the publication date.
  Versions never released on their own get a suffix such as `(not released
  separately; shipped in 1.7.2)` when the release that ships them is
  prepared.
- **Checks:** `scripts/check-version.sh` (CI job *Version consistency*, every
  push/PR) and `scripts/check-version.sh --tag vX.Y.Z` plus a CMake-cache
  comparison in both release build jobs. These check *consistency*. A check that
  the version actually *increased* against the base branch (job *Version
  increase*, PR-only, Dependabot exempt, not a required check) is proposed in
  PR #69. Until that merges, the increase is a convention only.
- Tags: `vX.Y.Z`, created only after merge.

## 13. Reproducing a release build

To rebuild a released version as closely as the repository allows:

1. Check out tag `vX.Y.Z`.
2. Linux: an `ubuntu-24.04` environment with the apt package list in §5.2;
   configure `-G Ninja -DCMAKE_BUILD_TYPE=Release` (release.yml does not set
   `ZTERMINAL_WERROR`; `ZTERMINAL_DEV_OVERRIDES` is OFF for Release), build,
   `ctest`, `cpack -G DEB`.
3. macOS: `macos-15` arm64 with Homebrew `qt cmake ninja libsodium pkg-config
   socat`, configure as in §6.2 (without `-DZTERMINAL_WERROR=ON` to match
   release.yml), build, `ctest`, `cpack -G DragNDrop`.
4. Compare tool versions with the release run's log (runner image version,
   compiler identification, apt/brew package versions). Builds are **not
   bit-for-bit reproducible**: the toolchain floats (§5.2), the `.dmg` is
   ad-hoc signed per build, and no `SOURCE_DATE_EPOCH` is set.
5. The provenance attestation (`gh attestation verify`) identifies the
   workflow run that produced a published package.

## 14. Open questions / ambiguities

Resolved on 2026-11-19 and folded into the sections above: required status
checks (§11.5), toolchain drift (§5.2, documented and not pinned), macOS file
locations (§10), Dependabot exemption and enforcing the version increase
(§12, PR #69), the changelog dating convention (§12), and the doc wording
fixes (job count, patch count, libvterm comment, README bump wording, PLAN.md
`ZTERMINAL_UPDATE_URL`).

Still open:

1. **Release builds differ from CI builds.** `release.yml` configures without
   `-DZTERMINAL_WERROR=ON`, does not repeat the must-run/no-skip test
   assertions or the `.deb` Depends checks, and does not run the macOS launch
   smoke test. It relies on CI having passed on the merged commit (now
   enforced by the required checks).
2. **CMake ≥ 3.21 vs features used.** `CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE`
   needs 3.23; with 3.21–3.22 the dmg would show a license prompt. Raise the
   minimum on macOS?
3. **Generator.** All docs and CI use Ninja, but nothing enforces it; other
   generators are untested.
4. **`--help` text on macOS.** `usageText()` in `core/CommandLine.cpp` (a
   user-visible string, checked by `tst_groups`) says sessions and groups live
   in `~/.config/zterminal/…`. That is correct on Linux and wrong on macOS
   (`~/Library/Preferences/zterminal/…`, §10). The fix is a code change and
   needs its own PR. `packaging/zt` says the same, but `zt` is installed on
   Linux only.
5. **Theme sharing with zmail on macOS.** zterminal reads zmail's themes from
   `<config>/zmail/themes`, which on macOS is
   `~/Library/Preferences/zmail/themes`. Whether zmail on macOS writes its
   themes there has not been checked.
6. **`docs/PLAN.md` paths.** The historical design notes still give
   `~/.config/zterminal/…` for every file. They are superseded by the README
   and this spec and were left as written.
