#!/bin/sh
# Publish a draft release, retagging it to a specific commit.
#
# Usage: ci/publish-release.sh <os> <cpu> <repo> <sha>
#   os:   build.sh os arg  (linux, mac)
#   cpu:  build.sh cpu arg (x64, arm64)
#   repo: owner/repo       (e.g. dmkurilov/v8-monolith)
#   sha:  commit SHA to point the tag at (merge commit on main)
#
# Requires GH_TOKEN in the environment.
set -eu

os=$1
cpu=$2
repo=$3
sha=$4

cd "$(dirname "$0")/.." || exit 1

version=$(grep "^${os}:" v8-version.txt | sed 's/.*#[[:space:]]*//')
tag="${os}-${cpu}-v${version}"

echo "Publishing release $tag ..."

# Move the tag to the merge commit on main.
gh api -X PATCH "repos/${repo}/git/refs/tags/${tag}" \
  -f sha="$sha" \
  -F force=true

gh release edit "$tag" --draft=false
