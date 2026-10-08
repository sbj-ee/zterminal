# Security policy

zterminal stores passwords in an encrypted vault, hands them to `ssh` through
an askpass helper, parses output from remote hosts, and installs its own
updates. Reports about any of these are welcome.

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Report it privately through GitHub:
[Report a vulnerability](https://github.com/sbj-ee/zterminal/security/advisories/new)
(repository **Security** tab → **Report a vulnerability**).

Include what you can of:

- the zterminal version (Help → About, or `zterminal --version`) and the
  platform (Linux distribution or macOS version);
- what an attacker needs (a malicious host, a crafted file, local access, …)
  and what they gain;
- steps or an input that reproduces it. For emulator bugs, the raw bytes that
  trigger it are the most useful thing you can send.

This is a one-maintainer project. Expect an acknowledgement within a week. A
confirmed issue is fixed in a new release, and the advisory is published once
that release is out, with credit unless you ask otherwise.

## Supported versions

Only the latest release gets security fixes. The in-app updater (Help → Check
for Updates) moves you to it.

## In scope

- **Terminal emulation:** memory-safety bugs, hangs or crashes reachable from
  program output, including in the bundled libvterm
  (`third_party/libvterm/`, see its `README.zterminal.md` for the local fixes).
- **Password vault** (`core/Vault*`): anything that weakens the encryption at
  rest, leaks a password or the master key, or sends a stored password to a
  target other than the one it was saved for.
- **Askpass** (`askpass/`, `core/AskpassServer`): another process obtaining a
  stored password, or substituting the helper.
- **Updates** (`core/Update*`, `core/Minisign`): installing a package that is
  not signed by the release key, or passing attacker-controlled arguments to
  the privileged install step.
- **Paste and titles:** bypassing the paste guard or bracketed-paste filtering,
  or escape-sequence injection through window titles.
- **Session import and SSH arguments:** an imported session or `zt` argument
  that makes `ssh` run a command, load a library, or skip host-key checks.
- **Release pipeline:** `.github/workflows/`, signing and provenance.

## Out of scope

- An attacker who already runs code as your user or as root. As the README
  says, the vault protects passwords at rest, not against malware in your
  session.
- Vulnerabilities in the system `ssh`, Qt, libsodium or the OS. Report those
  upstream; do report it here if zterminal's use of them is the problem.
- Sessions you configure yourself with unsafe SSH options.
- macOS Gatekeeper prompts: the `.app` is ad-hoc signed and not notarized
  (see `docs/HARDENING.md`).

## Verifying a release

`SHA256SUMS` is signed with minisign and the packages carry SLSA build
provenance. The README's Install section and `docs/RELEASING.md` give the
public key and the commands; `docs/HARDENING.md` describes the build
hardening, sanitizer and fuzzing setup.
