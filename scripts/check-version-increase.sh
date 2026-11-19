#!/usr/bin/env bash
# Version-increase check for pull requests: the version in CMakeLists.txt
# (project(zterminal VERSION x.y.z)) must be greater than the base branch's.
# Every PR bumps the version (README "Version bumps"); Dependabot PRs are
# exempt, which ci.yml handles by not running this job for them.
#   scripts/check-version-increase.sh <base-ref>     e.g. origin/main
set -euo pipefail
cd "$(dirname "$0")/.."

base="${1:?usage: check-version-increase.sh <base-ref>}"

extract() { sed -nE 's/^project\(zterminal VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -n1; }

head_ver=$(extract < CMakeLists.txt)
base_ver=$(git show "${base}:CMakeLists.txt" | extract)
[ -n "$head_ver" ] || { echo "check-version-increase: no project(zterminal VERSION x.y.z) in CMakeLists.txt" >&2; exit 1; }
[ -n "$base_ver" ] || { echo "check-version-increase: no version in ${base}:CMakeLists.txt" >&2; exit 1; }

# Strictly greater: different, and the higher of the two by version sort.
if [ "$head_ver" != "$base_ver" ] \
   && [ "$(printf '%s\n%s\n' "$base_ver" "$head_ver" | sort -V | tail -n1)" = "$head_ver" ]; then
  echo "check-version-increase: OK ($base_ver on $base -> $head_ver)"
else
  echo "check-version-increase: version $head_ver is not greater than $base_ver on $base." >&2
  echo "Every pull request bumps project(zterminal VERSION ...) in CMakeLists.txt: patch for fixes and documentation, minor for features (README \"Version bumps\")." >&2
  exit 1
fi
