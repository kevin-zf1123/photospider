#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

case "$CI_BUILD_PROFILE" in
  default)
    BUILD_TESTING=ON
    unset PHOTOSPIDER_BUILD_IPC || true
    ensure_ci_configured cmake_configure
    run_logged validate_profile_cache require_ci_profile_cache
    ensure_ci_targets build_required_targets \
      graph_cli \
      test_propagation \
      lifecycle_op_plugin \
      override_lifecycle_op_plugin \
      destroy_count_scheduler_plugin \
      cpu_work_stealing_example_plugin \
      gpu_pipeline_example_plugin \
      serial_debug_example_plugin
    ensure_ci_all build_all
    run_logged build_smoke_inventory \
      python3 -B "$SCRIPT_DIR/build_smoke_inventory.py" plan \
        --build-dir "$BUILD_DIR" \
        --ctest-executable "${CTEST_COMMAND:-ctest}" \
        --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
        --label "$BUILD_SMOKE_LABEL" \
        --inventory-output "$CI_ARTIFACT_DIR/ctest_inventory.json" \
        --matrix-output "$CI_ARTIFACT_DIR/build_smoke_matrix.json" \
        --names-output "$CI_ARTIFACT_DIR/build_smoke_names.z"
    mark_ci_build_reusable
    echo "Default profile build and build-smoke inventory completed." |
      tee "$CI_ARTIFACT_DIR/summary.log"
    ;;
  *)
    echo "Unsupported CI_BUILD_PROFILE: $CI_BUILD_PROFILE" >&2
    exit 2
    ;;
esac
