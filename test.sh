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
  *) echo "Invalid os: $os (use mac or linux)" >&2; exit 1 ;;
esac

case "$cpu" in
  x64|arm64) ;;
  *) echo "Invalid cpu: $cpu (use x64 or arm64)" >&2; exit 1 ;;
esac

cd "$(dirname "$0")" || exit 1
out_dir="out.gn/${os}.${cpu}.release"
lib_path="build/v8/$out_dir/obj/libv8_monolith.a"
if [ ! -f "$lib_path" ]; then
  echo "Library not found: $lib_path (run build.sh first)" >&2
  exit 1
fi

mkdir -p build/v8/test_build

case "$os" in
  mac)
    echo "Building test binary for mac/$cpu..."
    (
      cd build/v8/test_build
      c++ -std=c++23 \
        -I../../../ \
        -I../include -I../include/libplatform \
        ../../../v8_wrapper.cc ../../../test_main.cc \
        "../$out_dir/obj/libv8_monolith.a" \
        -framework CoreFoundation \
        -o v8_test
    )
    ;;
  linux)
    WRAPPER_OBJ=build/v8/test_build/v8_wrapper.o
    TEST_OBJ=build/v8/test_build/test_main.o
    V8_DIR=build/v8
    CLANG="$V8_DIR/third_party/llvm-build/Release+Asserts/bin/clang++"

    # 1) v8_wrapper.o — the only TU that sees V8 types. Built with V8's
    #    bundled clang + libc++ so v8:: and std::__Cr::* symbols match
    #    libv8_monolith.a. This TU exposes only v8wrap::* types externally.
    echo "Compiling v8_wrapper.o (V8 / libc++ side)..."
    "$CLANG" -std=c++23 -nostdinc++ \
      -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE \
      -DV8_GN_HEADER \
      -isystem "$V8_DIR/third_party/libc++/src/include" \
      -isystem "$V8_DIR/third_party/libc++abi/src/include" \
      -isystem "$V8_DIR/buildtools/third_party/libc++" \
      -I"$V8_DIR/include" -I"$V8_DIR/include/libplatform" \
      -I"$V8_DIR/$out_dir/gen/include" \
      -I"$PWD" \
      -c v8_wrapper.cc \
      -o "$WRAPPER_OBJ"

    # 2) test_main.o — plain libstdc++ compilation. Proves the boundary:
    #    this TU has ZERO V8 / libc++ flags and only sees v8_wrapper.h.
    echo "Compiling test_main.o (libstdc++ side, stock toolchain)..."
    g++ -std=c++23 \
      -I"$PWD" \
      -c test_main.cc \
      -o "$TEST_OBJ"

    # 3) Link. g++ pulls in libstdc++ for test_main.o's std::*. V8's libc++
    #    symbols (std::__Cr::*) are already merged into libv8_monolith.a by
    #    build.sh and don't collide. We link with V8's bundled lld because
    #    libv8_monolith.a carries libc++'s/libc++abi's duplicate C++-ABI
    #    symbols (__cxa_*) that GNU ld rejects as multiple-definition; lld
    #    handles them cleanly.
    echo "Linking v8_test..."
    g++ -o build/v8/test_build/v8_test \
      -fuse-ld=lld -B"$V8_DIR/third_party/llvm-build/Release+Asserts/bin" \
      "$TEST_OBJ" "$WRAPPER_OBJ" \
      "build/v8/$out_dir/obj/libv8_monolith.a" \
      -lpthread -ldl
    ;;
esac

echo "Running test..."
(cd build/v8/test_build && ./v8_test)
echo "Test passed. To clean run rm -rf build/"
