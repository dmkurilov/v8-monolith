#!/bin/sh
# Create (or replace) a draft GitHub release with the built tarball.
#
# Usage: ci/draft-release.sh <tag> <version> <tarball> <os> <cpu>
#
# Requires GH_TOKEN in the environment.
set -eu

tag=$1
version=$2
tarball=$3
os=$4
cpu=$5

# If a draft with this tag already exists (PR was updated),
# delete it so we can recreate with fresh artifacts.
gh release delete "$tag" --yes 2>/dev/null || true
git push origin --delete "refs/tags/$tag" 2>/dev/null || true

gh release create "$tag" "$tarball" \
  --draft \
  --title "V8 $version — $os $cpu" \
  --notes "V8 monolith static library for $os $cpu.

Built from Chromium release **$version**.

**Contents:**
- \`lib/libv8_monolith.a\` — static library (V8 + libc++ + libc++abi)
- \`include/\` — V8 public headers + generated \`v8-gn.h\`
- \`v8_wrapper.h\` / \`v8_wrapper.cc\` — stdlib-agnostic wrapper API"
