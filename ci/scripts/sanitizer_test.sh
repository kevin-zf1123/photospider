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

ensure_ci_configured "cmake_configure_$SANITIZER"
capture_ci_target_inventory
runtime_contract=$(ci_runtime_contract)
case "$runtime_contract" in
  legacy_scheduler)
    run_logged "validate_sanitizer_targets_$SANITIZER" require_ci_targets \
      test_scheduler \
      test_compute_service_split \
      test_propagation_contracts
    ensure_ci_targets "build_sanitizer_$SANITIZER" \
      test_scheduler \
      test_compute_service_split \
      test_propagation_contracts

    run_gtest_checked "sanitizer_scheduler_$SANITIZER" \
      "$BUILD_DIR/tests/test_scheduler" \
      "SchedulerDirtyReadyTasks.SourceFirstOrderOnSerialAndCpuSchedulers:SchedulerTestM33.ParallelComputeWithNewScheduler"
    ;;
  policy_execution)
    run_logged "validate_sanitizer_targets_$SANITIZER" require_ci_targets \
      test_policy_execution \
      test_compute_run \
      test_resource_admission \
      test_compute_service_split \
      test_propagation_contracts
    ensure_ci_targets "build_sanitizer_$SANITIZER" \
      test_policy_execution \
      test_compute_run \
      test_resource_admission \
      test_compute_service_split \
      test_propagation_contracts

    run_gtest_checked "sanitizer_policy_execution_$SANITIZER" \
      "$BUILD_DIR/tests/test_policy_execution" \
      "PhysicalExecutionIntegration.*:PolicyExecutionFixture.*:ExecutionServiceReservedStart.*"
    run_gtest_checked "sanitizer_execution_policy_$SANITIZER" \
      "$BUILD_DIR/tests/test_compute_run" \
      "ExecutionServicePolicy.*:Issue75DeviceRouting.*"
    run_gtest_checked "sanitizer_resource_admission_$SANITIZER" \
      "$BUILD_DIR/tests/test_resource_admission" ""
    ;;
esac

run_gtest_checked "sanitizer_compute_$SANITIZER" \
  "$BUILD_DIR/tests/test_compute_service_split" \
  "ComputeGeometrySplit.*:ComputeCachePolicySplit.*:TaskGraphPlanningSplit.PreservesSequentialParallelPlanParity:IntentUpdateCoordinatorSplit.ValidatesRtDirtyRoiAndCoordinatesRtFirstConcurrency"

run_gtest_checked "sanitizer_propagation_$SANITIZER" \
  "$BUILD_DIR/tests/test_propagation_contracts" ""

echo "$runtime_contract $SANITIZER sanitizer checks passed." |
  tee "$CI_ARTIFACT_DIR/summary.log"
