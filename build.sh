#!/bin/sh
set -e

if [ $# -ne 2 ]; then
  echo "Usage: $0 os cpu" >&2
  echo "  os:  mac, linux" >&2
  echo "  cpu: x64, arm64" >&2
  exit 1
fi

os=$1
cpu=$2

case "$os" in
  mac|linux) ;;
  *)
    echo "Invalid os: $os (use mac or linux)" >&2
    exit 1
    ;;
esac

case "$cpu" in
  x64|arm64) ;;
  *)
    echo "Invalid cpu: $cpu (use x64 or arm64)" >&2
    exit 1
    ;;
esac

cd "$(dirname "$0")" || exit 1
case "$os" in
  mac) platform=macos ;;
  linux) platform=linux ;;
esac
commit=$(grep "^${platform}:" v8-version.txt | awk '{ print $2; exit }')
if [ -z "$commit" ]; then
  echo "Could not read commit for $platform from v8-version.txt" >&2
  exit 1
fi

args_file="args.${os}.${cpu}.gn"
if [ ! -f "$args_file" ]; then
  echo "Args file not found: $args_file. Platform ($os/$cpu) is not supported." >&2
  exit 1
fi

echo "Checking out V8 $platform version ($commit)..."
(
  cd build/v8
  git checkout "$commit"
  PATH="$PWD/../depot_tools:$PATH" gclient sync
)
echo "Dependencies synced."

out_dir="out.gn/${os}.${cpu}.release"
echo "Configuring ($out_dir)..."
mkdir -p "build/v8/$out_dir"
cp "$args_file" "build/v8/$out_dir/args.gn"
(
  cd build/v8
  PATH="$PWD/../depot_tools:$PATH" gn gen "$out_dir"
)
echo "Building v8_monolith..."
(
  cd build/v8
  # v8-gn.h captures embedder-visible build defines (V8_ENABLE_SANDBOX,
  # V8_COMPRESS_POINTERS, etc.) — embedders include it via -DV8_GN_HEADER so
  # the header layout of v8::Local etc. matches the .a's ABI.
  PATH="$PWD/../depot_tools:$PATH" ninja -C "$out_dir" v8_monolith gen_v8_gn
)

# On Linux, V8 was built against its bundled libc++ which lives in separate
# archives. Merge them into libv8_monolith.a so embedders link a single .a.
# (On macOS the toolchain libc++ comes from the SDK; nothing to bundle.)
if [ "$os" = "linux" ]; then
  obj_dir="build/v8/$out_dir/obj"
  v8_archive="$obj_dir/libv8_monolith.a"

  # Force a clean re-link of libv8_monolith.a so we always start from a
  # libc++-free archive — otherwise re-running build.sh would compound libc++
  # members on each invocation. The AR step is fast (~2s).
  rm -f "$v8_archive"
  (
    cd build/v8
    PATH="$PWD/../depot_tools:$PATH" ninja -C "$out_dir" v8_monolith
  )

  echo "Bundling libc++ + libc++abi into libv8_monolith.a..."
  merged=$(mktemp)
  ar -M <<EOF
CREATE $merged
ADDLIB $v8_archive
ADDLIB $obj_dir/buildtools/third_party/libc++/libc++.a
ADDLIB $obj_dir/buildtools/third_party/libc++abi/libc++abi.a
SAVE
END
EOF
  mv "$merged" "$v8_archive"
fi

echo "v8_monolith is successfully built. You can run test.sh"
