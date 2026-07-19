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
    run_logged ctest_discovery ctest -N --test-dir "$BUILD_DIR"

    total_tests=$(
      awk '/Total Tests:/ {print $3}' \
        "$CI_ARTIFACT_DIR/ctest_discovery.log" | tail -n 1
    )
    if [[ -z "$total_tests" || "$total_tests" -le 0 ]]; then
      echo "CTest discovery did not find any tests." >&2
      exit 1
    fi
    mark_ci_build_reusable
    echo "Default profile discovered $total_tests tests." |
      tee "$CI_ARTIFACT_DIR/summary.log"
    ;;
  ipc-disabled)
    BUILD_TESTING=OFF
    PHOTOSPIDER_BUILD_IPC=OFF
    ensure_ci_configured cmake_configure
    run_logged validate_profile_cache require_ci_profile_cache
    ensure_ci_targets build_photospider photospider
    mark_ci_build_reusable
    echo "IPC-disabled profile built photospider with BUILD_TESTING=OFF and IPC=OFF." |
      tee "$CI_ARTIFACT_DIR/summary.log"
    ;;
  dependency-disabled)
    BUILD_TESTING=OFF
    PHOTOSPIDER_BUILD_IPC=OFF
    PHOTOSPIDER_ENABLE_OPENCV=OFF
    PHOTOSPIDER_ENABLE_YAML=OFF
    CI_DISABLE_OPENCV_YAML_FIND=ON
    ensure_ci_configured cmake_configure
    run_logged validate_profile_cache require_ci_profile_cache
    ensure_ci_targets build_dependency_disabled_products \
      photospider_kernel \
      photospider
    mark_ci_build_reusable
    echo "Dependency-disabled profile built kernel and Host without OpenCV or YAML." |
      tee "$CI_ARTIFACT_DIR/summary.log"
    ;;
  *)
    echo "Unsupported CI_BUILD_PROFILE: $CI_BUILD_PROFILE" >&2
    exit 2
    ;;
esac
