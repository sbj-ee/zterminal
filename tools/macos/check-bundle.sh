#!/usr/bin/env bash
# Fail if anything inside zterminal.app still links to (or searches) Homebrew or
# /usr/local. Checks every Mach-O file: main binary, frameworks, dylibs, plugins.
# Also fails on Team ID / signature mismatches and on ad-hoc + hardened runtime
# (the v1.3.0 dyld "different Team IDs" launch crash).
#   usage: tools/macos/check-bundle.sh path/to/zterminal.app
set -euo pipefail
app="${1:?usage: check-bundle.sh path/to/zterminal.app}"
[[ -x "$app/Contents/MacOS/zterminal" ]] || { echo "not an app bundle: $app" >&2; exit 2; }

bad='(/opt/homebrew|/usr/local)'
n=0; hits=0
while IFS= read -r -d '' f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  n=$((n + 1))
  if otool -L "$f" | tail -n +2 | grep -E "$bad" >/dev/null; then
    echo "LINK  ${f#"$app"/}:"
    otool -L "$f" | tail -n +2 | grep -E "$bad" | sed 's/^/        /'
    hits=$((hits + 1))
  fi
  if otool -l "$f" | grep -A2 LC_RPATH | grep -E "path $bad" >/dev/null; then
    echo "RPATH ${f#"$app"/}:"
    otool -l "$f" | grep -A2 LC_RPATH | grep -E "path $bad" | sed 's/^/        /'
    hits=$((hits + 1))
  fi
done < <(find "$app/Contents" -type f -print0)

echo "check-bundle: $n Mach-O files checked (main binary, frameworks, dylibs, plugins)"
echo "check-bundle: main binary links:"
otool -L "$app/Contents/MacOS/zterminal" | tail -n +2 | sed 's/^/    /'
echo "check-bundle: plugins: $(find "$app/Contents/PlugIns" -name '*.dylib' 2>/dev/null | wc -l | tr -d ' ')," \
     "frameworks: $(find "$app/Contents/Frameworks" -maxdepth 1 -name '*.framework' 2>/dev/null | wc -l | tr -d ' ')," \
     "dylibs: $(find "$app/Contents/Frameworks" -maxdepth 1 -name '*.dylib' 2>/dev/null | wc -l | tr -d ' ')"
for need in Contents/Resources/zterminal.icns Contents/Resources/qt.conf \
            Contents/PlugIns/platforms/libqcocoa.dylib \
            Contents/MacOS/zterminal-askpass; do
  [[ -e "$app/$need" ]] || { echo "MISSING $need"; hits=$((hits + 1)); }
done
# Local Network privacy (macOS 15+): empty CFBundleIdentifier or a missing usage
# description makes LAN access from the local shell fail with "No route to host"
# and never prompts (Apple TN3179).
bid="$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$app/Contents/Info.plist" 2>/dev/null || true)"
if [[ -z "$bid" ]]; then
  echo "MISSING/EMPTY Info.plist CFBundleIdentifier (need ee.sbj.zterminal)"
  hits=$((hits + 1))
elif [[ "$bid" != "ee.sbj.zterminal" ]]; then
  echo "UNEXPECTED CFBundleIdentifier='$bid' (expected ee.sbj.zterminal)"
  hits=$((hits + 1))
fi
if ! /usr/libexec/PlistBuddy -c 'Print NSLocalNetworkUsageDescription' "$app/Contents/Info.plist" >/dev/null 2>&1; then
  echo "MISSING Info.plist NSLocalNetworkUsageDescription"
  hits=$((hits + 1))
fi
# libsodium must be bundled (vault).
if ! find "$app/Contents/Frameworks" -maxdepth 1 -name 'libsodium*.dylib' | grep -q .; then
  echo "MISSING Contents/Frameworks/libsodium*.dylib"
  hits=$((hits + 1))
fi
# Qt SerialPort if the app links it.
if otool -L "$app/Contents/MacOS/zterminal" | grep -qi SerialPort; then
  if ! find "$app/Contents/Frameworks" -maxdepth 1 -name 'QtSerialPort.framework' | grep -q .; then
    echo "MISSING Contents/Frameworks/QtSerialPort.framework"
    hits=$((hits + 1))
  fi
fi
# Code-signing identity consistency: every nested Mach-O must share the main
# binary's TeamIdentifier and Signature kind. Mismatches are the v1.3.0 dyld
# "different Team IDs" launch crash (Hardened Runtime library validation).
# Also reject ad-hoc + hardened runtime: ad-hoc has no Team ID, so library
# validation treats separately-signed nested frameworks as a mismatch even
# when both TeamIdentifiers are empty / "not set".
if command -v codesign >/dev/null 2>&1; then
  main_bin="$app/Contents/MacOS/zterminal"
  main_info="$(codesign -dv --verbose=4 "$main_bin" 2>&1 || true)"
  main_team="$(printf '%s\n' "$main_info" | sed -n 's/^TeamIdentifier=//p' | head -1)"
  main_sig="$(printf '%s\n' "$main_info" | sed -n 's/^Signature=//p' | head -1)"
  [[ "$main_team" == "not set" || -z "$main_team" ]] && main_team=""
  if printf '%s\n' "$main_info" | grep -Eq 'flags=[^ ]*runtime' \
     && printf '%s\n' "$main_info" | grep -qi 'Signature=adhoc'; then
    echo "BAD SIGNING: ad-hoc + hardened runtime on main binary"
    echo "        (library validation rejects nested frameworks with empty Team IDs;"
    echo "         sign ad-hoc without --options runtime, or use a Developer ID)"
    echo "$main_info" | grep -E '^(Identifier|Signature|TeamIdentifier|Format)=|flags=' | sed 's/^/        /'
    hits=$((hits + 1))
  fi
  sig_n=0
  while IFS= read -r -d '' f; do
    file -b "$f" | grep -q 'Mach-O' || continue
    sig_n=$((sig_n + 1))
    info="$(codesign -dv "$f" 2>&1 || true)"
    team="$(printf '%s\n' "$info" | sed -n 's/^TeamIdentifier=//p' | head -1)"
    sig="$(printf '%s\n' "$info" | sed -n 's/^Signature=//p' | head -1)"
    [[ "$team" == "not set" || -z "$team" ]] && team=""
    if [[ "$team" != "$main_team" || "$sig" != "$main_sig" ]]; then
      echo "TEAM/SIG MISMATCH ${f#"$app"/}:"
      echo "        team='${team:-not set}' sig='${sig:-?}' (main team='${main_team:-not set}' sig='${main_sig:-?}')"
      hits=$((hits + 1))
    fi
  done < <(find "$app/Contents" -type f -print0)
  echo "check-bundle: $sig_n Mach-O signatures compared to main (team='${main_team:-not set}' sig='${main_sig:-?}')"
else
  echo "check-bundle: codesign not available; skipped Team ID / signature checks"
fi

if (( hits > 0 )); then
  echo "check-bundle: FAILED ($hits problem(s))" >&2
  exit 1
fi
echo "check-bundle: OK, no /opt/homebrew or /usr/local references; signing identities match"
