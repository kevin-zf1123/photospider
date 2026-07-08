#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

ensure_ci_configured cmake_configure
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
mark_ci_build_reusable
run_logged ctest_discovery ctest -N --test-dir "$BUILD_DIR"

total_tests=$(awk '/Total Tests:/ {print $3}' "$CI_ARTIFACT_DIR/ctest_discovery.log" | tail -n 1)
if [[ -z "$total_tests" || "$total_tests" -le 0 ]]; then
  echo "CTest discovery did not find any tests." >&2
  exit 1
fi
echo "CTest discovered $total_tests tests." | tee "$CI_ARTIFACT_DIR/summary.log"
