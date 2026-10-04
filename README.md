# zterminal

A PuTTY-like terminal emulator for Linux, built with C++20, Qt 6 Widgets, and
[libvterm](https://www.leonerd.org.uk/code/libvterm/).

![zterminal 0.1.0 running a colour test, with a selection highlighted](docs/screenshot.png)

**Status:** early development (0.1.0, unreleased). See [docs/PLAN.md](docs/PLAN.md) for the plan
and milestones.

## What works today

- Local shell in a PTY (`$SHELL`), with `TERM=xterm-256color` and `COLORTERM=truecolor`
- xterm-style emulation via libvterm: 16, 256 and 24-bit colour, bold, italic, underline,
  reverse, strike, alternate screen, wide characters, bracketed paste, and the 1000/1002/1003/1006
  mouse modes
- Scrollback (10,000 lines by default): wheel, scroll bar, Shift+PgUp/PgDn/Home/End
- Mouse, PuTTY/X11 style:
  - left-drag selects, and releasing **copies to PRIMARY and CLIPBOARD**
  - double-click selects a word, triple-click a line
  - **right-click pastes CLIPBOARD**
  - **Ctrl+right-click opens the menu** (plain right-click never shows a menu)
  - **middle-click** is configurable: paste PRIMARY (default), paste CLIPBOARD, or off
  - when a program grabs the mouse (vim, htop, tmux), **hold Shift** to select or paste locally
- Menu bar: File, Edit, View, Session, Settings, Help. Items that aren't implemented yet are
  shown greyed out.
- App shortcuts use Ctrl+Shift+… (Copy Ctrl+Shift+C, Paste Ctrl+Shift+V, …) plus F11 and
  Shift+Insert, so plain Ctrl+C, Ctrl+W and the F-keys still reach the program.
- Window title `zterminal <version> — <session>` (the active tab's). The version comes from CMake.
- Tabs (0.7.0): each tab is its own local, SSH or serial session. New Tab Ctrl+Shift+T, Close
  Tab Ctrl+Shift+W, Next/Previous Ctrl+PgDn/Ctrl+PgUp (or Ctrl+Shift+] / [). Opening a session
  from File > New Session / Open Saved Session (or Duplicate) adds a tab. Closing a tab or
  window with a running session asks first. Logging, the REC marker, stored passwords and the
  paste confirmation work per tab. No split panes.
- `zt` launcher: starts zterminal detached from your shell
- Saved sessions (0.2.0): local shell or SSH, via a PuTTY-style dialog (File > New Session,
  Open Saved Session, Save Session). SSH runs the system `ssh` with a validated argv
  (no shell). Session files never contain passwords.

- Serial consoles (0.3.0): QSerialPort, 9600 8N1 by default (Cisco console), any baud,
  data/parity/stop bits, flow control, local echo, CR / CR+LF / LF on Enter, paste pacing
  (ms per character and per line, queued, with Cancel), Session > Send Break (300 ms by
  default), a clear `dialout` explanation on permission errors, and a Reconnect banner
  when the adapter is unplugged.
- Password vault (0.4.0): optional, encrypted (Argon2id + XChaCha20-Poly1305, libsodium) in
  `~/.config/zterminal/vault.bin`. Per SSH session, "Use stored password" answers ssh's first
  password prompt through an `SSH_ASKPASS` helper; per serial session, Session > Send Stored
  Login. Settings > Password Vault: Create/Unlock, Lock (Ctrl+Shift+L), Change Master Password;
  auto-lock after 15 idle minutes (Preferences). See [Password vault](#password-vault).
- Session logging (0.5.0): Session > Start/Stop Logging (Ctrl+Shift+G) or automatically per
  saved session; a red **● REC** marker and `[REC]` in the title while logging. See
  [Session logs](#session-logs).
- Safe copy and paste (0.6.0): copies are plain text with trailing whitespace trimmed, and a
  paste with more than one line asks first (line count, preview, "don't ask again for this
  session"). Both are on by default in Preferences. See [Copy and paste](#copy-and-paste).

Planned next: Find and update checks (see the plan).

## Usage

```sh
zt                      # local shell, returns to your prompt immediately
zt core-sw1             # open a saved session (quote names with spaces: zt "lab box")
zt --list-sessions      # list saved sessions
zt ssh user@host        # ad-hoc SSH through the system ssh (all ssh args pass through)
zt serial /dev/ttyUSB0  # ad-hoc serial console at 9600 8N1 (or: zt serial /dev/ttyACM0 115200)
zt -e htop              # run a program
zt --version
```

An unknown name fails at once with the list of saved sessions. Sessions are stored one
INI file per session in `~/.config/zterminal/sessions/` (file name = percent-encoded
session name; owner-only permissions), for example:

```ini
[session]
name=core-sw1
type=ssh

[ssh]
host=10.0.0.1
user=admin
port=22
keyFile=~/.ssh/id_lab
jumpHost=sbj@vertex
extraArgs=-o ServerAliveInterval=30
```

A serial session (`type=serial`) has a `[serial]` group: `device`, `baudRate` (9600),
`dataBits` (8), `parity` (none|even|odd|mark|space), `stopBits` (1|2), `flowControl`
(none|rtscts|xonxoff), `localEcho`, `enterSends` (cr|crlf|lf), `charDelayMs`, `lineDelayMs`
and `breakMs` (300), plus optional `loginUser`. An SSH session may have `authFromVault=true`
("Use stored password"); that flag is all the INI ever says about a password.

Serial devices belong to the `dialout` group. If opening one says "Permission denied", run
`sudo usermod -aG dialout $USER` and log out and back in.

The SSH example above runs `ssh -o ServerAliveInterval=30 -l admin -i /home/you/.ssh/id_lab -J sbj@vertex -- 10.0.0.1`.
Host, user and jump host are validated (no leading `-`, no spaces or shell characters),
extra arguments must be ssh options, and the host always follows `--`.

## Password vault

Prefer SSH keys with `ssh-agent`; the vault is for devices and hosts where you must use a
password. Creating the vault asks for a master password twice. **There is no recovery: if you
forget the master password, the stored passwords are lost.**

- SSH: in the session dialog tick **Use stored password**, type the password, press **Save**.
  It goes to the vault, never to the session file. When the session starts, ssh calls
  `zterminal-askpass` (`SSH_ASKPASS_REQUIRE=force`), which fetches the password once over a
  private one-shot Unix socket; if the server rejects it, ssh asks you in the terminal as usual.
  If the vault is locked you're asked to unlock it (Cancel = type the password yourself).
- Serial: set **Login user** and **Login password**, then use Session > Send Stored Login. The
  password is sent only when the device shows a password prompt.

**Threat model.** The vault protects the passwords **at rest**: in the file, in backups and
against casual access to your disk. It does **not** protect against malware running as your
user, root, keyloggers, or anyone who can read zterminal's memory while the vault is unlocked
(the key and secrets are in locked, zeroed-on-free memory, but Qt's password fields keep their
own copy while you type). Lock it when you step away, or let auto-lock do it.

Later: unlock via the Secret Service / GNOME Keyring.

## Session logs

Start with Session > Start Logging (Ctrl+Shift+G), or tick "Log this session to a file
automatically" in the session dialog. Logs go to `~/zterminal-logs/<session>-<YYYYMMDD-HHMMSS>.log`
(folder and per-line ISO timestamps in Preferences > Session logs); the folder is created 0700
and each log 0600. Logs are plain text: colours and other escape sequences are removed,
carriage-return/backspace overwrites keep only the final text, and full-screen programs
(vim, less, htop) are left out.

```text
=== zterminal 0.5.0 log of "core-sw1" started 2026-10-03T20:55:01.120 ===
2026-10-03T20:55:01.402 Welcome to core-sw1
2026-10-03T20:55:01.403 [zterminal: logging paused: password prompt (terminal echo off)]
2026-10-03T20:55:03.871 [zterminal: logging resumed]
2026-10-03T20:55:03.990 core-sw1# show clock
```

Only what the terminal displays is logged, never what you type. Logging also pauses while a
vault dialog is open, during Send Stored Login, and while the terminal is at a no-echo
password prompt, so stored and typed passwords stay out of the log. A remote program that
echoes a password back can't be detected; treat logs as sensitive anyway.

## Copy and paste

Selecting copies (PRIMARY, and CLIPBOARD unless turned off); right-click pastes CLIPBOARD;
Ctrl+Shift+C/V also work. Copies are plain text only, and trailing spaces/tabs are trimmed from
each line (Preferences > Mouse and clipboard > "Trim trailing whitespace from copied lines").

A paste that contains a line break would run commands as it arrives, so zterminal asks first:
**Paste N lines?** with a preview of the first lines (control characters shown as symbols),
the serial pacing estimate on paced consoles, and **Don't ask again for this session**. Cancel
is the default button. Single-line pastes go straight through. Turn it off with "Ask before
pasting text with more than one line". When the program enabled bracketed paste (bash, zsh,
vim), the paste is wrapped in bracketed-paste markers as before and the dialog says so.

## Building

Requires CMake 3.21+, a C++20 compiler, Ninja, Qt 6.4+ (Widgets, SerialPort, Test) and libsodium (via pkg-config).

```sh
sudo apt-get install -y qt6-base-dev qt6-serialport-dev libsodium-dev pkg-config libgl1-mesa-dev cmake ninja-build g++
cmake -B build -G Ninja
cmake --build build
./build/app/zterminal
```

## Testing

```sh
ctest --test-dir build --output-on-failure      # headless (QT_QPA_PLATFORM=offscreen)
./tests/scripts/colortest.sh                    # run inside zterminal: 16/256/truecolor check
```

## Packaging

```sh
cd build && cpack -G DEB      # -> zterminal_<version>_amd64.deb (zterminal, zt, /usr/libexec/zterminal/zterminal-askpass)
```

## Version bumps

Bump **only** `project(zterminal VERSION x.y.z)` in `CMakeLists.txt`. `src/version.hpp.in` is
generated into the build tree, and the title, About, `--version` and the `.deb` name all follow
from it.

## Layout

- `core/`: PTY, libvterm wrapper and scrollback, selection, settings (no widgets)
- `app/`: Qt Widgets UI (`MainWindow`, `TerminalView`, Preferences)
- `tests/`: Qt Test suites plus the `colortest.sh` script
- `third_party/libvterm/`: bundled, unmodified libvterm 0.3.3 (MIT)
- `askpass/`: `zterminal-askpass`, the plain-C `SSH_ASKPASS` helper
- `packaging/`: `zt` launcher and `.desktop` file

## License

MIT, see [LICENSE](LICENSE). The bundled libvterm is MIT, © Paul Evans (see
`third_party/libvterm/LICENSE`).
