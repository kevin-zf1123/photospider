#!/usr/bin/env bash

set -Eeuo pipefail

# @file integration_plan.sh
# @brief Configure and validate the non-authoritative pre-build CI inventory.
# @note GoogleTest POST_BUILD discovery has not run in this job. An empty
#   build-smoke preview is therefore valid and is never routed to a matrix.

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

# @var prebuild_matrix_file
# @brief Disposable configuration-time matrix preview retained for diagnostics.
# @note The workflow never exposes this file as a job output; only the complete
#   build-integrity inventory is authoritative for matrix routing.
prebuild_matrix_file="$CI_ARTIFACT_DIR/prebuild_smoke_matrix.json"
run_logged prebuild_smoke_inventory \
  python3 -B "$SCRIPT_DIR/build_smoke_inventory.py" plan \
    --build-dir "$BUILD_DIR" \
    --ctest-executable "${CTEST_COMMAND:-ctest}" \
    --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    --label "$BUILD_SMOKE_LABEL" \
    --inventory-output "$CI_ARTIFACT_DIR/prebuild_ctest_inventory.json" \
    --matrix-output "$prebuild_matrix_file" \
    --names-output "$CI_ARTIFACT_DIR/prebuild_smoke_names.z" \
    --allow-empty

echo "Configuration-time CTest inventory preflight completed; build-smoke routing waits for the complete build." |
  tee "$CI_ARTIFACT_DIR/summary.log"
