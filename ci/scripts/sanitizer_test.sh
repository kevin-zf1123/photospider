#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SANITIZER=${SANITIZER:-asan}
case "$SANITIZER" in
  asan)
    export USE_ASAN=ON
    export USE_TSAN=OFF
    ;;
  tsan)
    export USE_ASAN=OFF
    export USE_TSAN=ON
    ;;
  *)
    echo "Unsupported SANITIZER='$SANITIZER'. Use 'asan' or 'tsan'." >&2
    exit 2
    ;;
esac

export CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-RelWithDebInfo}
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
export BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci-$SANITIZER"}

# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

run_gtest_checked() {
  local name=$1
  local binary=$2
  local filter=${3:-}
  local list_log="$CI_ARTIFACT_DIR/${name}_list.log"
  local -a list_cmd=("$binary" --gtest_list_tests)
  local -a run_cmd=("$binary")
  if [[ -n "$filter" ]]; then
    list_cmd+=(--gtest_filter="$filter")
    run_cmd+=(--gtest_filter="$filter")
  fi
  "${list_cmd[@]}" > "$list_log" 2>&1
  local selected_count
  selected_count=$(grep -Ec '^  [A-Za-z0-9_]' "$list_log" || true)
  if [[ "$selected_count" -le 0 ]]; then
    echo "No GoogleTest cases selected for $name." >&2
    cat "$list_log" >&2
    exit 1
  fi
  echo "$selected_count GoogleTest case(s) selected for $name." |
    tee "$CI_ARTIFACT_DIR/${name}_selected.log"
  run_logged "$name" "${run_cmd[@]}"
}

ensure_ci_configured "cmake_configure_$SANITIZER"
ensure_ci_targets "build_sanitizer_$SANITIZER" \
  test_scheduler \
  test_compute_service_split \
  test_propagation_contracts

run_gtest_checked "sanitizer_scheduler_$SANITIZER" \
  "$BUILD_DIR/tests/test_scheduler" \
  "SchedulerDirtyReadyTasks.SourceFirstOrderOnSerialAndCpuSchedulers:SchedulerTestM33.ParallelComputeWithNewScheduler"

run_gtest_checked "sanitizer_compute_$SANITIZER" \
  "$BUILD_DIR/tests/test_compute_service_split" \
  "ComputeGeometrySplit.*:ComputeCachePolicySplit.*:TaskGraphPlanningSplit.PreservesSequentialParallelPlanParity:IntentUpdateCoordinatorSplit.ValidatesRtDirtyRoiAndCoordinatesRtFirstConcurrency"

run_gtest_checked "sanitizer_propagation_$SANITIZER" \
  "$BUILD_DIR/tests/test_propagation_contracts"

echo "$SANITIZER sanitizer checks passed." | tee "$CI_ARTIFACT_DIR/summary.log"
