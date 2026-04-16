#!/bin/bash
# Detect which platforms have changed versions in v8-version.txt.
#
# Usage: ci/detect.sh <base-ref>
#   base-ref: git ref to diff against (commit SHA, branch, HEAD~1, etc.)
#
# Outputs a GitHub Actions matrix JSON to $GITHUB_OUTPUT.
set -euo pipefail

base_ref=$1

cd "$(dirname "$0")/.." || exit 1

parse() {
  grep -E '^(linux|mac):' "$1" | sed 's/#.*//' | awk '{print $1, $2}' | tr -d ':'
}

git show "${base_ref}:v8-version.txt" > /tmp/base.txt 2>/dev/null || true

parse /tmp/base.txt  > /tmp/base_versions.txt || true
parse v8-version.txt > /tmp/head_versions.txt

changed=()
while read -r os commit; do
  base_commit=$(awk -v p="$os" '$1==p {print $2}' /tmp/base_versions.txt)
  if [ "$commit" != "$base_commit" ]; then
    case "$os" in
      linux) changed+=('{"os":"linux","cpu":"x64","runner":"ubuntu-24.04"}') ;;
      mac)   changed+=('{"os":"mac","cpu":"arm64","runner":"macos-14"}') ;;
    esac
  fi
done < /tmp/head_versions.txt

if [ ${#changed[@]} -eq 0 ]; then
  echo "No platform versions changed."
  echo 'matrix={"include":[]}' >> "$GITHUB_OUTPUT"
else
  joined=$(printf ',%s' "${changed[@]}")
  echo "matrix={\"include\":[${joined:1}]}" >> "$GITHUB_OUTPUT"
fi
