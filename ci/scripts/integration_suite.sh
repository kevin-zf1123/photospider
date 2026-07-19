#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

CI_ARTIFACT_ROOT=${CI_ARTIFACT_ROOT:-"$REPO_ROOT/CI-results"}
DEFAULT_BUILD_DIR=$BUILD_DIR
DEFAULT_DISCOVERY_LOG="$CI_ARTIFACT_ROOT/build-integrity/ctest_discovery.log"
STATIC_SMOKE_RUNNER="$REPO_ROOT/tests/integration/static_product_consumer_smoke.py"
IPC_DISABLED_SMOKE_RUNNER="$REPO_ROOT/tests/integration/ipc_disabled_install_smoke.py"
DEPENDENCY_DISABLED_SMOKE_RUNNER="$REPO_ROOT/tests/integration/dependency_disabled_install_smoke.py"
suite_status=0

# @brief Run one independent integration shard and retain aggregate failure.
# @param $@ Command and arguments for the shard.
# @return Zero so later independent shards still run.
# @throws Nothing; child failure is stored in suite_status.
# @note suite_status preserves failure for the script's final exit status.
run_integration_check() {
  if ! "$@"; then
    suite_status=1
  fi
}

CI_BUILD_PROFILE=default BUILD_TESTING=ON BUILD_DIR="$DEFAULT_BUILD_DIR" \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/build-integrity" \
  bash "$SCRIPT_DIR/build_integrity.sh"
require_ctest_runner_pair \
  "$DEFAULT_DISCOVERY_LOG" StaticProductConsumerSmoke "$STATIC_SMOKE_RUNNER"
require_ctest_runner_pair \
  "$DEFAULT_DISCOVERY_LOG" IpcDisabledInstallSmoke \
  "$IPC_DISABLED_SMOKE_RUNNER"
require_ctest_runner_pair \
  "$DEFAULT_DISCOVERY_LOG" DependencyDisabledInstallSmoke \
  "$DEPENDENCY_DISABLED_SMOKE_RUNNER"
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/ctest-full" \
  bash "$SCRIPT_DIR/ctest_full.sh"
if ctest_inventory_has_exact_test \
  "$DEFAULT_DISCOVERY_LOG" StaticProductConsumerSmoke; then
  run_integration_check env \
    CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
    SMOKE_TEST=static-product-consumer \
    CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/static-product-consumer-smoke" \
    bash "$SCRIPT_DIR/build_smoke_test.sh"
fi
if ctest_inventory_has_exact_test \
  "$DEFAULT_DISCOVERY_LOG" IpcDisabledInstallSmoke; then
  IPC_DISABLED_BUILD_DIR=${IPC_DISABLED_BUILD_DIR:-"$REPO_ROOT/build/ci-ipc-disabled"}
  if env \
    CI_BUILD_PROFILE=ipc-disabled BUILD_TESTING=OFF \
      PHOTOSPIDER_BUILD_IPC=OFF BUILD_DIR="$IPC_DISABLED_BUILD_DIR" \
      CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/build-integrity-ipc-disabled" \
      bash "$SCRIPT_DIR/build_integrity.sh"; then
    run_integration_check env \
      CI_BUILD_PROFILE=ipc-disabled BUILD_DIR="$IPC_DISABLED_BUILD_DIR" \
      CI_REUSE_BUILD=ON SMOKE_TEST=ipc-disabled-install \
      CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/ipc-disabled-install-smoke" \
      bash "$SCRIPT_DIR/build_smoke_test.sh"
  else
    suite_status=1
  fi
fi
if ctest_inventory_has_exact_test \
  "$DEFAULT_DISCOVERY_LOG" DependencyDisabledInstallSmoke; then
  DEPENDENCY_DISABLED_BUILD_DIR=${DEPENDENCY_DISABLED_BUILD_DIR:-"$REPO_ROOT/build/ci-dependency-disabled"}
  if env \
    CI_BUILD_PROFILE=dependency-disabled BUILD_TESTING=OFF \
      PHOTOSPIDER_BUILD_IPC=OFF PHOTOSPIDER_ENABLE_OPENCV=OFF \
      PHOTOSPIDER_ENABLE_YAML=OFF CI_DISABLE_OPENCV_YAML_FIND=ON \
      BUILD_DIR="$DEPENDENCY_DISABLED_BUILD_DIR" \
      CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/build-integrity-dependency-disabled" \
      bash "$SCRIPT_DIR/build_integrity.sh"; then
    run_integration_check env \
      CI_BUILD_PROFILE=dependency-disabled \
      BUILD_DIR="$DEPENDENCY_DISABLED_BUILD_DIR" CI_REUSE_BUILD=ON \
      SMOKE_TEST=dependency-disabled-install \
      CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/dependency-disabled-install-smoke" \
      bash "$SCRIPT_DIR/build_smoke_test.sh"
  else
    suite_status=1
  fi
fi
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/graph-cli" \
  bash "$SCRIPT_DIR/graph_cli_script_test.sh"
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/propagation" \
  bash "$SCRIPT_DIR/propagation_script_test.sh"
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/plugin-load" \
  bash "$SCRIPT_DIR/plugin_load_test.sh"
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/scheduler-repeat" \
  SCHEDULER_REPEAT="${SCHEDULER_REPEAT:-5}" \
  GPU_PIPELINE_REPEAT="${GPU_PIPELINE_REPEAT:-3}" \
  bash "$SCRIPT_DIR/scheduler_repeat_test.sh"

exit "$suite_status"
