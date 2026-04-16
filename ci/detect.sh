#!/bin/bash
# Detect which platforms to build.
#
# Usage:
#   ci/detect.sh <base-ref>             Diff v8-version.txt against base-ref
#   ci/detect.sh --platforms <list>      Force specific platforms (linux, mac, all)
#
# Outputs a GitHub Actions matrix JSON to $GITHUB_OUTPUT.
set -euo pipefail

cd "$(dirname "$0")/.." || exit 1

emit() {
  local os=$1
  case "$os" in
    linux) echo '{"os":"linux","cpu":"x64","runner":"ubuntu-24.04"}' ;;
    mac)   echo '{"os":"mac","cpu":"arm64","runner":"macos-14"}' ;;
  esac
}

changed=()

if [ "$1" = "--platforms" ]; then
  for p in $2; do
    case "$p" in
      all)   changed+=("$(emit linux)" "$(emit mac)") ;;
      linux) changed+=("$(emit linux)") ;;
      mac)   changed+=("$(emit mac)") ;;
      *)     echo "Unknown platform: $p" >&2; exit 1 ;;
    esac
  done
else
  base_ref=$1

  parse() {
    grep -E '^(linux|mac):' "$1" | sed 's/#.*//' | awk '{print $1, $2}' | tr -d ':'
  }

  git show "${base_ref}:v8-version.txt" > /tmp/base.txt 2>/dev/null || true

  parse /tmp/base.txt  > /tmp/base_versions.txt || true
  parse v8-version.txt > /tmp/head_versions.txt

  while read -r os commit; do
    base_commit=$(awk -v p="$os" '$1==p {print $2}' /tmp/base_versions.txt)
    if [ "$commit" != "$base_commit" ]; then
      changed+=("$(emit "$os")")
    fi
  done < /tmp/head_versions.txt
fi

if [ ${#changed[@]} -eq 0 ]; then
  echo "No platform versions changed."
  echo 'matrix={"include":[]}' >> "$GITHUB_OUTPUT"
else
  joined=$(printf ',%s' "${changed[@]}")
  echo "matrix={\"include\":[${joined:1}]}" >> "$GITHUB_OUTPUT"
fi
