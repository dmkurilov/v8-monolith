#!/bin/sh
# Publish a draft release, retagging it to a specific commit.
#
# Usage: ci/publish-release.sh <os> <cpu> <sha>
#   os:  build.sh os arg  (linux, mac)
#   cpu: build.sh cpu arg (x64, arm64)
#   sha: commit SHA to point the tag at (merge commit on main)
#
# Requires GH_TOKEN in the environment.
set -eu

os=$1
cpu=$2
sha=$3

cd "$(dirname "$0")/.." || exit 1

version=$(grep "^${os}:" v8-version.txt | sed 's/.*#[[:space:]]*//')
tag="${os}-${cpu}-v${version}"

echo "Publishing release $tag ..."

# Publish the draft and point its tag at the merge commit on main.
gh release edit "$tag" --draft=false --target "$sha"
