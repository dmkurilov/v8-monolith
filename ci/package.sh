#!/bin/sh
# Package a v8_monolith release tarball.
#
# Usage: ci/package.sh <os> <cpu>
#   os:  build.sh os arg  (linux, mac)
#   cpu: build.sh cpu arg (x64, arm64)
#
# Outputs tag, version, tarball to $GITHUB_OUTPUT.
set -eu

os=$1
cpu=$2

cd "$(dirname "$0")/.." || exit 1

version=$(grep "^${os}:" v8-version.txt | sed 's/.*#[[:space:]]*//')
tag="${os}-${cpu}-v${version}"

out_dir="build/v8/out.gn/${os}.${cpu}.release"
pkg="v8-monolith-${os}-${cpu}"
mkdir -p "$pkg/lib" "$pkg/include"

cp "$out_dir/obj/libv8_monolith.a"    "$pkg/lib/"
cp -r build/v8/include/*              "$pkg/include/"
cp "$out_dir/gen/include/v8-gn.h"     "$pkg/include/"
cp v8_wrapper.h v8_wrapper.cc         "$pkg/"

tar czf "${pkg}.tar.gz" "$pkg"

echo "tag=${tag}"            >> "$GITHUB_OUTPUT"
echo "version=${version}"   >> "$GITHUB_OUTPUT"
echo "tarball=${pkg}.tar.gz" >> "$GITHUB_OUTPUT"
