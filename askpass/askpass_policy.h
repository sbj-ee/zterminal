/*
 * Which ssh prompts zterminal-askpass may answer with the stored password.
 * Plain C (no Qt) so the helper links nothing else; also linked into the
 * unit tests and the fuzzer.
 */
#ifndef ZTERMINAL_ASKPASS_POLICY_H
#define ZTERMINAL_ASKPASS_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns 1 when `prompt` is a password prompt that the stored password for
 * `target` may answer, 0 when the helper must ask on the terminal instead.
 *
 *   target    "user@host" (user may be empty: "@host") the password was
 *             stored for, as ssh will print it (ZTERMINAL_ASKPASS_TARGET);
 *             NULL or "" when unknown.
 *   alias     ssh's HostKeyAlias for the target, or NULL/"" (OpenSSH prints
 *             it instead of the host in keyboard-interactive prompts).
 *   via_jump  nonzero when the connection goes through a jump host or a
 *             proxy (ProxyJump / -J / ProxyCommand).
 *
 * Rules:
 *   - Only prompts containing "password" (any case) are considered.
 *   - A prompt ssh itself labels with the account it is for is answered only
 *     when that label is the target:
 *       "(user@host) <server prompt>"   keyboard-interactive, OpenSSH >= 8.4
 *       "user@host's password: "        password authentication
 *     Only this leading/whole-prompt label counts: text after it comes from
 *     the server and can't vouch for anything.
 *   - A bare prompt ("Password:", Cisco-style keyboard-interactive from
 *     OpenSSH < 8.4) carries no label, so its origin is unknown. It is
 *     answered only when there is no jump host/proxy (then only the target
 *     can be asking); with a jump host it is refused, because the jump host
 *     could be the one asking.
 */
int zt_askpass_may_answer(const char *prompt, const char *target, const char *alias, int via_jump);

#ifdef __cplusplus
}
#endif

#endif
