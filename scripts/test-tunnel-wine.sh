#!/usr/bin/env bash
# Builds the Windows tunnel integration test and runs it under Wine.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root/build/mingw}"

cmake -S "$root" -B "$build_dir" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --target test_tunnel

WINEDEBUG=-all wine "$build_dir/test_tunnel.exe"
