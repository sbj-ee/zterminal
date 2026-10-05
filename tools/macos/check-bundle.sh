#!/usr/bin/env bash
# Fail if anything inside zterminal.app still links to (or searches) Homebrew or
# /usr/local. Checks every Mach-O file: main binary, frameworks, dylibs, plugins.
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
if (( hits > 0 )); then
  echo "check-bundle: FAILED ($hits problem(s))" >&2
  exit 1
fi
echo "check-bundle: OK, no /opt/homebrew or /usr/local references"
