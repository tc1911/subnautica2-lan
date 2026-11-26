#!/usr/bin/env bash
# Builds and runs the native unit tests (wire protocol only, no Windows needed).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root/build/native}"

cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build "$build_dir" --target test_protocol
ctest --test-dir "$build_dir" --output-on-failure
