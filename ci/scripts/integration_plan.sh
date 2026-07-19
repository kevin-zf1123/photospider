#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci-plan"}
CI_BUILD_PROFILE=default
BUILD_TESTING=ON
unset PHOTOSPIDER_BUILD_IPC
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

run_logged cmake_configure configure_ci_build
run_logged validate_profile_cache require_ci_profile_cache
matrix_file="$CI_ARTIFACT_DIR/build_smoke_matrix.json"
run_logged build_smoke_plan \
  python3 -B "$SCRIPT_DIR/build_smoke_inventory.py" plan \
    --build-dir "$BUILD_DIR" \
    --ctest-executable "${CTEST_COMMAND:-ctest}" \
    --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    --label "$BUILD_SMOKE_LABEL" \
    --inventory-output "$CI_ARTIFACT_DIR/ctest_inventory.json" \
    --matrix-output "$matrix_file" \
    --names-output "$CI_ARTIFACT_DIR/build_smoke_names.z"

# @brief Persist one planner value to logs and the GitHub output channel.
# @param $1 Stable output key consumed by the workflow.
# @param $2 Single-line output value.
# @return The first failed write status, or zero.
# @throws Nothing; write failures terminate through set -e.
# @note Local callers without GITHUB_OUTPUT still receive outputs.log.
emit_output() {
  local key=$1
  local value=$2
  printf '%s=%s\n' "$key" "$value" |
    tee -a "$CI_ARTIFACT_DIR/outputs.log"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$key" "$value" >> "$GITHUB_OUTPUT"
  fi
}

: > "$CI_ARTIFACT_DIR/outputs.log"
build_smoke_matrix=$(<"$matrix_file")
emit_output build_smoke_matrix "$build_smoke_matrix"
