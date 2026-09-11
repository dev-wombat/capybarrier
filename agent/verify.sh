#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${CAPYBARRIER_BUILD_DIR:-${root_dir}/build-agent}"

git -C "${root_dir}" diff --check
cmake -S "${root_dir}" -B "${build_dir}" -DBARRIER_BUILD_INSTALLER=OFF -DBARRIER_BUILD_TESTS=ON
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure
