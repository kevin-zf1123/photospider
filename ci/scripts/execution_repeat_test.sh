#!/usr/bin/env bash

set -Eeuo pipefail

# @file execution_repeat_test.sh
# @brief Repeat the configured runtime model's deterministic behavior tests.
# @note Legacy scheduler and policy/execution profiles are selected only from
#   the complete configured target inventory.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

EXECUTION_REPEAT=${EXECUTION_REPEAT:-5}
LEGACY_GPU_REPEAT=${LEGACY_GPU_REPEAT:-3}

ensure_ci_configured cmake_configure
capture_ci_target_inventory
runtime_contract=$(ci_runtime_contract)
case "$runtime_contract" in
  legacy_scheduler)
    run_logged validate_execution_repeat_targets require_ci_targets \
      test_scheduler \
      test_gpu_pipeline_scheduler \
      test_milestone2
    ensure_ci_targets build_execution_repeat_tests \
      test_scheduler \
      test_gpu_pipeline_scheduler \
      test_milestone2

    run_gtest_checked scheduler_ready_repeat \
      "$BUILD_DIR/tests/test_scheduler" \
      "SchedulerDirtyReadyTasks.SourceFirstOrderOnSerialAndCpuSchedulers:SchedulerTestM33.ParallelComputeWithNewScheduler" \
      --gtest_repeat="$EXECUTION_REPEAT" \
      --gtest_shuffle=0

    run_gtest_checked scheduler_runtime_repeat \
      "$BUILD_DIR/tests/test_milestone2" \
      "GraphRuntimeSchedulerTest.StartStartsAttachedSchedulers:GraphRuntimeSchedulerTest.ReplaceScheduler" \
      --gtest_repeat="$EXECUTION_REPEAT" \
      --gtest_shuffle=0

    run_gtest_checked gpu_pipeline_repeat \
      "$BUILD_DIR/tests/test_gpu_pipeline_scheduler" \
      "GpuPipelineSchedulerTest.ProductionComputeUsesDeviceImplementation:GpuPipelineSchedulerTest.NewEpochCancelsStale" \
      --gtest_repeat="$LEGACY_GPU_REPEAT" \
      --gtest_shuffle=0
    ;;
  policy_execution)
    run_logged validate_execution_repeat_targets require_ci_targets \
      test_policy_registry \
      test_policy_execution \
      test_compute_run \
      test_resource_admission
    ensure_ci_targets build_execution_repeat_tests \
      test_policy_registry \
      test_policy_execution \
      test_compute_run \
      test_resource_admission

    run_gtest_checked policy_registry_repeat \
      "$BUILD_DIR/tests/test_policy_registry" "" \
      --gtest_repeat="$EXECUTION_REPEAT" \
      --gtest_shuffle=0

    run_gtest_checked policy_execution_repeat \
      "$BUILD_DIR/tests/test_policy_execution" \
      "PhysicalExecutionIntegration.*:PolicyExecutionFixture.*:ExecutionServiceReservedStart.*" \
      --gtest_repeat="$EXECUTION_REPEAT" \
      --gtest_shuffle=0

    run_gtest_checked execution_policy_repeat \
      "$BUILD_DIR/tests/test_compute_run" \
      "ExecutionServicePolicy.*:Issue75DeviceRouting.*" \
      --gtest_repeat="$EXECUTION_REPEAT" \
      --gtest_shuffle=0

    run_gtest_checked resource_admission_repeat \
      "$BUILD_DIR/tests/test_resource_admission" "" \
      --gtest_repeat="$EXECUTION_REPEAT" \
      --gtest_shuffle=0
    ;;
esac

echo "$runtime_contract repeat checks passed." |
  tee "$CI_ARTIFACT_DIR/summary.log"
