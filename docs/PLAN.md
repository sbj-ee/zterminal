# zterminal: plan (draft for Stephen's approval)

> Status: **approved by Stephen, 2026-10-03 6:49 PM CT.** Repo: `sbj-ee/zterminal` (public, MIT). The original planning mockup is [`mockup.png`](mockup.png) (a static drawing, not a build).

**Approved decisions:** C++20, Qt6 Widgets, bundled libvterm, CMake/CPack. **Linux amd64 only** (no macOS) and **.deb only**. **No tabs**: one window per session. Selecting copies to **PRIMARY and CLIPBOARD**. **Right-click pastes CLIPBOARD**. **Ctrl+right-click opens the menu**. **Middle-click is configurable** (default: paste PRIMARY). There is a full menu bar (§3), and the title is `zterminal <ver> — <session>`, with the version coming from CMake. Updates are checked at startup, and the user can install them after a checksum verification (§4.10).

zterminal is a PuTTY-like terminal for Linux. It has a classic menu bar, saved sessions (local shell, SSH through the system `ssh`, serial console), xterm-grade emulation with 16, 256, and 24-bit color, and X11-style mouse copy/paste.

## 1. Recommendation: C++20 + Qt 6 Widgets + libvterm, MIT license

| Option | VT correctness | PRIMARY on X11/Wayland | Serial | License fit (public, MIT) | Fit with siblings |
|---|---|---|---|---|---|
| **Qt6 + libvterm** ✅ | Good. Used by Neovim and emacs-libvterm. Handles truecolor, mouse 1000/1002/1003/1006, bracketed paste (`vterm_keyboard_start_paste`), OSC 52 via `VTermSelectionCallbacks`, altscreen, reflow | `QClipboard::Selection` on xcb, and on Wayland through `zwp_primary_selection_v1` (Qt client since 2018) | `QSerialPort` (Qt6 SerialPort 6.4.2 in noble) | libvterm is **MIT**. Qt is LGPLv3 with dynamic linking. MIT works | Same stack as **zwriter**: CMake 3.21, C++20, Qt6 Widgets, CPack .deb/.dmg |
| Qt + QTermWidget | Konsole-derived and mature, but its emulator is older and less xterm-exact | Same as Qt | Not a backend. It assumes a PTY, so serial needs a pty shim | **GPL-2.0** would force zterminal to GPL | Good on Qt |
| Rust + alacritty_terminal/vte + egui/iced | alacritty_terminal is excellent | egui/iced/winit have weak or no PRIMARY-selection support today, and that is a must-have | `serialport` crate, fine | Apache-2.0/MIT, fine | New language and toolchain, nothing shared |
| C + GTK4 + VTE | Best-in-class (GNOME Terminal) | Native on GNOME | Needs a pty bridge or a custom `VtePty` | LGPL-3.0, OK to link | Different toolkit and language. GTK4 VTE is less customizable for PuTTY mouse semantics |

**Why:** Qt6 plus libvterm is the only option that meets every hard requirement with a permissive license. libvterm is a small, well-tested C99 core with no I/O. It takes bytes in and gives screen-damage callbacks out, so the PTY, ssh, and serial backends all feed it the same way. Its mouse, bracketed-paste, and OSC 52 hooks let us implement PuTTY semantics exactly. Qt gives us PRIMARY selection on X11 and Wayland through one API (`QClipboard::Selection`), `QSerialPort` for the SG250 console, and the same CMake/CPack/CI setup as zwriter. QTermWidget's GPL would block the MIT license the siblings use. Rust GUI toolkits lack PRIMARY support. VTE is excellent but would leave us fighting GTK for mouse and paste behavior and would put zterminal on a third toolkit.

**Sibling check (read-only, public GitHub):** zwriter uses C++20 and Qt6 Widgets (`qt6-base-dev` on ubuntu-latest, `brew qt` on macos-14), AUTOMOC, a single `project(VERSION)` source of truth feeding a generated `version.hpp`, `cmake/Packaging.cmake` (CPack DEB plus DragNDrop), ctest with `QT_QPA_PLATFORM=offscreen`, and MIT. **Correction to the brief:** zedit is *not* Qt. It uses Dear ImGui, GLFW, and OpenGL, with a `core/` + `frontend_imgui/` split, Catch2, and FetchContent. GLFW has no PRIMARY selection, which is another reason not to reuse zedit's stack. zterminal will copy zedit's `core/` (GUI-free, unit-tested) + `app/` layout and zwriter's Qt/CMake/CPack setup.

**libvterm sourcing:** Ubuntu 24.04 (Pop!_OS 24.04 base) ships `libvterm-dev 0.3.3`. To keep the version pinned, we vendor 0.3.3 in `third_party/libvterm` (MIT, about 10 C files), the same pattern as zedit's vendored `stb`. The build can switch to the system copy.

## 2. Scope

### v1 (Linux amd64 only)
- **Sessions:** local shell (`$SHELL`, login optional) and **SSH via `/usr/bin/ssh`** in a PTY. Fields: host, user, port, identity file, `-J` jump host, extra args. `~/.ssh/config` aliases work as-is. zterminal never stores passwords. **Serial:** device, baud, data/parity/stop, flow control, plus optional per-character and per-line paste delay for Cisco consoles. Break key (`QSerialPort::setBreakEnabled`) for ROMMON.
- **Emulation:** xterm-256color, `COLORTERM=truecolor`, 16/256/24-bit color, bold/italic/underline/reverse, altscreen, wide/combining chars, mouse modes 1000/1002/1003/1006, bracketed paste, title (OSC 0/2). **OSC 52 write** is off by default and can be enabled per session. OSC 52 *read* is never allowed.
- **Mouse/clipboard (Stephen's spec):** see §4.6.
- **Scrollback:** configurable (default 10,000 lines), wheel, Shift+PgUp/PgDn, reflow on resize.
- **Appearance:** font family and size (Ctrl +/−), cursor shape and blink, color schemes (PuTTY default, xterm, Solarized dark/light, user-defined palette of 16 colors plus fg/bg).
- **UI:** a classic menu bar (File, Edit, View, Session, Settings, Help; see §3) that can be hidden from View. A PuTTY-style session dialog (list, load, save, delete, open), one window per session (tabs later).
- **Find** (moved into v1): a find bar over the screen and scrollback. Plain or regex, case toggle, all matches highlighted, Enter/Shift+Enter for next/previous, Esc closes.
- **Version everywhere:** the window title reads `zterminal 0.1.0 — <session name>`. About and Check for Updates show the same build version (§4.9).
- **Updates:** a check at startup (can be turned off in Preferences) plus Help → Check for Updates. On a newer release the user picks **Install / Later / Skip this version**. Install downloads the `.deb`, verifies it against `SHA256SUMS`, and runs `pkexec apt install` (§4.10).
- **`zt` launcher:** the `.deb` installs `/usr/bin/zt`, a tiny POSIX `sh` wrapper (§4.11) that starts zterminal detached, so the calling shell gets its prompt back. Usage: `zt` (local shell), `zt <saved-session>`, `zt ssh user@host [ssh args]`, `zt --version`, `zt --help`.
- **Packaging:** **`.deb` only**, via CPack (zwriter style, depends on `qt6-wayland`). The file is named `zterminal_<ver>_amd64.deb`, and a tag push publishes it with `SHA256SUMS` to GitHub Releases.

### Later
Tabs and splits, session logging to a file (PuTTY "All session output"), hyperlinks (OSC 8), URL click, sixel/kitty graphics, Telnet/raw TCP, import of PuTTY sessions, keyword highlighting, signed releases (minisign/GPG). macOS is **not planned** (Stephen: Linux only).

## 3. Menus (Stephen's spec)

A classic `QMenuBar`. Every item is a `QAction` owned by an `Actions` registry in `MainWindow`, so the menu, the Ctrl+right-click menu, and shortcuts all trigger the same action. Items that don't apply are disabled; for example, Send Break is greyed out unless the session is serial.

| Menu | Items (default shortcut) |
|---|---|
| **File** | New Session… (Ctrl+Shift+N), Open Saved Session… (Ctrl+Shift+O), Save Session (Ctrl+Shift+S), Close (Ctrl+Shift+W), Quit (Ctrl+Shift+Q) |
| **Edit** | Copy (Ctrl+Shift+C), Paste (Ctrl+Shift+V; Shift+Insert pastes PRIMARY), Select All (Ctrl+Shift+A), Find… (Ctrl+Shift+F) |
| **View** | Font Size Up (Ctrl+Shift+=), Font Size Down (Ctrl+Shift+−), Reset Font Size (Ctrl+Shift+0), Color Scheme ▸ (radio list), Full Screen (F11), Show Menu Bar (toggle, Ctrl+Shift+M) |
| **Session** | Duplicate Session (Ctrl+Shift+D), Restart Session (Ctrl+Shift+R, enabled once the process has exited or after confirming), Send Break (serial only), Clear Scrollback (Ctrl+Shift+K), Reset Terminal, Change Settings… (opens this session's settings and applies live where possible; **until per-session settings land (M6) it opens Preferences**) |
| **Settings** | Preferences… (**Ctrl+Shift+,**, also in the Ctrl+right-click menu; global defaults: mouse, clipboard, fonts, schemes, shortcuts, "Check for updates at startup" (default on), and clearing a skipped version) |
| **Help** | About zterminal (version, Qt and libvterm versions, MIT license, repo link), Check for Updates… (manual check; the startup check is controlled in Preferences) |

**Shortcut policy:** terminal programs need every plain Ctrl+letter (Ctrl+C, Ctrl+V for vim's literal insert, Ctrl+W, Ctrl+R, and so on), and some need F-keys (mc, htop use F1–F10). So app shortcuts use only **Ctrl+Shift+…**, F11, and Shift+Insert. `TerminalView` handles `QEvent::ShortcutOverride` so that only registered app shortcuts are taken and every other key goes to the session. The Preferences shortcut editor flags clashes, and a "pass all keys to session" toggle disables app shortcuts entirely. F10 is deliberately not bound to the menu (mc uses it). **Hidden menu bar:** View → Show Menu Bar off hides it **for that window only. It is not persisted**, so every new window starts with the menu bar visible (changed in 0.1.1). Ctrl+Shift+M or the Ctrl+right-click menu (which always includes "Show Menu Bar") brings it back. Full screen never hides it.

**Always an in-window menu bar:** `menuBar()->setNativeMenuBar(false)`. 0.1.0 had no visible menu bar on vertex (Pop!_OS 24.04, GNOME Wayland) because the Fildem global-menu extension owns `com.canonical.AppMenu.Registrar` on the session bus. Qt then exports `QMenuBar` over D-Bus (`/MenuBar/N`) and hides the in-window bar, and nothing on that desktop displayed the exported menus. `tests/tst_globalmenu` reproduces this with a fake registrar under `dbus-run-session` + `xvfb-run`.

## 4. Architecture
```
core/   (no Qt GUI; unit-tested)          app/   (Qt6 Widgets)
  Backend (iface): write(), resize(), signals bytesReceived/closed
    PtyBackend     forkpty + QSocketNotifier, SIGWINCH/TIOCSWINSZ
    SshBackend     PtyBackend running `ssh [opts] host`
    SerialBackend  QSerialPort (resize is a no-op)
  Terminal        libvterm wrapper: screen, scrollback ring, damage, modes
  Selection       cell-range model: char/word/line, rectangular (Alt)
  SessionStore    ~/.config/zterminal/sessions/*.ini
  Settings        ~/.config/zterminal/zterminal.ini
  Search          match finder over screen+scrollback
  UpdateCheck     semver parse/compare (pure, tested)
                                          MainWindow     menu bar, Actions registry (QAction),
                                                         title "zterminal <ver> — <session>"
                                          TerminalView   QWidget + QPainter renderer, ShortcutOverride
                                          MouseController  §4.6 rules
                                          FindBar, SessionDialog, SettingsDialog
                                          AboutDialog, UpdateChecker (QNetwork)
build: project(VERSION) → configure_file(version.hpp.in) → zterminal::kVersionString
```
1. **PTY/process:** `forkpty()`, `TERM=xterm-256color`, `COLORTERM=truecolor`. Read through `QSocketNotifier`. Writes are queued and non-blocking. Child exit is reported in the window ("[process exited 0], press Enter to restart").
2. **SSH:** no custom SSH stack. We build an argv (never a shell string) for system `ssh`, run it in a PTY, and leave host-key and password prompts to ssh itself.
3. **Serial:** `QSerialPort` on `/dev/ttyUSB*`/`ttyS*`/`ttyACM*`, with a port list from `QSerialPortInfo`. If the user can't open the device, show a clear hint about `dialout` group membership. Paste pacing applies here.
4. **Emulation core:** vendored libvterm 0.3.3. Screen callbacks feed damage rectangles. `sb_pushline`/`sb_popline` feed our scrollback ring, and reflow is enabled. `VTERM_PROP_MOUSE` tells us whether the app has grabbed the mouse.
5. **Renderer:** `QPainter` on a cell grid with a glyph/run cache that repaints only damaged rows. Batches runs of the same style. HiDPI aware. OpenGL only if profiling demands it.
6. **Selection/clipboard (Stephen's spec):**
   - **Select = copy.** Left-drag selects, double-click selects a word (with configurable delimiter set), and triple-click selects a line. Alt+drag makes a rectangular selection. On release, the text is copied to **PRIMARY** (`QClipboard::Selection`, when `supportsSelection()`) and to **CLIPBOARD** when "copy on select to CLIPBOARD" is on (default **on**, PuTTY-like).
   - **Right-click = paste** (default). Source is configurable (CLIPBOARD by default, or PRIMARY).
   - **No context menu by default. Ctrl+right-click opens the menu** (Copy, Paste, Copy as HTML later, Clear scrollback, Reset terminal, Duplicate session, Settings). This is always intercepted and never sent to the app.
   - **Middle-click paste is optional:** setting = Paste PRIMARY (default) / Paste CLIPBOARD / Off.
   - **Mouse reporting:** when the app grabs the mouse (vim, htop, tmux), clicks go to the app. **Holding Shift bypasses reporting** so every rule above applies locally.
   - Pastes go through bracketed paste when mode 2004 is set. Line endings are normalized to CR. An optional multi-line paste confirmation (default off, but recommended for serial sessions) guards against accidentally right-click-pasting a config into a switch. On serial, the paste delay setting is honored.
   - Settings > Mouse offers "Right button: Paste / Extend selection / Menu" for xterm-style users.
   - The Ctrl+right-click menu reuses the registry's actions: Copy, Paste, Select All, Find, Show Menu Bar, Duplicate/Restart Session, Clear Scrollback, Reset Terminal, Preferences.
7. **Session store:** one INI per session via `QSettings::IniFormat` in `~/.config/zterminal/sessions/` (no extra dependency, human-editable, the same QSettings approach zwriter uses). A `Default Settings` session works like PuTTY's (still planned).
   - **Implemented in 0.2.0** (`core/Session*`, `app/SessionDialog`): file name = the session name percent-encoded (`/`, `%`, control characters encoded; a leading `.` becomes `%2E`), so a name can't escape the directory; the real name is also stored as `session/name`. Keys: `session/{name,type}` (`local`/`ssh`/`serial`), `ssh/{host,user,port,keyFile,jumpHost,extraArgs}`, optional `appearance/{fontFamily,fontSize,colorScheme}` overrides. Directory 0700, files 0600. **No password field exists**; a hand-added one is ignored and dropped on the next save.
   - Names: non-empty, no control characters, no surrounding spaces, no leading `-` (it would look like an option to `zt`), not `ssh`.
   - **SSH argv** (`buildSshCommand`): `ssh [extra options] [-p port] [-l user] [-i key] [-J jump] -- host`, run via the PTY's `execvp` (no shell). Host: `[A-Za-z0-9._:%[]-]+`, no leading `-`; user: `[A-Za-z0-9._$\-]+`, no leading `-`; jump: comma-separated `[ssh://][user@]host[:port]`; port 1–65535; key file `~/` expanded in code. Extra arguments are split like a command line (`QProcess::splitCommand`, double quotes) and must all be ssh options or option values (getopt rules), so they can't add a destination or remote command; `--` and long options are refused. The same validation runs again when a saved session starts, so a hand-edited file can't inject either.
   - Dialog: saved list + name + Load/Save/Delete on the left; Type (Local shell / SSH / Serial greyed out), SSH fields, a read-only "Runs:" preview of the exact argv, and per-session font/scheme overrides on the right; Open starts the session in a new window (by name when it matches the saved copy, else as an ad-hoc `zterminal ssh …`/local window). With a per-session override, font zoom and View → Color Scheme change (and save) the override instead of the global default.
   - File → New Session (blank dialog), Open Saved Session (list focused), Save Session (name prompt; saves the window's local/SSH session, including an ad-hoc `zt ssh …` one, and the window then *is* that session for its title and Duplicate). `-e` command windows can't be saved.
8. **Settings UI:** a PuTTY-style tree (Session, Terminal, Window/Appearance, Colours, Mouse/Selection, Keyboard/Shortcuts, Connection → SSH, Serial). Session → Change Settings opens the same tree scoped to the running session. Settings → Preferences edits Default Settings plus global options. **Since 0.1.1:** OK/Apply save to `~/.config/zterminal/zterminal.ini` and apply live. Each window is its own process and watches that file (`QFileSystemWatcher` on the file and its directory, because QSettings replaces the file atomically), so every open window follows a change, including font zoom and View → Color Scheme. The v0.1 dialog covers font family and size, colour scheme, middle-click, copy-on-select to CLIPBOARD, scrollback, and "Check for updates at startup". That last one is stored now even though the checker arrives in M6, and the dialog says so.
9. **Version (single source of truth):** `project(zterminal VERSION 0.1.0)` in the top-level CMakeLists. `configure_file(src/version.hpp.in → build/generated/version.hpp)` defines `zterminal::kVersionString` (zwriter's scheme). Nothing hand-codes a version. The window title (`zterminal 0.1.0 — SG250 console`), About, the UpdateChecker comparison, the CPack `.deb` name, and the release tag `v0.1.0` all derive from it. A program-set title (OSC 0/2) is appended as a suffix (`… — SG250 console — vim foo.c`) and can be turned off; the `zterminal <ver> — <session>` prefix is never replaced.
10. **Updates (check, then optional verified install):**
    - **Check:** a port of zwriter's `UpdateChecker` (`src/UpdateChecker.{hpp,cpp}`, copied into this repo rather than shared). It sends one non-blocking `QNetworkAccessManager` GET to `https://api.github.com/repos/sbj-ee/zterminal/releases/latest` with `User-Agent: zterminal-update-check` and a 5 s timeout. A re-click while a check is running is ignored. It compares `tag_name` with `kVersionString` using semver that tolerates a leading `v`. A failure gives a short reason (rate limit with the reset time, 404 for no release, HTTP status, or network error).
    - **When it runs:** **at startup**, about 3 s after the window shows (Preferences → "Check for updates at startup", default on), and from Help → Check for Updates. A startup check is silent unless it finds a newer release that hasn't been skipped. A manual check always reports its result, including "You're up to date (0.1.0)" and failures. Unlike zwriter, which only checks manually, this follows Stephen's spec.
    - **Dialog:** "zterminal vX.Y.Z is available (you have 0.1.0)", with a release-notes excerpt and buttons **Install / Later / Skip this version**. Skip stores the tag in `updates/skippedVersion`. A startup check then stays quiet for that tag but tells the user about anything newer. A manual check still shows a skipped tag. **Nothing installs without the user clicking Install.**
    - **Install:**
      1. Find the `zterminal_<ver>_amd64.deb` and `SHA256SUMS` assets in the release JSON.
      2. Download both to a `QTemporaryDir` with a progress dialog that can be cancelled.
      3. Compute SHA256 with `QCryptographicHash::Sha256` and compare it to the `SHA256SUMS` line for that file name. **Abort** if they don't match, if the `SHA256SUMS` asset or its line is missing, or if the `.deb` asset is missing. Each abort shows a clear message and offers the release page instead.
      4. Run `pkexec apt install -y ./zterminal_<ver>_amd64.deb` through `QProcess`, with the working directory set to the temp dir and the argv list built in code (never a shell string). polkit shows its own password prompt.
      5. On success, offer **Restart now**, which re-execs `/usr/bin/zterminal` with the same session arguments.
    - **Fallbacks:** if `pkexec` or `apt` isn't on `PATH` (`QStandardPaths::findExecutable`), if the app isn't running from the installed `.deb` (for example a build tree or another prefix), or if pkexec is dismissed (exit 126/127) or apt fails, open the release page with `QDesktopServices` and show the `.deb` path.
    - **Release CI** (`.github/workflows/release.yml`, on `v*` tag push): build on ubuntu-24.04, run the tests, `cpack -G DEB`, then `sha256sum zterminal_*_amd64.deb > SHA256SUMS`. It checks that the tag matches `PROJECT_VERSION` and uploads both files to the GitHub Release.

11. **`zt` launcher** (`/usr/bin/zt`, POSIX sh): `--help` and `--version` run in the foreground (the version comes from `zterminal --version`, so it's never duplicated). Every other invocation runs `setsid -f zterminal "$@" </dev/null >/dev/null 2>&1`, falling back to `nohup … &` if `setsid` is missing, and returns at once. `zterminal` parses arguments with `QCommandLineParser`:
    - no args → local shell
    - `<name>` → saved session (0.2.0). `zt` runs `zterminal --check-session <name>` first, while stderr is still the terminal, so an unknown name prints `zterminal: unknown session "x"; saved sessions: a, b` and exits 2 instead of failing silently after detaching. `zterminal <unknown>` prints the same and exits 2. `--list-sessions` prints the names.
    - `ssh <user@host> [args…]` → ad-hoc SSH: the system `ssh` runs in the PTY with the args passed as an argv list, and the title is `zterminal <ver> — ssh user@host`
    - `-e/--command <prog> [args]`.
    
    **Name check (2026-10-03):** packages.ubuntu.com noble contents, exact filename `zt`, has no results, so no package ships a `zt` file. noble package names: no exact `zt`, no `zterminal`. On vertex (Pop!_OS 24.04, Ubuntu + Pop repos), `apt-cache policy zt zterminal` finds nothing and `zt` isn't on PATH. Pop's Contents index couldn't be fetched from the box, so a Pop-only package can't be fully ruled out. The `.deb` will declare `Conflicts:` with nothing; we re-check before v1.0.

## 5. Test plan
- **Unit (Qt Test suites run by ctest, `QT_QPA_PLATFORM=offscreen`, the same headless approach as zwriter):** feed escape-sequence fixtures into `core::Terminal` and assert cell text and attributes. Covered: SGR 30–37/90–97/38;5;n/38;2;r;g;b, altscreen, scroll regions, reflow, word/line selection boundaries, wide chars, bracketed-paste wrapping, OSC 52 gating, SessionStore round-trip, ssh argv building (including injection-proof host names), the Search matcher (regex, matches that wrap lines), and UpdateCheck semver (`v0.1.0` vs `0.1`, `1.10.0` > `1.9.9`). An update-check test points the API base URL at a local `QTcpServer` serving canned replies (200 newer/same/older, 404, 403 rate limit, timeout). Further tests cover skip-version logic and the install pipeline with a fake `pkexec` script on `PATH`: matching checksum → install invoked with the exact argv, mismatched checksum / missing `SHA256SUMS` / missing `.deb` asset → abort with no install, and no `pkexec` → falls back to the release page. A version test asserts that `kVersionString == PROJECT_VERSION` and that `MainWindow::windowTitle()` starts with `zterminal <PROJECT_VERSION> — `.
- **Backends:** a PTY echo test (`/bin/cat`) and resize to `stty size`. Serial via a `socat -d -d pty,raw,echo=0 pty,raw,echo=0` pair in CI, plus checks of the pacing timing.
- **Conformance (manual, recorded per release):** `vttest` (menus 1–3, 6 mouse, 11 xterm), **esctest2** (Thomas Dickey's fork, run against zterminal, with known failures tracked in `docs/esctest-baseline.txt`), and `tests/scripts/truecolor.sh` (24-bit gradient awk script), `256colors.sh`, `msgcat --color=test`, plus vim/htop/tmux/mc/less smoke tests.
- **Copy/paste manual checklist,** run on **dragon (GNOME 46 Wayland)**, under XWayland (`QT_QPA_PLATFORM=xcb`), and on an X11 session:
  1. Drag-select → middle-click in gedit pastes it (PRIMARY). Ctrl+V pastes it when copy-to-CLIPBOARD is on, and not when it's off.
  2. Double-click selects a word (including `user@host`, `/path/to/file` with the delimiter config). Triple-click selects a line.
  3. Right-click pastes and **no menu appears**. Ctrl+right-click shows the menu. Changing the right-button setting takes effect.
  4. Middle-click works with each of PRIMARY, CLIPBOARD, and Off. With Off, middle-click does nothing.
  5. In vim with `mouse=a` and in htop, clicks go to the app. Shift+drag selects and Shift+right-click pastes.
  6. Paste into bash shows a bracketed paste (no auto-execute of multi-line text). Paste into an SG250 serial session honors line delay and the confirm dialog.
  7. `printf '\e]52;c;%s\a' $(echo hi|base64)` is ignored by default and sets CLIPBOARD when enabled.
  8. Left-drag highlights the text, and on release it is copied. A new selection or a click clears the old highlight.
- **`zt`:** a ctest runs `zt --version` and `zt --help` against the build-tree script. Manual check: `zt` returns the prompt immediately, closing the calling terminal doesn't kill zterminal, and `zt ssh vertex` opens ssh.
- **Menus/shortcuts checklist:** every menu item works, and disabled states are correct (Send Break only on serial). Ctrl+Shift+C/V copy and paste while plain Ctrl+C still interrupts `sleep 100` and Ctrl+W still deletes a word in bash. F1–F10 reach mc and htop. Hide the menu bar, then restore it with Ctrl+Shift+M and with Ctrl+right-click. F11 toggles fullscreen. Find highlights matches in scrollback. The title shows `zterminal <ver> — <session>` and matches About. Check for Updates gives the right message before the first release (404), when up to date, and with a fake newer tag.
- **Update install (manual, before each release):** install vN-1 from its `.deb` and publish vN. Then confirm: the startup dialog appears; Later stays quiet until the next start; Skip suppresses vN; Install shows the polkit prompt, apt upgrades the package, and Restart comes back on vN. Also confirm that a tampered `SHA256SUMS` aborts the install, and that turning off "Check for updates at startup" stops any network call (checked with `strace -f -e connect`).

## 6. Milestones
- **M0 Scaffold:** repo, CMake/CPack/CI cloned from zwriter, `project(VERSION 0.1.0)` and `version.hpp.in`, vendored libvterm, MIT LICENSE, empty window titled `zterminal 0.1.0`.
- **M1 Local shell:** PtyBackend, libvterm, QPainter renderer, keyboard input, 16/256/truecolor. Passes the truecolor script.
- **M2 Selection & clipboard:** the full §4.6 spec, mouse reporting with Shift bypass, bracketed paste. Copy/paste checklist passes on dragon.
- **M3 Scrollback, appearance & menus:** ring buffer, reflow, fonts, schemes, cursor. Menu bar and Actions registry with the Ctrl+Shift shortcut policy, show/hide menu bar, fullscreen, Find bar, versioned title, About.
- **M4 Sessions + SSH:** SessionStore, PuTTY-style dialog, SshBackend.
- **M5 Serial:** SerialBackend, pacing, break. Verified on the SG250 console.
- **M6 Conformance & polish:** vttest/esctest baseline, settings tree (Preferences and per-session Change Settings), OSC 52 option. **Updates:** startup and manual check, the Install/Later/Skip dialog, the verified download plus pkexec apt install with fallbacks and restart, and `release.yml` publishing the `.deb` + `SHA256SUMS` on tag.
- **M7 Release v1.0.0:** `.deb` + `SHA256SUMS` on GitHub Releases, README with screenshots.

## 7. Risks
- **Update integrity:** `SHA256SUMS` is published in the **same release** as the `.deb`, so it only guards against corrupted or truncated downloads. It does **not** protect against a compromised GitHub account or release, because an attacker could replace both files. TLS to github.com is the only authenticity guarantee. **Later hardening:** sign `SHA256SUMS` with minisign (or GPG) using a key kept offline, and embed the public key in the binary so the app refuses unsigned or invalidly signed releases.
- **Privilege:** the install runs apt as root through polkit, so the app must never pass anything to apt other than a file it downloaded and verified, by its own path. No shell is involved, and the argv is fixed.
- **Install environment:** pkexec, apt, or a polkit agent may be missing (for example minimal systems or SSH-forwarded X). The fallback is the release page. Builds that aren't from the `.deb` never offer Install.
- **Network/privacy:** the startup check contacts api.github.com once per launch (unauthenticated rate limit: 60/hour/IP). It can be turned off and sends nothing except the User-Agent.
- **Wayland:** Qt client-side decorations on GNOME are plain, and PRIMARY depends on the compositor exposing `zwp_primary_selection_v1` (GNOME 46 does). The X11/XWayland path is the fallback for testing.
- **libvterm limits:** no OSC 8 or sixel, and partial DECRQSS. These are accepted for v1 and tracked in the esctest baseline.

## 8. Decisions (former open questions, resolved by Stephen on 2026-10-03)
1. Apple Silicon/macOS: **no**, Linux amd64 only.
2. Copy-on-select goes to **PRIMARY + CLIPBOARD**. Right-click pastes **CLIPBOARD**. Middle-click is configurable, default **paste PRIMARY**.
3. **No tabs**: one window per session.
4. Multi-line paste confirm: off by default, with a per-session option (recommended for serial). *Default, changeable.*
5. Session files: INI via QSettings. *Default, changeable.*
6. SG250 console device and baud: settings per session, default 115200 8N1. To verify on the hardware in M5.
7. License: **MIT**.
8. Session logging: later (not v1). *Default, changeable.*
9. Packaging: **.deb only**.
10. Shortcuts: **Ctrl+Shift+…** as in §3, font size on Ctrl+Shift+=/−/0. *Default, changeable.*
11. Window title: **append the program's title** after the session name. *Default, changeable.*
12. UpdateChecker: **copied per repo** for now, and may become a shared library later. *Default, changeable.*

## 9. Sources
- libvterm: https://www.leonerd.org.uk/code/libvterm/ (MIT, v0.3.3). Selection/OSC 52 API in `vterm.h`: https://github.com/neovim/libvterm (mirror) and emacs-libvterm usage at https://github.com/akermu/emacs-libvterm/blob/master/vterm-module.c
- Ubuntu noble packages: https://packages.ubuntu.com/noble/libvterm-dev (0.3.3-2build1), https://packages.ubuntu.com/noble/qt6-serialport-dev and https://packages.ubuntu.com/noble/qt6-wayland (6.4.2), https://packages.ubuntu.com/noble/libvte-2.91-gtk4-dev (0.76)
- Qt Wayland primary selection: https://github.com/qt/qtwayland/commit/5ec182df699041699f514d164a161c299fde5d19 and https://doc.qt.io/qt-6/qtwaylandcompositor-attribution-wayland-primary-selection-protocol.html. `QClipboard::Selection`: https://doc.qt.io/qt-6/qclipboard.html
- Wayland primary-selection protocol: https://wayland.app/protocols/primary-selection-unstable-v1
- QSerialPort: https://doc.qt.io/qt-6/qserialport.html
- QTermWidget (GPL-2.0): https://github.com/lxqt/qtermwidget. VTE (LGPL-3.0): https://gitlab.gnome.org/GNOME/vte. alacritty (Apache-2.0): https://github.com/alacritty/alacritty
- xterm control sequences (mouse, 2004, OSC 52): https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
- vttest: https://invisible-island.net/vttest/. esctest2: https://github.com/ThomasDickey/esctest2. Truecolor test: https://github.com/termstandard/colors
- PuTTY mouse/selection semantics: https://the.earth.li/~sgtatham/putty/0.83/htmldoc/Chapter4.html#config-selection
- zwriter UpdateChecker (model for Help → Check for Updates): https://github.com/sbj-ee/zwriter/blob/HEAD/src/UpdateChecker.cpp and https://github.com/sbj-ee/zwriter/blob/HEAD/src/UpdateChecker.hpp. GitHub "latest release" API: https://docs.github.com/en/rest/releases/releases#get-the-latest-release. pkexec: https://www.freedesktop.org/software/polkit/docs/latest/pkexec.1.html. minisign: https://jedisct1.github.io/minisign/
- Siblings (read-only): https://github.com/sbj-ee/zwriter (CMakeLists.txt, ci.yml, cmake/Packaging.cmake, src/MainWindow.cpp) and https://github.com/sbj-ee/zedit (README, CMakeLists.txt)
