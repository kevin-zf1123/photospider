#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

SCHEDULER_REPEAT=${SCHEDULER_REPEAT:-5}
GPU_PIPELINE_REPEAT=${GPU_PIPELINE_REPEAT:-3}

ensure_ci_configured cmake_configure
ensure_ci_targets build_scheduler_tests test_scheduler test_gpu_pipeline_scheduler test_milestone2

run_logged scheduler_ready_repeat \
  "$BUILD_DIR/tests/test_scheduler" \
  --gtest_filter=SchedulerDirtyReadyTasks.SourceFirstOrderOnSerialAndCpuSchedulers:SchedulerTestM33.ParallelComputeWithNewScheduler \
  --gtest_repeat="$SCHEDULER_REPEAT" \
  --gtest_shuffle=0

run_logged scheduler_runtime_repeat \
  "$BUILD_DIR/tests/test_milestone2" \
  --gtest_filter=GraphRuntimeSchedulerTest.StartStartsAttachedSchedulers:GraphRuntimeSchedulerTest.ReplaceScheduler \
  --gtest_repeat="$SCHEDULER_REPEAT" \
  --gtest_shuffle=0

run_logged gpu_pipeline_repeat \
  "$BUILD_DIR/tests/test_gpu_pipeline_scheduler" \
  --gtest_filter=GpuPipelineSchedulerTest.ProductionComputeUsesDeviceImplementation:GpuPipelineSchedulerTest.NewEpochCancelsStale \
  --gtest_repeat="$GPU_PIPELINE_REPEAT" \
  --gtest_shuffle=0

echo "scheduler repeat checks passed." | tee "$CI_ARTIFACT_DIR/summary.log"
