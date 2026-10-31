# Changelog

All notable changes to zterminal. The version is the single
`project(zterminal VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps
it ([semantic versioning](https://semver.org/): patch for fixes and
documentation, minor for features) and adds an entry here. Releases are
tagged `vX.Y.Z`, published by `.github/workflows/release.yml`, and signed with
minisign (from 1.3.0; see [docs/RELEASING.md](docs/RELEASING.md)). The entries
below 1.5.1 are summarized from the GitHub Releases and tags.

## 1.6.2 — unreleased

### Changed

- CI and release macOS builds moved from the `macos-14` to the `macos-15` runner image,
  which GitHub retires on November 2, 2026.

### Fixed

- macOS packaging: Homebrew Qt 6.12's `macdeployqt` no longer writes
  `Contents/Resources/qt.conf`, so the `.dmg` build now writes it itself
  (`[Paths] Plugins = PlugIns`) when missing, keeping bundled plugins found at runtime.

## 1.6.1

- **Fix (macOS): crash when unlocking the vault.** Opening a session with a
  stored password (or creating the vault / changing the master password)
  crashed with `EXC_BREAKPOINT` (pointer-authentication trap) on macOS 27 with
  Qt 6.11.1. The busy cursor shown during unlock goes through Qt's Cocoa
  cursor conversion (`QImage::toCGImage`), which in Qt 6.11.0/6.11.1 passes a
  dangling colour space to `CGImageCreate` (QTBUG-147602, fixed in Qt 6.11.2).
  The vault dialogs no longer set an override cursor on macOS; on Linux the
  wait cursor is unchanged and always restored by an RAII guard.
- Tests: `tst_vaultui` checks the override-cursor stack is empty after unlock.

## 1.6.0 — unreleased

- **Session groups.** A group is an ordered list of saved-session names that
  opens as tabs in a new window (or this one): File → Open Group, File → Manage
  Groups…, Open Group by Name (Ctrl+Shift+P), `zterminal --group NAME` / `-g`,
  `--list-groups`, `--check-group`, and `zt --group`. One vault unlock for the
  whole group, staggered unattended starts (no dialog per tab), missing or
  unreviewed members skipped with one summary. Groups are stored in
  `~/.config/zterminal/groups/` (older versions ignore them); no per-member
  overrides, nesting, sidebar, splits or export yet.
- Session → Reconnect All Disconnected.
- Tabs that are waiting for a password or a host-key answer are marked `? ` and
  counted in the status bar.
- Deleting a saved session that groups use offers to remove it from them.
- Fix: a tab started in the background (a group's tabs, or any future
  background open) began with a 1×9 terminal because a never-shown view keeps
  Qt's default size; it now starts at the window's terminal size.
- Tests: `tst_groups`, `tst_groupui` (both required to run, not skip, in CI).

## 1.5.1 — unreleased

- Documentation only: README expanded (what zterminal is, install and release
  verification with minisign, status and known gaps, tests and CI, security
  notes, version-bump rule). Added this changelog.
- Version bump rule: every pull request now bumps the version.

## 1.5.0 — 2026-10-10

- Vault: no idle timeout by default (one unlock lasts for the run); the status
  bar shows the lock state (open padlock with ∞, or the timeout). The 15 minutes
  that 1.4.0 and earlier saved as their default are dropped once on upgrade.

## 1.4.0 — 2026-10-08

- Report focus in/out (mode 1004).
- macOS: Cmd+C copies and Cmd+V pastes.
- CI: warnings as errors, clang-tidy job, nightly fuzzing with a persistent
  corpus, run-by-hand workflow, Dependabot minor/patch updates merge once CI
  passes.
- Docs: PLAN.md status corrected, SECURITY.md added, stale README install
  commands fixed.

## 1.3.2 — 2026-10-07

- Theme editor: Qt's own colour picker opens on the editor, non-blocking.

## 1.3.1 — 2026-10-06

- Fix macOS launch crash in 1.3.0 (ad-hoc signature with hardened runtime: Team
  ID mismatch).

## 1.3.0 — 2026-10-06

- Security hardening batch: session import review and refusal of dangerous ssh
  options; vault passwords bound to their target; askpass answers only the
  target's own prompt; process and memory hardening; control characters
  stripped from pastes and program titles sanitized.
- Updates: minisign-signed `SHA256SUMS`; release public key embedded
  (`32960BBA77B36F1F`); hardened install step.
- Least-privilege release workflow, pinned actions, provenance, hardening
  flags, ASan/UBSan and fuzz CI.

## 1.2.0 — 2026-10-06

- Theme editor and custom themes in a `*.ztheme.json` format shared with zmail.
- Brand colour schemes Boilermakers, Badgers and Packers (developed as 1.1.0,
  released together with 1.2.0; there is no `v1.1.0` tag).
- README Themes section, per-theme screenshots.

## 1.0.0 — 2026-10-05

First full release for Linux amd64 and macOS Apple Silicon: local shell, SSH and
serial sessions in tabs; encrypted password vault; session logging, safe paste,
Find, SSH keepalive and reconnect; export/import of saved sessions as JSON;
Zorro-Z icon and blinking vertical-bar cursor; in-app updates (SHA-256 checked
until 1.3.0). Preceded by the pre-release tags `v0.10.0-macos.1` to
`v0.10.0-macos.5` (macOS bring-up).
