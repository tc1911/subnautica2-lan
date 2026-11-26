#!/usr/bin/env bash
# Cross-compiles sn2lan.dll for Windows x64 with MinGW-w64.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root/build/mingw}"

cmake -S "$root" -B "$build_dir" \
    -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/mingw-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$build_dir" --target sn2lan sn2lan_dwmapi

echo "built: $build_dir/sn2lan.dll"
echo "built: $build_dir/dwmapi.dll"
