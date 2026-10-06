# Releasing zterminal

1. Bump `project(zterminal VERSION x.y.z)` in `CMakeLists.txt` (nothing else).
2. Merge to `main`, then tag and push: `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. `.github/workflows/release.yml` builds and tests on Linux and macOS, packages
   the `.deb` and `.dmg`, writes `SHA256SUMS`, **signs it** with minisign
   (`SHA256SUMS.minisig`), checks that signature against the public key in
   `core/UpdateSigningKey.cpp` at the tagged commit, and creates the GitHub
   Release. Without the signing secrets, or with no public key in the source,
   the publish job stops and nothing is released.

## Signing key (one-time setup)

The in-app updater installs only releases whose `SHA256SUMS` verifies against
the minisign public key compiled into the app, so a compromised GitHub release
alone can't push malware to users.

1. **Generate the key on your own machine** (not in CI, not on a shared box):

   ```sh
   brew install minisign            # or: sudo apt install minisign
   minisign -G -p zterminal-release.pub -s zterminal-release.key
   ```

   Choose a strong password. `zterminal-release.key` is the secret key
   (encrypted with that password); `zterminal-release.pub` is public.

2. **Keep the secret key safe:** store `zterminal-release.key` and its password
   in your password manager, plus an offline backup (an encrypted USB stick).
   If it is lost, users on the old key can't update in-app any more (they'd
   install the next release by hand); if it leaks, rotate it (below).

3. **Give CI the key** (repository secrets, never committed):

   ```sh
   gh secret set MINISIGN_SECRET_KEY -R sbj-ee/zterminal < zterminal-release.key
   gh secret set MINISIGN_PASSWORD   -R sbj-ee/zterminal    # paste the password when asked
   ```

4. **Embed the public key:** copy the second line of `zterminal-release.pub`
   (it starts with `RW`) into `kUpdateSigningPublicKey` in
   `core/UpdateSigningKey.cpp`, and replace `<release public key>` in the
   README's Install section with it. Commit, then release as usual.

5. Check a release by hand once:

   ```sh
   minisign -Vm SHA256SUMS -P RW...your-public-key...
   ```

## Transition from 1.0.x

1.0.x verifies only SHA-256 (from the same release). The first release with
signing is installed through 1.0.x's updater as before, or by hand; from then on
every update is signature-checked. Ask users to verify that first release with
`minisign -Vm` if they install it manually.

## Rotating the key

Generate a new key pair, update the secrets and `core/UpdateSigningKey.cpp`,
and release. Clients that still have the old key can't verify releases signed
with the new key, so tell users to install that release by hand.
